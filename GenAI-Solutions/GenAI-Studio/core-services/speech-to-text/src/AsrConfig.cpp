// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "AsrConfig.hpp"

#include <cstdlib>
#include <stdexcept>
#include <algorithm>
#include <cctype>

namespace {
std::string normalizeDirectoryPath(std::string path) {
    if (!path.empty() && path.back() != '/') path.push_back('/');
    return path;
}

int parsePositiveIntEnv(const char* env_key, int default_value) {
    const char* raw_value = std::getenv(env_key);
    if (raw_value == nullptr || *raw_value == '\0') return default_value;

    try {
        const int parsed = std::stoi(raw_value);
        if (parsed <= 0) {
            throw std::runtime_error(
                std::string(env_key) + " must be > 0 (actual: " + raw_value + ")");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error(
            std::string("Invalid integer for ") + env_key + ": " + raw_value);
    }
}

std::string parseStringEnv(const char* env_key, const std::string& default_value) {
    const char* raw_value = std::getenv(env_key);
    if (raw_value == nullptr) return default_value;
    return std::string(raw_value);
}

bool parseBoolEnv(const char* env_key, bool default_value) {
    const char* raw_value = std::getenv(env_key);
    if (raw_value == nullptr || *raw_value == '\0') return default_value;

    std::string value(raw_value);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
    if (value == "0" || value == "false" || value == "no" || value == "off") return false;

    throw std::runtime_error(
        std::string("Invalid boolean for ") + env_key +
        ". Expected one of: true/false, 1/0, yes/no, on/off");
}
}  // namespace

AsrRuntimeConfig AsrRuntimeConfig::fromInputs(const std::string& input_model_path,
                                              uint16_t input_port,
                                              const std::string& input_vad_model_path) {
    AsrRuntimeConfig config;
    config.model_path = normalizeDirectoryPath(input_model_path);
    config.vad_model_path = input_vad_model_path;
    config.port = input_port;

    const int max_audio_body_mb = parsePositiveIntEnv("ASR_MAX_AUDIO_BODY_MB", 50);
    const int max_pcm_body_mb = parsePositiveIntEnv("ASR_MAX_PCM_BODY_MB", 50);
    const int engine_lock_timeout_seconds = parsePositiveIntEnv("ASR_ENGINE_LOCK_TIMEOUT_S", 30);
    config.realtime_enabled = parseBoolEnv("ASR_REALTIME_ENABLED", true);
    const int realtime_max_sessions = parsePositiveIntEnv("ASR_REALTIME_MAX_SESSIONS", 100);
    const int realtime_max_pending_pcm_mb =
        parsePositiveIntEnv("ASR_REALTIME_MAX_PENDING_PCM_MB_PER_SESSION", 16);
    const int realtime_max_total_pending_pcm_mb =
        parsePositiveIntEnv("ASR_REALTIME_MAX_TOTAL_PENDING_PCM_MB", 256);
    const int realtime_finalize_wait_ms =
        parsePositiveIntEnv("ASR_REALTIME_FINALIZE_WAIT_MS", 2000);
    config.legacy_deprecation_enabled = parseBoolEnv("ASR_LEGACY_DEPRECATION_ENABLED", true);
    config.legacy_hard_disable = parseBoolEnv("ASR_LEGACY_HARD_DISABLE", false);
    config.legacy_compat_response = parseBoolEnv("ASR_LEGACY_COMPAT_RESPONSE", true);
    config.legacy_sunset_rfc1123 = parseStringEnv(
        "ASR_LEGACY_SUNSET_RFC1123", "Sat, 01 Aug 2026 00:00:00 GMT");
    config.legacy_migration_doc_url = parseStringEnv("ASR_LEGACY_MIGRATION_DOC_URL", "");

    config.max_audio_body_bytes = static_cast<size_t>(max_audio_body_mb) * 1024ULL * 1024ULL;
    config.max_pcm_body_bytes = static_cast<size_t>(max_pcm_body_mb) * 1024ULL * 1024ULL;
    config.engine_lock_timeout = std::chrono::seconds(engine_lock_timeout_seconds);
    config.realtime_max_sessions = static_cast<size_t>(realtime_max_sessions);
    config.realtime_max_pending_pcm_bytes_per_session =
        static_cast<size_t>(realtime_max_pending_pcm_mb) * 1024ULL * 1024ULL;
    config.realtime_max_total_pending_pcm_bytes =
        static_cast<size_t>(realtime_max_total_pending_pcm_mb) * 1024ULL * 1024ULL;
    config.realtime_finalize_wait_ms = static_cast<size_t>(realtime_finalize_wait_ms);

    return config;
}

std::vector<std::string> AsrRuntimeConfig::validate() const {
    std::vector<std::string> errors;

    if (model_path.empty()) {
        errors.emplace_back("model_path must be non-empty");
    }
    if (port == 0) {
        errors.emplace_back("port must be in range 1-65535");
    }
    if (max_audio_body_bytes == 0) {
        errors.emplace_back("max_audio_body_bytes must be > 0");
    }
    if (max_pcm_body_bytes == 0) {
        errors.emplace_back("max_pcm_body_bytes must be > 0");
    }
    if (engine_lock_timeout.count() <= 0) {
        errors.emplace_back("engine_lock_timeout must be > 0s");
    }
    if (realtime_max_sessions == 0) {
        errors.emplace_back("realtime_max_sessions must be > 0");
    }
    if (realtime_max_pending_pcm_bytes_per_session == 0) {
        errors.emplace_back("realtime_max_pending_pcm_bytes_per_session must be > 0");
    }
    if (realtime_max_total_pending_pcm_bytes == 0) {
        errors.emplace_back("realtime_max_total_pending_pcm_bytes must be > 0");
    }
    if (realtime_max_total_pending_pcm_bytes < realtime_max_pending_pcm_bytes_per_session) {
        errors.emplace_back(
            "realtime_max_total_pending_pcm_bytes must be >= realtime_max_pending_pcm_bytes_per_session");
    }
    if (realtime_finalize_wait_ms <= 0) {
        errors.emplace_back("realtime_finalize_wait_ms must be > 0");
    }
    if (legacy_deprecation_enabled && legacy_sunset_rfc1123.empty()) {
        errors.emplace_back("legacy_sunset_rfc1123 must be non-empty when legacy_deprecation_enabled=true");
    }

    return errors;
}
