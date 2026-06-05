// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------

#include "AsrService.hpp"
#include "ErrorResponder.hpp"

#define CPPHTTPLIB_THREAD_POOL_COUNT 4
#include "httplib.h"
#include <nlohmann/json.hpp>

#include <exception>

using json = nlohmann::json;

namespace {
constexpr const char* kLegacySuccessorEndpoint = "/v1/audio/transcriptions";

bool isLegacyEndpointPath(const std::string& path) {
    return path == "/generate" || path == "/transcribe" || path == "/transcribe/raw";
}

void applyLegacyDeprecationHeaders(const AsrRuntimeConfig& config,
                                   httplib::Response& res) {
    if (!config.legacy_deprecation_enabled) return;

    res.set_header("Deprecation", "true");
    if (!config.legacy_sunset_rfc1123.empty()) {
        res.set_header("Sunset", config.legacy_sunset_rfc1123);
    }

    std::string link_header =
        std::string("<") + kLegacySuccessorEndpoint + ">; rel=\"successor-version\"";
    if (!config.legacy_migration_doc_url.empty()) {
        link_header += ", <" + config.legacy_migration_doc_url + ">; rel=\"deprecation\"";
    }
    res.set_header("Link", link_header);
}
}  // namespace

void AsrService::configureServerPolicies(httplib::Server& svr) {
    svr.set_post_routing_handler([this](const httplib::Request& req,
                                        httplib::Response& res) {
        counters_.requests_total.fetch_add(1, std::memory_order_relaxed);
        if (res.status >= 400) {
            counters_.requests_error.fetch_add(1, std::memory_order_relaxed);
        }
        asr::errors::ensureRequestId(req, res);
        if (isLegacyEndpointPath(req.path)) {
            applyLegacyDeprecationHeaders(config_, res);
        }
        asr::errors::normalizeErrorResponse(req, res);
    });

    svr.set_error_handler([](const httplib::Request& req,
                             httplib::Response& res) {
        if (!res.body.empty()) return;
        json err;
        if (res.status == 404) {
            err["error"]["message"] = "Not found: " + req.path;
            err["error"]["type"]    = "invalid_request_error";
            err["error"]["code"]    = "not_found";
        } else if (res.status == 405) {
            err["error"]["message"] = "Method not allowed";
            err["error"]["type"]    = "invalid_request_error";
            err["error"]["code"]    = "method_not_allowed";
        } else {
            err["error"]["message"] = "Server error (HTTP " +
                                      std::to_string(res.status) + ")";
            err["error"]["type"]    = "server_error";
        }
        res.set_content(err.dump(), "application/json");
    });

    svr.set_exception_handler([](const httplib::Request& /*req*/,
                                 httplib::Response& res,
                                 std::exception_ptr ep) {
        std::string msg = "Internal server error";
        try { if (ep) std::rethrow_exception(ep); }
        catch (const std::exception& e) { msg = e.what(); }
        catch (...) {}
        res.status = 500;
        res.set_content(
            json{{"error", {{"message", msg}, {"type", "server_error"}}}}.dump(),
            "application/json");
    });
}

void AsrService::registerMethodNotAllowedRoutes(httplib::Server& svr) {
    auto mna = [](const httplib::Request&, httplib::Response& res) {
        res.status = 405;
        res.set_content(
            json{{"error", {{"message", "Method not allowed. "
                                        "This endpoint only accepts POST."},
                            {"type",    "invalid_request_error"},
                            {"code",    "method_not_allowed"}}}}.dump(),
            "application/json");
    };
    svr.Get   ("/v1/audio/transcriptions", mna);
    svr.Put   ("/v1/audio/transcriptions", mna);
    svr.Delete("/v1/audio/transcriptions", mna);
    svr.Get   ("/v1/audio/translations",   mna);
    svr.Put   ("/v1/audio/translations",   mna);
    svr.Delete("/v1/audio/translations",   mna);
    svr.Get   ("/generate",       mna);
    svr.Get   ("/transcribe",     mna);
    svr.Get   ("/transcribe/raw", mna);
}
