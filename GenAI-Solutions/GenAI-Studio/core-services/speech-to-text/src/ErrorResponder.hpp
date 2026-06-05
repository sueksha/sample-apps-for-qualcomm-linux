// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <optional>
#include <string>

namespace httplib {
struct Request;
struct Response;
}

namespace asr::errors {

constexpr int kBusyHttpStatus = 429;

std::string ensureRequestId(const httplib::Request& req, httplib::Response& res);

void setCanonicalErrorForRequest(const httplib::Request& req,
                                 httplib::Response& res,
                                 int status,
                                 const std::string& message,
                                 const std::string& type = "",
                                 const std::string& code = "",
                                 std::optional<bool> retryable = std::nullopt);

void setBusyRateLimitedError(const httplib::Request& req,
                             httplib::Response& res,
                             const std::string& message = "Model is busy. Try again later.");

void normalizeErrorResponse(const httplib::Request& req, httplib::Response& res);

}  // namespace asr::errors
