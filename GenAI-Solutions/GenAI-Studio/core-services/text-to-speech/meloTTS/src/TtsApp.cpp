// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------

#include "TtsApp.hpp"

#define CPPHTTPLIB_THREAD_POOL_COUNT 4
#include "httplib.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

using json  = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Ms    = std::chrono::duration<double, std::milli>;
namespace fs = std::filesystem;

namespace TtsService {

namespace {

constexpr size_t kMaxSpeechBodyBytes = 1 * 1024 * 1024;
constexpr size_t kDefaultMaxSynthesisChars = 1600;
// TODO: make these runtime-configurable once backend dynamic controls are enabled.
constexpr const char* kFixedVoice = "alloy";
constexpr double      kFixedSpeed = 1.0;

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string modelPrefixForLanguage(const std::string& language) {
    const std::string lang = toLowerCopy(language);
    if (lang == "english") return "melo_en";
    if (lang == "spanish") return "melo_es";
    if (lang == "chinese") return "melo_zh";
    return "";
}

std::vector<std::string> findQnnCandidates(const fs::path& model_dir) {
    std::vector<std::string> candidates;
    for (const auto& entry : fs::directory_iterator(model_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string file_name = entry.path().filename().string();
        if (file_name.size() >= 4 && file_name.substr(file_name.size() - 4) == ".qnn") {
            candidates.push_back(entry.path().string());
        }
    }
    return candidates;
}

int scoreModelCandidate(const std::string& full_path,
                        const std::string& language_prefix) {
    const std::string file_name = fs::path(full_path).filename().string();
    const std::string lower     = toLowerCopy(file_name);
    int score = 0;

    if (!language_prefix.empty() &&
        file_name.size() >= language_prefix.size() &&
        file_name.compare(0, language_prefix.size(), language_prefix) == 0) {
        score += 1000;
    }
    if (lower.find("notebook_v73") != std::string::npos) {
        score += 500;
    }
    if (lower.find("v2.33.0") != std::string::npos) {
        score += 200;
    }
    if (lower.find("aihub") != std::string::npos) {
        score -= 200;
    }
    if (lower.find("q44") != std::string::npos) {
        score -= 100;
    }
    return score;
}

std::string selectBestModel(std::vector<std::string> candidates,
                            const std::string& language_prefix) {
    if (candidates.empty()) {
        return "";
    }
    std::sort(candidates.begin(), candidates.end(),
              [&](const std::string& left, const std::string& right) {
                  const int left_score  = scoreModelCandidate(left, language_prefix);
                  const int right_score = scoreModelCandidate(right, language_prefix);
                  if (left_score != right_score) {
                      return left_score > right_score;
                  }
                  return left < right;
              });
    return candidates.front();
}

void setJsonError(httplib::Response&  res,
                  int                 status,
                  const std::string&  message,
                  const std::string&  type = "invalid_request_error",
                  const std::string&  code = "") {
    json err;
    err["error"]["message"] = message;
    err["error"]["type"]    = type;
    if (!code.empty()) {
        err["error"]["code"] = code;
    }
    res.status = status;
    res.set_content(err.dump(), "application/json");
}

bool parseJsonBody(const httplib::Request& req,
                   httplib::Response&      res,
                   json&                   out_body) {
    try {
        out_body = json::parse(req.body);
        return true;
    } catch (const json::parse_error& e) {
        setJsonError(res, 400, std::string("JSON parse error: ") + e.what());
        return false;
    }
}

bool extractRequiredNonEmptyString(const json&         body,
                                   const char*         key,
                                   httplib::Response&  res,
                                   const std::string&  message) {
    if (!body.contains(key) ||
        !body[key].is_string() ||
        body[key].get<std::string>().empty()) {
        setJsonError(res, 400, message);
        return false;
    }
    return true;
}

bool validateOpenAiSpeechContract(const json& body,
                                  httplib::Response& res) {
    if (!extractRequiredNonEmptyString(
            body,
            "model",
            res,
            "'model' field is required and must be a non-empty string")) {
        return false;
    }

    if (body.contains("speed")) {
        if (!body["speed"].is_number()) {
            setJsonError(res,
                         400,
                         "'speed' must be a number in range [0.25, 4.0]",
                         "invalid_request_error");
            return false;
        }
        const double speed = body["speed"].get<double>();
        if (speed < 0.25 || speed > 4.0) {
            setJsonError(res,
                         400,
                         "'speed' must be in range [0.25, 4.0]",
                         "invalid_request_error");
            return false;
        }
    }
    return true;
}

bool validateOptionalRequestFieldTypes(const json& body,
                                       httplib::Response& res,
                                       bool require_openai_contract) {
    if (body.contains("voice") && !body["voice"].is_string()) {
        setJsonError(res,
                     400,
                     "'voice' must be a string",
                     "invalid_request_error");
        return false;
    }
    if (body.contains("speed") && !body["speed"].is_number()) {
        setJsonError(res,
                     400,
                     "'speed' must be a number in range [0.25, 4.0]",
                     "invalid_request_error");
        return false;
    }
    if (require_openai_contract &&
        body.contains("response_format") &&
        !body["response_format"].is_string()) {
        setJsonError(res,
                     400,
                     "'response_format' must be a string",
                     "invalid_request_error");
        return false;
    }
    return true;
}

std::string normalizeResponseFormat(const json& body,
                                    httplib::Response& res) {
    if (!body.contains("response_format") || !body["response_format"].is_string()) {
        return "wav";
    }
    const std::string requested = toLowerCopy(body["response_format"].get<std::string>());
    if (requested == "wav" || requested == "pcm") {
        return "wav";
    }
    setJsonError(res,
                 400,
                 "Unsupported response_format '" + requested +
                 "'. Supported values are 'wav' and 'pcm'.",
                 "invalid_request_error");
    return "";
}

void consumeLockedVoiceAndSpeed(const json& body) {
    std::string requested_voice = kFixedVoice;
    if (body.contains("voice") && body["voice"].is_string()) {
        requested_voice = body["voice"].get<std::string>();
    }
    double requested_speed = kFixedSpeed;
    if (body.contains("speed") && body["speed"].is_number()) {
        requested_speed = body["speed"].get<double>();
    }
    if (requested_voice != kFixedVoice || requested_speed != kFixedSpeed) {
        std::cout << "[TtsApp] voice/speed normalized to fixed backend defaults"
                  << " (voice=" << kFixedVoice
                  << ", speed=" << kFixedSpeed << ")\n";
    }
}

bool tryAcquireTtsLock(std::timed_mutex& mutex,
                       std::unique_lock<std::timed_mutex>& lock,
                       httplib::Response& res) {
    lock = std::unique_lock<std::timed_mutex>(mutex, std::defer_lock);
    if (lock.try_lock()) {
        return true;
    }
    res.set_header("Retry-After", "1");
    setJsonError(res,
                 429,
                 "TTS engine is busy, retry shortly",
                 "rate_limit_error",
                 "rate_limit_exceeded");
    return false;
}

size_t maxSynthesisChars() {
    const char* raw = std::getenv("TTS_MAX_INPUT_CHARS");
    if (!raw || !*raw) {
        return kDefaultMaxSynthesisChars;
    }
    try {
        const int parsed = std::stoi(raw);
        if (parsed < 128) {
            return 128;
        }
        return static_cast<size_t>(parsed);
    } catch (...) {
        return kDefaultMaxSynthesisChars;
    }
}

std::string normalizeTextForSynthesis(const std::string& input, bool& changed) {
    changed = false;
    std::string filtered;
    filtered.reserve(input.size());
    for (unsigned char ch : input) {
        if (ch == 0) {
            changed = true;
            continue;
        }
        if (std::iscntrl(ch) && !std::isspace(ch)) {
            filtered.push_back(' ');
            changed = true;
            continue;
        }
        filtered.push_back(static_cast<char>(ch));
    }

    std::string normalized;
    normalized.reserve(filtered.size());
    bool prev_space = false;
    for (unsigned char ch : filtered) {
        if (std::isspace(ch)) {
            if (!prev_space) {
                normalized.push_back(' ');
            }
            prev_space = true;
            continue;
        }
        prev_space = false;
        normalized.push_back(static_cast<char>(ch));
    }
    if (!normalized.empty() && normalized.front() == ' ') {
        normalized.erase(normalized.begin());
        changed = true;
    }
    if (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
        changed = true;
    }

    bool has_alnum = false;
    for (unsigned char ch : normalized) {
        if (std::isalnum(ch)) {
            has_alnum = true;
            break;
        }
    }
    if (!has_alnum) {
        normalized = "Please read this punctuation input.";
        changed = true;
    }

    const size_t max_chars = maxSynthesisChars();
    if (normalized.size() > max_chars) {
        size_t cut = normalized.rfind(' ', max_chars);
        if (cut == std::string::npos || cut < (max_chars / 2)) {
            cut = max_chars;
        }
        normalized = normalized.substr(0, cut);
        changed = true;
    }

    return normalized;
}

} // namespace

// ===========================================================================
// AudioCollector::toWav
// ===========================================================================
std::vector<uint8_t> AudioCollector::toWav() const {
    WavHeader hdr;
    hdr.num_channels    = static_cast<uint16_t>(num_channels);
    hdr.sample_rate     = static_cast<uint32_t>(sample_rate);
    hdr.bits_per_sample = static_cast<uint16_t>(bits_per_sample);
    hdr.byte_rate       = static_cast<uint32_t>(
        sample_rate * num_channels * (bits_per_sample / 8));
    hdr.block_align     = static_cast<uint16_t>(
        num_channels * (bits_per_sample / 8));
    hdr.data_size       = static_cast<uint32_t>(pcm_data.size());
    // chunk_size = total file size - 8 (RIFF header id + chunk_size field)
    hdr.chunk_size      = static_cast<uint32_t>(
        sizeof(WavHeader) - 8 + pcm_data.size());

    std::vector<uint8_t> wav;
    wav.resize(sizeof(WavHeader) + pcm_data.size());
    std::memcpy(wav.data(), &hdr, sizeof(WavHeader));
    if (!pcm_data.empty()) {
        std::memcpy(wav.data() + sizeof(WavHeader),
                    pcm_data.data(), pcm_data.size());
    }
    return wav;
}

// ===========================================================================
// TtsApp – constructor
// ===========================================================================
TtsApp::TtsApp(const std::string& model_path,
               const std::string& language,
               float              speaking_rate,
               float              pitch,
               float              volume_gain,
               int                sample_rate,
               int                port)
    : model_path_(model_path),
      language_(language),
      speaking_rate_(speaking_rate),
      pitch_(pitch),
      volume_gain_(volume_gain),
      sample_rate_(sample_rate),
      port_(port),
      start_time_(Clock::now())
{
    server_created_unix_.store(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // ---- Resolve the bundled model file ------------------------------------
    const std::string model_file = resolveModelFile();
    std::cout << "[TtsApp] Model file : " << model_file  << "\n"
              << "[TtsApp] Language   : " << language_   << "\n"
              << "[TtsApp] Port       : " << port_       << "\n";

    // ---- Create callback and engine ----------------------------------------
    callback_ = new AudioCollector();
    engine_   = TTS::TTSEngine::getInstance();
    engine_->registerResultCallback(callback_);

    // ---- Build TTSConfig ---------------------------------------------------
    //
    // KEY_MODEL_PATH   – path to the single bundled .qnn model file
    // KEY_LANGUAGE     – "English" | "Spanish" | "Chinese"
    // KEY_AUDIO_ENCODING – "0" = LINEAR16 (int16 PCM)
    // KEY_SPEAKING_RATE  – [0.25, 4.0], default 1.0
    // KEY_PITCH          – [-20.0, 20.0], default 0.0
    // KEY_VOLUME_GAIN    – [-96.0, 16.0] dB, default 0.0
    // KEY_SAMPLE_RATE    – output sample rate Hz, default 44100
    //
    TTS::TTSConfig config = TTS::TTSConfig::Builder()
        .setConfig(TTS::ConfigKey::KEY_MODEL_PATH,
                   model_file)
        .setConfig(TTS::ConfigKey::KEY_LANGUAGE,
                   language_)
        .setConfig(TTS::ConfigKey::KEY_AUDIO_ENCODING,
                   "0")   // LINEAR16
        .setConfig(TTS::ConfigKey::KEY_SPEAKING_RATE,
                   std::to_string(speaking_rate_))
        .setConfig(TTS::ConfigKey::KEY_PITCH,
                   std::to_string(pitch_))
        .setConfig(TTS::ConfigKey::KEY_VOLUME_GAIN,
                   std::to_string(volume_gain_))
        .setConfig(TTS::ConfigKey::KEY_SAMPLE_RATE,
                   std::to_string(sample_rate_))
        .build();

    std::cout << "[TtsApp] Initializing TTS engine ...\n";
    const int32_t ret = engine_->init(&config);
    if (ret != 0) {
        throw std::runtime_error(
            "TTSEngine::init() failed with code " + std::to_string(ret));
    }
    std::cout << "[TtsApp] TTS engine ready.\n";
}

// ===========================================================================
// TtsApp – destructor
// ===========================================================================
TtsApp::~TtsApp() {
    if (engine_) {
        // Transition state machine: START → INIT → IDLE
        engine_->stop();
        engine_->deInit();
    }
    delete callback_;
    callback_ = nullptr;
}

// ===========================================================================
// resolveModelFile
//
// Accepts either:
//   (a) a direct path to a .qnn file, or
//   (b) a directory – searches for the first *.qnn file inside it.
// ===========================================================================
std::string TtsApp::resolveModelFile() const {
    const fs::path model_path(model_path_);
    if (fs::is_regular_file(model_path)) {
        return model_path_;
    }
    if (!fs::is_directory(model_path)) {
        throw std::runtime_error(
            "No .qnn model file found at: " + model_path_ +
            "\nExpected a bundled model generated by qnn_model_generation.py"
            " (e.g. melo_en.64_bit.qnn_v2.33.0.qnn)");
    }
    const std::string language_prefix = modelPrefixForLanguage(language_);
    const auto candidates = findQnnCandidates(model_path);
    const std::string selected = selectBestModel(candidates, language_prefix);
    if (!selected.empty()) {
        return selected;
    }
    throw std::runtime_error(
        "No .qnn model file found at: " + model_path_ +
        "\nExpected a bundled model generated by qnn_model_generation.py"
        " (e.g. melo_en.64_bit.qnn_v2.33.0.qnn)");
}

// ===========================================================================
// synthesize
//
// Runs one TTS synthesis cycle:
//   1. Reset the audio collector
//   2. Call engine_->start(text)  – synchronous, blocks until onDone()
//   3. Call engine_->stop()       – resets state machine to INIT
//   4. Return WAV bytes
//
// Caller MUST hold tts_mutex_.
// ===========================================================================
std::vector<uint8_t> TtsApp::synthesize(const std::string& text) {
    callback_->reset();

    const auto t0 = Clock::now();
    const int32_t ret = engine_->start(text);
    const auto t1 = Clock::now();

    if (ret != 0) {
        throw std::runtime_error(
            "TTSEngine::start() failed with code " + std::to_string(ret));
    }

    // Reset state machine: START → INIT (required before next start())
    engine_->stop();

    if (callback_->has_error) {
        throw std::runtime_error(callback_->error_msg);
    }
    if (callback_->pcm_data.empty()) {
        throw std::runtime_error("TTS generated empty audio payload");
    }

    const double synth_ms  = Ms(t1 - t0).count();
    const double audio_ms  = callback_->durationMs();
    const double rtf        = (audio_ms > 0) ? (synth_ms / audio_ms) : 0.0;

    std::cout << std::fixed << std::setprecision(1)
              << "[TtsApp] Synthesized " << callback_->pcm_data.size()
              << " B PCM  audio=" << audio_ms << " ms"
              << "  synth=" << synth_ms << " ms"
              << "  RTF=" << std::setprecision(3) << rtf << "\n";

    return callback_->toWav();
}

// ===========================================================================
// setupRoutes
// ===========================================================================
void TtsApp::setupErrorHandlers(httplib::Server& svr) {
    svr.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        if (!res.body.empty()) {
            return;
        }
        if (res.status == 404) {
            setJsonError(res, 404, "Not found: " + req.path, "invalid_request_error", "not_found");
            return;
        }
        if (res.status == 405) {
            setJsonError(res, 405, "Method not allowed", "invalid_request_error", "method_not_allowed");
            return;
        }
        const int status = res.status > 0 ? res.status : 500;
        setJsonError(res,
                     status,
                     "Server error (HTTP " + std::to_string(status) + ")",
                     "server_error");
    });

    svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string message = "Internal server error";
        try {
            if (ep) {
                std::rethrow_exception(ep);
            }
        } catch (const std::exception& e) {
            message = e.what();
        } catch (...) {
        }
        setJsonError(res, 500, message, "server_error");
    });
}

void TtsApp::registerHealthRoute(httplib::Server& svr) {
    svr.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
        const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            Clock::now() - start_time_).count();
        json body;
        body["status"]         = "ok";
        body["model"]          = "melo-tts-" + language_;
        body["language"]       = language_;
        body["uptime_seconds"] = uptime;
        body["version"]        = "2.0.0";
        res.set_content(body.dump(), "application/json");
    });
}

void TtsApp::registerModelsRoute(httplib::Server& svr) {
    svr.Get("/v1/models", [this](const httplib::Request&, httplib::Response& res) {
        json obj;
        obj["id"]         = "melo-tts-" + language_;
        obj["object"]     = "model";
        obj["created"]    = server_created_unix_.load();
        obj["owned_by"]   = "qualcomm";
        obj["permission"] = json::array();
        obj["root"]       = "melo-tts-" + language_;
        obj["parent"]     = nullptr;

        json body;
        body["object"] = "list";
        body["data"]   = json::array({obj});
        res.set_content(body.dump(), "application/json");
    });
}

void TtsApp::registerMethodNotAllowedRoutes(httplib::Server& svr) {
    auto method_not_allowed = [](const httplib::Request&, httplib::Response& res) {
        setJsonError(res,
                     405,
                     "Method not allowed. This endpoint only accepts POST.",
                     "invalid_request_error",
                     "method_not_allowed");
    };
    svr.Get("/generate", method_not_allowed);
    svr.Get("/v1/audio/speech", method_not_allowed);
    svr.Delete("/generate", method_not_allowed);
    svr.Delete("/v1/audio/speech", method_not_allowed);
    svr.Put("/generate", method_not_allowed);
    svr.Put("/v1/audio/speech", method_not_allowed);
}

void TtsApp::registerGenerateRoute(httplib::Server& svr) {
    svr.Post("/generate", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Deprecation", "true");
        handleGenerateRequest(req, res);
    });
}

void TtsApp::registerOpenAiSpeechRoute(httplib::Server& svr) {
    svr.Post("/v1/audio/speech", [this](const httplib::Request& req, httplib::Response& res) {
        handleOpenAiSpeechRequest(req, res);
    });
}

void TtsApp::handleSynthesisRequest(const httplib::Request& req,
                                    httplib::Response&      res,
                                    const char*             text_field,
                                    const char*             missing_field_error,
                                    bool                    require_openai_contract) {
    if (req.body.size() > kMaxSpeechBodyBytes) {
        setJsonError(res,
                     413,
                     "Request body exceeds 1 MB limit",
                     "invalid_request_error",
                     "request_too_large");
        return;
    }

    json body;
    if (!parseJsonBody(req, res, body)) {
        return;
    }
    if (!extractRequiredNonEmptyString(body,
                                       text_field,
                                       res,
                                       missing_field_error)) {
        return;
    }
    if (!validateOptionalRequestFieldTypes(body, res, require_openai_contract)) {
        return;
    }
    if (require_openai_contract && !validateOpenAiSpeechContract(body, res)) {
        return;
    }
    const std::string normalized_format = normalizeResponseFormat(body, res);
    if (normalized_format.empty()) {
        return;
    }
    consumeLockedVoiceAndSpeed(body);
    const std::string raw_text = body[text_field].get<std::string>();
    bool text_changed = false;
    const std::string text = normalizeTextForSynthesis(raw_text, text_changed);
    if (text.empty()) {
        setJsonError(res,
                     400,
                     "Input text is empty after normalization",
                     "invalid_request_error");
        return;
    }
    if (text_changed) {
        std::cout << "[TtsApp] normalized synthesis input len "
                  << raw_text.size() << " -> " << text.size() << "\n";
    }

    std::unique_lock<std::timed_mutex> lock;
    if (!tryAcquireTtsLock(tts_mutex_, lock, res)) {
        return;
    }

    try {
        auto synth_with_recovery = [this, &text]() -> std::vector<uint8_t> {
            try {
                return synthesize(text);
            } catch (const std::exception& first_error) {
                const std::string first_msg = first_error.what();
                if (first_msg.find("empty audio payload") == std::string::npos) {
                    throw;
                }

                std::cout << "[TtsApp] empty audio payload; retrying same request once\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                try {
                    return synthesize(text);
                } catch (const std::exception& second_error) {
                    const std::string second_msg = second_error.what();
                    if (second_msg.find("empty audio payload") == std::string::npos) {
                        throw;
                    }

                    static const std::string kFallbackText =
                        "Please read this sentence clearly.";
                    std::cout << "[TtsApp] empty audio payload persisted; "
                              << "using fallback synthesis text\n";
                    return synthesize(kFallbackText);
                }
            }
        };

        const auto wav = synth_with_recovery();
        res.set_content(std::string(wav.begin(), wav.end()), "audio/wav");
    } catch (const std::exception& e) {
        setJsonError(res, 500, e.what(), "server_error");
    }
}

void TtsApp::handleGenerateRequest(const httplib::Request& req,
                                   httplib::Response&      res) {
    handleSynthesisRequest(req,
                           res,
                           "text",
                           "'text' field is required and must be a non-empty string",
                           false);
}

void TtsApp::handleOpenAiSpeechRequest(const httplib::Request& req,
                                       httplib::Response&      res) {
    handleSynthesisRequest(req,
                           res,
                           "input",
                           "'input' field is required and must be a non-empty string",
                           true);
}

void TtsApp::setupRoutes(httplib::Server& svr) {
    setupErrorHandlers(svr);
    registerHealthRoute(svr);
    registerModelsRoute(svr);
    registerMethodNotAllowedRoutes(svr);
    registerGenerateRoute(svr);
    registerOpenAiSpeechRoute(svr);
}

// ===========================================================================
// run
// ===========================================================================
void TtsApp::run() {
    httplib::Server svr;
    setupRoutes(svr);
    std::cout << "[TtsApp] Server listening on 0.0.0.0:" << port_ << "\n";
    svr.listen("0.0.0.0", port_);
}

} // namespace TtsService
