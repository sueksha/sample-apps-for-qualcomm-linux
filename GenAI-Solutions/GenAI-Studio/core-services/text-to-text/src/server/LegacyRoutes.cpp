// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "ChatApp.hpp"

#include <chrono>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "server/ServerUtils.hpp"
#include "server/HttpServer.hpp"
#include "server/ModelAccessPolicy.hpp"
#include "server/ModelCatalog.hpp"

using namespace App;
using json = nlohmann::json;

void ChatApp::registerLegacyCompatibilityRoutes_(httplib::Server& svr) {
    svr.Post("/reset_model", [this](const httplib::Request& req, httplib::Response& res) {
        if (!authenticateInternalRequest_(req, res, "/reset_model")) return;

        std::unique_lock<std::timed_mutex> lk(model_mutex_, std::defer_lock);
        if (!lk.try_lock_for(ModelAccessPolicy::kBusyWaitTimeout)) {
            ModelAccessPolicy::SetRetryAfterHeader(res);
            ServerUtils::SetJsonError(
                res, 503, "Model is busy", "server_error", "model_busy");
            return;
        }

        const bool ok = genie_.reset();
        json response;
        response["answer"] = ok ? "Model reset successful" : "Model reset unsuccessful";
        response["status"] = ok ? "success" : "failure";
        res.set_content(response.dump(), "application/json");
    });

    svr.Post("/reload_model", [this](const httplib::Request& req, httplib::Response& res) {
        if (!authenticateInternalRequest_(req, res, "/reload_model")) return;

        json response;
        try {
            json body;
            try {
                body = json::parse(req.body);
            } catch (const json::parse_error& e) {
                ServerUtils::SetJsonError(res,
                                          400,
                                          std::string("Invalid JSON: ") + e.what(),
                                          "invalid_request_error");
                return;
            }

            const std::string system_prompt =
                ServerUtils::SanitizeContent(ServerUtils::SafeStr(body, "system_prompt", ""));
            const std::string sampler_block =
                ServerUtils::SanitizeContent(ServerUtils::SafeStr(body, "sampler_block", ""));
            const int max_tokens = ServerUtils::SafeInt(body, "max_tokens", -1);

            const bool has_model_switch_fields = body.contains("genie_config_path") ||
                                                 body.contains("model_config_path") ||
                                                 body.contains("config_path") ||
                                                 body.contains("model_dir") || body.contains("model") ||
                                                 body.contains("model_name");

            std::string config_path;
            std::string model_key;
            std::string resolve_err;
            if (has_model_switch_fields &&
                !ModelCatalog::ResolveModelConfigPath(body, config_path, model_key, resolve_err)) {
                ServerUtils::SetJsonError(
                    res, 400, resolve_err, "invalid_request_error");
                return;
            }

            ModelAccessPolicy::DrainFlagGuard drain_guard{drain_in_progress_};

            std::string drain_err;
            if (!waitForStreamingWorkersToDrain_(ModelAccessPolicy::kBusyWaitTimeout, drain_err)) {
                ModelAccessPolicy::SetRetryAfterHeader(res);
                ServerUtils::SetJsonError(res,
                                          503,
                                          "Model is busy: " + drain_err,
                                          "server_error",
                                          "model_busy");
                return;
            }

            std::unique_lock<std::timed_mutex> lk(model_mutex_, std::defer_lock);
            if (!lk.try_lock_for(ModelAccessPolicy::kBusyWaitTimeout)) {
                ModelAccessPolicy::SetRetryAfterHeader(res);
                ServerUtils::SetJsonError(
                    res, 503, "Model is busy", "server_error", "model_busy");
                return;
            }

            if (!config_path.empty()) {
                std::string load_err;
                if (!reloadModelFromConfigPath_(config_path, load_err)) {
                    ServerUtils::SetJsonError(
                        res, 500, "Reload failed: " + load_err, "server_error");
                    return;
                }
            }

            if (max_tokens > 0 && max_tokens <= 65536) {
                max_tokens_default_ = static_cast<uint32_t>(max_tokens);
                genie_.setMaxTokens(max_tokens_default_);
            }

            if (!sampler_block.empty()) {
                genie_.applySamplerConfig(sampler_block);
            }

            genie_.reset();

            response["answer"] = "Model reload successful";
            response["status"] = "success";
            response["active_model_id"] = active_model_id_;
            response["active_genie_config_path"] = active_model_config_path_;
            if (!model_key.empty()) response["model"] = model_key;
        } catch (const std::exception& e) {
            ServerUtils::SetJsonError(res,
                                      500,
                                      std::string("Reload failed: ") + e.what(),
                                      "server_error");
            return;
        }

        res.set_content(response.dump(), "application/json");
    });
}
