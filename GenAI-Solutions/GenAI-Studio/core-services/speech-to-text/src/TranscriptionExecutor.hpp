// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "AsrConfig.hpp"
#include "WhisperEngine.hpp"

struct TranscriptionExecution {
    TranscriptionResult result;
    double save_file_ms = 0.0;
    double lock_wait_ms = 0.0;
};

struct RealtimeTranscribeAttempt {
    bool success = true;
    int error_status = 0;
    std::string error_message;
    double lock_wait_ms = 0.0;
    std::optional<TranscriptionResult> result;
};

class TranscriptionExecutor {
public:
    TranscriptionExecutor(WhisperEngine& engine,
                          std::timed_mutex& engine_mutex,
                          const AsrRuntimeConfig& config);

    bool transcribeFileAudio(const std::string& audio_content,
                             const std::string& language,
                             bool translate,
                             TranscriptionExecution& out,
                             int& error_status,
                             std::string& error_message) const;

    bool transcribePcmAudio(const std::vector<uint8_t>& pcm_bytes,
                            const std::string& language,
                            bool translate,
                            TranscriptionExecution& out,
                            int& error_status,
                            std::string& error_message) const;

    RealtimeTranscribeAttempt transcribePcmWithRollback(
        const std::vector<uint8_t>& pcm_bytes,
        const std::string& language,
        bool translate,
        const std::function<void()>& rollback_fn) const;

private:
    WhisperEngine& engine_;
    std::timed_mutex& engine_mutex_;
    const AsrRuntimeConfig& config_;
};
