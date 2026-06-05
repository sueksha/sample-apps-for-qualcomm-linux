// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include <iostream>
#include <string>
#include <cstdlib>
#include <vector>
#include "ImageGenService.hpp"

namespace {
struct AppConfig {
    std::string model_dir = "/opt/runtime/";
    std::string tokenizer_dir;
    uint16_t port = 8084;
    std::string api_key;
    size_t cache_size = 16;
};

std::string withTrailingSlash(std::string value) {
    if (!value.empty() && value.back() != '/') value.push_back('/');
    return value;
}

bool parsePort(const std::string& text, uint16_t& out_port) {
    try {
        out_port = static_cast<uint16_t>(std::stoi(text));
        return true;
    } catch (...) {
        return false;
    }
}

bool parseCacheSize(const std::string& text, size_t& out_cache_size) {
    try {
        out_cache_size = static_cast<size_t>(std::stoul(text));
        return true;
    } catch (...) {
        return false;
    }
}

bool parseArgs(int argc,
               char* argv[],
               AppConfig& cfg,
               std::string& error,
               bool& show_help) {
    show_help = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](std::string& out) -> bool {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };

        if (arg == "-h" || arg == "--help") {
            show_help = true;
            return true;
        }

        std::string value;
        if (arg == "--model-dir" || arg == "-m") {
            if (!next(value)) {
                error = "Missing value for --model-dir";
                return false;
            }
            cfg.model_dir = withTrailingSlash(value);
            continue;
        }
        if (arg == "--tokenizer-dir" || arg == "-t") {
            if (!next(value)) {
                error = "Missing value for --tokenizer-dir";
                return false;
            }
            cfg.tokenizer_dir = withTrailingSlash(value);
            continue;
        }
        if (arg == "--port" || arg == "-p") {
            if (!next(value) || !parsePort(value, cfg.port)) {
                error = "Invalid port: " + value;
                return false;
            }
            continue;
        }
        if (arg == "--api-key") {
            if (!next(value)) {
                error = "Missing value for --api-key";
                return false;
            }
            cfg.api_key = value;
            continue;
        }
        if (arg == "--cache-size") {
            if (!next(value) || !parseCacheSize(value, cfg.cache_size)) {
                error = "Invalid cache-size: " + value;
                return false;
            }
            continue;
        }

        error = "Unknown argument: " + arg;
        return false;
    }
    return true;
}
}  // namespace

static void usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  --model-dir <path>   Directory containing QNN model .bin files\n"
        << "                       (default: /opt/runtime/)\n"
        << "  --tokenizer-dir <path> Directory containing vocab.json and merges.txt\n"
        << "                       (default: <model-dir>tokenizer/)\n"
        << "  --port <port>        HTTP listen port (default: 8084)\n"
        << "  --api-key <key>      API key for Bearer auth (optional)\n"
        << "  --cache-size <n>     Response cache size (default: 16)\n"
        << "  -h, --help           Show this help message\n"
        << "\n"
        << "Model files required in <model-dir>:\n"
        << "  text_encoder.bin\n"
        << "    or text_encoder_qairt_context.bin\n"
        << "    or stable_diffusion_v2_1-text_encoder-qualcomm_qcs9075.bin\n"
        << "  unet.bin\n"
        << "    or unet_qairt_context.bin\n"
        << "    or stable_diffusion_v2_1-unet-qualcomm_qcs9075.bin\n"
        << "  vae.bin\n"
        << "    or vae_decoder.bin / vae_qairt_context.bin\n"
        << "    or stable_diffusion_v2_1-vae-qualcomm_qcs9075.bin\n"
        << "  tokenizer/vocab.json CLIP BPE vocabulary\n"
        << "  tokenizer/merges.txt CLIP BPE merge rules\n"
        << "  QAIRT libs via QAIRT_LIB_DIR/QAIRT_ADSP_LIB_DIR\n"
        << "    (or legacy copied into model-dir)\n"
        << "  Optional direct runner override path:\n"
        << "  cpp_sd21_qnn_direct/sd21_qnn_cpp_direct\n"
        << "  (default direct runner is embedded in image as /usr/bin/sd21_qnn_cpp_direct)\n"
        << "\n"
        << "Endpoints:\n"
        << "  POST /v1/images/generations  OpenAI-compatible image generation\n"
        << "  POST /generate               Simple JSON endpoint (returns PNG)\n"
        << "  GET  /health                 Liveness check\n"
        << "  GET  /v1/models              Model list\n"
        << "  GET  /v1/images/generations/params  Parameter info\n"
        << "\n"
        << "Example:\n"
        << "  " << prog << " --model-dir /opt/runtime/ --port 8084\n";
}

int main(int argc, char* argv[]) {
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    AppConfig cfg;
    std::string parse_error;
    bool show_help = false;
    if (!parseArgs(argc, argv, cfg, parse_error, show_help)) {
        std::cerr << "[SD] " << parse_error << "\n";
        usage(argv[0]);
        return 1;
    }
    if (show_help) {
        usage(argv[0]);
        return 0;
    }

    if (cfg.api_key.empty()) {
        const char* env_key = std::getenv("IMAGE_GEN_API_KEY");
        if (env_key != nullptr) cfg.api_key = env_key;
    }
    const char* env_cache = std::getenv("IMAGE_GEN_CACHE_SIZE");
    if (env_cache != nullptr) {
        size_t env_cache_size = 0;
        if (parseCacheSize(env_cache, env_cache_size)) cfg.cache_size = env_cache_size;
    }

    if (cfg.tokenizer_dir.empty()) {
        cfg.tokenizer_dir = cfg.model_dir + "tokenizer/";
    }

    std::cout << "[SD] Model dir : " << cfg.model_dir << "\n"
              << "[SD] Tokenizer dir: " << cfg.tokenizer_dir << "\n"
              << "[SD] HTTP port : " << cfg.port      << "\n"
              << "[SD] Auth      : " << (cfg.api_key.empty() ? "disabled" : "enabled") << "\n"
              << "[SD] Cache     : " << cfg.cache_size << "\n";

    try {
        ImageGenService service(cfg.model_dir,
                                cfg.tokenizer_dir,
                                cfg.port,
                                cfg.api_key,
                                cfg.cache_size);
        service.run();   // blocks until server is stopped
    } catch (const std::exception& e) {
        std::cerr << "[SD] Fatal error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
