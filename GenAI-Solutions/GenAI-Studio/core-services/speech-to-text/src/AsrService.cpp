// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
//
// ==========================================================
//
//  Bug / Attack vector                  Root cause & fix
//  ────────────────────────────────────  ──────────────────────────────────
//  No 'file' field in multipart         Crow silently returned empty string →
//                                       explicit has_file() check → 400
//  Empty audio data                     Empty content passed to Whisper →
//                                       explicit empty check → 400
//  Audio file > 50 MB                   OOM / slow parse → 413 limit added
//  Invalid response_format              Silently defaulted → explicit 400
//  Wrong HTTP method (GET/DELETE on     Crow returned 404 instead of 405 →
//    POST-only endpoints)               explicit 405 handlers added
//  Concurrent requests                  WhisperEngine::request_mu_ blocks
//                                       forever → service-level timed_mutex
//                                       + 30 s timeout → 429 rate_limited
//  Temp file not cleaned on exception   No RAII → TempFileGuard added
//  Unhandled C++ exception              Server crash → exception handler added
//  Raw PCM empty body                   Passed to Whisper → 400 check added
//  Raw PCM too large                    OOM → 413 limit added
//  Error handler overrides 400 body     Guard: skip if body already set
//  Transcription timeout                504 returned instead of hanging
//
// ---------------------------------------------------------------------

#include "AsrService.hpp"
#include "ErrorResponder.hpp"
#include "ModelCatalog.hpp"
#include "OpenAiAudioContract.hpp"
#include "TranscriptionExecutor.hpp"
#define CPPHTTPLIB_THREAD_POOL_COUNT 4
#include "httplib.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <memory>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <set>
#include <unordered_map>
#include <optional>

using json  = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Ms    = std::chrono::duration<double, std::milli>;

// ===========================================================================
// Helpers
// ===========================================================================
static json errorJson(const std::string& msg,
                       const std::string& type = "server_error") {
    return json{{"error", {{"message", msg}, {"type", type}}}};
}

static std::string codeToLanguageName(const std::string& code) {
    static const std::unordered_map<std::string, std::string> table = {
        {"en", "english"}, {"zh", "chinese"}, {"de", "german"},
        {"es", "spanish"}, {"fr", "french"},  {"it", "italian"},
        {"ja", "japanese"},{"ko", "korean"},  {"pt", "portuguese"},
        {"ru", "russian"}, {"ar", "arabic"},  {"nl", "dutch"},
        {"pl", "polish"},  {"tr", "turkish"}, {"vi", "vietnamese"},
    };
    auto it = table.find(code);
    return (it != table.end()) ? it->second : code;
}

static const std::set<std::string> VALID_FORMATS = {
    "json", "text", "verbose_json", "srt", "vtt", "diarized_json"
};

static const std::set<std::string> SUPPORTED_AUDIO_EXTENSIONS = {
    ".wav", ".mp3", ".mp4", ".mpeg", ".mpga", ".m4a", ".webm", ".flac", ".ogg"
};

static const std::set<std::string> SUPPORTED_AUDIO_CONTENT_TYPES = {
    "audio/wav",
    "audio/x-wav",
    "audio/wave",
    "audio/mpeg",
    "audio/mp3",
    "audio/mp4",
    "audio/x-m4a",
    "audio/m4a",
    "audio/webm",
    "audio/flac",
    "audio/ogg",
    "application/ogg",
    "video/mp4",
    "application/octet-stream"
};

static std::string toLowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string trimCopy(std::string s) {
    const auto is_not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), is_not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), is_not_space).base(), s.end());
    return s;
}

static std::string fileExtensionLower(const std::string& filename) {
    if (filename.empty()) return "";
    std::filesystem::path p(filename);
    return toLowerCopy(p.extension().string());
}

static bool looksLikeAudioMagic(const std::string& content) {
    const auto size = content.size();
    if (size >= 12 &&
        std::memcmp(content.data(), "RIFF", 4) == 0 &&
        std::memcmp(content.data() + 8, "WAVE", 4) == 0) {
        return true; // WAV
    }
    if (size >= 3 && std::memcmp(content.data(), "ID3", 3) == 0) {
        return true; // MP3 (ID3 header)
    }
    if (size >= 2) {
        const unsigned char b0 = static_cast<unsigned char>(content[0]);
        const unsigned char b1 = static_cast<unsigned char>(content[1]);
        if (b0 == 0xFF && (b1 == 0xFB || b1 == 0xF3 || b1 == 0xF2)) {
            return true; // MP3 frame sync
        }
    }
    if (size >= 4) {
        if (std::memcmp(content.data(), "fLaC", 4) == 0) return true;
        if (std::memcmp(content.data(), "OggS", 4) == 0) return true;
        const unsigned char ebml[] = {0x1A, 0x45, 0xDF, 0xA3};
        if (std::memcmp(content.data(), ebml, 4) == 0) return true; // WebM/Matroska
    }
    if (size >= 8) {
        if (std::memcmp(content.data() + 4, "ftyp", 4) == 0) return true; // MP4/M4A
    }
    return false;
}

static bool validateAudioUpload(const httplib::MultipartFormData& file_field,
                                std::string& err_msg) {
    const std::string ext = fileExtensionLower(file_field.filename);
    std::string content_type = toLowerCopy(file_field.content_type);
    const auto semicolon = content_type.find(';');
    if (semicolon != std::string::npos) {
        content_type = content_type.substr(0, semicolon);
    }
    content_type = trimCopy(content_type);

    const bool ext_is_known = !ext.empty() &&
                              SUPPORTED_AUDIO_EXTENSIONS.find(ext) != SUPPORTED_AUDIO_EXTENSIONS.end();
    const bool type_is_known = !content_type.empty() &&
                               (SUPPORTED_AUDIO_CONTENT_TYPES.find(content_type) != SUPPORTED_AUDIO_CONTENT_TYPES.end() ||
                                content_type.rfind("audio/", 0) == 0);
    if (ext_is_known || type_is_known) return true;

    if (looksLikeAudioMagic(file_field.content)) return true;

    if (!ext.empty() && !ext_is_known) {
        err_msg = "Unsupported audio file extension '" + ext +
                  "'. Supported: wav, mp3, mp4, mpeg, mpga, m4a, webm, flac, ogg";
        return false;
    }
    if (!content_type.empty() && !type_is_known) {
        err_msg = "Unsupported content type '" + content_type +
                  "'. Expected an audio content type";
        return false;
    }
    if (!file_field.filename.empty() || !content_type.empty()) {
        err_msg = "Uploaded file does not look like a supported audio format";
        return false;
    }
    return true;
}

static double round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

static double estimateDurationSeconds(const std::string& text) {
    if (text.empty()) return 0.0;
    std::istringstream iss(text);
    size_t words = 0;
    std::string word;
    while (iss >> word) ++words;
    if (words == 0) return 0.0;
    return std::max(0.5, static_cast<double>(words) * 0.45);
}

static std::string formatTimestamp(double sec, bool vtt_format) {
    if (sec < 0) sec = 0;
    const int64_t total_ms = static_cast<int64_t>(std::llround(sec * 1000.0));
    const int64_t hours = total_ms / 3600000;
    const int64_t minutes = (total_ms % 3600000) / 60000;
    const int64_t seconds = (total_ms % 60000) / 1000;
    const int64_t millis = total_ms % 1000;
    std::ostringstream ss;
    ss << std::setw(2) << std::setfill('0') << hours << ":"
       << std::setw(2) << std::setfill('0') << minutes << ":"
       << std::setw(2) << std::setfill('0') << seconds
       << (vtt_format ? "." : ",")
       << std::setw(3) << std::setfill('0') << millis;
    return ss.str();
}

static std::string renderSrt(const std::string& text, double duration_sec) {
    std::ostringstream ss;
    ss << "1\n"
       << formatTimestamp(0.0, false)
       << " --> "
       << formatTimestamp(std::max(duration_sec, 0.1), false)
       << "\n"
       << text << "\n";
    return ss.str();
}

static std::string renderVtt(const std::string& text, double duration_sec) {
    std::ostringstream ss;
    ss << "WEBVTT\n\n"
       << formatTimestamp(0.0, true)
       << " --> "
       << formatTimestamp(std::max(duration_sec, 0.1), true)
       << "\n"
       << text << "\n";
    return ss.str();
}

static json buildDiarizedResponse(const std::string& task,
                                  const TranscriptionResult& result,
                                  const json& x_timing) {
    const double duration_sec = estimateDurationSeconds(result.text);
    return json{
        {"task", task},
        {"duration", duration_sec},
        {"text", result.text},
        {"segments", json::array({
            json{
                {"type", "transcript.text.segment"},
                {"id", "seg_0001"},
                {"start", 0.0},
                {"end", duration_sec},
                {"text", result.text},
                {"speaker", "A"}
            }
        })},
        {"x_process_time_ms", result.process_time_ms},
        {"x_timing", x_timing}
    };
}

static json buildTimingJson(const TranscriptionResult& result,
                            double request_total_ms,
                            double save_file_ms,
                            double lock_wait_ms) {
    return json{
        {"request_total_ms", round3(request_total_ms)},
        {"save_file_ms",     round3(save_file_ms)},
        {"lock_wait_ms",     round3(lock_wait_ms)},
        {"engine_total_ms",  result.timing.total_ms},
        {"engine_config_ms", result.timing.configure_ms},
        {"engine_start_ms",  result.timing.start_ms},
        {"engine_feed_ms",   result.timing.feed_ms},
        {"engine_wait_ms",   result.timing.wait_ms},
        {"engine_stop_ms",   result.timing.stop_ms}
    };
}

static void setTimingHeaders(httplib::Response& res,
                             double request_total_ms,
                             double save_file_ms,
                             double lock_wait_ms,
                             const TranscriptionResult& result) {
    res.set_header("X-Request-Total-Ms", std::to_string(round3(request_total_ms)));
    res.set_header("X-Save-File-Ms",     std::to_string(round3(save_file_ms)));
    res.set_header("X-Lock-Wait-Ms",     std::to_string(round3(lock_wait_ms)));
    res.set_header("X-Engine-Total-Ms",  std::to_string(result.timing.total_ms));
    res.set_header("X-Engine-Config-Ms", std::to_string(result.timing.configure_ms));
    res.set_header("X-Engine-Start-Ms",  std::to_string(result.timing.start_ms));
    res.set_header("X-Engine-Feed-Ms",   std::to_string(result.timing.feed_ms));
    res.set_header("X-Engine-Wait-Ms",   std::to_string(result.timing.wait_ms));
    res.set_header("X-Engine-Stop-Ms",   std::to_string(result.timing.stop_ms));
}

// ===========================================================================
// SSE streaming helper – streams transcription result word-by-word
// Format matches OpenAI's streaming transcription SSE protocol:
//   data: {"type":"transcript.text.delta","delta":"word "}
//   data: {"type":"transcript.text.done","text":"...","language":"..."}
//   data: [DONE]
// ===========================================================================
static void streamTranscriptionSSE(httplib::Response& res,
                                    const TranscriptionResult& result,
                                    const std::string& task = "transcribe",
                                    const std::string& response_format = "json") {
    res.set_header("Cache-Control",    "no-cache");
    res.set_header("Connection",       "keep-alive");
    res.set_header("X-Accel-Buffering","no");

    std::string text = result.text;
    std::string lang = result.language;
    std::string task_copy = task;
    const bool send_segment_event = (response_format == "diarized_json");
    const double duration_sec = estimateDurationSeconds(result.text);
    auto written = std::make_shared<bool>(false);

    res.set_chunked_content_provider(
        "text/event-stream",
        [text, lang, task_copy, written, send_segment_event, duration_sec](size_t /*offset*/,
                                          httplib::DataSink& sink) -> bool {
            if (*written) { sink.done(); return true; }
            *written = true;

            std::istringstream iss(text);
            std::string word;
            while (iss >> word) {
                json delta = {
                    {"type",  "transcript.text.delta"},
                    {"delta", word + " "}
                };
                std::string chunk = "data: " + delta.dump() + "\n\n";
                if (!sink.write(chunk.c_str(), chunk.size())) return false;
            }

            if (send_segment_event) {
                json seg_evt = {
                    {"type", "transcript.text.segment"},
                    {"id", "seg_0001"},
                    {"start", 0.0},
                    {"end", duration_sec},
                    {"text", text},
                    {"speaker", "A"}
                };
                std::string seg_str = "data: " + seg_evt.dump() + "\n\n";
                if (!sink.write(seg_str.c_str(), seg_str.size())) return false;
            }

            json done_evt = {
                {"type",     "transcript.text.done"},
                {"task",     task_copy},
                {"text",     text},
                {"language", lang},
                {"usage", {{"type", "duration"}, {"seconds", std::max(1.0, std::round(duration_sec))}}}
            };
            std::string done_str = "data: " + done_evt.dump() + "\n\n";
            if (!sink.write(done_str.c_str(), done_str.size())) return false;

            const char* end = "data: [DONE]\n\n";
            sink.write(end, std::strlen(end));
            sink.done();
            return true;
        }
    );
}

class OpenAiAudioRequestValidator {
public:
    explicit OpenAiAudioRequestValidator(size_t max_audio_body_bytes)
        : max_audio_body_bytes_(max_audio_body_bytes) {}

    bool validate(const httplib::Request& req,
                  httplib::Response& res,
                  OpenAiAudioRouteKind route_kind,
                  OpenAiAudioRequestParams& params,
                  std::string& audio_content) const;

private:
    bool failValidation(httplib::Response& res,
                        int status,
                        const std::string& message) const;
    bool validateAudioPayload(const httplib::Request& req,
                              httplib::Response& res,
                              std::string& audio_content) const;
    bool validateTranscriptionInclude(const httplib::Request& req,
                                      httplib::Response& res,
                                      OpenAiAudioRouteKind route_kind) const;
    bool resolveAndValidateModel(const httplib::Request& req,
                                 httplib::Response& res,
                                 OpenAiAudioRouteKind route_kind,
                                 OpenAiAudioRequestParams& params) const;
    bool resolveAndValidateResponseFormat(const httplib::Request& req,
                                          httplib::Response& res,
                                          OpenAiAudioRouteKind route_kind,
                                          OpenAiAudioRequestParams& params) const;
    void resolveLanguageAndStream(const httplib::Request& req,
                                  OpenAiAudioRouteKind route_kind,
                                  OpenAiAudioRequestParams& params) const;

    size_t max_audio_body_bytes_;
};

bool OpenAiAudioRequestValidator::failValidation(httplib::Response& res,
                                                 int status,
                                                 const std::string& message) const {
    res.status = status;
    res.set_content(
        errorJson(message, "invalid_request_error").dump(),
        "application/json");
    return false;
}

bool OpenAiAudioRequestValidator::validateAudioPayload(const httplib::Request& req,
                                                       httplib::Response& res,
                                                       std::string& audio_content) const {
    if (req.body.size() > max_audio_body_bytes_) {
        return failValidation(res, 413, "Audio file exceeds configured limit");
    }

    if (!req.has_file("file")) {
        return failValidation(res, 400, "No 'file' field in multipart form data");
    }

    const auto& file_field = req.get_file_value("file");
    if (file_field.content.empty()) {
        return failValidation(res, 400, "Empty audio data in 'file' field");
    }

    if (file_field.content.size() > max_audio_body_bytes_) {
        return failValidation(res, 413, "Audio file exceeds configured limit");
    }

    std::string validation_error;
    if (!validateAudioUpload(file_field, validation_error)) {
        return failValidation(res, 400, validation_error);
    }

    audio_content = file_field.content;
    return true;
}

bool OpenAiAudioRequestValidator::validateTranscriptionInclude(const httplib::Request& req,
                                                               httplib::Response& res,
                                                               OpenAiAudioRouteKind route_kind) const {
    if (route_kind != OpenAiAudioRouteKind::Transcriptions) return true;
    if (!req.has_file("include") && !req.has_file("include[]")) return true;

    const auto include_field = req.has_file("include")
                             ? req.get_file_value("include")
                             : req.get_file_value("include[]");
    if (include_field.content.find("logprobs") != std::string::npos) {
        return failValidation(res, 400, "include=logprobs is not supported by this backend");
    }
    return true;
}

bool OpenAiAudioRequestValidator::resolveAndValidateModel(const httplib::Request& req,
                                                          httplib::Response& res,
                                                          OpenAiAudioRouteKind route_kind,
                                                          OpenAiAudioRequestParams& params) const {
    if (!req.has_file("model")) {
        return failValidation(res, 400, "No 'model' field in multipart form data");
    }

    const auto& model_field = req.get_file_value("model");
    const std::string requested_model = trimCopy(model_field.content);
    if (requested_model.empty()) {
        return failValidation(res, 400, "Empty 'model' value in multipart form data");
    }

    std::string normalized_model;
    if (route_kind == OpenAiAudioRouteKind::Translations) {
        if (!asr::catalog::normalizeTranslationModel(requested_model, normalized_model)) {
            return failValidation(
                res, 400, "Unsupported model '" + requested_model + "' for translations");
        }
        params.model = normalized_model;
        return true;
    }

    if (!asr::catalog::normalizeTranscriptionModel(requested_model, normalized_model)) {
        return failValidation(
            res, 400, "Unsupported model '" + requested_model + "' for transcriptions");
    }
    params.model = normalized_model;
    return true;
}

bool OpenAiAudioRequestValidator::resolveAndValidateResponseFormat(
    const httplib::Request& req,
    httplib::Response& res,
    OpenAiAudioRouteKind route_kind,
    OpenAiAudioRequestParams& params) const {
    if (req.has_file("response_format")) {
        const auto& response_format_field = req.get_file_value("response_format");
        if (!response_format_field.content.empty()) {
            params.response_format = response_format_field.content;
        }
    }

    if (VALID_FORMATS.find(params.response_format) == VALID_FORMATS.end()) {
        const char* allowed_formats = (route_kind == OpenAiAudioRouteKind::Translations)
                                    ? "json, text, srt, verbose_json, vtt"
                                    : "json, text, srt, verbose_json, vtt, diarized_json";
        return failValidation(
            res,
            400,
            "Invalid response_format '" + params.response_format +
            "'. Must be one of: " + allowed_formats);
    }

    if (route_kind == OpenAiAudioRouteKind::Translations &&
        params.response_format == "diarized_json") {
        return failValidation(
            res, 400, "response_format diarized_json is not supported for translations");
    }

    if (route_kind == OpenAiAudioRouteKind::Transcriptions &&
        (req.has_file("timestamp_granularities") || req.has_file("timestamp_granularities[]")) &&
        params.response_format != "verbose_json") {
        return failValidation(
            res, 400, "timestamp_granularities requires response_format=verbose_json");
    }

    return true;
}

void OpenAiAudioRequestValidator::resolveLanguageAndStream(const httplib::Request& req,
                                                           OpenAiAudioRouteKind route_kind,
                                                           OpenAiAudioRequestParams& params) const {
    if (route_kind != OpenAiAudioRouteKind::Translations && req.has_file("language")) {
        const auto& language_field = req.get_file_value("language");
        if (!language_field.content.empty()) params.language = language_field.content;
    }

    if (route_kind == OpenAiAudioRouteKind::TranscriptionsStream) {
        params.stream_mode = true;
    } else if (req.has_file("stream")) {
        const auto& stream_field = req.get_file_value("stream");
        params.stream_mode = (stream_field.content == "true" || stream_field.content == "1");
    }

    if (route_kind == OpenAiAudioRouteKind::Transcriptions &&
        params.model == "whisper-tiny" &&
        params.stream_mode) {
        params.stream_mode = false;
    }
}

bool OpenAiAudioRequestValidator::validate(const httplib::Request& req,
                                           httplib::Response& res,
                                           OpenAiAudioRouteKind route_kind,
                                           OpenAiAudioRequestParams& params,
                                           std::string& audio_content) const {
    params = OpenAiAudioRequestParams{};
    audio_content.clear();

    if (!validateAudioPayload(req, res, audio_content)) return false;
    if (!validateTranscriptionInclude(req, res, route_kind)) return false;
    if (!resolveAndValidateModel(req, res, route_kind, params)) return false;
    if (!resolveAndValidateResponseFormat(req, res, route_kind, params)) return false;
    resolveLanguageAndStream(req, route_kind, params);
    return true;
}

static void setOpenAiResponse(
    httplib::Response& res,
    const TranscriptionResult& result,
    const OpenAiAudioRequestParams& params,
    const std::string& task,
    const json& x_timing) {
    const double estimated_duration_seconds = estimateDurationSeconds(result.text);

    if (params.stream_mode) {
        streamTranscriptionSSE(res, result, task, params.response_format);
        return;
    }

    if (params.response_format == "text") {
        res.set_content(result.text, "text/plain");
    } else if (params.response_format == "srt") {
        res.set_content(renderSrt(result.text, estimated_duration_seconds), "text/plain");
    } else if (params.response_format == "vtt") {
        res.set_content(renderVtt(result.text, estimated_duration_seconds), "text/vtt");
    } else if (params.response_format == "diarized_json") {
        res.set_content(buildDiarizedResponse(task, result, x_timing).dump(), "application/json");
    } else if (params.response_format == "verbose_json") {
        res.set_content(json{
            {"task", task},
            {"language", codeToLanguageName(result.language)},
            {"duration", estimated_duration_seconds},
            {"text", result.text},
            {"words", json::array()},
            {"segments", json::array()},
            {"x_process_time_ms", result.process_time_ms},
            {"x_timing", x_timing}
        }.dump(), "application/json");
    } else {
        res.set_content(json{
            {"text", result.text},
            {"x_timing", x_timing}
        }.dump(), "application/json");
    }
}

// ===========================================================================
// AsrService constructor
// ===========================================================================
AsrService::AsrService(const AsrRuntimeConfig& config)
    : config_(config),
      engine_(config.model_path, config.vad_model_path),
      port_(config.port)
{
    engine_.initialize();
}

void AsrService::handleOpenAiRequest(const httplib::Request& req,
                                     httplib::Response& res,
                                     OpenAiAudioRouteKind route_kind,
                                     bool translate,
                                     const std::string& task,
                                     std::atomic<uint64_t>& counter) {
    counter.fetch_add(1, std::memory_order_relaxed);
    const auto t_request_start = Clock::now();
    const OpenAiAudioRequestValidator request_validator(config_.max_audio_body_bytes);
    const TranscriptionExecutor transcription_executor(engine_, engine_mutex_, config_);

    OpenAiAudioRequestParams params;
    std::string audio_content;
    if (!request_validator.validate(
            req,
            res,
            route_kind,
            params,
            audio_content)) {
        return;
    }

    std::string route_path = "/v1/audio/transcriptions";
    if (route_kind == OpenAiAudioRouteKind::Translations) {
        route_path = "/v1/audio/translations";
    } else if (route_kind == OpenAiAudioRouteKind::TranscriptionsStream) {
        route_path = "/v1/audio/transcriptions/stream";
    }

    std::cout << "[ASR] POST " << route_path
              << " lang=" << params.language
              << " model=" << params.model
              << " fmt=" << params.response_format
              << " stream=" << (params.stream_mode ? "true" : "false")
              << " size=" << audio_content.size() << "B\n";

    TranscriptionExecution execution;
    int error_status = 0;
    std::string error_message;
    if (!transcription_executor.transcribeFileAudio(
            audio_content,
            params.language,
            translate,
            execution,
            error_status,
            error_message)) {
        if (error_status == asr::errors::kBusyHttpStatus) {
            asr::errors::setBusyRateLimitedError(req, res, error_message);
            return;
        }
        res.status = error_status;
        res.set_content(errorJson(error_message).dump(), "application/json");
        return;
    }

    const double request_total_ms = Ms(Clock::now() - t_request_start).count();
    const json x_timing = buildTimingJson(
        execution.result,
        request_total_ms,
        execution.save_file_ms,
        execution.lock_wait_ms);
    setTimingHeaders(
        res,
        request_total_ms,
        execution.save_file_ms,
        execution.lock_wait_ms,
        execution.result);
    setOpenAiResponse(res, execution.result, params, task, x_timing);
}

void AsrService::handleLegacyMultipartTranscription(const httplib::Request& req,
                                                    httplib::Response& res) {
    counters_.legacy_total.fetch_add(1, std::memory_order_relaxed);
    const auto t_request_start = Clock::now();
    const TranscriptionExecutor transcription_executor(engine_, engine_mutex_, config_);

    if (config_.legacy_hard_disable) {
        asr::errors::setCanonicalErrorForRequest(
            req,
            res,
            410,
            "Legacy endpoint is disabled. Use /v1/audio/transcriptions",
            "invalid_request_error",
            "legacy_endpoint_disabled",
            false);
        return;
    }

    if (req.body.size() > config_.max_audio_body_bytes) {
        res.status = 413;
        res.set_content(
            errorJson("Audio file exceeds configured limit",
                      "invalid_request_error").dump(),
            "application/json");
        return;
    }
    if (!req.has_file("audio")) {
        res.status = 400;
        res.set_content(
            errorJson("No 'audio' field in multipart form data",
                      "invalid_request_error").dump(),
            "application/json");
        return;
    }
    const auto& audio_field = req.get_file_value("audio");
    if (audio_field.content.empty()) {
        res.status = 400;
        res.set_content(
            errorJson("Empty audio data in 'audio' field",
                      "invalid_request_error").dump(),
            "application/json");
        return;
    }
    if (audio_field.content.size() > config_.max_audio_body_bytes) {
        res.status = 413;
        res.set_content(
            errorJson("Audio file exceeds configured limit",
                      "invalid_request_error").dump(),
            "application/json");
        return;
    }
    {
        std::string validation_error;
        if (!validateAudioUpload(audio_field, validation_error)) {
            res.status = 400;
            res.set_content(
                errorJson(validation_error, "invalid_request_error").dump(),
                "application/json");
            return;
        }
    }

    std::string lang = "en";
    if (req.has_param("lang")) lang = req.get_param_value("lang");

    std::cout << "[ASR] POST " << req.path
              << " lang=" << lang
              << " size=" << audio_field.content.size() << "B\n";

    TranscriptionExecution execution;
    int error_status = 0;
    std::string error_message;
    if (!transcription_executor.transcribeFileAudio(
            audio_field.content,
            lang,
            /*translate=*/false,
            execution,
            error_status,
            error_message)) {
        if (error_status == asr::errors::kBusyHttpStatus) {
            asr::errors::setBusyRateLimitedError(req, res, error_message);
            return;
        }
        if (error_message == "Failed to write temp file") {
            res.status = 500;
            res.set_content(errorJson(error_message).dump(), "application/json");
            return;
        }
        res.status = 500;
        res.set_content(
            json{{"message", "Transcription failed"},
                 {"error", error_message},
                 {"status", "failure"}}.dump(),
            "application/json");
        return;
    }

    const double request_total_ms = Ms(Clock::now() - t_request_start).count();
    setTimingHeaders(
        res,
        request_total_ms,
        execution.save_file_ms,
        execution.lock_wait_ms,
        execution.result);
    const json x_timing = buildTimingJson(
        execution.result,
        request_total_ms,
        execution.save_file_ms,
        execution.lock_wait_ms);

    if (config_.legacy_compat_response) {
        res.set_content(json{
            {"message",         "Text generated"},
            {"text",            execution.result.text},
            {"language",        execution.result.language},
            {"process_time_ms", execution.result.process_time_ms},
            {"x_timing",        x_timing},
            {"status",          "success"}
        }.dump(), "application/json");
    } else {
        res.set_content(json{
            {"text", execution.result.text},
            {"x_timing", x_timing}
        }.dump(), "application/json");
    }
}

void AsrService::handleLegacyRawTranscription(const httplib::Request& req,
                                              httplib::Response& res) {
    counters_.legacy_total.fetch_add(1, std::memory_order_relaxed);
    const auto t_request_start = Clock::now();
    const TranscriptionExecutor transcription_executor(engine_, engine_mutex_, config_);

    if (config_.legacy_hard_disable) {
        asr::errors::setCanonicalErrorForRequest(
            req,
            res,
            410,
            "Legacy endpoint is disabled. Use /v1/audio/transcriptions",
            "invalid_request_error",
            "legacy_endpoint_disabled",
            false);
        return;
    }

    if (req.body.empty()) {
        res.status = 400;
        res.set_content(
            errorJson("Empty request body", "invalid_request_error").dump(),
            "application/json");
        return;
    }
    if (req.body.size() > config_.max_pcm_body_bytes) {
        res.status = 413;
        res.set_content(
            errorJson("PCM data exceeds configured limit",
                      "invalid_request_error").dump(),
            "application/json");
        return;
    }
    if ((req.body.size() % 2) != 0) {
        res.status = 400;
        res.set_content(
            errorJson("PCM payload must contain an even number of bytes",
                      "invalid_request_error").dump(),
            "application/json");
        return;
    }

    std::string lang = "en";
    if (req.has_param("lang")) lang = req.get_param_value("lang");

    const std::vector<uint8_t> pcm(req.body.begin(), req.body.end());
    TranscriptionExecution execution;
    int error_status = 0;
    std::string error_message;
    if (!transcription_executor.transcribePcmAudio(
            pcm,
            lang,
            /*translate=*/false,
            execution,
            error_status,
            error_message)) {
        if (error_status == asr::errors::kBusyHttpStatus) {
            asr::errors::setBusyRateLimitedError(req, res, error_message);
            return;
        }
        res.status = 500;
        res.set_content(errorJson(error_message).dump(), "application/json");
        return;
    }

    const double request_total_ms = Ms(Clock::now() - t_request_start).count();
    setTimingHeaders(
        res,
        request_total_ms,
        execution.save_file_ms,
        execution.lock_wait_ms,
        execution.result);
    const json x_timing = buildTimingJson(
        execution.result,
        request_total_ms,
        execution.save_file_ms,
        execution.lock_wait_ms);

    res.set_content(json{
        {"text", execution.result.text},
        {"x_timing", x_timing}
    }.dump(), "application/json");
}

void AsrService::registerCoreRoutes(httplib::Server& svr) {
    svr.Get("/health",
    [this](const httplib::Request&, httplib::Response& res) {
        const bool ready = engine_.isReady();
        res.status = ready ? 200 : 503;
        res.set_content(json{
            {"status",  ready ? "ok" : "not_ready"},
            {"version", engine_.getVersion()},
            {"model",   "whisper-tiny"}
        }.dump(), "application/json");
    });

    svr.Get("/metrics",
    [this](const httplib::Request&, httplib::Response& res) {
        const RealtimeStoreStats realtime_stats = realtime_store_.snapshotStats();
        res.set_content(json{
            {"requests_total", counters_.requests_total.load(std::memory_order_relaxed)},
            {"requests_error", counters_.requests_error.load(std::memory_order_relaxed)},
            {"openai_transcriptions_total", counters_.openai_transcriptions_total.load(std::memory_order_relaxed)},
            {"openai_translations_total", counters_.openai_translations_total.load(std::memory_order_relaxed)},
            {"openai_stream_total", counters_.openai_stream_total.load(std::memory_order_relaxed)},
            {"legacy_total", counters_.legacy_total.load(std::memory_order_relaxed)},
            {"realtime_sessions_created_total", counters_.realtime_sessions_created_total.load(std::memory_order_relaxed)},
            {"realtime_sessions_rejected_total", counters_.realtime_sessions_rejected_total.load(std::memory_order_relaxed)},
            {"realtime_audio_append_total", counters_.realtime_audio_append_total.load(std::memory_order_relaxed)},
            {"realtime_finalize_total", counters_.realtime_finalize_total.load(std::memory_order_relaxed)},
            {"realtime_enabled", config_.realtime_enabled},
            {"realtime_active_sessions", realtime_stats.active_sessions},
            {"realtime_pending_pcm_bytes", realtime_stats.pending_pcm_bytes},
            {"realtime_limits", {
                {"max_sessions", config_.realtime_max_sessions},
                {"max_pending_pcm_bytes_per_session", config_.realtime_max_pending_pcm_bytes_per_session},
                {"max_total_pending_pcm_bytes", config_.realtime_max_total_pending_pcm_bytes}
            }}
        }.dump(), "application/json");
    });

    svr.Get("/v1/models",
    [](const httplib::Request&, httplib::Response& res) {
        const int64_t created = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        json model_entries = json::array();
        for (const auto& model_id : asr::catalog::transcriptionModels()) {
            model_entries.push_back(json{
                {"id", model_id},
                {"created", created},
                {"object", "model"},
                {"owned_by", "qualcomm"}
            });
        }
        res.set_content(json{
            {"object", "list"},
            {"data", model_entries}
        }.dump(), "application/json");
    });

    svr.Get(R"(/v1/models/(.+))",
    [](const httplib::Request& req, httplib::Response& res) {
        const std::string requested = req.matches[1];
        if (asr::catalog::validTranscriptionModels().find(requested) ==
            asr::catalog::validTranscriptionModels().end()) {
            res.status = 404;
            res.set_content(
                errorJson("Model not found: " + requested, "invalid_request_error").dump(),
                "application/json");
            return;
        }
        const int64_t created = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        res.set_content(json{
            {"id", requested},
            {"object", "model"},
            {"created", created},
            {"owned_by", "qualcomm"}
        }.dump(), "application/json");
    });

    svr.Get("/languages",
    [](const httplib::Request&, httplib::Response& res) {
        const auto codes =
            WhisperFunction::Whisper::getSupportedLanguageCodes();
        json arr = json::array();
        for (const auto& c : codes) arr.push_back(c);
        res.set_content(
            json{{"languages", arr}}.dump(), "application/json");
    });
}

// ===========================================================================
// run() – HTTP server
// ===========================================================================
void AsrService::run() {
    httplib::Server svr;
    const size_t payload_max_length =
        std::max(config_.max_audio_body_bytes, config_.max_pcm_body_bytes);
    svr.set_payload_max_length(payload_max_length);

    configureServerPolicies(svr);
    registerMethodNotAllowedRoutes(svr);
    registerOpenAiRoutes(svr);
    registerLegacyRoutes(svr);
    registerCoreRoutes(svr);
    if (config_.realtime_enabled) {
        registerRealtimeRoutes(svr);
    }

    std::cout << "[AsrService] Listening on 0.0.0.0:" << port_ << "\n";
    svr.listen("0.0.0.0", static_cast<int>(port_));
}
