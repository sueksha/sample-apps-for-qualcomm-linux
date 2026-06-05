// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "ImageGenService.hpp"

#include "crow.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <thread>
#include <unistd.h>
#include <functional>
#include <unordered_map>

using json = nlohmann::json;

namespace {
constexpr const char* kCanonicalModel = "stable-diffusion-2-1";
constexpr int kDefaultN = 1;
constexpr int kMaxN = 10;
constexpr int kMaxPromptChars = 32000;
constexpr size_t kMaxPayloadBytes = 64ULL * 1024 * 1024;
const std::chrono::minutes kImageUrlTtl{60};
std::mutex g_image_store_mu;
std::unordered_map<std::string,
    std::pair<std::vector<uint8_t>, std::chrono::steady_clock::time_point>> g_image_store;
std::atomic<uint64_t> g_image_counter{0};

double round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

json errorJson(const std::string& msg, const std::string& type = "server_error") {
    return json{{"error", {{"message", msg}, {"type", type}, {"code", type}}}};
}

json generationTimingJson(const GenerationTiming& t) {
    return json{
        {"tokenize_ms",    t.tokenize_ms},
        {"text_encode_ms", t.text_encode_ms},
        {"latent_init_ms", t.latent_init_ms},
        {"denoise_ms",     t.denoise_ms},
        {"unet_ms",        t.unet_ms},
        {"vae_decode_ms",  t.vae_decode_ms},
        {"png_encode_ms",  t.png_encode_ms},
        {"runner_exec_ms", t.runner_exec_ms},
        {"ppm_read_ms",    t.ppm_read_ms},
        {"total_ms",       t.total_ms}
    };
}

std::string canonicalizeModelId(const std::string& model_id) {
    if (model_id.empty()) return kCanonicalModel;
    if (model_id == kCanonicalModel || model_id == "sd2.1" ||
        model_id == "stable-diffusion-v2-1" ||
        model_id == "stable-diffusion-v1-5" ||
        model_id == "stable-diffusion-v1.5" ||
        model_id == "dall-e-2" ||
        model_id == "dall-e-3" ||
        model_id == "gpt-image-1" ||
        model_id == "gpt-image-1-mini" ||
        model_id == "gpt-image-1.5") {
        return kCanonicalModel;
    }
    return {};
}

std::string parseFormField(const httplib::Request& req,
                           const std::string& key,
                           const std::string& def = "") {
    if (req.has_file(key)) {
        const auto& v = req.get_file_value(key);
        if (!v.content.empty()) return v.content;
    }
    if (req.has_param(key)) {
        const std::string v = req.get_param_value(key);
        if (!v.empty()) return v;
    }
    return def;
}

int parseIntOrDefault(const std::string& value, int def) {
    if (value.empty()) return def;
    try { return std::stoi(value); } catch (...) { return def; }
}

int64_t parseInt64OrDefault(const std::string& value, int64_t def) {
    if (value.empty()) return def;
    try { return std::stoll(value); } catch (...) { return def; }
}

int envIntOrDefault(const char* key, int def) {
    const char* raw = std::getenv(key);
    if (!raw || !*raw) return def;
    return parseIntOrDefault(raw, def);
}

std::string npuLockFile() {
    const char* raw = std::getenv("IMAGEGEN_NPU_LOCK_FILE");
    if (raw && *raw) return std::string(raw);
    return "/opt/genai-lock/npu.lock";
}

int npuLockTimeoutMs() {
    return std::max(1000, envIntOrDefault("IMAGEGEN_NPU_LOCK_TIMEOUT_MS", 300000));
}

class ScopedNpuLock {
public:
    ScopedNpuLock(const std::string& lock_file, int timeout_ms) {
        const std::filesystem::path lock_path(lock_file);
        if (lock_path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(lock_path.parent_path(), ec);
        }
        fd_ = ::open(lock_file.c_str(), O_CREAT | O_RDWR, 0666);
        if (fd_ < 0) {
            throw std::runtime_error("npu_lock_open_failed");
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        while (true) {
            if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
                locked_ = true;
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("npu_lock_timeout");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    ~ScopedNpuLock() {
        if (fd_ >= 0) {
            if (locked_) {
                ::flock(fd_, LOCK_UN);
            }
            ::close(fd_);
        }
    }

    ScopedNpuLock(const ScopedNpuLock&) = delete;
    ScopedNpuLock& operator=(const ScopedNpuLock&) = delete;

private:
    int fd_{-1};
    bool locked_{false};
};

GenerationResult generateWithNpuLock(StableDiffusionEngine& engine, const GenerationRequest& req) {
    try {
        ScopedNpuLock lk(npuLockFile(), npuLockTimeoutMs());
        return engine.generate(req);
    } catch (const std::exception& e) {
        GenerationResult failed;
        failed.success = false;
        failed.error = std::string("npu_lock_failure: ") + e.what();
        return failed;
    }
}

std::string makeImageStoreId() {
    const uint64_t counter = g_image_counter.fetch_add(1);
    const int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    return "img_" + std::to_string(now) + "_" + std::to_string(counter);
}

void pruneImageStoreLocked(const std::chrono::steady_clock::time_point& now) {
    for (auto it = g_image_store.begin(); it != g_image_store.end();) {
        if (now > it->second.second) it = g_image_store.erase(it);
        else ++it;
    }
}

std::string putImageAndBuildUrl(const httplib::Request& req,
                                const std::vector<uint8_t>& png_bytes) {
    const std::string id = makeImageStoreId();
    {
        std::lock_guard<std::mutex> lk(g_image_store_mu);
        const auto now = std::chrono::steady_clock::now();
        pruneImageStoreLocked(now);
        g_image_store[id] = {png_bytes, now + kImageUrlTtl};
    }
    const std::string host = req.get_header_value("Host");
    if (host.empty()) return "/v1/images/files/" + id;
    return "http://" + host + "/v1/images/files/" + id;
}

bool getImageFromStore(const std::string& id, std::vector<uint8_t>& png_bytes) {
    std::lock_guard<std::mutex> lk(g_image_store_mu);
    const auto now = std::chrono::steady_clock::now();
    pruneImageStoreLocked(now);
    auto it = g_image_store.find(id);
    if (it == g_image_store.end()) return false;
    png_bytes = it->second.first;
    return true;
}

bool parseSize(const std::string& size, int& w, int& h) {
    if (size.empty()) return true;
    const auto x = size.find('x');
    if (x == std::string::npos) return false;
    try {
        w = std::stoi(size.substr(0, x));
        h = std::stoi(size.substr(x + 1));
    } catch (...) {
        return false;
    }
    return true;
}

void setJsonErrorResponse(httplib::Response& res,
                          int status,
                          const std::string& msg,
                          const std::string& type = "invalid_request_error") {
    res.status = status;
    res.set_content(errorJson(msg, type).dump(), "application/json");
}

bool validatePromptValue(const std::string& prompt, std::string& err_msg) {
    if (prompt.empty()) {
        err_msg = "'prompt' field is required and must be non-empty";
        return false;
    }
    if (prompt.size() > static_cast<size_t>(kMaxPromptChars)) {
        err_msg = "'prompt' exceeds maximum length";
        return false;
    }
    return true;
}

bool validateNValue(int n, std::string& err_msg) {
    if (n < 1 || n > kMaxN) {
        err_msg = "'n' must be between 1 and 10";
        return false;
    }
    return true;
}

bool validateSizeValue(const std::string& size, std::string& err_msg) {
    int req_w = 512;
    int req_h = 512;
    if (!parseSize(size, req_w, req_h)) {
        err_msg = "Invalid size format. Expected <width>x<height>";
        return false;
    }
    if (req_w != 512 || req_h != 512) {
        err_msg = "Only size=512x512 is supported by current model";
        return false;
    }
    return true;
}

bool validateResponseFormatValue(const std::string& response_format, std::string& err_msg) {
    if (response_format != "b64_json" && response_format != "url") {
        err_msg = "response_format must be b64_json or url";
        return false;
    }
    return true;
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool envTruthy(const char* value) {
    if (!value) return false;
    const std::string normalized = lowerCopy(value);
    return normalized == "1" || normalized == "true" ||
           normalized == "yes" || normalized == "on";
}

bool releaseI2tBeforeGenerateEnabled() {
    const char* raw = std::getenv("IMAGEGEN_I2T_RELEASE_BEFORE_GENERATE");
    if (!raw || !*raw) return false;
    return envTruthy(raw);
}

bool parseHttpEndpoint(const std::string& endpoint,
                       std::string& host,
                       int& port,
                       std::string& path) {
    static constexpr const char* kPrefix = "http://";
    if (endpoint.rfind(kPrefix, 0) != 0) return false;
    const std::string rest = endpoint.substr(std::strlen(kPrefix));
    const size_t slash = rest.find('/');
    const std::string host_port = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    const size_t colon = host_port.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= host_port.size()) return false;
    host = host_port.substr(0, colon);
    try {
        port = std::stoi(host_port.substr(colon + 1));
    } catch (...) {
        return false;
    }
    return port > 0;
}

bool containsCi(const std::string& text, const std::string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > text.size()) return false;
    for (size_t i = 0; i + needle.size() <= text.size(); ++i) {
        size_t j = 0;
        while (j < needle.size()) {
            const unsigned char tc = static_cast<unsigned char>(text[i + j]);
            const unsigned char nc = static_cast<unsigned char>(needle[j]);
            if (std::tolower(tc) != std::tolower(nc)) break;
            ++j;
        }
        if (j == needle.size()) return true;
    }
    return false;
}

int parseRunnerRc(const std::string& error) {
    const std::string marker = "rc=";
    const size_t pos = error.find(marker);
    if (pos == std::string::npos) return -1;
    size_t begin = pos + marker.size();
    size_t end = begin;
    while (end < error.size() && std::isdigit(static_cast<unsigned char>(error[end]))) {
        ++end;
    }
    if (begin == end) return -1;
    try {
        return std::stoi(error.substr(begin, end - begin));
    } catch (...) {
        return -1;
    }
}

bool isRc256Failure(const GenerationResult& result) {
    return !result.success && parseRunnerRc(result.error) == 256;
}

bool isTransientBackendFailure(const GenerationResult& result) {
    if (result.success) return false;
    const std::string& error = result.error;
    const int rc = parseRunnerRc(error);
    if (rc == 256) return true;
    if (containsCi(error, "npu_lock_failure")) return true;
    if (containsCi(error, "npu_lock_timeout")) return true;
    if (containsCi(error, "engine not initialized")) return true;
    if (containsCi(error, "resource busy")) return true;
    if (containsCi(error, "temporarily unavailable")) return true;
    if (containsCi(error, "timed out") || containsCi(error, "timeout")) return true;
    return false;
}

void setGenerationFailureResponse(httplib::Response& res,
                                  const GenerationResult& result) {
    if (isTransientBackendFailure(result)) {
        setJsonErrorResponse(
            res,
            503,
            "Transient backend failure after retries. Please retry the request.",
            "backend_temporarily_unavailable");
        return;
    }
    setJsonErrorResponse(
        res,
        500,
        result.error.empty() ? "Image generation failed" : result.error,
        "server_error");
}
}  // namespace

/*static*/
std::string ImageGenService::base64Encode(const uint8_t* data, size_t len) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) b |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) b |= static_cast<uint32_t>(data[i + 2]);

        out += kTable[(b >> 18) & 0x3F];
        out += kTable[(b >> 12) & 0x3F];
        out += (i + 1 < len) ? kTable[(b >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? kTable[(b >> 0) & 0x3F] : '=';
    }
    return out;
}

/*static*/
std::string ImageGenService::makeCacheKey(const std::string& model_id,
                                          const GenerationRequest& req) {
    std::ostringstream oss;
    oss << model_id << '\n'
        << req.prompt << '\n'
        << req.negative_prompt << '\n'
        << req.seed << '\n'
        << req.num_steps << '\n'
        << std::fixed << std::setprecision(4) << req.guidance_scale << '\n'
        << req.width << 'x' << req.height << '\n'
        << req.prediction_type_override << '\n'
        << std::fixed << std::setprecision(5) << req.vae_scaling_factor_override;
    return oss.str();
}

bool ImageGenService::cacheGet(const std::string& key,
                               std::vector<uint8_t>& png,
                               int64_t& elapsed_ms,
                               GenerationTiming& timing) {
    if (cache_size_ == 0) return false;
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_map_.find(key);
    if (it == cache_map_.end()) return false;
    cache_lru_.splice(cache_lru_.begin(), cache_lru_, it->second.lru_it);
    png = it->second.png_bytes;
    elapsed_ms = it->second.elapsed_ms;
    timing = it->second.timing;
    return true;
}

void ImageGenService::cachePut(const std::string& key,
                               const std::vector<uint8_t>& png,
                               int64_t elapsed_ms,
                               const GenerationTiming& timing) {
    if (cache_size_ == 0) return;
    std::lock_guard<std::mutex> lock(cache_mutex_);

    auto it = cache_map_.find(key);
    if (it != cache_map_.end()) {
        it->second.png_bytes = png;
        it->second.elapsed_ms = elapsed_ms;
        it->second.timing = timing;
        cache_lru_.splice(cache_lru_.begin(), cache_lru_, it->second.lru_it);
        return;
    }

    cache_lru_.push_front(key);
    cache_map_.emplace(key, CacheValue{png, elapsed_ms, timing, cache_lru_.begin()});
    while (cache_map_.size() > cache_size_) {
        const std::string evict = cache_lru_.back();
        cache_lru_.pop_back();
        cache_map_.erase(evict);
    }
}

ImageGenService::ImageGenService(const std::string& model_dir,
                                 const std::string& tokenizer_dir,
                                 uint16_t port,
                                 std::string api_key,
                                 size_t cache_size)
    : engine_(model_dir, tokenizer_dir),
      port_(port),
      api_key_(std::move(api_key)),
      cache_size_(cache_size) {}


namespace {
bool parseModelField(const json& body, std::string& model_id, json& err) {
    if (!body.contains("model")) return true;
    if (!body["model"].is_string()) {
        err = errorJson("'model' must be a string", "invalid_request_error");
        return false;
    }
    model_id = canonicalizeModelId(body["model"].get<std::string>());
    if (model_id.empty()) {
        err = errorJson("Unsupported model. Use stable-diffusion-2-1",
                        "invalid_request_error");
        return false;
    }
    return true;
}

bool parsePromptField(const json& body, GenerationRequest& gr, json& err) {
    if (!body.contains("prompt") || !body["prompt"].is_string()) {
        err = errorJson("'prompt' field is required and must be non-empty",
                        "invalid_request_error");
        return false;
    }
    gr.prompt = body["prompt"].get<std::string>();
    std::string validation_error;
    if (!validatePromptValue(gr.prompt, validation_error)) {
        err = errorJson(validation_error, "invalid_request_error");
        return false;
    }
    return true;
}

bool parseCoreOptionalFields(const json& body, GenerationRequest& gr, json& err) {
    if (body.contains("negative_prompt")) {
        if (!body["negative_prompt"].is_string()) {
            err = errorJson("'negative_prompt' must be a string", "invalid_request_error");
            return false;
        }
        gr.negative_prompt = body["negative_prompt"].get<std::string>();
    }

    if (body.contains("seed")) {
        if (!body["seed"].is_number_integer()) {
            err = errorJson("'seed' must be an integer", "invalid_request_error");
            return false;
        }
        gr.seed = body["seed"].get<int64_t>();
    }

    if (body.contains("steps")) {
        if (!body["steps"].is_number_integer()) {
            err = errorJson("'steps' must be an integer", "invalid_request_error");
            return false;
        }
        const int steps = body["steps"].get<int>();
        if (steps < 1 || steps > 50) {
            err = errorJson("'steps' must be between 1 and 50", "invalid_request_error");
            return false;
        }
        gr.num_steps = steps;
    }

    if (body.contains("guidance_scale")) {
        if (!body["guidance_scale"].is_number()) {
            err = errorJson("'guidance_scale' must be a number", "invalid_request_error");
            return false;
        }
        const float guidance = body["guidance_scale"].get<float>();
        if (guidance < 1.0f || guidance > 20.0f) {
            err = errorJson("'guidance_scale' must be between 1.0 and 20.0",
                            "invalid_request_error");
            return false;
        }
        gr.guidance_scale = guidance;
    }

    return true;
}

bool parsePredictionTypeField(const json& body, GenerationRequest& gr, json& err) {
    if (!body.contains("prediction_type")) return true;
    if (!body["prediction_type"].is_string()) {
        err = errorJson("'prediction_type' must be a string", "invalid_request_error");
        return false;
    }
    const auto p = body["prediction_type"].get<std::string>();
    if (p != "epsilon" && p != "v_prediction") {
        err = errorJson("'prediction_type' must be epsilon or v_prediction",
                        "invalid_request_error");
        return false;
    }
    gr.prediction_type_override = p;
    return true;
}

bool parseVaeScalingField(const json& body, GenerationRequest& gr, json& err) {
    if (!body.contains("vae_scaling_factor")) return true;
    if (!body["vae_scaling_factor"].is_number()) {
        err = errorJson("'vae_scaling_factor' must be a number",
                        "invalid_request_error");
        return false;
    }
    gr.vae_scaling_factor_override = body["vae_scaling_factor"].get<float>();
    if (!(gr.vae_scaling_factor_override > 0.0f)) {
        err = errorJson("'vae_scaling_factor' must be > 0",
                        "invalid_request_error");
        return false;
    }
    return true;
}

bool parseNField(const json& body, int& n, json& err) {
    if (!body.contains("n")) return true;
    if (!body["n"].is_number_integer()) {
        err = errorJson("'n' must be an integer", "invalid_request_error");
        return false;
    }
    n = body["n"].get<int>();
    std::string validation_error;
    if (!validateNValue(n, validation_error)) {
        err = errorJson(validation_error, "invalid_request_error");
        return false;
    }
    return true;
}

bool parseSizeField(const json& body, json& err) {
    if (!body.contains("size")) return true;
    if (!body["size"].is_string()) {
        err = errorJson("'size' must be a string", "invalid_request_error");
        return false;
    }
    std::string validation_error;
    if (!validateSizeValue(body["size"].get<std::string>(), validation_error)) {
        err = errorJson(validation_error, "invalid_request_error");
        return false;
    }
    return true;
}

bool parseResponseFormatField(const json& body, std::string& response_format, json& err) {
    if (!body.contains("response_format")) return true;
    if (!body["response_format"].is_string()) {
        err = errorJson("'response_format' must be a string",
                        "invalid_request_error");
        return false;
    }
    const auto rf = body["response_format"].get<std::string>();
    std::string validation_error;
    if (!validateResponseFormatValue(rf, validation_error)) {
        err = errorJson(validation_error, "invalid_request_error");
        return false;
    }
    response_format = rf;
    return true;
}

bool parseRequestJson(const httplib::Request& req,
                      GenerationRequest& gr,
                      std::string& model_id,
                      int& n,
                      std::string& response_format,
                      json& err) {
    json body;
    try {
        body = json::parse(req.body);
    } catch (const std::exception& e) {
        err = errorJson(std::string("Invalid JSON: ") + e.what(), "invalid_request_error");
        return false;
    }

    model_id = kCanonicalModel;
    n = kDefaultN;
    response_format = "url";
    if (!parseModelField(body, model_id, err)) return false;
    if (!parsePromptField(body, gr, err)) return false;
    if (!parseCoreOptionalFields(body, gr, err)) return false;
    if (!parsePredictionTypeField(body, gr, err)) return false;
    if (!parseVaeScalingField(body, gr, err)) return false;
    if (!parseNField(body, n, err)) return false;
    if (!parseSizeField(body, err)) return false;
    if (!parseResponseFormatField(body, response_format, err)) return false;
    return true;
}
}  // namespace

void ImageGenService::run() {
    httplib::Server server;
    server.new_task_queue = [] { return new httplib::ThreadPool(1); };
    server.set_payload_max_length(kMaxPayloadBytes);

    registerRoutes(server);

    std::cout << "[ImageGenService] Listening on 0.0.0.0:" << port_
              << " auth=" << (api_key_.empty() ? "disabled" : "enabled")
              << " cache_size=" << cache_size_ << "\n";
    if (!server.listen("0.0.0.0", port_)) {
        throw std::runtime_error("Failed to start HTTP server");
    }
}

bool ImageGenService::ensureReady(httplib::Response& res) {
    try {
        std::call_once(init_once_, [this]() { engine_.initialize(); });
        return engine_.isReady();
    } catch (const std::exception& e) {
        setJsonErrorResponse(res, 500, std::string("Engine init failed: ") + e.what(), "server_error");
        return false;
    }
}

bool ImageGenService::checkAuth(const httplib::Request& req, httplib::Response& res) const {
    if (api_key_.empty()) return true;

    static const std::string kBearerPrefix = "Bearer ";
    const std::string auth = req.get_header_value("Authorization");
    if (auth.rfind(kBearerPrefix, 0) != 0 ||
        auth.substr(kBearerPrefix.size()) != api_key_) {
        res.status = 401;
        res.set_header("WWW-Authenticate", "Bearer");
        res.set_content(
            errorJson("Invalid or missing bearer token", "authentication_error").dump(),
            "application/json");
        return false;
    }
    return true;
}

bool ImageGenService::tryReleaseI2tSession() const {
    const char* arb_env = std::getenv("IMAGEGEN_I2T_ARBITRATION_ENABLED");
    if (arb_env && !envTruthy(arb_env)) {
        return false;
    }

    const std::string endpoint = std::getenv("IMAGEGEN_I2T_RELEASE_URL")
        ? std::getenv("IMAGEGEN_I2T_RELEASE_URL")
        : "http://127.0.0.1:8080/v1/session/release";

    std::string host;
    int port = 0;
    std::string path;
    if (!parseHttpEndpoint(endpoint, host, port, path)) {
        std::cerr << "[ImageGenService] Invalid IMAGEGEN_I2T_RELEASE_URL: " << endpoint << "\n";
        return false;
    }

    httplib::Client cli(host, port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(10, 0);
    cli.set_write_timeout(10, 0);
    const httplib::Headers headers = {
        {"Content-Type", "application/json"},
        {"X-Session-Id", "__default__"}
    };
    auto resp = cli.Post(path.c_str(), headers, "{}", "application/json");
    if (!resp) {
        std::cerr << "[ImageGenService] Failed to call I2T release endpoint: " << endpoint << "\n";
        return false;
    }

    if (resp->status >= 200 && resp->status < 300) {
        std::cout << "[ImageGenService] Released I2T session before retry.\n";
        return true;
    }

    if (resp->status == 404 && path == "/v1/session/release") {
        auto reset_resp = cli.Post("/v1/session/reset", headers, "{}", "application/json");
        if (reset_resp && reset_resp->status >= 200 && reset_resp->status < 300) {
            std::cout << "[ImageGenService] Fallback reset of I2T session succeeded before retry.\n";
            return true;
        }
        if (reset_resp) {
            std::cerr << "[ImageGenService] I2T reset fallback returned status="
                      << reset_resp->status << "\n";
        } else {
            std::cerr << "[ImageGenService] Failed to call I2T reset fallback endpoint.\n";
        }
    }

    std::cerr << "[ImageGenService] I2T release endpoint returned status=" << resp->status << "\n";
    return false;
}

GenerationResult ImageGenService::generateWithRecovery(const GenerationRequest& req) {
    if (releaseI2tBeforeGenerateEnabled()) {
        if (tryReleaseI2tSession()) {
            std::cout << "[ImageGenService] I2T release succeeded before generation.\n";
        } else {
            std::cerr << "[ImageGenService] I2T release failed/disabled before generation.\n";
        }
    }

    GenerationResult current = generateWithNpuLock(engine_, req);
    if (current.success || !isTransientBackendFailure(current)) return current;

    const int max_attempts =
        std::max(1, envIntOrDefault("IMAGEGEN_TRANSIENT_RETRY_ATTEMPTS", 8));
    const int base_backoff_ms =
        std::max(100, envIntOrDefault("IMAGEGEN_TRANSIENT_RETRY_BACKOFF_MS", 1200));

    std::cerr << "[ImageGenService] transient generation failure on first attempt: "
              << current.error << "\n";

    for (int attempt = 2; attempt <= max_attempts; ++attempt) {
        const bool is_rc256 = isRc256Failure(current);
        if (is_rc256) {
            if (tryReleaseI2tSession()) {
                std::cout << "[ImageGenService] I2T release succeeded before retry.\n";
            } else {
                std::cerr << "[ImageGenService] I2T release failed/disabled before retry.\n";
            }
        }

        const int backoff_ms = is_rc256 ? (base_backoff_ms + 300) : base_backoff_ms;
        std::cerr << "[ImageGenService] retrying generation attempt "
                  << attempt << "/" << max_attempts
                  << " after transient failure.\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));

        GenerationResult next = generateWithNpuLock(engine_, req);
        if (next.success) {
            std::cout << "[ImageGenService] generation recovered on retry attempt "
                      << attempt << ".\n";
            return next;
        }

        std::cerr << "[ImageGenService] retry attempt " << attempt
                  << " failed. previous_error=" << current.error
                  << " retry_error=" << next.error << "\n";

        if (!isTransientBackendFailure(next)) {
            return next;
        }
        if (next.error.empty()) {
            next.error = current.error;
        }
        current = std::move(next);
    }

    if (!current.success && current.error.empty()) {
        current.error = "Transient backend failure after retries";
    }
    return current;
}

bool ImageGenService::preflightRequest(const httplib::Request& req,
                                       httplib::Response& res,
                                       bool require_ready) {
    if (!checkAuth(req, res)) return false;
    if (require_ready && !ensureReady(res)) return false;
    return true;
}

void ImageGenService::registerRoutes(httplib::Server& server) {
    registerCoreGetRoutes(server);
    registerImageGetRoutes(server);
    registerPostRoutes(server);
}

void ImageGenService::registerCoreGetRoutes(httplib::Server& server) {
    server.Get("/health",
               [this](const httplib::Request& req, httplib::Response& res) {
                   handleHealth(req, res);
               });

    server.Get("/v1/models",
               [this](const httplib::Request& req, httplib::Response& res) {
                   handleModels(req, res);
               });

    server.Get(R"(/v1/models/(.+))",
               [this](const httplib::Request& req, httplib::Response& res) {
                   handleModelById(req, res);
               });
}

void ImageGenService::registerImageGetRoutes(httplib::Server& server) {
    server.Get(R"(/v1/images/files/(.+))",
               [this](const httplib::Request& req, httplib::Response& res) {
                   handleImageFile(req, res);
               });

    server.Get("/v1/images/generations/params",
               [this](const httplib::Request& req, httplib::Response& res) {
                   handleGenerationParams(req, res);
               });
}

void ImageGenService::registerPostRoutes(httplib::Server& server) {
    server.Post("/generate",
                [this](const httplib::Request& req, httplib::Response& res) {
                    handleLegacyGenerate(req, res);
                });

    server.Post("/v1/images/generations",
                [this](const httplib::Request& req, httplib::Response& res) {
                    handleV1Generations(req, res);
                });

    server.Post("/v1/images/edits",
                [this](const httplib::Request& req, httplib::Response& res) {
                    handleImageEdits(req, res);
                });

    server.Post("/v1/images/variations",
                [this](const httplib::Request& req, httplib::Response& res) {
                    handleImageVariations(req, res);
                });
}

void ImageGenService::handleHealth(const httplib::Request&, httplib::Response& res) {
    const bool ready = ensureReady(res);
    if (res.status == 500) return;
    res.status = ready ? 200 : 503;
    res.set_content(json{{"status", ready ? "ok" : "not_ready"}}.dump(),
                    "application/json");
}

void ImageGenService::handleModels(const httplib::Request& req, httplib::Response& res) {
    if (!checkAuth(req, res)) return;
    const int64_t created = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    json payload = {
        {"object", "list"},
        {"data",
         json::array({
             json{
                 {"id", kCanonicalModel},
                 {"object", "model"},
                 {"created", created},
                 {"owned_by", "qualcomm"},
                 {"capabilities", json::array({"text-to-image"})},
                 {"max_steps", 50},
                 {"output_size", "512x512"},
             },
             json{
                 {"id", "dall-e-2"},
                 {"object", "model"},
                 {"created", created},
                 {"owned_by", "qualcomm"}
             },
             json{
                 {"id", "dall-e-3"},
                 {"object", "model"},
                 {"created", created},
                 {"owned_by", "qualcomm"}
             },
             json{
                 {"id", "gpt-image-1"},
                 {"object", "model"},
                 {"created", created},
                 {"owned_by", "qualcomm"}
             },
             json{
                 {"id", "gpt-image-1-mini"},
                 {"object", "model"},
                 {"created", created},
                 {"owned_by", "qualcomm"}
             },
             json{
                 {"id", "gpt-image-1.5"},
                 {"object", "model"},
                 {"created", created},
                 {"owned_by", "qualcomm"}
             },
         })},
    };
    res.set_content(payload.dump(), "application/json");
}

void ImageGenService::handleModelById(const httplib::Request& req, httplib::Response& res) {
    if (!checkAuth(req, res)) return;
    const std::string requested = req.matches[1];
    if (canonicalizeModelId(requested).empty()) {
        res.status = 404;
        res.set_content(errorJson("Model not found: " + requested,
                                  "invalid_request_error").dump(),
                        "application/json");
        return;
    }
    const int64_t created = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    json payload = {
        {"id", requested},
        {"object", "model"},
        {"created", created},
        {"owned_by", "qualcomm"}
    };
    res.set_content(payload.dump(), "application/json");
}

void ImageGenService::handleImageFile(const httplib::Request& req, httplib::Response& res) {
    // Keep file URLs OpenAI-like: retrievable without auth header.
    // If Authorization is provided, still validate it.
    if (req.has_header("Authorization") && !checkAuth(req, res)) return;
    const std::string image_id = req.matches[1];
    std::vector<uint8_t> png;
    if (!getImageFromStore(image_id, png)) {
        res.status = 404;
        res.set_content(errorJson("Image URL expired or not found",
                                  "invalid_request_error").dump(),
                        "application/json");
        return;
    }
    res.set_content(std::string(png.begin(), png.end()), "image/png");
}

void ImageGenService::handleGenerationParams(const httplib::Request& req, httplib::Response& res) {
    if (!checkAuth(req, res)) return;
    json payload = {
        {"model", kCanonicalModel},
        {"supported_sizes", json::array({"512x512"})},
        {"response_formats", json::array({"b64_json", "url"})},
        {"steps", {{"min", 1}, {"max", 50}, {"default", 20}}},
        {"guidance_scale",
         {{"min", 1.0}, {"max", 20.0}, {"default", 7.5}}},
        {"seed", {{"type", "integer"}, {"default", 42}, {"random", -1}}},
    };
    res.set_content(payload.dump(), "application/json");
}

void ImageGenService::handleLegacyGenerate(const httplib::Request& req, httplib::Response& res) {
    const auto t_req_start = std::chrono::steady_clock::now();
    const auto t_ready_start = std::chrono::steady_clock::now();
    if (!preflightRequest(req, res, true)) return;
    const double ensure_ready_ms = round3(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_ready_start).count());

    GenerationRequest gr;
    std::string model_id;
    int n = 1;
    std::string response_format;
    json err;
    const auto t_parse_start = std::chrono::steady_clock::now();
    if (!parseRequestJson(req, gr, model_id, n, response_format, err)) {
        res.status = 400;
        res.set_content(err.dump(), "application/json");
        return;
    }
    (void)response_format;
    if (n != 1) {
        setJsonErrorResponse(res, 400, "/generate supports only n=1");
        return;
    }
    const double parse_request_ms = round3(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_parse_start).count());

    const std::string key = makeCacheKey(model_id, gr);
    std::vector<uint8_t> png;
    int64_t elapsed_ms = 0;
    GenerationTiming timing;
    const auto t_cache_start = std::chrono::steady_clock::now();
    const bool cache_hit = cacheGet(key, png, elapsed_ms, timing);
    const double cache_lookup_ms = round3(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_cache_start).count());
    if (cache_hit) {
        res.set_header("X-Cache-Hit", "1");
        res.set_header("X-Process-Time-Ms", std::to_string(elapsed_ms));
        res.set_header("X-Engine-Total-Ms", std::to_string(timing.total_ms));
        res.set_header("X-Ensure-Ready-Ms", std::to_string(ensure_ready_ms));
        res.set_header("X-Parse-Request-Ms", std::to_string(parse_request_ms));
        res.set_header("X-Cache-Lookup-Ms", std::to_string(cache_lookup_ms));
        res.set_header(
            "X-Request-Total-Ms",
            std::to_string(round3(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_req_start).count())));
        res.set_header("X-Model", model_id);
        res.set_content(std::string(png.begin(), png.end()), "image/png");
        return;
    }

    std::cout << "[ImageGenService] /generate model=" << model_id
              << " steps=" << gr.num_steps << "\n";
    const auto t_generate_start = std::chrono::steady_clock::now();
    const auto result = generateWithRecovery(gr);
    const double generate_call_ms = round3(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_generate_start).count());
    if (!result.success) {
        setGenerationFailureResponse(res, result);
        return;
    }
    cachePut(key, result.png_bytes, result.elapsed_ms, result.timing);

    res.set_header("X-Cache-Hit", "0");
    res.set_header("X-Process-Time-Ms", std::to_string(result.elapsed_ms));
    res.set_header("X-Engine-Total-Ms", std::to_string(result.timing.total_ms));
    res.set_header("X-Generate-Call-Ms", std::to_string(generate_call_ms));
    res.set_header("X-Ensure-Ready-Ms", std::to_string(ensure_ready_ms));
    res.set_header("X-Parse-Request-Ms", std::to_string(parse_request_ms));
    res.set_header("X-Cache-Lookup-Ms", std::to_string(cache_lookup_ms));
    res.set_header(
        "X-Request-Total-Ms",
        std::to_string(round3(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_req_start).count())));
    res.set_header("X-Model", model_id);
    res.set_content(std::string(result.png_bytes.begin(), result.png_bytes.end()),
                    "image/png");
}

void ImageGenService::handleV1Generations(const httplib::Request& req, httplib::Response& res) {
    const auto t_req_start = std::chrono::steady_clock::now();
    const auto t_ready_start = std::chrono::steady_clock::now();
    if (!preflightRequest(req, res, true)) return;
    const double ensure_ready_ms = round3(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_ready_start).count());

    GenerationRequest gr;
    std::string model_id;
    int n = 1;
    std::string response_format;
    json err;
    const auto t_parse_start = std::chrono::steady_clock::now();
    if (!parseRequestJson(req, gr, model_id, n, response_format, err)) {
        res.status = 400;
        res.set_content(err.dump(), "application/json");
        return;
    }
    const double parse_request_ms = round3(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_parse_start).count());

    json data = json::array();
    double cache_lookup_ms = 0.0;
    double base64_encode_ms = 0.0;
    int cache_hits = 0;
    int64_t elapsed_sum = 0;
    GenerationTiming first_timing;
    bool first_timing_set = false;

    for (int i = 0; i < n; ++i) {
        GenerationRequest req_i = gr;
        req_i.seed = gr.seed + static_cast<int64_t>(i);
        const std::string key = makeCacheKey(model_id, req_i);
        std::vector<uint8_t> png;
        int64_t elapsed_ms = 0;
        GenerationTiming timing;
        const auto t_cache_start = std::chrono::steady_clock::now();
        const bool cache_hit = cacheGet(key, png, elapsed_ms, timing);
        cache_lookup_ms += round3(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_cache_start).count());

        if (!cache_hit) {
            std::cout << "[ImageGenService] /v1/images/generations model=" << model_id
                      << " steps=" << req_i.num_steps << " seed=" << req_i.seed << "\n";
            const auto result = generateWithRecovery(req_i);
            if (!result.success) {
                setGenerationFailureResponse(res, result);
                return;
            }
            png = result.png_bytes;
            elapsed_ms = result.elapsed_ms;
            timing = result.timing;
            cachePut(key, png, elapsed_ms, timing);
        } else {
            ++cache_hits;
        }
        elapsed_sum += elapsed_ms;
        if (!first_timing_set) {
            first_timing = timing;
            first_timing_set = true;
        }

        if (response_format == "b64_json") {
            const auto t_b64_start = std::chrono::steady_clock::now();
            const std::string b64 = base64Encode(png.data(), png.size());
            base64_encode_ms += round3(
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_b64_start).count());
            data.push_back(json{
                {"b64_json", b64},
                {"revised_prompt", req_i.prompt}
            });
        } else {
            const std::string url = putImageAndBuildUrl(req, png);
            data.push_back(json{
                {"url", url},
                {"revised_prompt", req_i.prompt}
            });
        }
    }

    const int64_t ts = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const double request_total_ms = round3(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_req_start).count());

    json x_timing = {
        {"request_total_ms", request_total_ms},
        {"ensure_ready_ms",  ensure_ready_ms},
        {"parse_request_ms", parse_request_ms},
        {"cache_lookup_ms",  round3(cache_lookup_ms)},
        {"base64_encode_ms", base64_encode_ms},
        {"cache_hit_count",  cache_hits},
        {"cache_miss_count", n - cache_hits},
        {"n",                n},
        {"response_format",  response_format},
        {"engine",           generationTimingJson(first_timing_set ? first_timing : GenerationTiming{})}
    };

    json payload = {
        {"created", ts},
        {"data", data},
        {"usage", {{"prompt_tokens", 0}, {"completion_tokens", 0}, {"total_tokens", 0}}},
        {"model", model_id},
        {"x_process_time_ms", n > 0 ? elapsed_sum / n : 0},
        {"x_timing", x_timing},
    };
    res.set_header("X-Cache-Hit", std::to_string(cache_hits));
    res.set_header("X-Process-Time-Ms", std::to_string(n > 0 ? elapsed_sum / n : 0));
    res.set_header("X-Engine-Total-Ms",
                   std::to_string(first_timing_set ? first_timing.total_ms : 0));
    res.set_header("X-Request-Total-Ms", std::to_string(request_total_ms));
    res.set_header("X-Ensure-Ready-Ms", std::to_string(ensure_ready_ms));
    res.set_header("X-Parse-Request-Ms", std::to_string(parse_request_ms));
    res.set_header("X-Cache-Lookup-Ms", std::to_string(round3(cache_lookup_ms)));
    res.set_header("X-Base64-Encode-Ms", std::to_string(base64_encode_ms));
    res.set_content(payload.dump(), "application/json");
}

void ImageGenService::handleMultipartImage(const httplib::Request& req,
                                           httplib::Response& res,
                                           bool require_prompt,
                                           const std::string& default_prompt,
                                           const std::string& task_name) {
    const auto t_req_start = std::chrono::steady_clock::now();
    if (!preflightRequest(req, res, true)) return;

    httplib::MultipartFormData image;
    bool has_image = false;
    if (req.has_file("image")) {
        image = req.get_file_value("image");
        has_image = true;
    } else if (req.has_file("image[]")) {
        image = req.get_file_value("image[]");
        has_image = true;
    }
    if (!has_image) {
        setJsonErrorResponse(res, 400, "'image' file is required");
        return;
    }
    if (image.content.empty()) {
        setJsonErrorResponse(res, 400, "Empty 'image' content");
        return;
    }
    if (!image.content_type.empty()) {
        const std::string content_type = lowerCopy(image.content_type);
        if (content_type.rfind("image/", 0) != 0) {
            setJsonErrorResponse(res, 400, "'image' must be an image/* upload");
            return;
        }
    }

    std::string mask_content;
    if (req.has_file("mask")) {
        const auto& mask = req.get_file_value("mask");
        if (mask.content.empty()) {
            setJsonErrorResponse(res, 400, "Empty 'mask' content");
            return;
        }
        if (!mask.content_type.empty()) {
            const std::string content_type = lowerCopy(mask.content_type);
            if (content_type.rfind("image/", 0) != 0) {
                setJsonErrorResponse(res, 400, "'mask' must be an image/* upload");
                return;
            }
        }
        mask_content = mask.content;
    } else if (req.has_file("mask[]")) {
        const auto& mask = req.get_file_value("mask[]");
        if (mask.content.empty()) {
            setJsonErrorResponse(res, 400, "Empty 'mask' content");
            return;
        }
        if (!mask.content_type.empty()) {
            const std::string content_type = lowerCopy(mask.content_type);
            if (content_type.rfind("image/", 0) != 0) {
                setJsonErrorResponse(res, 400, "'mask' must be an image/* upload");
                return;
            }
        }
        mask_content = mask.content;
    }

    if (task_name == "variation" && !mask_content.empty()) {
        setJsonErrorResponse(res, 400, "'mask' is not supported for image variations");
        return;
    }

    GenerationRequest gr;
    std::string model_id = canonicalizeModelId(parseFormField(req, "model", kCanonicalModel));
    if (model_id.empty()) {
        setJsonErrorResponse(res, 400, "Unsupported model");
        return;
    }

    std::string prompt = parseFormField(req, "prompt", "");
    if (require_prompt && prompt.empty()) {
        setJsonErrorResponse(res, 400, "'prompt' is required for this endpoint");
        return;
    }
    if (prompt.empty()) prompt = default_prompt;
    std::string validation_error;
    if (!validatePromptValue(prompt, validation_error)) {
        setJsonErrorResponse(res, 400, validation_error);
        return;
    }
    gr.prompt = prompt;

    const std::string quality = parseFormField(req, "quality", "auto");
    if (!quality.empty() && quality != "auto" && quality != "low" &&
        quality != "medium" && quality != "high" &&
        quality != "standard" && quality != "hd") {
        setJsonErrorResponse(res, 400, "Unsupported 'quality' value");
        return;
    }

    const std::string style = parseFormField(req, "style", "");
    if (!style.empty() && style != "vivid" && style != "natural") {
        setJsonErrorResponse(res, 400, "Unsupported 'style' value");
        return;
    }

    const std::string background = parseFormField(req, "background", "");
    if (!background.empty() && background != "auto" &&
        background != "transparent" && background != "opaque") {
        setJsonErrorResponse(res, 400, "Unsupported 'background' value");
        return;
    }

    const std::string output_format = parseFormField(req, "output_format", "png");
    if (!output_format.empty() && output_format != "png" &&
        output_format != "jpeg" && output_format != "webp") {
        setJsonErrorResponse(res, 400, "Unsupported 'output_format' value");
        return;
    }

    const std::string output_compression = parseFormField(req, "output_compression", "");
    if (!output_compression.empty()) {
        const int comp = parseIntOrDefault(output_compression, -1);
        if (comp < 0 || comp > 100) {
            setJsonErrorResponse(res, 400, "'output_compression' must be between 0 and 100");
            return;
        }
    }

    const std::string negative_prompt = parseFormField(req, "negative_prompt", "");
    if (!negative_prompt.empty()) gr.negative_prompt = negative_prompt;

    int n = parseIntOrDefault(parseFormField(req, "n", "1"), 1);
    if (!validateNValue(n, validation_error)) {
        setJsonErrorResponse(res, 400, validation_error);
        return;
    }

    std::string response_format = parseFormField(req, "response_format", "url");
    if (!validateResponseFormatValue(response_format, validation_error)) {
        setJsonErrorResponse(res, 400, validation_error);
        return;
    }

    std::string size = parseFormField(req, "size", "512x512");
    if (!validateSizeValue(size, validation_error)) {
        setJsonErrorResponse(res, 400, validation_error);
        return;
    }

    const auto seed_override = parseFormField(req, "seed", "");
    if (!seed_override.empty()) {
        gr.seed = parseInt64OrDefault(seed_override, gr.seed);
    } else {
        size_t hashed = std::hash<std::string>{}(image.content);
        if (!mask_content.empty()) {
            hashed ^= (std::hash<std::string>{}(mask_content) + 0x9e3779b97f4a7c15ULL +
                       (hashed << 6) + (hashed >> 2));
        }
        hashed ^= (std::hash<std::string>{}(prompt) + 0x9e3779b97f4a7c15ULL +
                   (hashed << 6) + (hashed >> 2));
        gr.seed = static_cast<int64_t>(hashed % 2147483647ULL);
    }

    const std::string steps_s = parseFormField(req, "steps", "");
    if (!steps_s.empty()) {
        try {
            size_t idx = 0;
            const int parsed = std::stoi(steps_s, &idx);
            if (idx != steps_s.size() || parsed < 1 || parsed > 50) {
                setJsonErrorResponse(res, 400, "'steps' must be an integer between 1 and 50");
                return;
            }
            gr.num_steps = parsed;
        } catch (...) {
            setJsonErrorResponse(res, 400, "'steps' must be an integer between 1 and 50");
            return;
        }
    }
    const std::string guidance_s = parseFormField(req, "guidance_scale", "");
    if (!guidance_s.empty()) {
        try {
            size_t idx = 0;
            const float parsed = std::stof(guidance_s, &idx);
            if (idx != guidance_s.size() || parsed < 1.0f || parsed > 20.0f) {
                setJsonErrorResponse(
                    res, 400, "'guidance_scale' must be a number between 1.0 and 20.0");
                return;
            }
            gr.guidance_scale = parsed;
        } catch (...) {
            setJsonErrorResponse(
                res, 400, "'guidance_scale' must be a number between 1.0 and 20.0");
            return;
        }
    }

    json data = json::array();
    int64_t elapsed_sum = 0;
    int cache_hits = 0;

    for (int i = 0; i < n; ++i) {
        GenerationRequest req_i = gr;
        req_i.seed = gr.seed + static_cast<int64_t>(i);
        const std::string key = makeCacheKey(model_id, req_i);
        std::vector<uint8_t> png;
        int64_t elapsed_ms = 0;
        GenerationTiming timing;
        const bool cache_hit = cacheGet(key, png, elapsed_ms, timing);
        if (!cache_hit) {
            const auto result = generateWithRecovery(req_i);
            if (!result.success) {
                setGenerationFailureResponse(res, result);
                return;
            }
            png = result.png_bytes;
            elapsed_ms = result.elapsed_ms;
            timing = result.timing;
            cachePut(key, png, elapsed_ms, timing);
        } else {
            ++cache_hits;
        }
        elapsed_sum += elapsed_ms;
        if (response_format == "b64_json") {
            data.push_back(json{
                {"b64_json", base64Encode(png.data(), png.size())},
                {"revised_prompt", req_i.prompt}
            });
        } else {
            data.push_back(json{
                {"url", putImageAndBuildUrl(req, png)},
                {"revised_prompt", req_i.prompt}
            });
        }
    }

    const int64_t created = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const double request_total_ms = round3(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_req_start).count());
    json payload = {
        {"created", created},
        {"data", data},
        {"model", model_id},
        {"usage", {{"prompt_tokens", 0}, {"completion_tokens", 0}, {"total_tokens", 0}}},
        {"x_process_time_ms", n > 0 ? elapsed_sum / n : 0},
        {"x_timing", {
            {"request_total_ms", request_total_ms},
            {"task", task_name},
            {"n", n},
            {"cache_hit_count", cache_hits},
            {"cache_miss_count", n - cache_hits}
        }}
    };
    res.set_content(payload.dump(), "application/json");
}

void ImageGenService::handleImageEdits(const httplib::Request& req, httplib::Response& res) {
    handleMultipartImage(req, res, true, "Edited image", "edit");
}

void ImageGenService::handleImageVariations(const httplib::Request& req, httplib::Response& res) {
    handleMultipartImage(req, res, false, "Variation of provided image", "variation");
}
