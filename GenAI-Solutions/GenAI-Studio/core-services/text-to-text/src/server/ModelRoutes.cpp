// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "ChatApp.hpp"

#include <chrono>

#include <nlohmann/json.hpp>

#include "server/ServerUtils.hpp"
#include "server/HttpServer.hpp"
#include "server/ModelAccessPolicy.hpp"
#include "server/ModelCatalog.hpp"

using namespace App;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

void ChatApp::registerPublicModelRoutes_(httplib::Server& svr) {
    svr.Get("/v1/models", [this](const httplib::Request&, httplib::Response& res) {
        json obj;
        obj["id"] = active_model_id_;
        obj["object"] = "model";
        obj["created"] = server_created_unix_;
        obj["owned_by"] = "qualcomm";
        obj["permission"] = json::array();
        obj["root"] = active_model_id_;
        obj["parent"] = nullptr;

        json body;
        body["object"] = "list";
        body["data"] = json::array({obj});
        res.set_content(body.dump(), "application/json");
    });

    svr.Get(R"(/v1/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string requested = req.matches[1];
        if (requested != active_model_id_) {
            ServerUtils::SetJsonError(res,
                                      404,
                                      "Model not found: " + requested,
                                      "invalid_request_error",
                                      "model_not_found");
            return;
        }

        json obj;
        obj["id"] = active_model_id_;
        obj["object"] = "model";
        obj["created"] = server_created_unix_;
        obj["owned_by"] = "qualcomm";
        obj["permission"] = json::array();
        obj["root"] = active_model_id_;
        obj["parent"] = nullptr;
        res.set_content(obj.dump(), "application/json");
    });
}

void ChatApp::registerModelManagementRoutes_(httplib::Server& svr) {
    svr.Get("/v1/internal/models", [this](const httplib::Request& req, httplib::Response& res) {
        if (!authenticateInternalRequest_(req, res, "/v1/internal/models")) return;

        const auto models = ModelCatalog::DiscoverLocalModels(active_model_config_path_, active_model_id_);
        json data = json::array();
        for (const auto& model : models) {
            data.push_back({{"key", model.key},
                            {"id", model.model_id},
                            {"model_dir", model.model_dir},
                            {"genie_config_path", model.config_path},
                            {"active", model.active}});
        }

        json body = {{"object", "list"},
                     {"active_model_id", active_model_id_},
                     {"active_genie_config_path", active_model_config_path_},
                     {"data", data}};
        res.set_content(body.dump(), "application/json");
    });

    svr.Post("/v1/internal/models/load", [this](const httplib::Request& req, httplib::Response& res) {
        if (!authenticateInternalRequest_(req, res, "/v1/internal/models/load")) return;

        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            ServerUtils::SetJsonError(res, 400, "Invalid JSON", "invalid_request_error");
            return;
        }

        std::string config_path;
        std::string model_key;
        std::string resolve_err;
        if (!ModelCatalog::ResolveModelConfigPath(body, config_path, model_key, resolve_err)) {
            ServerUtils::SetJsonError(res, 400, resolve_err, "invalid_request_error");
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

        std::string reload_err;
        if (!reloadModelFromConfigPath_(config_path, reload_err)) {
            ServerUtils::SetJsonError(
                res, 500, "Model load failed: " + reload_err, "server_error");
            return;
        }

        json out = {{"status", "success"},
                    {"active_model_id", active_model_id_},
                    {"active_genie_config_path", active_model_config_path_}};
        if (!model_key.empty()) out["model"] = model_key;
        res.set_content(out.dump(), "application/json");
    });
}

void ChatApp::registerHealthRoutes_(httplib::Server& svr) {
    svr.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
        const auto uptime =
            std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - server_start_time_)
                .count();
        json body;
        body["status"] = "ok";
        body["model"] = active_model_id_;
        body["uptime_seconds"] = uptime;
        body["version"] = "2.0.0";
        res.set_content(body.dump(), "application/json");
    });

    svr.Get("/ready", [this](const httplib::Request&, httplib::Response& res) {
        const bool ready = isReady_();
        const auto uptime =
            std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - server_start_time_)
                .count();

        json body;
        body["status"] = ready ? "ready" : "not_ready";
        body["model"] = active_model_id_;
        body["uptime_seconds"] = uptime;
        body["drain_in_progress"] = drain_in_progress_.load(std::memory_order_relaxed);
        body["version"] = "2.0.0";

        if (!ready) {
            res.status = 503;
            body["reason"] = "Model unavailable or maintenance in progress";
        }
        res.set_content(body.dump(), "application/json");
    });
}
