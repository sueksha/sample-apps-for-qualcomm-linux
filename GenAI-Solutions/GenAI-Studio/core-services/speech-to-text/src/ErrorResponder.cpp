// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "ErrorResponder.hpp"

#include "httplib.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>

using json = nlohmann::json;

namespace asr::errors {
namespace {
constexpr const char* kBusyRetryAfterSeconds = "2";

std::string trimCopy(std::string s) {
    const auto is_not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), is_not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), is_not_space).base(), s.end());
    return s;
}

std::string makeRequestId() {
    static std::atomic<uint64_t> counter{0};
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "req_" + std::to_string(now_ms) + "_" + std::to_string(counter.fetch_add(1));
}

std::string defaultErrorTypeForStatus(int status) {
    return (status >= 400 && status < 500) ? "invalid_request_error" : "server_error";
}

std::string defaultErrorCodeForStatus(int status) {
    switch (status) {
        case 400: return "invalid_request";
        case 404: return "not_found";
        case 405: return "method_not_allowed";
        case 410: return "gone";
        case 413: return "payload_too_large";
        case 429: return "rate_limited";
        case 500: return "internal_error";
        case 503: return "service_unavailable";
        case 504: return "upstream_timeout";
        default:
            return (status >= 500) ? "server_error" : "invalid_request";
    }
}

bool defaultRetryableForStatus(int status) {
    return status == 429 || status == 503 || status == 504;
}

std::string defaultErrorMessageForStatus(int status) {
    switch (status) {
        case 400: return "Invalid request";
        case 404: return "Not found";
        case 405: return "Method not allowed";
        case 410: return "Endpoint is gone";
        case 413: return "Payload too large";
        case 429: return "Too many requests";
        case 500: return "Internal server error";
        case 503: return "Service temporarily unavailable";
        case 504: return "Upstream timed out";
        default: return "Request failed";
    }
}

struct ParsedErrorFields {
    std::string message;
    std::string type;
    std::string code;
    std::optional<bool> retryable;
};

ParsedErrorFields parseErrorFieldsFromBody(const std::string& body) {
    ParsedErrorFields out;
    if (body.empty()) return out;

    try {
        const json parsed = json::parse(body);
        if (parsed.contains("error") && parsed["error"].is_object()) {
            const auto& err = parsed["error"];
            if (err.contains("message") && err["message"].is_string()) {
                out.message = err["message"].get<std::string>();
            }
            if (err.contains("type") && err["type"].is_string()) {
                out.type = err["type"].get<std::string>();
            }
            if (err.contains("code") && err["code"].is_string()) {
                out.code = err["code"].get<std::string>();
            }
            if (err.contains("retryable") && err["retryable"].is_boolean()) {
                out.retryable = err["retryable"].get<bool>();
            }
        }
        if (out.message.empty() && parsed.contains("message") && parsed["message"].is_string()) {
            out.message = parsed["message"].get<std::string>();
        }
        if (out.message.empty() && parsed.contains("answer") && parsed["answer"].is_string()) {
            out.message = parsed["answer"].get<std::string>();
        }
        if (out.message.empty() && parsed.contains("error") && parsed["error"].is_string()) {
            out.message = parsed["error"].get<std::string>();
        }
    } catch (...) {
        // Ignore parse errors and fall back to defaults.
    }

    return out;
}

void setCanonicalErrorResponse(httplib::Response& res,
                               int status,
                               const std::string& message,
                               const std::string& type,
                               const std::string& code,
                               const std::string& request_id,
                               bool retryable) {
    const std::string payload = json{{"error", {
        {"message", message},
        {"type", type},
        {"code", code},
        {"request_id", request_id},
        {"retryable", retryable}
    }}}.dump();

    res.status = status;
    res.body = payload;
    res.headers.erase("Content-Type");
    res.headers.erase("Content-Length");
    res.set_header("Content-Type", "application/json");
    res.set_header("Content-Length", std::to_string(payload.size()));
    if (status == kBusyHttpStatus &&
        trimCopy(res.get_header_value("Retry-After")).empty()) {
        res.set_header("Retry-After", kBusyRetryAfterSeconds);
    }
}

}  // namespace

std::string ensureRequestId(const httplib::Request& req,
                            httplib::Response& res) {
    const std::string existing_id = trimCopy(res.get_header_value("X-Request-Id"));
    if (!existing_id.empty()) return existing_id;

    std::string request_id = trimCopy(req.get_header_value("X-Request-Id"));
    if (request_id.empty()) request_id = makeRequestId();
    res.set_header("X-Request-Id", request_id);
    return request_id;
}

void setCanonicalErrorForRequest(const httplib::Request& req,
                                 httplib::Response& res,
                                 int status,
                                 const std::string& message,
                                 const std::string& type,
                                 const std::string& code,
                                 std::optional<bool> retryable) {
    const std::string request_id = ensureRequestId(req, res);
    setCanonicalErrorResponse(
        res,
        status,
        message,
        type.empty() ? defaultErrorTypeForStatus(status) : type,
        code.empty() ? defaultErrorCodeForStatus(status) : code,
        request_id,
        retryable.value_or(defaultRetryableForStatus(status)));
}

void setBusyRateLimitedError(const httplib::Request& req,
                             httplib::Response& res,
                             const std::string& message) {
    setCanonicalErrorForRequest(
        req,
        res,
        kBusyHttpStatus,
        message.empty() ? "Model is busy. Try again later." : message,
        "server_error",
        "rate_limited",
        true);
}

void normalizeErrorResponse(const httplib::Request& req,
                            httplib::Response& res) {
    if (res.status < 400) return;

    const ParsedErrorFields parsed = parseErrorFieldsFromBody(res.body);
    const std::string request_id = ensureRequestId(req, res);
    const std::string message = parsed.message.empty()
        ? defaultErrorMessageForStatus(res.status) : parsed.message;
    const std::string type = parsed.type.empty()
        ? defaultErrorTypeForStatus(res.status) : parsed.type;
    const std::string code = parsed.code.empty()
        ? defaultErrorCodeForStatus(res.status) : parsed.code;
    const bool retryable = parsed.retryable.value_or(defaultRetryableForStatus(res.status));

    setCanonicalErrorResponse(
        res, res.status, message, type, code, request_id, retryable);
}

}  // namespace asr::errors
