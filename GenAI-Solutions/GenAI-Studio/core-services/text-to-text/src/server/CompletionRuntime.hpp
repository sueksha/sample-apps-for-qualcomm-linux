// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace App::CompletionRuntime {
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::duration<double, std::milli>;

struct TokenQueue {
    std::queue<std::string> q;
    std::mutex mtx;
    std::condition_variable cv;
    bool done{false};
    std::string error;
    std::atomic<bool> cancelled{false};

    Clock::time_point t_first_token{};
    Clock::time_point t_last_token{};
    uint32_t token_count{0};
    bool first_token_set{false};

    void Push(std::string token);
    void Finish(std::string err = "");
    void Cancel();
    std::optional<std::string> Pop();
};

struct TimingStats {
    double ttft_ms{0.0};
    double generation_ms{0.0};
    double total_ms{0.0};
    double tokens_per_second{0.0};
    double inter_token_latency_ms{0.0};
    uint32_t completion_tokens{0};
};

struct StageTimingStats {
    double request_parse_ms{0.0};
    double request_validate_ms{0.0};
    double prompt_build_ms{0.0};
    double max_tokens_stage_ms{0.0};
    double reset_stage_ms{0.0};
    double lock_wait_ms{0.0};
    double inference_ms{0.0};
};

bool IsExplicitlySet(const nlohmann::json& body, const char* key);
std::string BuildPromptFromMessages(const nlohmann::json& messages,
                                    bool& has_user_content,
                                    std::string& parse_error,
                                    const std::string& prompt_template = "llama3");
std::string MakeSseChunk(const std::string& id,
                         int64_t created,
                         const std::string& model,
                         const std::string& token,
                         bool is_first,
                         bool is_last);
TimingStats ComputeTimingStats(const Clock::time_point& t_start, const TokenQueue& tq);
nlohmann::json TimingToJson(const TimingStats& stats);
void AppendStageTiming(nlohmann::json& target, const StageTimingStats& stage_timing);

std::string GenerateCompletionId();
int64_t UnixTimestamp();

void StoreCompletion(nlohmann::json completion);
std::vector<nlohmann::json> SnapshotStoredCompletions();
bool GetStoredCompletion(const std::string& id, nlohmann::json& out);
bool DeleteStoredCompletion(const std::string& id);
} // namespace App::CompletionRuntime
