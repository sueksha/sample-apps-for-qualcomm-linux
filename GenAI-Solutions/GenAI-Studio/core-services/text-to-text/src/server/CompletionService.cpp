// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "server/CompletionService.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "Logger.hpp"
#include "server/CompletionRuntime.hpp"
#include "server/HttpServer.hpp"
#include "server/ModelAccessPolicy.hpp"
#include "server/ModelCatalog.hpp"
#include "server/ServerUtils.hpp"

using namespace App;
using json = nlohmann::json;
using Clock = CompletionRuntime::Clock;
using Ms = CompletionRuntime::Ms;

namespace {
constexpr size_t kMaxCompletionRequestBytes = 10 * 1024 * 1024;
constexpr size_t kDefaultMaxPromptChars = 2048;

struct CompletionRequestSpec {
    json messages;
    bool do_stream{false};
    bool do_store{false};
    bool include_usage{false};
    std::string model_name;
    int n_choices{1};
    uint32_t max_tokens{0};
};

size_t maxPromptChars() {
    const char* raw = std::getenv("TG_MAX_PROMPT_CHARS");
    if (!raw || !*raw) {
        return kDefaultMaxPromptChars;
    }
    try {
        const int parsed = std::stoi(raw);
        if (parsed < 256) {
            return 256;
        }
        return static_cast<size_t>(parsed);
    } catch (...) {
        return kDefaultMaxPromptChars;
    }
}

std::string truncatePromptForBackend(const std::string& prompt, size_t max_chars) {
    if (prompt.size() <= max_chars) {
        return prompt;
    }
    // Keep the tail so the assistant header suffix emitted by prompt builders
    // remains intact for generation.
    return prompt.substr(prompt.size() - max_chars);
}

bool ParseCompletionRequestBody(const httplib::Request& req,
                                json& body,
                                CompletionRuntime::StageTimingStats& stage_ts,
                                httplib::Response& res) {
    if (req.body.size() > kMaxCompletionRequestBytes) {
        ServerUtils::SetJsonError(res,
                     413,
                     "Request body exceeds 10 MB limit",
                     "invalid_request_error",
                     "request_too_large");
        return false;
    }

    const auto t_parse_start = Clock::now();
    try {
        body = json::parse(req.body);
    } catch (const json::parse_error& e) {
        ServerUtils::SetJsonError(res,
                     400,
                     std::string("JSON parse error: ") + e.what(),
                     "invalid_request_error");
        return false;
    }

    stage_ts.request_parse_ms = Ms(Clock::now() - t_parse_start).count();
    return true;
}

bool ValidateMessagesArray(const json& body, json& messages, httplib::Response& res) {
    if (!body.contains("messages") || !body["messages"].is_array() ||
        body["messages"].empty()) {
        ServerUtils::SetJsonError(res,
                     400,
                     "'messages' must be a non-empty array",
                     "invalid_request_error");
        return false;
    }

    messages = body["messages"];
    for (size_t i = 0; i < messages.size(); ++i) {
        if (!messages[i].is_object()) {
            ServerUtils::SetJsonError(res,
                         400,
                         "messages[" + std::to_string(i) +
                             "] must be a JSON object, got: " +
                             std::string(messages[i].type_name()),
                         "invalid_request_error");
            return false;
        }
    }

    return true;
}

bool ValidateUnsupportedKeys(const json& body, httplib::Response& res) {
    static const std::vector<std::string> unsupported_keys = {
        "tools",          "tool_choice",   "parallel_tool_calls", "functions",
        "function_call",  "audio",         "modalities",          "prediction",
        "web_search_options", "top_logprobs", "logprobs"};

    for (const auto& key : unsupported_keys) {
        if (CompletionRuntime::IsExplicitlySet(body, key.c_str())) {
            ServerUtils::SetJsonError(res,
                         400,
                         "Unsupported parameter for this model: " + key,
                         "invalid_request_error",
                         "",
                         key);
            return false;
        }
    }

    return true;
}

bool ValidateOptionalBoolField(const json& body,
                               const char* key,
                               httplib::Response& res) {
    if (!body.contains(key)) return true;
    if (body[key].is_boolean()) return true;
    ServerUtils::SetJsonError(res,
                 400,
                 std::string("'") + key + "' must be a boolean",
                 "invalid_request_error",
                 "",
                 key);
    return false;
}

bool ValidateOptionalStringField(const json& body,
                                 const char* key,
                                 httplib::Response& res) {
    if (!body.contains(key)) return true;
    if (body[key].is_string()) return true;
    ServerUtils::SetJsonError(res,
                 400,
                 std::string("'") + key + "' must be a string",
                 "invalid_request_error",
                 "",
                 key);
    return false;
}

bool ValidatePrimitiveRequestTypes(const json& body, httplib::Response& res) {
    if (!ValidateOptionalBoolField(body, "stream", res)) return false;
    if (!ValidateOptionalBoolField(body, "store", res)) return false;
    if (!ValidateOptionalStringField(body, "model", res)) return false;
    return true;
}

enum class IntegerParseStatus {
    kMissing,
    kValid,
    kInvalid,
};

IntegerParseStatus TryParseIntegerLike(const json& body, const char* key, int64_t& out) {
    if (!body.contains(key) || body[key].is_null()) return IntegerParseStatus::kMissing;

    const auto& value = body[key];
    try {
        if (value.is_number_integer()) {
            out = value.get<int64_t>();
            return IntegerParseStatus::kValid;
        }
        if (value.is_number_unsigned()) {
            const auto parsed = value.get<uint64_t>();
            if (parsed > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                return IntegerParseStatus::kInvalid;
            }
            out = static_cast<int64_t>(parsed);
            return IntegerParseStatus::kValid;
        }
        if (value.is_number_float()) {
            const double parsed = value.get<double>();
            if (!std::isfinite(parsed)) return IntegerParseStatus::kInvalid;
            double integral_part = 0.0;
            if (std::modf(parsed, &integral_part) != 0.0) {
                return IntegerParseStatus::kInvalid;
            }
            if (integral_part < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
                integral_part > static_cast<double>(std::numeric_limits<int64_t>::max())) {
                return IntegerParseStatus::kInvalid;
            }
            out = static_cast<int64_t>(integral_part);
            return IntegerParseStatus::kValid;
        }
        if (value.is_string()) {
            const std::string raw = value.get<std::string>();
            size_t consumed = 0;
            const long long parsed = std::stoll(raw, &consumed, 10);
            if (consumed != raw.size()) return IntegerParseStatus::kInvalid;
            out = static_cast<int64_t>(parsed);
            return IntegerParseStatus::kValid;
        }
    } catch (...) {
        return IntegerParseStatus::kInvalid;
    }
    return IntegerParseStatus::kInvalid;
}

bool ParseMaxTokens(const json& body,
                    uint32_t max_tokens_default,
                    uint32_t& out_max_tokens,
                    httplib::Response& res) {
    out_max_tokens = max_tokens_default;

    // max_tokens wins when both are present.
    const char* key = nullptr;
    if (body.contains("max_tokens")) {
        key = "max_tokens";
    } else if (body.contains("max_completion_tokens")) {
        key = "max_completion_tokens";
    } else {
        return true;
    }

    int64_t parsed = 0;
    if (TryParseIntegerLike(body, key, parsed) != IntegerParseStatus::kValid) {
        ServerUtils::SetJsonError(res,
                                  400,
                                  std::string("'") + key +
                                      "' must be an integer between 1 and 65536",
                                  "invalid_request_error",
                                  "",
                                  key);
        return false;
    }

    if (parsed < 1 || parsed > 65536) {
        ServerUtils::SetJsonError(res,
                                  400,
                                  std::string("'") + key +
                                      "' must be between 1 and 65536",
                                  "invalid_request_error",
                                  "",
                                  key);
        return false;
    }

    out_max_tokens = static_cast<uint32_t>(parsed);
    return true;
}

bool ParseChoiceCount(const json& body, int& n_choices, httplib::Response& res) {
    n_choices = 1;
    if (!body.contains("n")) return true;

    int64_t parsed = 0;
    if (TryParseIntegerLike(body, "n", parsed) != IntegerParseStatus::kValid) {
        ServerUtils::SetJsonError(res,
                                  400,
                                  "'n' must be an integer",
                                  "invalid_request_error",
                                  "",
                                  "n");
        return false;
    }

    if (parsed < 1 || parsed > 128) {
        ServerUtils::SetJsonError(res,
                                  400,
                                  "'n' must be between 1 and 128",
                                  "invalid_request_error",
                                  "",
                                  "n");
        return false;
    }

    n_choices = static_cast<int>(parsed);
    return true;
}

bool ValidateResponseFormat(const json& body, httplib::Response& res) {
    if (!body.contains("response_format")) {
        return true;
    }

    const auto& response_format = body["response_format"];
    bool ok = false;
    if (response_format.is_string()) {
        ok = response_format.get<std::string>() == "auto";
    } else if (response_format.is_object()) {
        if (response_format.contains("type") &&
            !response_format["type"].is_string()) {
            ServerUtils::SetJsonError(res,
                         400,
                         "'response_format.type' must be a string",
                         "invalid_request_error",
                         "",
                         "response_format.type");
            return false;
        }
        const std::string type = ServerUtils::SafeStr(response_format, "type", "");
        ok = type.empty() || type == "text";
    }

    if (!ok) {
        ServerUtils::SetJsonError(res,
                     400,
                     "Unsupported response_format for this model",
                     "invalid_request_error",
                     "",
                     "response_format");
        return false;
    }

    return true;
}

bool ParseAndValidateCompletionRequest(const httplib::Request& req,
                                       const std::string& active_model_id,
                                       uint32_t max_tokens_default,
                                       CompletionRequestSpec& out,
                                       CompletionRuntime::StageTimingStats& stage_ts,
                                       httplib::Response& res) {
    json body;
    if (!ParseCompletionRequestBody(req, body, stage_ts, res)) {
        return false;
    }

    const auto t_validate_start = Clock::now();
    if (!ValidateMessagesArray(body, out.messages, res)) {
        return false;
    }
    if (!ValidateUnsupportedKeys(body, res)) {
        return false;
    }
    if (!ValidatePrimitiveRequestTypes(body, res)) {
        return false;
    }
    if (!ValidateResponseFormat(body, res)) {
        return false;
    }

    out.do_stream = body.contains("stream") ? body["stream"].get<bool>() : false;
    out.do_store = body.contains("store") ? body["store"].get<bool>() : false;
    out.model_name = body.contains("model") ? body["model"].get<std::string>()
                                            : active_model_id;
    if (out.model_name.empty()) {
        out.model_name = active_model_id;
    }
    
    APP_LOG_DEBUG() << "Requested model: '" << out.model_name << "', Active model: '" << active_model_id << "'";
    
    // Try to normalize the requested model name
    std::string normalized_model;
    if (!ModelCatalog::NormalizeRequestedModelName(out.model_name, active_model_id, normalized_model)) {
        // Normalization failed - model is not compatible
        APP_LOG_WARN() << "Model normalization failed for '" << out.model_name << "' against active '" << active_model_id << "'";
        ServerUtils::SetJsonError(
            res,
            400,
            "Unsupported model '" + out.model_name + "'. Active model is '" +
                active_model_id + "'. Use GET /v1/models to discover valid ids.",
            "invalid_request_error",
            "model_not_found",
            "model");
        return false;
    }
    APP_LOG_DEBUG() << "Model normalized to: '" << normalized_model << "'";
    // Use the normalized model name
    out.model_name = normalized_model;

    if (!ParseChoiceCount(body, out.n_choices, res)) {
        return false;
    }

    if (out.do_stream && out.n_choices != 1) {
        ServerUtils::SetJsonError(res,
                     400,
                     "stream=true currently supports only n=1",
                     "invalid_request_error",
                     "",
                     "n");
        return false;
    }

    out.include_usage = false;
    if (body.contains("stream_options")) {
        if (!body["stream_options"].is_object()) {
            ServerUtils::SetJsonError(res,
                         400,
                         "'stream_options' must be an object",
                         "invalid_request_error",
                         "",
                         "stream_options");
            return false;
        }
        if (body["stream_options"].contains("include_usage") &&
            !body["stream_options"]["include_usage"].is_boolean()) {
            ServerUtils::SetJsonError(res,
                         400,
                         "'stream_options.include_usage' must be a boolean",
                         "invalid_request_error",
                         "",
                         "stream_options.include_usage");
            return false;
        }
        out.include_usage = body["stream_options"].contains("include_usage")
                                ? body["stream_options"]["include_usage"].get<bool>()
                                : false;
    }

    if (!ParseMaxTokens(body, max_tokens_default, out.max_tokens, res)) {
        return false;
    }

    stage_ts.request_validate_ms = Ms(Clock::now() - t_validate_start).count();
    return true;
}

bool BuildPromptForCompletion(const CompletionRequestSpec& request,
                              const std::string& prompt_template,
                              std::string& prompt,
                              CompletionRuntime::StageTimingStats& stage_ts,
                              httplib::Response& res) {
    const auto t_prompt_start = Clock::now();

    bool has_user_content = false;
    std::string prompt_parse_error;
    prompt = CompletionRuntime::BuildPromptFromMessages(
        request.messages, has_user_content, prompt_parse_error, prompt_template);

    if (!prompt_parse_error.empty()) {
        ServerUtils::SetJsonError(res, 400, prompt_parse_error, "invalid_request_error");
        return false;
    }

    if (!has_user_content) {
        ServerUtils::SetJsonError(
            res,
            400,
            "No user message with non-empty content found. At least one message with role "
            "'user' and non-empty content is required.",
            "invalid_request_error");
        return false;
    }

    const size_t max_prompt_chars = maxPromptChars();
    if (prompt.size() > max_prompt_chars) {
        APP_LOG_WARN() << "prompt length " << prompt.size()
                       << " exceeds TG_MAX_PROMPT_CHARS=" << max_prompt_chars
                       << "; truncating for backend safety";
        prompt = truncatePromptForBackend(prompt, max_prompt_chars);
    }

    stage_ts.prompt_build_ms = Ms(Clock::now() - t_prompt_start).count();
    return true;
}

std::string ResolvePromptTemplate(const httplib::Request& req,
                                  bool allow_debug_template_override,
                                  const std::string& active_model_id,
                                  const std::string& active_model_config_path) {
    const std::string model_default_template =
        ModelCatalog::DefaultPromptTemplateForModel(active_model_config_path, active_model_id);
    if (!allow_debug_template_override) {
        return model_default_template;
    }

    const std::string header_template =
        req.get_header_value("X-Internal-Prompt-Template");
    if (header_template.empty()) {
        return model_default_template;
    }
    return header_template;
}

bool PrimeModelForCompletionWithHeldLock(Genie& genie,
                                         uint32_t max_tokens,
                                         CompletionRuntime::StageTimingStats& stage_ts,
                                         httplib::Response& res) {
    try {
        const auto t_max_tokens_stage = Clock::now();
        genie.setMaxTokens(max_tokens);
        stage_ts.max_tokens_stage_ms = Ms(Clock::now() - t_max_tokens_stage).count();

        const auto t_reset_stage = Clock::now();
        if (!genie.reset()) {
            APP_LOG_ERROR() << "genie.reset() failed";
        }
        stage_ts.reset_stage_ms = Ms(Clock::now() - t_reset_stage).count();
        return true;
    } catch (const std::exception& e) {
        ServerUtils::SetJsonError(
            res, 500, std::string("Model priming failed: ") + e.what(), "server_error");
        return false;
    } catch (...) {
        ServerUtils::SetJsonError(res, 500, "Model priming failed", "server_error");
        return false;
    }
}

bool TryAcquireModelLock(std::unique_lock<std::timed_mutex>& lock,
                         std::chrono::seconds timeout,
                         CompletionRuntime::StageTimingStats& stage_ts,
                         httplib::Response& res) {
    const auto t_lock_wait = Clock::now();
    if (!lock.try_lock_for(timeout)) {
        ModelAccessPolicy::SetRetryAfterHeader(res);
        ServerUtils::SetJsonError(res,
                     503,
                     "Model is busy, try again later",
                     "server_error",
                     "model_busy");
        return false;
    }

    stage_ts.lock_wait_ms = Ms(Clock::now() - t_lock_wait).count();
    return true;
}

void SetTimingHeaders(httplib::Response& res,
                      const CompletionRuntime::TimingStats& timing,
                      const CompletionRuntime::StageTimingStats& stage_ts) {
    auto set_timing_header = [&res](const char* name, double value) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(3) << value;
        res.set_header(name, ss.str());
    };

    set_timing_header("x-time-to-first-token-ms", timing.ttft_ms);
    set_timing_header("x-generation-time-ms", timing.generation_ms);
    set_timing_header("x-total-time-ms", timing.total_ms);
    set_timing_header("x-tokens-per-second", timing.tokens_per_second);
    set_timing_header("x-inter-token-latency-ms", timing.inter_token_latency_ms);
    set_timing_header("x-request-parse-ms", stage_ts.request_parse_ms);
    set_timing_header("x-request-validate-ms", stage_ts.request_validate_ms);
    set_timing_header("x-prompt-build-ms", stage_ts.prompt_build_ms);
    set_timing_header("x-max-tokens-stage-ms", stage_ts.max_tokens_stage_ms);
    set_timing_header("x-reset-stage-ms", stage_ts.reset_stage_ms);
    set_timing_header("x-lock-wait-ms", stage_ts.lock_wait_ms);
    set_timing_header("x-inference-ms", stage_ts.inference_ms);
}

void StartStreamingCompletionResponse(
    Genie& genie,
    std::timed_mutex& model_mutex,
    std::mutex& stream_worker_mutex,
    size_t& active_stream_worker_count,
    std::condition_variable& stream_workers_cv,
    const std::string& prompt,
    const CompletionRequestSpec& request,
    const std::string& completion_id,
    int64_t created,
    const Clock::time_point& request_start,
    CompletionRuntime::StageTimingStats stage_ts,
    httplib::Response& res) {
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("X-Accel-Buffering", "no");

    auto token_queue = std::make_shared<CompletionRuntime::TokenQueue>();
    auto sent_first = std::make_shared<bool>(false);
    auto inference_ms = std::make_shared<double>(0.0);
    auto streamed_text = std::make_shared<std::string>();

    std::unique_lock<std::timed_mutex> lock(model_mutex, std::defer_lock);
    if (!TryAcquireModelLock(lock, ModelAccessPolicy::kBusyWaitTimeout, stage_ts, res)) {
        return;
    }
    if (!PrimeModelForCompletionWithHeldLock(genie, request.max_tokens, stage_ts, res)) {
        return;
    }

    {
        std::lock_guard<std::mutex> worker_lk(stream_worker_mutex);
        ++active_stream_worker_count;
    }

    try {
        std::thread([&genie,
                     prompt,
                     token_queue,
                     inference_ms,
                     &stream_worker_mutex,
                     &active_stream_worker_count,
                     &stream_workers_cv,
                     lock = std::move(lock)]() mutable {
            const auto t_infer_start = Clock::now();
            try {
                genie.queryStream(prompt, [&token_queue](const std::string& token) {
                    if (token_queue->cancelled.load(std::memory_order_relaxed)) return;
                    token_queue->Push(token);
                });
                token_queue->Finish();
            } catch (const std::exception& e) {
                APP_LOG_ERROR() << "queryStream: " << e.what() << " - retry";
                try {
                    genie.reload();
                    genie.queryStream(prompt, [&token_queue](const std::string& token) {
                        if (token_queue->cancelled.load(std::memory_order_relaxed)) return;
                        token_queue->Push(token);
                    });
                    token_queue->Finish();
                } catch (const std::exception& retry_err) {
                    APP_LOG_ERROR() << "retry failed: " << retry_err.what();
                    token_queue->Finish(retry_err.what());
                }
            } catch (...) {
                APP_LOG_ERROR() << "queryStream failed with unknown exception";
                token_queue->Finish("Unknown streaming error");
            }

            *inference_ms = Ms(Clock::now() - t_infer_start).count();

            {
                std::lock_guard<std::mutex> worker_lk(stream_worker_mutex);
                if (active_stream_worker_count > 0) --active_stream_worker_count;
            }
            stream_workers_cv.notify_all();
        }).detach();
    } catch (...) {
        {
            std::lock_guard<std::mutex> worker_lk(stream_worker_mutex);
            if (active_stream_worker_count > 0) --active_stream_worker_count;
        }
        stream_workers_cv.notify_all();

        ServerUtils::SetJsonError(
            res, 500, "Failed to start streaming worker", "server_error");
        return;
    }

    res.set_chunked_content_provider(
        "text/event-stream",
        [token_queue,
         sent_first,
         completion_id,
         created,
         request,
         request_start,
         stage_ts,
         inference_ms,
         streamed_text](size_t, httplib::DataSink& sink) -> bool {
            if (!*sent_first) {
                *sent_first = true;
                const std::string chunk =
                    "data: " + CompletionRuntime::MakeSseChunk(completion_id,
                                                                created,
                                                                request.model_name,
                                                                "",
                                                                true,
                                                                false) +
                    "\n\n";
                if (!sink.write(chunk.c_str(), chunk.size())) {
                    token_queue->Cancel();
                    return false;
                }
                return true;
            }

            auto tok = token_queue->Pop();
            if (!tok.has_value()) {
                const CompletionRuntime::TimingStats timing =
                    CompletionRuntime::ComputeTimingStats(request_start, *token_queue);
                CompletionRuntime::StageTimingStats final_stage = stage_ts;
                final_stage.inference_ms = *inference_ms;

                APP_LOG_INFO() << "stream done tokens=" << timing.completion_tokens
                               << " ttft=" << timing.ttft_ms << "ms"
                               << " tps=" << timing.tokens_per_second;

                std::string final_data =
                    "data: " + CompletionRuntime::MakeSseChunk(completion_id,
                                                                created,
                                                                request.model_name,
                                                                "",
                                                                false,
                                                                true) +
                    "\n\n";

                json usage_chunk;
                usage_chunk["id"] = completion_id;
                usage_chunk["object"] = "chat.completion.chunk";
                usage_chunk["created"] = created;
                usage_chunk["model"] = request.model_name;
                usage_chunk["choices"] = json::array();
                usage_chunk["usage"] = {{"prompt_tokens", 0},
                                        {"completion_tokens", timing.completion_tokens},
                                        {"total_tokens", timing.completion_tokens},
                                        {"prompt_tokens_details", {{"cached_tokens", 0}}},
                                        {"completion_tokens_details",
                                         {{"reasoning_tokens", 0}}}};
                usage_chunk["x_timing"] = CompletionRuntime::TimingToJson(timing);
                CompletionRuntime::AppendStageTiming(usage_chunk["x_timing"],
                                                     final_stage);

                if (!token_queue->error.empty() &&
                    token_queue->error != "client disconnected") {
                    json error_body;
                    error_body["error"]["message"] = token_queue->error;
                    error_body["error"]["type"] = "server_error";
                    final_data += "data: " + error_body.dump() + "\n\n";
                } else {
                    if (request.include_usage) {
                        final_data += "data: " + usage_chunk.dump() + "\n\n";
                    }
                    if (request.do_store) {
                        json stored;
                        stored["id"] = completion_id;
                        stored["object"] = "chat.completion";
                        stored["created"] = created;
                        stored["model"] = request.model_name;
                        stored["choices"] = json::array(
                            {json{{"index", 0},
                                  {"message",
                                   {{"role", "assistant"},
                                    {"content", *streamed_text}}},
                                  {"finish_reason", "stop"}}});
                        stored["usage"] = {{"prompt_tokens", 0},
                                           {"completion_tokens",
                                            timing.completion_tokens},
                                           {"total_tokens",
                                            timing.completion_tokens}};
                        CompletionRuntime::StoreCompletion(std::move(stored));
                    }
                }

                final_data += "data: [DONE]\n\n";
                sink.write(final_data.c_str(), final_data.size());
                sink.done();
                return true;
            }

            const std::string chunk =
                "data: " + CompletionRuntime::MakeSseChunk(completion_id,
                                                            created,
                                                            request.model_name,
                                                            *tok,
                                                            false,
                                                            false) +
                "\n\n";
            *streamed_text += *tok;
            if (!sink.write(chunk.c_str(), chunk.size())) {
                token_queue->Cancel();
                return false;
            }
            return true;
        });
}

void RunNonStreamingCompletion(Genie& genie,
                               std::timed_mutex& model_mutex,
                               const std::string& prompt,
                               const CompletionRequestSpec& request,
                               const std::string& completion_id,
                               int64_t created,
                               const Clock::time_point& request_start,
                               CompletionRuntime::StageTimingStats stage_ts,
                               httplib::Response& res) {
    std::unique_lock<std::timed_mutex> lock(model_mutex, std::defer_lock);
    if (!TryAcquireModelLock(lock, ModelAccessPolicy::kBusyWaitTimeout, stage_ts, res)) {
        return;
    }
    if (!PrimeModelForCompletionWithHeldLock(genie, request.max_tokens, stage_ts, res)) {
        return;
    }

    CompletionRuntime::TokenQueue token_queue;
    const auto t_infer_start = Clock::now();
    try {
        genie.queryStream(
            prompt,
            [&token_queue](const std::string& token) { token_queue.Push(token); });
    } catch (const std::exception& e) {
        APP_LOG_ERROR() << "query failed: " << e.what() << " - retry";
        try {
            genie.reload();
            genie.queryStream(
                prompt,
                [&token_queue](const std::string& token) { token_queue.Push(token); });
        } catch (const std::exception& retry_err) {
            APP_LOG_ERROR() << "retry failed: " << retry_err.what();
            ServerUtils::SetJsonError(res,
                         500,
                         std::string("Generation failed: ") + retry_err.what(),
                         "server_error");
            return;
        }
    }

    stage_ts.inference_ms = Ms(Clock::now() - t_infer_start).count();
    lock.unlock();

    token_queue.Finish();
    std::string answer;
    while (auto tok = token_queue.Pop()) {
        answer += *tok;
    }

    const CompletionRuntime::TimingStats timing =
        CompletionRuntime::ComputeTimingStats(request_start, token_queue);
    APP_LOG_INFO() << "non-stream done tokens=" << timing.completion_tokens
                   << " ttft=" << timing.ttft_ms << "ms"
                   << " tps=" << timing.tokens_per_second;

    SetTimingHeaders(res, timing, stage_ts);

    json response_body;
    response_body["id"] = completion_id;
    response_body["object"] = "chat.completion";
    response_body["created"] = created;
    response_body["model"] = request.model_name;
    response_body["system_fingerprint"] = "genie-v2";
    response_body["choices"] = json::array();
    for (int i = 0; i < request.n_choices; ++i) {
        response_body["choices"].push_back(
            json{{"index", i},
                 {"message", {{"role", "assistant"}, {"content", answer}}},
                 {"finish_reason", "stop"}});
    }

    const uint32_t total_completion_tokens =
        timing.completion_tokens * static_cast<uint32_t>(request.n_choices);
    response_body["usage"] = {{"prompt_tokens", 0},
                              {"completion_tokens", total_completion_tokens},
                              {"total_tokens", total_completion_tokens},
                              {"prompt_tokens_details", {{"cached_tokens", 0}}},
                              {"completion_tokens_details",
                               {{"reasoning_tokens", 0}}}};
    response_body["x_timing"] = CompletionRuntime::TimingToJson(timing);
    CompletionRuntime::AppendStageTiming(response_body["x_timing"], stage_ts);
    if (request.do_store) CompletionRuntime::StoreCompletion(response_body);

    res.set_content(response_body.dump(), "application/json");
}

}  // namespace

void App::CompletionService::HandleChatCompletionPost(
    Genie& genie,
    std::timed_mutex& model_mutex,
    std::mutex& stream_worker_mutex,
    size_t& active_stream_worker_count,
    std::condition_variable& stream_workers_cv,
    const std::string& active_model_id,
    const std::string& active_model_config_path,
    uint32_t max_tokens_default,
    bool allow_debug_template_override,
    const httplib::Request& req,
    httplib::Response& res) {
    const auto request_start = Clock::now();
    CompletionRuntime::StageTimingStats stage_ts;
    APP_LOG_INFO() << "/v1/chat/completions body=" << req.body.size() << "B";

    CompletionRequestSpec request;
    if (!ParseAndValidateCompletionRequest(
            req, active_model_id, max_tokens_default, request, stage_ts, res)) {
        return;
    }

    const std::string prompt_template =
        ResolvePromptTemplate(
            req, allow_debug_template_override, active_model_id, active_model_config_path);
    std::string prompt;
    if (!BuildPromptForCompletion(request, prompt_template, prompt, stage_ts, res)) {
        return;
    }

    APP_LOG_DEBUG() << "prompt length=" << prompt.size()
                    << " max_tokens=" << request.max_tokens
                    << " stream=" << request.do_stream
                    << " prompt_template=" << prompt_template;

    const std::string completion_id = CompletionRuntime::GenerateCompletionId();
    const int64_t created = CompletionRuntime::UnixTimestamp();

    if (request.do_stream) {
        StartStreamingCompletionResponse(genie,
                                         model_mutex,
                                         stream_worker_mutex,
                                         active_stream_worker_count,
                                         stream_workers_cv,
                                         prompt,
                                         request,
                                         completion_id,
                                         created,
                                         request_start,
                                         stage_ts,
                                         res);
        return;
    }

    RunNonStreamingCompletion(genie,
                              model_mutex,
                              prompt,
                              request,
                              completion_id,
                              created,
                              request_start,
                              stage_ts,
                              res);
}
