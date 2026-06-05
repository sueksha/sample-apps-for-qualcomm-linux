// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include "StableDiffusionEngine.hpp"

namespace httplib {
class Request;
class Response;
class Server;
}

// ---------------------------------------------------------------------------
// ImageGenService
//
// Crow-based HTTP server exposing the Stable Diffusion engine.
//
// OpenAI-compatible endpoints:
//   POST /v1/images/generations  – OpenAI image generation API
//   GET  /v1/models              – lists stable-diffusion-v1-5
//
// Additional endpoints:
//   POST /generate               – simple JSON body {"prompt":"..."}
//   GET  /health                 – liveness check
//
// POST /v1/images/generations request body (JSON):
//   {
//     "model":           "stable-diffusion-v1-5",  // accepted, ignored
//     "prompt":          "A cat on a beach",        // REQUIRED
//     "negative_prompt": "...",                     // optional
//     "n":               1,                         // only 1 supported
//     "size":            "512x512",                 // only 512x512
//     "response_format": "b64_json",                // b64_json | url
//     "seed":            42,                        // optional
//     "steps":           20,                        // 1-50
//     "guidance_scale":  7.5                        // 1.0-20.0
//   }
//
// Response (b64_json):
//   {
//     "created": 1234567890,
//     "data": [{ "b64_json": "<base64-encoded PNG>" }]
//   }
// ---------------------------------------------------------------------------
class ImageGenService {
public:
    ImageGenService(const std::string& model_dir,
                    const std::string& tokenizer_dir,
                    uint16_t port,
                    std::string api_key,
                    size_t cache_size);
    void run();

private:
    struct CacheValue {
        std::vector<uint8_t> png_bytes;
        int64_t elapsed_ms = 0;
        GenerationTiming timing;
        std::list<std::string>::iterator lru_it;
    };

    StableDiffusionEngine engine_;
    uint16_t              port_;
    std::string           api_key_;
    size_t                cache_size_ = 8;
    std::once_flag        init_once_;
    std::mutex            cache_mutex_;
    std::list<std::string> cache_lru_;
    std::unordered_map<std::string, CacheValue> cache_map_;

    // Base64 encode bytes → string
    static std::string base64Encode(const uint8_t* data, size_t len);
    static std::string makeCacheKey(const std::string& model_id,
                                    const GenerationRequest& req);
    bool cacheGet(const std::string& key, std::vector<uint8_t>& png, int64_t& elapsed_ms, GenerationTiming& timing);
    void cachePut(const std::string& key, const std::vector<uint8_t>& png, int64_t elapsed_ms, const GenerationTiming& timing);
    bool ensureReady(httplib::Response& res);
    bool checkAuth(const httplib::Request& req, httplib::Response& res) const;
    bool tryReleaseI2tSession() const;
    GenerationResult generateWithRecovery(const GenerationRequest& req);
    bool preflightRequest(const httplib::Request& req,
                         httplib::Response& res,
                         bool require_ready);
    void registerRoutes(httplib::Server& server);
    void registerCoreGetRoutes(httplib::Server& server);
    void registerImageGetRoutes(httplib::Server& server);
    void registerPostRoutes(httplib::Server& server);
    void handleHealth(const httplib::Request& req, httplib::Response& res);
    void handleModels(const httplib::Request& req, httplib::Response& res);
    void handleModelById(const httplib::Request& req, httplib::Response& res);
    void handleImageFile(const httplib::Request& req, httplib::Response& res);
    void handleGenerationParams(const httplib::Request& req, httplib::Response& res);
    void handleLegacyGenerate(const httplib::Request& req, httplib::Response& res);
    void handleV1Generations(const httplib::Request& req, httplib::Response& res);
    void handleMultipartImage(const httplib::Request& req,
                              httplib::Response& res,
                              bool require_prompt,
                              const std::string& default_prompt,
                              const std::string& task_name);
    void handleImageEdits(const httplib::Request& req, httplib::Response& res);
    void handleImageVariations(const httplib::Request& req, httplib::Response& res);
};
