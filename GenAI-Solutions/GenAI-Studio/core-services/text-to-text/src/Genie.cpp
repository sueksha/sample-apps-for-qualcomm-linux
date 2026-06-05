// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "Genie.hpp"

Genie::Genie(std::string config_json, uint32_t max_tokens)
    : config_json_(std::move(config_json)),
      max_tokens_(max_tokens) {}

Genie::~Genie() {
    cleanup();
}

bool Genie::isReady() const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    return cfg_ != nullptr && dlg_ != nullptr;
}

// ----------------------
// Unlocked helpers (require mu_ held)
// ----------------------
void Genie::_cleanupUnlocked() noexcept {
    if (dlg_) {
        if (GENIE_STATUS_SUCCESS != GenieDialog_free(dlg_)) {
            std::cerr << "[Genie] Warning: GenieDialog_free failed." << std::endl;
        }
        dlg_ = nullptr;
    }
    if (cfg_) {
        if (GENIE_STATUS_SUCCESS != GenieDialogConfig_free(cfg_)) {
            std::cerr << "[Genie] Warning: GenieDialogConfig_free failed." << std::endl;
        }
        cfg_ = nullptr;
    }
}

void Genie::_initializeUnlocked() {
    Genie_Status_t s;

    s = GenieDialogConfig_createFromJson(config_json_.c_str(), &cfg_);
    if (s != GENIE_STATUS_SUCCESS || !cfg_) {
        throw std::runtime_error(
            "GenieDialogConfig_createFromJson failed"
            " (status=" + std::to_string(static_cast<int>(s)) + ")."
            " Check config path/content: " + config_json_);
    }

    s = GenieDialog_create(cfg_, &dlg_);
    if (s != GENIE_STATUS_SUCCESS || !dlg_) {
        _cleanupUnlocked();
        throw std::runtime_error(
            "GenieDialog_create failed"
            " (status=" + std::to_string(static_cast<int>(s)) + ")."
            " Verify DSP firmware, LD_LIBRARY_PATH, ADSP_LIBRARY_PATH"
            " and that /dev/fastrpc-cdsp is accessible.");
    }

    s = GenieDialog_setMaxNumTokens(dlg_, max_tokens_);
    if (s != GENIE_STATUS_SUCCESS) {
        _cleanupUnlocked();
        throw std::runtime_error(
            "GenieDialog_setMaxNumTokens failed"
            " (status=" + std::to_string(static_cast<int>(s)) +
            ", max_tokens=" + std::to_string(max_tokens_) + ").");
    }
}

void Genie::applySamplerConfig(const std::string& samplerBlock) {
    // Must hold the mutex: dlg_ is shared with query/queryStream threads
    std::lock_guard<std::mutex> lock(mu_);

    Genie_Status_t status;

    GenieSampler_Handle_t samplerHandle = nullptr;
    status = GenieDialog_getSampler(dlg_, &samplerHandle);
    if (status != GENIE_STATUS_SUCCESS || !samplerHandle) {
        throw std::runtime_error(
            "GenieDialog_getSampler failed"
            " (status=" + std::to_string(static_cast<int>(status)) + ").");
    }

    GenieSamplerConfig_Handle_t samplerConfigHandle = nullptr;
    status = GenieSamplerConfig_createFromJson(samplerBlock.c_str(), &samplerConfigHandle);
    if (status != GENIE_STATUS_SUCCESS || !samplerConfigHandle) {
        throw std::runtime_error(
            "GenieSamplerConfig_createFromJson failed"
            " (status=" + std::to_string(static_cast<int>(status)) + ").");
    }

    // Apply setParam – free the config handle on any early exit to avoid leaks
    status = GenieSamplerConfig_setParam(samplerConfigHandle, "", samplerBlock.c_str());
    if (status != GENIE_STATUS_SUCCESS) {
        GenieSamplerConfig_free(samplerConfigHandle);
        throw std::runtime_error(
            "GenieSamplerConfig_setParam failed"
            " (status=" + std::to_string(static_cast<int>(status)) + ").");
    }

    // Apply config to sampler – free the config handle regardless of outcome
    status = GenieSampler_applyConfig(samplerHandle, samplerConfigHandle);
    GenieSamplerConfig_free(samplerConfigHandle);
    if (status != GENIE_STATUS_SUCCESS) {
        throw std::runtime_error(
            "GenieSampler_applyConfig failed"
            " (status=" + std::to_string(static_cast<int>(status)) + ").");
    }
}

// ----------------------
// Public API (locks once)
// ----------------------
void Genie::initialize() {
    std::lock_guard<std::mutex> lock(mu_);
    if (cfg_ || dlg_) {
        _cleanupUnlocked();
    }
    _initializeUnlocked();
}

void Genie::cleanup() noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    _cleanupUnlocked();
}

void Genie::reload() {
    std::lock_guard<std::mutex> lock(mu_);
    _cleanupUnlocked();
    _initializeUnlocked();
}

bool Genie::reset() noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    if (!dlg_) return false;
    return GENIE_STATUS_SUCCESS == GenieDialog_reset(dlg_);
}

bool Genie::setMaxTokens(uint32_t max_tokens) noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    max_tokens_ = max_tokens;
    if (!dlg_) return false;
    return GENIE_STATUS_SUCCESS == GenieDialog_setMaxNumTokens(dlg_, max_tokens_);
}

void Genie::setConfigJsonContent(std::string config_json) {
    std::lock_guard<std::mutex> lock(mu_);
    _cleanupUnlocked();
    config_json_ = std::move(config_json);
}

// ----------------------
// Blocking query callback
// ----------------------
void Genie::Callback(const char* response_back,
                     const GenieDialog_SentenceCode_t /*sentence_code*/,
                     const void* user_data) {
    auto* out = static_cast<std::string*>(const_cast<void*>(user_data));
    if (out && response_back) {
        out->append(response_back);
    }
}

std::string Genie::query(const std::string& prompt,
                         GenieDialog_SentenceCode_t sentence_code) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!dlg_) {
        throw std::runtime_error("Genie query called before initialization (dialog not ready).");
    }

    std::string model_response;
    const auto status = GenieDialog_query(
        dlg_,
        prompt.c_str(),
        sentence_code,
        &Genie::Callback,
        &model_response);

    if (status != GENIE_STATUS_SUCCESS) {
        throw std::runtime_error(
            "GenieDialog_query failed with status: " + std::to_string(status));
    }
    return model_response;
}

// ----------------------
// Streaming query callback
// ----------------------
// The callback is a plain C function pointer; exceptions must NOT propagate
// through it. We capture them in StreamContext::ex and re-throw after
// GenieDialog_query returns.
void Genie::StreamCallback(const char* response_back,
                           const GenieDialog_SentenceCode_t /*sentence_code*/,
                           const void* user_data) {
    auto* ctx = static_cast<StreamContext*>(const_cast<void*>(user_data));
    if (!ctx || !response_back) return;
    if (ctx->ex) return;  // a previous token already threw – skip remaining tokens
    try {
        ctx->fn(std::string(response_back));
    } catch (...) {
        ctx->ex = std::current_exception();
    }
}

void Genie::queryStream(const std::string& prompt,
                        std::function<void(const std::string&)> token_callback,
                        GenieDialog_SentenceCode_t sentence_code) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!dlg_) {
        throw std::runtime_error(
            "Genie queryStream called before initialization (dialog not ready).");
    }

    StreamContext ctx{std::move(token_callback), nullptr};

    const auto status = GenieDialog_query(
        dlg_,
        prompt.c_str(),
        sentence_code,
        &Genie::StreamCallback,
        &ctx);

    // Re-throw any exception that was captured inside the callback
    if (ctx.ex) {
        std::rethrow_exception(ctx.ex);
    }

    if (status != GENIE_STATUS_SUCCESS) {
        throw std::runtime_error(
            "GenieDialog_query (stream) failed with status: " + std::to_string(status));
    }
}
