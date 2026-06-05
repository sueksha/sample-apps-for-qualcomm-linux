// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "server/CompletionRuntime.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <unordered_map>

#include "server/ServerUtils.hpp"

using json = nlohmann::json;

namespace {
std::mutex g_store_mu;
std::deque<std::string> g_store_order;
std::unordered_map<std::string, json> g_store_items;
constexpr size_t kStoreCapacity = 200;

bool ParseTextContent(const json& content, std::string& out, std::string& err) {
    out.clear();
    if (content.is_null()) return true;
    if (content.is_string()) {
        out = App::ServerUtils::SanitizeContent(content.get<std::string>());
        return true;
    }
    if (!content.is_array()) {
        err = "'content' must be a string or an array of content parts";
        return false;
    }

    for (size_t idx = 0; idx < content.size(); ++idx) {
        const auto& part = content[idx];
        if (!part.is_object()) {
            err = "content[" + std::to_string(idx) + "] must be an object";
            return false;
        }
        const std::string type = App::ServerUtils::SafeStr(part, "type", "");
        if (type == "text") {
            if (!part.contains("text") || !part["text"].is_string()) {
                err = "content[" + std::to_string(idx) + "].text must be a string";
                return false;
            }
            out += App::ServerUtils::SanitizeContent(part["text"].get<std::string>());
            continue;
        }
        if (type == "image_url" || type == "input_audio" || type == "file") {
            err = "Unsupported content part type for this model: " + type;
            return false;
        }
        err = "Unsupported content part type: " + type;
        return false;
    }
    return true;
}

std::string NormalizePromptTemplateName(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}
} // namespace

namespace App::CompletionRuntime {
void TokenQueue::Push(std::string token) {
    if (cancelled.load(std::memory_order_relaxed)) return;

    const auto now = Clock::now();
    {
        std::lock_guard<std::mutex> lk(mtx);
        if (!first_token_set) {
            t_first_token = now;
            first_token_set = true;
        }
        t_last_token = now;
        ++token_count;
        q.push(std::move(token));
    }
    cv.notify_one();
}

void TokenQueue::Finish(std::string err) {
    {
        std::lock_guard<std::mutex> lk(mtx);
        done = true;
        error = std::move(err);
    }
    cv.notify_all();
}

void TokenQueue::Cancel() {
    cancelled.store(true, std::memory_order_relaxed);
    Finish("client disconnected");
}

std::optional<std::string> TokenQueue::Pop() {
    std::unique_lock<std::mutex> lk(mtx);
    cv.wait(lk, [this] { return !q.empty() || done; });
    if (!q.empty()) {
        auto s = std::move(q.front());
        q.pop();
        return s;
    }
    return std::nullopt;
}

bool IsExplicitlySet(const json& body, const char* key) {
    return body.contains(key) && !body[key].is_null();
}

std::string BuildPromptFromMessages(const json& messages,
                                    bool& has_user_content,
                                    std::string& parse_error,
                                    const std::string& prompt_template) {
    std::string prompt;
    has_user_content = false;
    parse_error.clear();

    const std::string template_name = NormalizePromptTemplateName(prompt_template);

    if (template_name.empty() || template_name == "llama3") {
        bool began = false;
        for (size_t msg_idx = 0; msg_idx < messages.size(); ++msg_idx) {
            const auto& msg = messages[msg_idx];
            if (!msg.is_object()) continue;

            const std::string role = App::ServerUtils::SafeStr(msg, "role", "");
            if (role.empty()) {
                parse_error = "messages[" + std::to_string(msg_idx) + "].role is required";
                return {};
            }

            std::string content;
            if (!msg.contains("content")) {
                content.clear();
            } else if (!ParseTextContent(msg["content"], content, parse_error)) {
                parse_error = "messages[" + std::to_string(msg_idx) + "]: " + parse_error;
                return {};
            }

            if (role == "system" || role == "developer") {
                prompt += "<|begin_of_text|><|start_header_id|>system<|end_header_id|>\n\n";
                prompt += content;
                prompt += " <|eot_id|>\n\n";
                began = true;
            } else if (role == "assistant") {
                prompt += "<|start_header_id|>assistant<|end_header_id|>\n\n";
                prompt += content;
                prompt += "<|eot_id|>";
            } else if (role == "user" || role == "tool" || role == "function") {
                if (!began) {
                    prompt += "<|begin_of_text|>";
                    began = true;
                }
                prompt += "<|start_header_id|>user<|end_header_id|>\n\n";
                prompt += content;
                prompt += "<|eot_id|>";
                if (!content.empty()) has_user_content = true;
            } else {
                parse_error = "Unsupported role in messages[" + std::to_string(msg_idx) + "]: " +
                              role;
                return {};
            }
        }
        prompt += "<|start_header_id|>assistant<|end_header_id|>\n\n";
        return prompt;
    }

    if (template_name == "chatml" || template_name == "qwen") {
        for (size_t msg_idx = 0; msg_idx < messages.size(); ++msg_idx) {
            const auto& msg = messages[msg_idx];
            if (!msg.is_object()) continue;

            const std::string role = App::ServerUtils::SafeStr(msg, "role", "");
            if (role.empty()) {
                parse_error = "messages[" + std::to_string(msg_idx) + "].role is required";
                return {};
            }

            std::string content;
            if (!msg.contains("content")) {
                content.clear();
            } else if (!ParseTextContent(msg["content"], content, parse_error)) {
                parse_error = "messages[" + std::to_string(msg_idx) + "]: " + parse_error;
                return {};
            }

            std::string role_tag;
            if (role == "system" || role == "developer") {
                role_tag = "system";
            } else if (role == "assistant") {
                role_tag = "assistant";
            } else if (role == "user" || role == "tool" || role == "function") {
                role_tag = "user";
                if (!content.empty()) has_user_content = true;
            } else {
                parse_error = "Unsupported role in messages[" + std::to_string(msg_idx) + "]: " +
                              role;
                return {};
            }

            prompt += "<|im_start|>" + role_tag + "\n";
            prompt += content;
            prompt += "\n<|im_end|>\n";
        }

        prompt += "<|im_start|>assistant\n";
        return prompt;
    }

    if (template_name == "falcon") {
        for (size_t msg_idx = 0; msg_idx < messages.size(); ++msg_idx) {
            const auto& msg = messages[msg_idx];
            if (!msg.is_object()) continue;

            const std::string role = App::ServerUtils::SafeStr(msg, "role", "");
            if (role.empty()) {
                parse_error = "messages[" + std::to_string(msg_idx) + "].role is required";
                return {};
            }

            std::string content;
            if (!msg.contains("content")) {
                content.clear();
            } else if (!ParseTextContent(msg["content"], content, parse_error)) {
                parse_error = "messages[" + std::to_string(msg_idx) + "]: " + parse_error;
                return {};
            }

            std::string role_tag;
            if (role == "system" || role == "developer") {
                role_tag = "System";
            } else if (role == "assistant") {
                role_tag = "Assistant";
            } else if (role == "user" || role == "tool" || role == "function") {
                role_tag = "User";
                if (!content.empty()) has_user_content = true;
            } else {
                parse_error = "Unsupported role in messages[" + std::to_string(msg_idx) + "]: " +
                              role;
                return {};
            }

            prompt += role_tag + ": " + content + "\n";
        }

        prompt += "Assistant:";
        return prompt;
    }

    parse_error = "Unsupported prompt template: " + prompt_template;
    return prompt;
}

std::string MakeSseChunk(const std::string& id,
                         int64_t created,
                         const std::string& model,
                         const std::string& token,
                         bool is_first,
                         bool is_last) {
    json delta;
    if (is_first) {
        delta["role"] = "assistant";
        delta["content"] = "";
    } else if (is_last) {
        delta = json::object();
    } else {
        delta["content"] = token;
    }

    json choice;
    choice["index"] = 0;
    choice["delta"] = delta;
    choice["finish_reason"] = is_last ? json("stop") : json(nullptr);

    json chunk;
    chunk["id"] = id;
    chunk["object"] = "chat.completion.chunk";
    chunk["created"] = created;
    chunk["model"] = model;
    chunk["choices"] = json::array({choice});
    return chunk.dump();
}

TimingStats ComputeTimingStats(const Clock::time_point& t_start, const TokenQueue& tq) {
    TimingStats stats;
    stats.completion_tokens = tq.token_count;
    if (!tq.first_token_set) return stats;

    const auto now = Clock::now();
    stats.ttft_ms = Ms(tq.t_first_token - t_start).count();
    stats.generation_ms =
        tq.token_count > 1 ? Ms(tq.t_last_token - tq.t_first_token).count() : 0.0;
    stats.total_ms = Ms(now - t_start).count();
    stats.tokens_per_second =
        stats.total_ms > 0 ? (tq.token_count * 1000.0 / stats.total_ms) : 0.0;
    stats.inter_token_latency_ms =
        tq.token_count > 1 ? stats.generation_ms / (tq.token_count - 1) : 0.0;
    return stats;
}

json TimingToJson(const TimingStats& stats) {
    auto round3 = [](double value) {
        return std::round(value * 1000.0) / 1000.0;
    };
    return json{{"prefill_time_ms", round3(stats.ttft_ms)},
                {"time_to_first_token_ms", round3(stats.ttft_ms)},
                {"generation_time_ms", round3(stats.generation_ms)},
                {"total_time_ms", round3(stats.total_ms)},
                {"tokens_per_second", round3(stats.tokens_per_second)},
                {"inter_token_latency_ms", round3(stats.inter_token_latency_ms)}};
}

void AppendStageTiming(json& target, const StageTimingStats& stage_timing) {
    auto round3 = [](double value) {
        return std::round(value * 1000.0) / 1000.0;
    };
    target["request_parse_ms"] = round3(stage_timing.request_parse_ms);
    target["request_validate_ms"] = round3(stage_timing.request_validate_ms);
    target["prompt_build_ms"] = round3(stage_timing.prompt_build_ms);
    target["max_tokens_stage_ms"] = round3(stage_timing.max_tokens_stage_ms);
    target["reset_stage_ms"] = round3(stage_timing.reset_stage_ms);
    target["lock_wait_ms"] = round3(stage_timing.lock_wait_ms);
    target["inference_ms"] = round3(stage_timing.inference_ms);
}

std::string GenerateCompletionId() {
    static std::atomic<uint64_t> counter{0};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return "chatcmpl-" + std::to_string(ms) + "-" +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

int64_t UnixTimestamp() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void StoreCompletion(json completion) {
    if (!completion.is_object() || !completion.contains("id")) return;
    const std::string id = App::ServerUtils::SafeStr(completion, "id", "");
    if (id.empty()) return;

    std::lock_guard<std::mutex> lk(g_store_mu);
    if (g_store_items.find(id) == g_store_items.end()) {
        g_store_order.push_back(id);
    }
    g_store_items[id] = std::move(completion);

    while (g_store_order.size() > kStoreCapacity) {
        const std::string old = g_store_order.front();
        g_store_order.pop_front();
        g_store_items.erase(old);
    }
}

std::vector<json> SnapshotStoredCompletions() {
    std::vector<json> out;
    std::lock_guard<std::mutex> lk(g_store_mu);
    out.reserve(g_store_order.size());
    for (const auto& id : g_store_order) {
        auto it = g_store_items.find(id);
        if (it != g_store_items.end()) out.push_back(it->second);
    }
    return out;
}

bool GetStoredCompletion(const std::string& id, json& out) {
    std::lock_guard<std::mutex> lk(g_store_mu);
    auto it = g_store_items.find(id);
    if (it == g_store_items.end()) return false;
    out = it->second;
    return true;
}

bool DeleteStoredCompletion(const std::string& id) {
    std::lock_guard<std::mutex> lk(g_store_mu);
    auto it = g_store_items.find(id);
    if (it == g_store_items.end()) return false;

    g_store_items.erase(it);
    g_store_order.erase(std::remove(g_store_order.begin(), g_store_order.end(), id),
                        g_store_order.end());
    return true;
}
} // namespace App::CompletionRuntime
