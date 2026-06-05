// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct AsrRuntimeConfig {
    std::string model_path;
    std::string vad_model_path;
    uint16_t port = 8081;

    size_t max_audio_body_bytes = 50ULL * 1024 * 1024;
    size_t max_pcm_body_bytes = 50ULL * 1024 * 1024;
    std::chrono::seconds engine_lock_timeout{30};
    bool realtime_enabled = true;
    size_t realtime_max_sessions = 100;
    size_t realtime_max_pending_pcm_bytes_per_session = 16ULL * 1024 * 1024;
    size_t realtime_max_total_pending_pcm_bytes = 256ULL * 1024 * 1024;
    size_t realtime_finalize_wait_ms = 2000;

    bool legacy_deprecation_enabled = true;
    bool legacy_hard_disable = false;
    bool legacy_compat_response = true;
    std::string legacy_sunset_rfc1123 = "Sat, 01 Aug 2026 00:00:00 GMT";
    std::string legacy_migration_doc_url;

    static AsrRuntimeConfig fromInputs(const std::string& model_path,
                                       uint16_t port,
                                       const std::string& vad_model_path);

    std::vector<std::string> validate() const;
};
