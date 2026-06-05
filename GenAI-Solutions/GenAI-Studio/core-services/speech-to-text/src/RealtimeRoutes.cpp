// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------

#include "AsrService.hpp"
#include "ErrorResponder.hpp"
#include "ModelCatalog.hpp"

#define CPPHTTPLIB_THREAD_POOL_COUNT 4
#include "httplib.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <sstream>

using json = nlohmann::json;

namespace {
constexpr int kDefaultRealtimeSampleRateHz = 16000;
constexpr double kDefaultRealtimeVadThreshold = 0.015;
constexpr int kDefaultRealtimeVadHangoverMs = 800;
constexpr int kDefaultRealtimeSessionTtlSec = 3600;

std::string makeRealtimeSessionId() {
    static std::atomic<uint64_t> counter{0};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "rtsess_" + std::to_string(now) + "_" + std::to_string(counter.fetch_add(1));
}

uint64_t pcmDurationMs(size_t pcm_bytes, int sample_rate_hz) {
    if (sample_rate_hz <= 0) return 0;
    const uint64_t sample_count = static_cast<uint64_t>(pcm_bytes / 2);
    return static_cast<uint64_t>((sample_count * 1000ULL) / static_cast<uint64_t>(sample_rate_hz));
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

json errorJson(const std::string& msg,
               const std::string& type = "server_error") {
    return json{{"error", {{"message", msg}, {"type", type}}}};
}

struct RealtimeSessionCreateInput {
    std::string model = "whisper-tiny";
    std::string task = "transcribe";
    std::string language = "en";
    int sample_rate_hz = kDefaultRealtimeSampleRateHz;
    int max_duration_s = 30;
    double vad_threshold = kDefaultRealtimeVadThreshold;
    int vad_hangover_ms = kDefaultRealtimeVadHangoverMs;
};

void setJsonResponse(httplib::Response& res,
                     const json& payload,
                     int status = 200) {
    res.status = status;
    res.set_content(payload.dump(), "application/json");
}

bool setInvalidFieldType(httplib::Response& res,
                         const std::string& field_name,
                         const std::string& expected_type) {
    setJsonResponse(
        res,
        errorJson("Invalid '" + field_name + "'. Expected " + expected_type,
                  "invalid_request_error"),
        400);
    return false;
}

bool parseCreateSessionBody(const httplib::Request& req,
                            httplib::Response& res,
                            json& body) {
    body = json::object();
    if (req.body.empty()) {
        return true;
    }

    try {
        body = json::parse(req.body);
        if (!body.is_object()) {
            setJsonResponse(
                res,
                errorJson("Realtime session request must be a JSON object",
                          "invalid_request_error"),
                400);
            return false;
        }
        return true;
    } catch (...) {
        setJsonResponse(
            res,
            errorJson("Invalid JSON in realtime session request", "invalid_request_error"),
            400);
        return false;
    }
}

bool resolveCreateModel(const std::string& model,
                        httplib::Response& res,
                        std::string& normalized_model) {
    normalized_model = model.empty() ? "whisper-tiny" : model;
    if (asr::catalog::validTranscriptionModels().find(normalized_model) !=
        asr::catalog::validTranscriptionModels().end()) {
        return true;
    }

    setJsonResponse(
        res,
        errorJson("Unsupported model '" + normalized_model + "' for realtime session",
                  "invalid_request_error"),
        400);
    return false;
}

bool resolveCreateTask(const std::string& task,
                       httplib::Response& res,
                       std::string& normalized_task) {
    normalized_task = task.empty() ? "transcribe" : task;
    if (normalized_task == "transcribe" || normalized_task == "translate") {
        return true;
    }

    setJsonResponse(
        res,
        errorJson("Invalid task. Must be 'transcribe' or 'translate'",
                  "invalid_request_error"),
        400);
    return false;
}

int sanitizeSampleRateHz(int sample_rate_hz) {
    if (sample_rate_hz < 8000 || sample_rate_hz > 48000) {
        return kDefaultRealtimeSampleRateHz;
    }
    return sample_rate_hz;
}

int sanitizeMaxDurationSec(int max_duration_s) {
    if (max_duration_s < 5) {
        return 5;
    }
    if (max_duration_s > 120) {
        return 120;
    }
    return max_duration_s;
}

void resolveTurnDetection(const json& body,
                          httplib::Response& res,
                          double& vad_threshold,
                          int& vad_hangover_ms,
                          bool& ok) {
    ok = true;
    vad_threshold = kDefaultRealtimeVadThreshold;
    vad_hangover_ms = kDefaultRealtimeVadHangoverMs;
    if (!body.contains("turn_detection")) {
        return;
    }

    const auto& turn_detection = body["turn_detection"];
    if (!turn_detection.is_object()) {
        ok = setInvalidFieldType(res, "turn_detection", "object");
        return;
    }
    if (turn_detection.contains("threshold")) {
        if (!turn_detection["threshold"].is_number()) {
            ok = setInvalidFieldType(res, "turn_detection.threshold", "number");
            return;
        }
        vad_threshold = turn_detection["threshold"].get<double>();
    }
    if (turn_detection.contains("silence_duration_ms")) {
        if (!turn_detection["silence_duration_ms"].is_number_integer()) {
            ok = setInvalidFieldType(res, "turn_detection.silence_duration_ms", "integer");
            return;
        }
        vad_hangover_ms = turn_detection["silence_duration_ms"].get<int>();
    }

    if (vad_threshold <= 0.0 || vad_threshold > 1.0) {
        vad_threshold = kDefaultRealtimeVadThreshold;
    }
    if (vad_hangover_ms < 200 || vad_hangover_ms > 5000) {
        vad_hangover_ms = kDefaultRealtimeVadHangoverMs;
    }
}

bool parseCreateInput(const json& body,
                      httplib::Response& res,
                      RealtimeSessionCreateInput& input) {
    input = RealtimeSessionCreateInput{};
    if (body.contains("model")) {
        if (!body["model"].is_string()) {
            return setInvalidFieldType(res, "model", "string");
        }
        input.model = body["model"].get<std::string>();
    }
    if (body.contains("task")) {
        if (!body["task"].is_string()) {
            return setInvalidFieldType(res, "task", "string");
        }
        input.task = body["task"].get<std::string>();
    }
    if (body.contains("language")) {
        if (!body["language"].is_string()) {
            return setInvalidFieldType(res, "language", "string");
        }
        input.language = body["language"].get<std::string>();
    }
    if (body.contains("sample_rate_hz")) {
        if (!body["sample_rate_hz"].is_number_integer()) {
            return setInvalidFieldType(res, "sample_rate_hz", "integer");
        }
        input.sample_rate_hz = sanitizeSampleRateHz(body["sample_rate_hz"].get<int>());
    }
    if (body.contains("max_duration_s")) {
        if (!body["max_duration_s"].is_number_integer()) {
            return setInvalidFieldType(res, "max_duration_s", "integer");
        }
        input.max_duration_s = sanitizeMaxDurationSec(body["max_duration_s"].get<int>());
    }

    if (input.language.empty()) {
        input.language = "en";
    }

    bool turn_detection_ok = true;
    resolveTurnDetection(
        body, res, input.vad_threshold, input.vad_hangover_ms, turn_detection_ok);
    if (!turn_detection_ok) {
        return false;
    }

    return true;
}

RealtimeSessionState buildSessionState(const std::string& session_id,
                                       const RealtimeSessionCreateInput& input) {
    const auto now = std::chrono::steady_clock::now();
    RealtimeSessionState state;
    state.id = session_id;
    state.model = input.model;
    state.language = input.language;
    state.task = input.task;
    state.sample_rate_hz = input.sample_rate_hz;
    state.max_duration_s = input.max_duration_s;
    state.vad_threshold = input.vad_threshold;
    state.vad_hangover_ms = input.vad_hangover_ms;
    state.created_at = now;
    state.updated_at = now;
    return state;
}

int64_t nowEpochSec() {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

json buildCreateSessionResponse(const std::string& session_id,
                                const RealtimeSessionCreateInput& input,
                                int64_t expires_at) {
    return json{
        {"id", session_id},
        {"object", "realtime.session"},
        {"model", input.model},
        {"task", input.task},
        {"language", input.language},
        {"input_audio_format", "pcm16"},
        {"sample_rate_hz", input.sample_rate_hz},
        {"max_duration_s", input.max_duration_s},
        {"turn_detection", {
            {"type", "server_vad"},
            {"threshold", input.vad_threshold},
            {"silence_duration_ms", input.vad_hangover_ms}
        }},
        {"expires_at", expires_at},
        {"client_secret", {
            {"value", session_id},
            {"expires_at", expires_at}
        }}
    };
}
}  // namespace

void AsrService::registerRealtimeInfoRoute(httplib::Server& svr) {
    svr.Get("/v1/realtime",
    [](const httplib::Request&, httplib::Response& res) {
        setJsonResponse(res, json{
            {"object", "realtime.info"},
            {"description", "HTTP realtime compatibility surface"},
            {"endpoints", json::array({
                "/v1/realtime/sessions",
                "/v1/realtime/sessions/{id}",
                "/v1/realtime/sessions/{id}/audio",
                "/v1/realtime/sessions/{id}/finalize"
            })},
            {"input_audio_format", "pcm16"},
            {"recommended_sample_rate_hz", kDefaultRealtimeSampleRateHz}
        });
    });
}

void AsrService::registerRealtimeSessionCreateRoute(httplib::Server& svr) {
    svr.Post("/v1/realtime/sessions",
    [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseCreateSessionBody(req, res, body)) {
            return;
        }

        RealtimeSessionCreateInput input;
        if (!parseCreateInput(body, res, input)) {
            return;
        }
        if (!resolveCreateModel(input.model, res, input.model)) {
            return;
        }
        if (!resolveCreateTask(input.task, res, input.task)) {
            return;
        }

        const std::string session_id = makeRealtimeSessionId();
        RealtimeSessionState state = buildSessionState(session_id, input);
        if (!realtime_store_.tryInsertSession(
                std::move(state),
                config_.realtime_max_sessions,
                kDefaultRealtimeSessionTtlSec)) {
            counters_.realtime_sessions_rejected_total.fetch_add(1, std::memory_order_relaxed);
            asr::errors::setBusyRateLimitedError(
                req,
                res,
                "Realtime session limit reached. Try again later.");
            return;
        }
        counters_.realtime_sessions_created_total.fetch_add(1, std::memory_order_relaxed);

        const int64_t expires_at = nowEpochSec() + kDefaultRealtimeSessionTtlSec;
        setJsonResponse(res, buildCreateSessionResponse(session_id, input, expires_at));
    });
}

void AsrService::registerRealtimeSessionGetRoute(httplib::Server& svr) {
    svr.Get(R"(/v1/realtime/sessions/([^/]+))",
    [this](const httplib::Request& req, httplib::Response& res) {
        const std::string session_id = req.matches[1];
        RealtimeSessionState session_snapshot;
        const RealtimeSessionStore::SessionLookup lookup = realtime_store_.withLiveSession(
            session_id,
            kDefaultRealtimeSessionTtlSec,
            [&session_snapshot](RealtimeSessionState& s, uint64_t /*total_pending_before*/) {
                session_snapshot = s;
            });
        if (lookup == RealtimeSessionStore::SessionLookup::kNotFound) {
            setJsonResponse(
                res,
                errorJson("Realtime session not found: " + session_id, "invalid_request_error"),
                404);
            return;
        }
        if (lookup == RealtimeSessionStore::SessionLookup::kExpired) {
            setJsonResponse(
                res,
                errorJson("Realtime session expired: " + session_id, "invalid_request_error"),
                404);
            return;
        }

        setJsonResponse(res, json{
            {"id", session_snapshot.id},
            {"object", "realtime.session"},
            {"model", session_snapshot.model},
            {"task", session_snapshot.task},
            {"language", session_snapshot.language},
            {"sample_rate_hz", session_snapshot.sample_rate_hz},
            {"max_duration_s", session_snapshot.max_duration_s},
            {"speech_active", session_snapshot.speech_active},
            {"chunk_count", session_snapshot.chunk_count},
            {"audio_ms_total", session_snapshot.total_audio_ms},
            {"audio_ms_pending", pcmDurationMs(session_snapshot.pending_pcm.size(), session_snapshot.sample_rate_hz)},
            {"text", joinWithSpaces(session_snapshot.segments)},
            {"segments", session_snapshot.segments}
        });
    });
}

void AsrService::registerRealtimeSessionDeleteRoute(httplib::Server& svr) {
    svr.Delete(R"(/v1/realtime/sessions/([^/]+))",
    [this](const httplib::Request& req, httplib::Response& res) {
        const std::string session_id = req.matches[1];
        if (!realtime_store_.eraseSession(session_id)) {
            setJsonResponse(
                res,
                errorJson("Realtime session not found: " + session_id, "invalid_request_error"),
                404);
            return;
        }
        setJsonResponse(res, json{
            {"id", session_id},
            {"object", "realtime.session.deleted"},
            {"deleted", true}
        });
    });
}

void AsrService::registerRealtimeSessionStreamRoutes(httplib::Server& svr) {
    svr.Post(R"(/v1/realtime/sessions/([^/]+)/audio)",
    [this](const httplib::Request& req, httplib::Response& res) {
        handleRealtimeAudioAppend(req, res);
    });

    svr.Post(R"(/v1/realtime/sessions/([^/]+)/finalize)",
    [this](const httplib::Request& req, httplib::Response& res) {
        handleRealtimeFinalize(req, res);
    });
}

void AsrService::registerRealtimeRoutes(httplib::Server& svr) {
    registerRealtimeInfoRoute(svr);
    registerRealtimeSessionCreateRoute(svr);
    registerRealtimeSessionGetRoute(svr);
    registerRealtimeSessionDeleteRoute(svr);
    registerRealtimeSessionStreamRoutes(svr);
}
