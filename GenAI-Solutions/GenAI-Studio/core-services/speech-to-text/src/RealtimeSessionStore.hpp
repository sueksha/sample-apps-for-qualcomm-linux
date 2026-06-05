// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct RealtimeSessionState {
    std::string id;
    std::string model = "whisper-tiny";
    std::string language = "en";
    std::string task = "transcribe"; // transcribe | translate
    std::string response_format = "json";
    int sample_rate_hz = 16000;
    int max_duration_s = 30;
    double vad_threshold = 0.015;
    int vad_hangover_ms = 800;
    bool speech_active = false;
    uint64_t trailing_silence_ms = 0;
    uint64_t total_audio_ms = 0;
    uint64_t chunk_count = 0;
    double last_rms = 0.0;
    bool finalizing = false;
    bool finalized = false;
    uint64_t active_transcriptions = 0;
    uint64_t finalizing_token = 0;
    std::chrono::system_clock::time_point finalized_at{};
    std::vector<uint8_t> pending_pcm;
    std::vector<std::string> segments;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point updated_at;
};

struct RealtimeStoreStats {
    size_t active_sessions = 0;
    uint64_t pending_pcm_bytes = 0;
};

class RealtimeSessionStore {
public:
    enum class SessionLookup {
        kFound,
        kNotFound,
        kExpired
    };

    RealtimeStoreStats snapshotStats() const;

    bool tryInsertSession(RealtimeSessionState state,
                          size_t max_sessions,
                          int session_ttl_sec);
    bool eraseSession(const std::string& session_id);

    SessionLookup withLiveSession(
        const std::string& session_id,
        int session_ttl_sec,
        const std::function<void(RealtimeSessionState&, uint64_t total_pending_before)>& fn);

    bool withSessionIfExists(
        const std::string& session_id,
        const std::function<void(RealtimeSessionState&)>& fn);

    void prependPendingIfExists(const std::string& session_id,
                                const std::vector<uint8_t>& pcm_prefix);

private:
    void pruneExpiredSessionsLocked(int session_ttl_sec,
                                    std::chrono::steady_clock::time_point now);
    uint64_t totalPendingBytesLocked() const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, RealtimeSessionState> sessions_;
};
