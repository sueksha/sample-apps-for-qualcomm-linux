// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "ChatApp.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "server/CompletionRuntime.hpp"
#include "server/CompletionService.hpp"
#include "server/HttpServer.hpp"
#include "server/ServerUtils.hpp"

using namespace App;
using json = nlohmann::json;

void ChatApp::handleChatCompletionPost_(const httplib::Request& req,
                                        httplib::Response& res) {
    CompletionService::HandleChatCompletionPost(genie_,
                                                model_mutex_,
                                                stream_worker_mutex_,
                                                active_stream_worker_count_,
                                                stream_workers_cv_,
                                                active_model_id_,
                                                active_model_config_path_,
                                                max_tokens_default_,
                                                debug_template_override_enabled_,
                                                req,
                                                res);
}

void ChatApp::registerChatCompletionRoutes_(httplib::Server& svr) {
    svr.Get("/v1/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
        if (!authenticateInternalRequest_(req, res, "/v1/chat/completions")) return;

        auto data = CompletionRuntime::SnapshotStoredCompletions();
        if (req.has_param("model")) {
            const std::string model_filter = req.get_param_value("model");
            data.erase(
                std::remove_if(
                    data.begin(),
                    data.end(),
                    [&model_filter](const json& item) {
                        return ServerUtils::SafeStr(item, "model", "") != model_filter;
                    }),
                data.end());
        }

        const std::string order = req.has_param("order") ? req.get_param_value("order")
                                                          : "asc";
        if (order == "desc") std::reverse(data.begin(), data.end());

        size_t start = 0;
        if (req.has_param("after")) {
            const std::string after = req.get_param_value("after");
            auto it = std::find_if(data.begin(), data.end(), [&after](const json& item) {
                return ServerUtils::SafeStr(item, "id", "") == after;
            });
            if (it != data.end()) start = static_cast<size_t>((it - data.begin()) + 1);
        }

        int limit = 20;
        if (req.has_param("limit")) {
            try {
                limit = std::stoi(req.get_param_value("limit"));
            } catch (...) {
                limit = 20;
            }
        }
        if (limit < 1) limit = 1;
        if (limit > 100) limit = 100;

        json page = json::array();
        for (size_t i = start;
             i < data.size() && static_cast<int>(page.size()) < limit;
             ++i) {
            page.push_back(data[i]);
        }

        json body;
        body["object"] = "list";
        body["data"] = page;
        if (page.empty()) {
            body["first_id"] = nullptr;
            body["last_id"] = nullptr;
        } else {
            body["first_id"] = ServerUtils::SafeStr(page.front(), "id", "");
            body["last_id"] = ServerUtils::SafeStr(page.back(), "id", "");
        }
        body["has_more"] = (start + page.size() < data.size());
        res.set_content(body.dump(), "application/json");
    });

    svr.Get(R"(/v1/chat/completions/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!authenticateInternalRequest_(req, res, "/v1/chat/completions/{id}")) return;

                const std::string completion_id = req.matches[1];
                json out;
                if (!CompletionRuntime::GetStoredCompletion(completion_id, out)) {
                    ServerUtils::SetJsonError(res,
                                              404,
                                              "Chat completion not found: " + completion_id,
                                              "invalid_request_error",
                                              "not_found");
                    return;
                }
                res.set_content(out.dump(), "application/json");
            });

    svr.Delete(R"(/v1/chat/completions/([^/]+))",
               [this](const httplib::Request& req, httplib::Response& res) {
                   if (!authenticateInternalRequest_(req, res, "/v1/chat/completions/{id}")) return;

                   const std::string completion_id = req.matches[1];
                   if (!CompletionRuntime::DeleteStoredCompletion(completion_id)) {
                       ServerUtils::SetJsonError(res,
                                                 404,
                                                 "Chat completion not found: " + completion_id,
                                                 "invalid_request_error",
                                                 "not_found");
                       return;
                   }

                   res.status = 200;
                   res.set_content(
                       json{{"id", completion_id},
                            {"object", "chat.completion.deleted"},
                            {"deleted", true}}
                           .dump(),
                       "application/json");
               });

    svr.Get(R"(/v1/chat/completions/(.+)/messages)",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!authenticateInternalRequest_(req, res, "/v1/chat/completions/{id}/messages")) return;

                const std::string completion_id = req.matches[1];
                json stored;
                if (!CompletionRuntime::GetStoredCompletion(completion_id, stored)) {
                    ServerUtils::SetJsonError(res,
                                              404,
                                              "Chat completion not found: " + completion_id,
                                              "invalid_request_error",
                                              "not_found");
                    return;
                }

                json body;
                body["object"] = "list";
                body["data"] = json::array();
                const auto& choices = stored.value("choices", json::array());
                for (const auto& choice : choices) {
                    json msg;
                    msg["id"] = ServerUtils::SafeStr(stored, "id", "");
                    msg["object"] = "chat.completion.message";
                    msg["created"] = stored.value("created", 0);
                    msg["role"] = "assistant";
                    msg["content"] =
                        choice.value("message", json::object()).value("content", "");
                    body["data"].push_back(std::move(msg));
                }

                if (body["data"].empty()) {
                    body["first_id"] = nullptr;
                    body["last_id"] = nullptr;
                } else {
                    body["first_id"] =
                        ServerUtils::SafeStr(body["data"].front(), "id", "");
                    body["last_id"] =
                        ServerUtils::SafeStr(body["data"].back(), "id", "");
                }
                body["has_more"] = false;
                res.set_content(body.dump(), "application/json");
            });

    {
        auto method_not_allowed = [this](const httplib::Request& req, httplib::Response& res) {
            if (!authenticateInternalRequest_(req, res, "/v1/chat/completions")) return;

            ServerUtils::SetJsonError(res,
                                      405,
                                      "Method not allowed. This endpoint only accepts POST.",
                                      "invalid_request_error",
                                      "method_not_allowed");
        };
        svr.Put("/v1/chat/completions", method_not_allowed);
        svr.Delete("/v1/chat/completions", method_not_allowed);
        svr.Patch("/v1/chat/completions", method_not_allowed);
    }

    svr.Post("/v1/chat/completions",
             [this](const httplib::Request& req, httplib::Response& res) {
                 handleChatCompletionPost_(req, res);
             });
}
