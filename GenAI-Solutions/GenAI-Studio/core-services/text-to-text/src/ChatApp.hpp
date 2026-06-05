// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <string>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <atomic>

#include "Genie.hpp"

namespace httplib {
class Server;
class Request;
class Response;
}

namespace App
{
constexpr const std::string_view c_exit_prompt = "exit";

class ChatApp
{
  private:
    std::string config_;
    std::string user_name_;

    uint32_t max_tokens_default_{200};

    Genie genie_;

    // std::timed_mutex allows try_lock_for() so HTTP handlers can return
    // HTTP 503 instead of blocking forever when the model is busy.
    std::timed_mutex model_mutex_;

    // Tracks detached streaming workers so destruction can wait for completion.
    std::mutex stream_worker_mutex_;
    std::condition_variable stream_workers_cv_;
    size_t active_stream_worker_count_{0};
    std::atomic<bool> drain_in_progress_{false};

    std::chrono::steady_clock::time_point server_start_time_{
        std::chrono::steady_clock::now()};
    int64_t     server_created_unix_{0};
    std::string active_model_id_{"genie"};
    std::string active_model_config_path_;
    std::string bind_host_{"0.0.0.0"};
    int bind_port_{8088};
    bool internal_auth_enforced_{false};
    std::string internal_api_key_;
    bool debug_template_override_enabled_{false};

    void initializeRuntimeConfigFromEnv_();
    bool authenticateInternalRequest_(const httplib::Request& req,
                                      httplib::Response& res,
                                      const char* endpoint_name) const;
    bool waitForStreamingWorkersToDrain_(std::chrono::seconds timeout, std::string& error);
    bool isReady_() const;
    void refreshActiveModelIdFromConfig_();
    bool reloadModelFromConfigPath_(const std::string& config_path, std::string& error);
    void configureErrorHandlers_(httplib::Server& svr);
    void registerPublicModelRoutes_(httplib::Server& svr);
    void registerModelManagementRoutes_(httplib::Server& svr);
    void registerHealthRoutes_(httplib::Server& svr);
    void registerChatCompletionRoutes_(httplib::Server& svr);
    void handleChatCompletionPost_(const httplib::Request& req, httplib::Response& res);
    void registerLegacyCompatibilityRoutes_(httplib::Server& svr);

  public:
    ChatApp(const std::string& config, const std::string& config_path = "");
    ChatApp() = delete;
    ChatApp(const ChatApp&) = delete;
    ChatApp(ChatApp&&) = delete;
    ChatApp& operator=(const ChatApp&) = delete;
    ChatApp& operator=(ChatApp&&) = delete;
    ~ChatApp();

    void ChatLoop();
};
} // namespace App
