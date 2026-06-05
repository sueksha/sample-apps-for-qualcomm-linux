// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------

#include "AsrService.hpp"
#include "ErrorResponder.hpp"
#include "TranscriptionExecutor.hpp"

#define CPPHTTPLIB_THREAD_POOL_COUNT 4
#include "httplib.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <optional>
#include <sstream>
#include <thread>

using json  = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Ms    = std::chrono::duration<double, std::milli>;

namespace {
constexpr int kDefaultRealtimeSampleRateHz = 16000;
constexpr int kDefaultRealtimeSessionTtlSec = 3600;

uint64_t nextFinalizeToken() {
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

bool parseBool(const std::string& value, bool default_value = false) {
    std::string v = value;
    const auto is_not_space = [](unsigned char c) { return !std::isspace(c); };
    v.erase(v.begin(), std::find_if(v.begin(), v.end(), is_not_space));
    v.erase(std::find_if(v.rbegin(), v.rend(), is_not_space).base(), v.end());
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (v.empty()) return default_value;
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return default_value;
}

int parseIntOrDefault(const std::string& value, int default_value) {
    if (value.empty()) return default_value;
    try {
        return std::stoi(value);
    } catch (...) {
        return default_value;
    }
}

uint64_t pcmDurationMs(size_t pcm_bytes, int sample_rate_hz) {
    if (sample_rate_hz <= 0) return 0;
    const uint64_t sample_count = static_cast<uint64_t>(pcm_bytes / 2);
    return static_cast<uint64_t>((sample_count * 1000ULL) / static_cast<uint64_t>(sample_rate_hz));
}

double computePcm16Rms(const std::string& pcm_bytes) {
    if (pcm_bytes.size() < 2) return 0.0;
    const size_t sample_count = pcm_bytes.size() / 2;
    if (sample_count == 0) return 0.0;
    const auto* raw = reinterpret_cast<const uint8_t*>(pcm_bytes.data());
    long double sum = 0.0;
    for (size_t i = 0; i < sample_count; ++i) {
        const uint16_t lo = static_cast<uint16_t>(raw[i * 2]);
        const uint16_t hi = static_cast<uint16_t>(raw[i * 2 + 1]);
        const int16_t s = static_cast<int16_t>((hi << 8) | lo);
        const long double normalized = static_cast<long double>(s) / 32768.0L;
        sum += normalized * normalized;
    }
    const long double mean = sum / static_cast<long double>(sample_count);
    return static_cast<double>(std::sqrt(mean));
}

std::string joinWithSpaces(const std::vector<std::string>& parts) {
    std::ostringstream ss;
    bool first = true;
    for (const auto& part : parts) {
        if (part.empty()) continue;
        if (!first) ss << ' ';
        ss << part;
        first = false;
    }
    return ss.str();
}

double round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

json errorJson(const std::string& msg,
               const std::string& type = "server_error") {
    return json{{"error", {{"message", msg}, {"type", type}}}};
}

void setRequestError(httplib::Response& res,
                     int status,
                     const std::string& message,
                     const std::string& type = "invalid_request_error") {
    res.status = status;
    res.set_content(errorJson(message, type).dump(), "application/json");
}

struct RealtimeAppendInput {
    bool commit_requested = false;
    int requested_sample_rate = kDefaultRealtimeSampleRateHz;
    std::string req_language;
    std::string req_task;
    double chunk_rms = 0.0;
    std::vector<uint8_t> chunk;
};

struct RealtimeAppendSessionState {
    bool speech_detected = false;
    bool segment_committed = false;
    uint64_t audio_ms_total = 0;
    uint64_t audio_ms_pending = 0;
    uint64_t chunk_count = 0;
    std::string session_language;
    std::string session_task;
    std::string text_before;
    std::vector<uint8_t> buffer_to_transcribe;
    int session_sample_rate = kDefaultRealtimeSampleRateHz;
    int session_error_status = 0;
    std::string session_error_message;
};

bool validateAppendPayload(const httplib::Request& req,
                           const AsrRuntimeConfig& config,
                           httplib::Response& res) {
    if (req.body.empty()) {
        setRequestError(res, 400, "Empty request body. Send PCM16 bytes.");
        return false;
    }
    if ((req.body.size() % 2) != 0) {
        setRequestError(res, 400, "PCM16 payload must have even byte length");
        return false;
    }
    if (req.body.size() > config.max_pcm_body_bytes) {
        setRequestError(res, 413, "PCM payload exceeds configured limit");
        return false;
    }
    return true;
}

RealtimeAppendInput parseAppendInput(const httplib::Request& req) {
    RealtimeAppendInput input;
    input.commit_requested = parseBool(
        req.has_param("commit") ? req.get_param_value("commit") : "", false);
    input.requested_sample_rate = parseIntOrDefault(
        req.has_param("sample_rate_hz") ? req.get_param_value("sample_rate_hz") : "",
        kDefaultRealtimeSampleRateHz);
    input.req_language = req.has_param("language")
                       ? req.get_param_value("language")
                       : "";
    input.req_task = req.has_param("task")
                   ? req.get_param_value("task")
                   : "";
    input.chunk_rms = computePcm16Rms(req.body);
    input.chunk.assign(req.body.begin(), req.body.end());
    return input;
}

void applyAppendToSession(RealtimeSessionState& session,
                          uint64_t total_pending_before,
                          const RealtimeAppendInput& input,
                          const AsrRuntimeConfig& config,
                          std::atomic<uint64_t>& rejected_counter,
                          RealtimeAppendSessionState& out) {
    const auto now = Clock::now();
    if (!input.req_language.empty()) session.language = input.req_language;
    if (input.req_task == "transcribe" || input.req_task == "translate") {
        session.task = input.req_task;
    }
    if (input.requested_sample_rate >= 8000 && input.requested_sample_rate <= 48000) {
        session.sample_rate_hz = input.requested_sample_rate;
    }
    session.updated_at = now;

    if (session.finalized) {
        out.session_error_status = 404;
        out.session_error_message = "Realtime session already finalized";
        return;
    }
    if (session.finalizing) {
        out.session_error_status = 429;
        out.session_error_message = "Realtime session is finalizing. Try again later.";
        return;
    }

    const uint64_t chunk_ms = pcmDurationMs(input.chunk.size(), session.sample_rate_hz);
    const uint64_t max_ms = static_cast<uint64_t>(session.max_duration_s) * 1000ULL;
    if (session.total_audio_ms + chunk_ms > max_ms) {
        out.session_error_status = 400;
        out.session_error_message = "Realtime session exceeded max_duration_s";
        return;
    }

    session.total_audio_ms += chunk_ms;
    session.chunk_count += 1;
    session.last_rms = input.chunk_rms;
    out.speech_detected = (input.chunk_rms >= session.vad_threshold);

    const uint64_t chunk_bytes = static_cast<uint64_t>(input.chunk.size());
    const uint64_t session_pending_before = static_cast<uint64_t>(session.pending_pcm.size());
    if (session_pending_before + chunk_bytes >
        static_cast<uint64_t>(config.realtime_max_pending_pcm_bytes_per_session)) {
        rejected_counter.fetch_add(1, std::memory_order_relaxed);
        out.session_error_status = 413;
        out.session_error_message =
            "Realtime session pending audio exceeds configured per-session limit";
        return;
    }
    if (total_pending_before + chunk_bytes >
        static_cast<uint64_t>(config.realtime_max_total_pending_pcm_bytes)) {
        rejected_counter.fetch_add(1, std::memory_order_relaxed);
        out.session_error_status = 429;
        out.session_error_message = "Realtime capacity exceeded. Try again later.";
        return;
    }

    session.pending_pcm.insert(session.pending_pcm.end(), input.chunk.begin(), input.chunk.end());
    if (out.speech_detected) {
        session.speech_active = true;
        session.trailing_silence_ms = 0;
    } else if (session.speech_active) {
        session.trailing_silence_ms += chunk_ms;
    }

    const bool vad_commit = session.speech_active &&
                            !out.speech_detected &&
                            (session.trailing_silence_ms >=
                             static_cast<uint64_t>(session.vad_hangover_ms));
    if ((vad_commit || input.commit_requested) && !session.pending_pcm.empty()) {
        out.buffer_to_transcribe = std::move(session.pending_pcm);
        session.pending_pcm.clear();
        session.speech_active = false;
        session.trailing_silence_ms = 0;
        out.segment_committed = true;
        session.active_transcriptions += 1;
    }

    out.session_language = session.language;
    out.session_task = session.task;
    out.session_sample_rate = session.sample_rate_hz;
    out.audio_ms_total = session.total_audio_ms;
    out.audio_ms_pending = pcmDurationMs(session.pending_pcm.size(), session.sample_rate_hz);
    out.chunk_count = session.chunk_count;
    out.text_before = joinWithSpaces(session.segments);
}

bool ensureLookupAndSessionState(const httplib::Request& req,
                                 RealtimeSessionStore::SessionLookup lookup,
                                 const std::string& session_id,
                                 const RealtimeAppendSessionState& append_state,
                                 httplib::Response& res) {
    if (lookup == RealtimeSessionStore::SessionLookup::kNotFound) {
        setRequestError(res, 404, "Realtime session not found: " + session_id);
        return false;
    }
    if (lookup == RealtimeSessionStore::SessionLookup::kExpired) {
        setRequestError(res, 404, "Realtime session expired: " + session_id);
        return false;
    }
    if (append_state.session_error_status != 0) {
        if (append_state.session_error_status == asr::errors::kBusyHttpStatus) {
            asr::errors::setBusyRateLimitedError(req, res, append_state.session_error_message);
            return false;
        }
        setRequestError(
            res, append_state.session_error_status, append_state.session_error_message);
        return false;
    }
    return true;
}

void updateSessionTextFromSegment(RealtimeSessionStore& store,
                                  const std::string& session_id,
                                  const std::string& delta_text,
                                  std::string& full_text,
                                  uint64_t& audio_ms_pending,
                                  uint64_t& audio_ms_total,
                                  uint64_t& chunk_count) {
    (void)store.withSessionIfExists(
        session_id,
        [&delta_text,
         &full_text,
         &audio_ms_pending,
         &audio_ms_total,
         &chunk_count](RealtimeSessionState& s) {
            if (!delta_text.empty()) s.segments.push_back(delta_text);
            full_text = joinWithSpaces(s.segments);
            audio_ms_pending = pcmDurationMs(s.pending_pcm.size(), s.sample_rate_hz);
            audio_ms_total = s.total_audio_ms;
            chunk_count = s.chunk_count;
        });
}

void releaseTranscriptionSlot(RealtimeSessionStore& store, const std::string& session_id) {
    (void)store.withSessionIfExists(session_id, [](RealtimeSessionState& s) {
        if (s.active_transcriptions > 0) {
            --s.active_transcriptions;
        }
    });
}
}  // namespace

void AsrService::handleRealtimeAudioAppend(const httplib::Request& req,
                                           httplib::Response& res) {
    counters_.realtime_audio_append_total.fetch_add(1, std::memory_order_relaxed);
    const auto t_request_start = Clock::now();
    const TranscriptionExecutor transcription_executor(engine_, engine_mutex_, config_);
    const std::string session_id = req.matches[1];
    if (!validateAppendPayload(req, config_, res)) return;
    const RealtimeAppendInput input = parseAppendInput(req);

    RealtimeAppendSessionState append_state;
    const RealtimeSessionStore::SessionLookup lookup = realtime_store_.withLiveSession(
        session_id,
        kDefaultRealtimeSessionTtlSec,
        [this, &input, &append_state](RealtimeSessionState& session,
                                      uint64_t total_pending_before) {
            applyAppendToSession(
                session,
                total_pending_before,
                input,
                config_,
                counters_.realtime_sessions_rejected_total,
                append_state);
        });
    if (!ensureLookupAndSessionState(req, lookup, session_id, append_state, res)) return;

    std::optional<TranscriptionResult> maybe_segment_result;
    double engine_lock_wait_ms = 0.0;
    if (append_state.segment_committed && !append_state.buffer_to_transcribe.empty()) {
        const RealtimeTranscribeAttempt attempt = transcription_executor.transcribePcmWithRollback(
            append_state.buffer_to_transcribe,
            append_state.session_language,
            append_state.session_task == "translate",
            [this, &session_id, &append_state]() {
                realtime_store_.prependPendingIfExists(
                    session_id, append_state.buffer_to_transcribe);
                releaseTranscriptionSlot(realtime_store_, session_id);
            });
        if (!attempt.success) {
            releaseTranscriptionSlot(realtime_store_, session_id);
            if (attempt.error_status == asr::errors::kBusyHttpStatus) {
                asr::errors::setBusyRateLimitedError(req, res, attempt.error_message);
                return;
            }
            setRequestError(res, attempt.error_status, attempt.error_message, "server_error");
            return;
        }
        maybe_segment_result = attempt.result;
        engine_lock_wait_ms = attempt.lock_wait_ms;
    }

    std::string full_text = append_state.text_before;
    std::string delta_text;
    json engine_timing = json::object();
    if (maybe_segment_result.has_value()) {
        delta_text = maybe_segment_result->text;
        updateSessionTextFromSegment(
            realtime_store_,
            session_id,
            delta_text,
            full_text,
            append_state.audio_ms_pending,
            append_state.audio_ms_total,
            append_state.chunk_count);
        releaseTranscriptionSlot(realtime_store_, session_id);
        engine_timing = {
            {"engine_total_ms", maybe_segment_result->timing.total_ms},
            {"engine_config_ms", maybe_segment_result->timing.configure_ms},
            {"engine_start_ms", maybe_segment_result->timing.start_ms},
            {"engine_feed_ms", maybe_segment_result->timing.feed_ms},
            {"engine_wait_ms", maybe_segment_result->timing.wait_ms},
            {"engine_stop_ms", maybe_segment_result->timing.stop_ms},
            {"lock_wait_ms", round3(engine_lock_wait_ms)}
        };
    }

    const double request_total_ms = Ms(Clock::now() - t_request_start).count();
    res.set_content(json{
        {"object", "realtime.transcription"},
        {"session_id", session_id},
        {"speech_detected", append_state.speech_detected},
        {"segment_committed", append_state.segment_committed},
        {"delta_text", delta_text},
        {"text", full_text},
        {"audio_ms_total", append_state.audio_ms_total},
        {"audio_ms_pending", append_state.audio_ms_pending},
        {"chunks_received", append_state.chunk_count},
        {"sample_rate_hz", append_state.session_sample_rate},
        {"last_rms", input.chunk_rms},
        {"x_timing", {
            {"request_total_ms", round3(request_total_ms)},
            {"engine", engine_timing}
        }}
    }.dump(), "application/json");
}

void AsrService::handleRealtimeFinalize(const httplib::Request& req,
                                        httplib::Response& res) {
    counters_.realtime_finalize_total.fetch_add(1, std::memory_order_relaxed);
    const auto t_request_start = Clock::now();
    const TranscriptionExecutor transcription_executor(engine_, engine_mutex_, config_);
    const std::string session_id = req.matches[1];

    std::vector<uint8_t> pending_to_transcribe;
    std::string session_language;
    std::string session_task;
    uint64_t audio_ms_total = 0;
    uint64_t chunk_count = 0;
    const RealtimeSessionStore::SessionLookup finalize_lookup = realtime_store_.withLiveSession(
        session_id,
        kDefaultRealtimeSessionTtlSec,
        [&pending_to_transcribe,
         &session_language,
         &session_task,
         &audio_ms_total,
         &chunk_count](RealtimeSessionState& s, uint64_t /*total_pending_before*/) {
            pending_to_transcribe = std::move(s.pending_pcm);
            s.pending_pcm.clear();
            s.speech_active = false;
            s.trailing_silence_ms = 0;
            s.updated_at = Clock::now();
            session_language = s.language;
            session_task = s.task;
            audio_ms_total = s.total_audio_ms;
            chunk_count = s.chunk_count;
        });
    if (finalize_lookup == RealtimeSessionStore::SessionLookup::kNotFound) {
        res.status = 404;
        res.set_content(
            errorJson("Realtime session not found: " + session_id, "invalid_request_error").dump(),
            "application/json");
        return;
    }
    if (finalize_lookup == RealtimeSessionStore::SessionLookup::kExpired) {
        res.status = 404;
        res.set_content(
            errorJson("Realtime session expired: " + session_id, "invalid_request_error").dump(),
            "application/json");
        return;
    }

    std::optional<TranscriptionResult> final_result;
    double engine_lock_wait_ms = 0.0;
    if (!pending_to_transcribe.empty()) {
        const RealtimeTranscribeAttempt attempt = transcription_executor.transcribePcmWithRollback(
            pending_to_transcribe,
            session_language,
            session_task == "translate",
            [this, &session_id, &pending_to_transcribe]() {
                realtime_store_.prependPendingIfExists(session_id, pending_to_transcribe);
            });
        if (!attempt.success) {
            if (attempt.error_status == asr::errors::kBusyHttpStatus) {
                asr::errors::setBusyRateLimitedError(req, res, attempt.error_message);
                return;
            }
            res.status = attempt.error_status;
            res.set_content(errorJson(attempt.error_message, "server_error").dump(), "application/json");
            return;
        }
        final_result = attempt.result;
        engine_lock_wait_ms = attempt.lock_wait_ms;
    }

    std::string final_text;
    std::vector<std::string> segments;
    uint64_t audio_ms_pending = 0;
    const bool found_after_transcribe = realtime_store_.withSessionIfExists(
        session_id,
        [&final_result,
         &segments,
         &final_text,
         &audio_ms_pending,
         &audio_ms_total,
         &chunk_count](RealtimeSessionState& s) {
            if (final_result.has_value() && !final_result->text.empty()) {
                s.segments.push_back(final_result->text);
            }
            segments = s.segments;
            final_text = joinWithSpaces(s.segments);
            audio_ms_pending = pcmDurationMs(s.pending_pcm.size(), s.sample_rate_hz);
            audio_ms_total = s.total_audio_ms;
            chunk_count = s.chunk_count;
        });
    if (!found_after_transcribe) {
        res.status = 404;
        res.set_content(
            errorJson("Realtime session not found: " + session_id, "invalid_request_error").dump(),
            "application/json");
        return;
    }

    const double request_total_ms = Ms(Clock::now() - t_request_start).count();
    json timing = {
        {"request_total_ms", round3(request_total_ms)},
        {"lock_wait_ms", round3(engine_lock_wait_ms)}
    };
    if (final_result.has_value()) {
        timing["engine_total_ms"] = final_result->timing.total_ms;
        timing["engine_feed_ms"] = final_result->timing.feed_ms;
        timing["engine_wait_ms"] = final_result->timing.wait_ms;
    }

    res.set_content(json{
        {"object", "realtime.transcription.final"},
        {"session_id", session_id},
        {"text", final_text},
        {"segments", segments},
        {"audio_ms_total", audio_ms_total},
        {"audio_ms_pending", audio_ms_pending},
        {"chunks_received", chunk_count},
        {"x_timing", timing}
    }.dump(), "application/json");
}
