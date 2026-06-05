// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <string>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <chrono>
#include "AsrConfig.hpp"
#include "OpenAiAudioContract.hpp"
#include "RealtimeSessionStore.hpp"
#include "WhisperEngine.hpp"

namespace httplib {
class Server;
struct Request;
struct Response;
}

// ---------------------------------------------------------------------------
// AsrService
//
// cpp-httplib-based HTTP server that exposes the Whisper transcription engine.
//
// Endpoints:
//   POST /v1/audio/transcriptions  – OpenAI Audio API (field "file")
//   POST /v1/audio/translations    – OpenAI Audio API translate to English
//   POST /generate                 – legacy multipart, field "audio"
//   POST /transcribe               – alias for /generate
//   POST /transcribe/raw           – raw PCM body (16 kHz, 16-bit, mono)
//   GET  /v1/realtime              – realtime compatibility info
//   POST /v1/realtime/sessions     – create realtime session
//   POST /v1/realtime/sessions/{id}/audio    – append PCM16 chunk
//   POST /v1/realtime/sessions/{id}/finalize – finalize transcript
//   GET  /health                   – liveness check
//   GET  /v1/models                – OpenAI models list (whisper aliases)
//   GET  /languages                – supported language codes
// ---------------------------------------------------------------------------
class AsrService {
public:
    explicit AsrService(const AsrRuntimeConfig& config);
    void run();

private:
    struct ServiceCounters {
        std::atomic<uint64_t> requests_total{0};
        std::atomic<uint64_t> requests_error{0};
        std::atomic<uint64_t> openai_transcriptions_total{0};
        std::atomic<uint64_t> openai_translations_total{0};
        std::atomic<uint64_t> openai_stream_total{0};
        std::atomic<uint64_t> legacy_total{0};
        std::atomic<uint64_t> realtime_sessions_created_total{0};
        std::atomic<uint64_t> realtime_sessions_rejected_total{0};
        std::atomic<uint64_t> realtime_audio_append_total{0};
        std::atomic<uint64_t> realtime_finalize_total{0};
    };

    AsrRuntimeConfig config_;
    WhisperEngine      engine_;
    uint16_t           port_;
    ServiceCounters    counters_;

    void handleOpenAiTranscriptions(const httplib::Request& req,
                                    httplib::Response& res);
    void handleOpenAiTranslations(const httplib::Request& req,
                                  httplib::Response& res);
    void handleOpenAiTranscriptionStream(const httplib::Request& req,
                                         httplib::Response& res);
    void handleOpenAiRequest(const httplib::Request& req,
                             httplib::Response& res,
                             OpenAiAudioRouteKind route_kind,
                             bool translate,
                             const std::string& task,
                             std::atomic<uint64_t>& counter);
    void handleLegacyMultipartTranscription(const httplib::Request& req,
                                            httplib::Response& res);
    void handleLegacyRawTranscription(const httplib::Request& req,
                                      httplib::Response& res);
    void handleRealtimeAudioAppend(const httplib::Request& req,
                                   httplib::Response& res);
    void handleRealtimeFinalize(const httplib::Request& req,
                                httplib::Response& res);
    void configureServerPolicies(httplib::Server& svr);
    void registerMethodNotAllowedRoutes(httplib::Server& svr);
    void registerOpenAiRoutes(httplib::Server& svr);
    void registerLegacyRoutes(httplib::Server& svr);
    void registerCoreRoutes(httplib::Server& svr);
    void registerRealtimeRoutes(httplib::Server& svr);
    void registerRealtimeInfoRoute(httplib::Server& svr);
    void registerRealtimeSessionCreateRoute(httplib::Server& svr);
    void registerRealtimeSessionGetRoute(httplib::Server& svr);
    void registerRealtimeSessionDeleteRoute(httplib::Server& svr);
    void registerRealtimeSessionStreamRoutes(httplib::Server& svr);

    // Serialises concurrent HTTP requests with a configurable timeout.
    // Prevents WhisperEngine::request_mu_ from blocking indefinitely.
    std::timed_mutex   engine_mutex_;

    RealtimeSessionStore realtime_store_;
};
