// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "ChatApp.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "Logger.hpp"
#include "server/ServerUtils.hpp"
#include "server/HttpServer.hpp"
#include "server/ModelCatalog.hpp"

using namespace App;
using json = nlohmann::json;

namespace {
bool ParseBoolEnv(const char* name, bool fallback) {
    const char* raw = std::getenv(name);
    if (!raw) return fallback;
    std::string value = raw;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
    if (value == "0" || value == "false" || value == "no" || value == "off") return false;
    return fallback;
}

int ParseIntEnv(const char* name, int fallback, int min_value, int max_value) {
    const char* raw = std::getenv(name);
    if (!raw) return fallback;

    try {
        const int parsed = std::stoi(raw);
        if (parsed < min_value || parsed > max_value) return fallback;
        return parsed;
    } catch (...) {
        return fallback;
    }
}
} // namespace

void ChatApp::initializeRuntimeConfigFromEnv_() {
    if (const char* bind_host_env = std::getenv("TG_BIND_HOST")) {
        const std::string bind_host_candidate = bind_host_env;
        if (!bind_host_candidate.empty()) {
            bind_host_ = bind_host_candidate;
        }
    }
    bind_port_ = ParseIntEnv("TG_BIND_PORT", 8088, 1, 65535);

    internal_auth_enforced_ = ParseBoolEnv("TG_INTERNAL_AUTH_ENFORCE", false);
    if (const char* internal_api_key_env = std::getenv("TG_INTERNAL_API_KEY")) {
        internal_api_key_ = internal_api_key_env;
    }
    debug_template_override_enabled_ =
        ParseBoolEnv("TG_DEBUG_ALLOW_TEMPLATE_OVERRIDE", false);
}

bool ChatApp::authenticateInternalRequest_(const httplib::Request& req,
                                           httplib::Response& res,
                                           const char* endpoint_name) const {
    if (!internal_auth_enforced_) return true;

    if (internal_api_key_.empty()) {
        ServerUtils::SetJsonError(
            res,
            500,
            "TG_INTERNAL_AUTH_ENFORCE is enabled but TG_INTERNAL_API_KEY is empty",
            "server_error");
        return false;
    }

    std::string provided_key = req.get_header_value("X-Internal-API-Key");
    if (provided_key.empty()) {
        const std::string auth = req.get_header_value("Authorization");
        constexpr const char* kBearer = "Bearer ";
        if (auth.rfind(kBearer, 0) == 0) {
            provided_key = auth.substr(std::char_traits<char>::length(kBearer));
        }
    }

    if (provided_key == internal_api_key_) return true;

    ServerUtils::SetJsonError(res,
                              401,
                              std::string("Unauthorized for endpoint: ") + endpoint_name,
                              "authentication_error");
    return false;
}

bool ChatApp::waitForStreamingWorkersToDrain_(std::chrono::seconds timeout, std::string& error) {
    error.clear();
    std::unique_lock<std::mutex> lk(stream_worker_mutex_);
    if (active_stream_worker_count_ == 0) return true;

    if (!stream_workers_cv_.wait_for(lk, timeout, [this]() {
            return active_stream_worker_count_ == 0;
        })) {
        error = "Timed out while waiting for active streams to finish";
        return false;
    }
    return true;
}

bool ChatApp::isReady_() const {
    if (drain_in_progress_.load(std::memory_order_relaxed)) return false;
    return genie_.isReady();
}

void ChatApp::refreshActiveModelIdFromConfig_() {
    if (const char* override = std::getenv("TG_MODEL_ID_OVERRIDE")) {
        const std::string active_model_id_override = override;
        if (!active_model_id_override.empty()) {
            active_model_id_ = active_model_id_override;
            return;
        }
    }

    active_model_id_ = ModelCatalog::AliasModelIdForConfigPath(
        active_model_config_path_, ModelCatalog::ModelIdFromConfigJson(config_, "genie"));
}

bool ChatApp::reloadModelFromConfigPath_(const std::string& config_path, std::string& error) {
    std::string new_config;
    std::string read_err;
    if (!ServerUtils::ReadFileToString(config_path, new_config, read_err)) {
        error = read_err;
        return false;
    }

    const std::string prev_config = config_;
    const std::string prev_model_config_path = active_model_config_path_;
    const std::string prev_model_id = active_model_id_;
    const std::filesystem::path next_base_dir = std::filesystem::path(config_path).parent_path();
    if (next_base_dir.empty()) {
        error = "Invalid model config path (missing parent dir): " + config_path;
        return false;
    }

    std::error_code prev_cwd_ec;
    const std::filesystem::path prev_cwd = std::filesystem::current_path(prev_cwd_ec);
    const bool prev_cwd_known = !prev_cwd_ec;
    bool cwd_switched = false;

    try {
        std::error_code switch_cwd_ec;
        std::filesystem::current_path(next_base_dir, switch_cwd_ec);
        if (switch_cwd_ec) {
            error = "Failed to switch model base dir to " + next_base_dir.string() + ": " +
                    switch_cwd_ec.message();
            return false;
        }
        cwd_switched = true;

        config_ = new_config;
        active_model_config_path_ = ServerUtils::NormalizePath(config_path);
        refreshActiveModelIdFromConfig_();
        genie_.setConfigJsonContent(config_);
        genie_.initialize();
        genie_.setMaxTokens(max_tokens_default_);
        genie_.reset();
    } catch (const std::exception& e) {
        const std::string primary_err = e.what();

        std::string cwd_rollback_err;
        if (cwd_switched && prev_cwd_known) {
            std::error_code restore_cwd_ec;
            std::filesystem::current_path(prev_cwd, restore_cwd_ec);
            if (restore_cwd_ec) {
                cwd_rollback_err =
                    " | cwd_rollback_failed: " + restore_cwd_ec.message();
            }
        }

        try {
            config_ = prev_config;
            active_model_config_path_ = prev_model_config_path;
            active_model_id_ = prev_model_id;
            genie_.setConfigJsonContent(config_);
            genie_.initialize();
            genie_.setMaxTokens(max_tokens_default_);
            genie_.reset();
        } catch (const std::exception& rollback_err) {
            error = primary_err + " | rollback_failed: " + rollback_err.what() +
                    cwd_rollback_err;
            return false;
        }

        error = primary_err + cwd_rollback_err;
        return false;
    }

    return true;
}

ChatApp::ChatApp(const std::string& config, const std::string& config_path)
    : config_(config),
      genie_(config_, max_tokens_default_),
      active_model_config_path_(ServerUtils::NormalizePath(config_path)) {
    Logger::instance().setFile("textgen_server.txt", true);
    Logger::instance().rotateOnSize(5 * 1024 * 1024, 3);
    Logger::instance().enableConsole(true);
    Logger::instance().setLevel(LogLevel::Debug);
    initializeRuntimeConfigFromEnv_();

    server_created_unix_ = ServerUtils::UnixTimestamp();
    refreshActiveModelIdFromConfig_();
    if (active_model_config_path_.empty()) {
        active_model_config_path_ = "<inline-config>";
    }

    APP_LOG_DEBUG() << "Server bind=" << bind_host_ << ":" << bind_port_
                    << " internal_auth_enforced=" << (internal_auth_enforced_ ? "true" : "false")
                    << " debug_template_override_enabled="
                    << (debug_template_override_enabled_ ? "true" : "false");
    APP_LOG_DEBUG() << "Initializing Genie (id=" << active_model_id_
                    << ", config=" << active_model_config_path_ << ")...";
    genie_.initialize();
    APP_LOG_INFO() << "Genie initialized.";
}

ChatApp::~ChatApp() {
    {
        std::unique_lock<std::mutex> lk(stream_worker_mutex_);
        stream_workers_cv_.wait(lk, [this]() { return active_stream_worker_count_ == 0; });
    }
    genie_.cleanup();
}

void ChatApp::configureErrorHandlers_(httplib::Server& svr) {
    svr.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        if (!res.body.empty()) return;

        if (res.status == 404) {
            ServerUtils::SetJsonError(
                res, 404, "Not found: " + req.path, "invalid_request_error", "not_found");
        } else if (res.status == 405) {
            ServerUtils::SetJsonError(
                res, 405, "Method not allowed", "invalid_request_error", "method_not_allowed");
        } else {
            ServerUtils::SetJsonError(
                res, res.status, "Server error (HTTP " + std::to_string(res.status) + ")", "server_error");
        }
    });

    svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string msg = "Internal server error";
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            msg = e.what();
        } catch (...) {
        }

        ServerUtils::SetJsonError(res, 500, msg, "server_error");
    });
}

void ChatApp::ChatLoop() {
    httplib::Server svr;

    configureErrorHandlers_(svr);
    registerPublicModelRoutes_(svr);
    registerModelManagementRoutes_(svr);
    registerHealthRoutes_(svr);
    registerChatCompletionRoutes_(svr);
    registerLegacyCompatibilityRoutes_(svr);

    APP_LOG_INFO() << "Server listening on " << bind_host_ << ":" << bind_port_;
    svr.listen(bind_host_, bind_port_);
}
