// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------

#include "TranscriptionExecutor.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::duration<double, std::milli>;

struct TempFileGuard {
    std::string path;
    explicit TempFileGuard(std::string p) : path(std::move(p)) {}
    ~TempFileGuard() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
};

std::string makeTempWavPath() {
    static std::atomic<uint64_t> counter{0};
    return "/tmp/asr_" + std::to_string(counter.fetch_add(1)) + ".wav";
}

bool saveToFile(const std::string& data, const std::string& path) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    return ofs.good();
}
} // namespace

TranscriptionExecutor::TranscriptionExecutor(WhisperEngine& engine,
                                             std::timed_mutex& engine_mutex,
                                             const AsrRuntimeConfig& config)
    : engine_(engine),
      engine_mutex_(engine_mutex),
      config_(config) {}

bool TranscriptionExecutor::transcribeFileAudio(const std::string& audio_content,
                                                const std::string& language,
                                                bool translate,
                                                TranscriptionExecution& out,
                                                int& error_status,
                                                std::string& error_message) const {
    out = TranscriptionExecution{};
    error_status = 0;
    error_message.clear();

    const std::string temp_wav_path = makeTempWavPath();
    TempFileGuard guard(temp_wav_path);
    const auto save_start = Clock::now();
    if (!saveToFile(audio_content, temp_wav_path)) {
        error_status = 500;
        error_message = "Failed to write temp file";
        return false;
    }
    out.save_file_ms = Ms(Clock::now() - save_start).count();

    std::unique_lock<std::timed_mutex> lock(engine_mutex_, std::defer_lock);
    const auto lock_start = Clock::now();
    if (!lock.try_lock_for(config_.engine_lock_timeout)) {
        error_status = 429;
        error_message = "Model is busy. Try again later.";
        return false;
    }
    out.lock_wait_ms = Ms(Clock::now() - lock_start).count();

    out.result = engine_.transcribeFile(temp_wav_path, translate ? "" : language, translate);
    lock.unlock();

    if (!out.result.success) {
        error_status = (out.result.error.find("timed out") != std::string::npos) ? 504 : 500;
        error_message = out.result.error;
        return false;
    }
    return true;
}

bool TranscriptionExecutor::transcribePcmAudio(const std::vector<uint8_t>& pcm_bytes,
                                               const std::string& language,
                                               bool translate,
                                               TranscriptionExecution& out,
                                               int& error_status,
                                               std::string& error_message) const {
    out = TranscriptionExecution{};
    error_status = 0;
    error_message.clear();

    std::unique_lock<std::timed_mutex> lock(engine_mutex_, std::defer_lock);
    const auto lock_start = Clock::now();
    if (!lock.try_lock_for(config_.engine_lock_timeout)) {
        error_status = 429;
        error_message = "Model is busy. Try again later.";
        return false;
    }
    out.lock_wait_ms = Ms(Clock::now() - lock_start).count();

    out.result = engine_.transcribePCM(pcm_bytes, language, translate);
    lock.unlock();

    if (!out.result.success) {
        error_status = (out.result.error.find("timed out") != std::string::npos) ? 504 : 500;
        error_message = out.result.error;
        return false;
    }
    return true;
}

RealtimeTranscribeAttempt TranscriptionExecutor::transcribePcmWithRollback(
    const std::vector<uint8_t>& pcm_bytes,
    const std::string& language,
    bool translate,
    const std::function<void()>& rollback_fn) const {
    RealtimeTranscribeAttempt out;
    if (pcm_bytes.empty()) return out;

    std::unique_lock<std::timed_mutex> lock(engine_mutex_, std::defer_lock);
    const auto lock_start = Clock::now();
    if (!lock.try_lock_for(config_.engine_lock_timeout)) {
        rollback_fn();
        out.success = false;
        out.error_status = 429;
        out.error_message = "Model is busy. Try again later.";
        return out;
    }
    out.lock_wait_ms = Ms(Clock::now() - lock_start).count();

    out.result = engine_.transcribePCM(pcm_bytes, language, translate);
    if (!out.result->success) {
        rollback_fn();
        out.success = false;
        out.error_status =
            out.result->error.find("timed out") != std::string::npos ? 504 : 500;
        out.error_message = out.result->error;
        return out;
    }

    return out;
}
