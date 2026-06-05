// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include <iostream>
#include <string>
#include <cstdlib>
#include <vector>
#include "AsrConfig.hpp"
#include "AsrService.hpp"

namespace {

struct CliOptions {
    std::string model_path = "/opt/genai-studio-models/speech-to-text/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075/";
    std::string vad_model_path;
    uint16_t port = 8081;
};

enum class ParseResult {
    kOk,
    kHelp,
    kError
};

static void usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  --model-path <path>   Directory containing Whisper model files\n"
        << "                                                                )\n"
        << "  --vad-model-path <path>\n"
        << "                        Optional explicit VAD model path\n"
        << "                        (libnnvad_model.so or speech_float.eai)\n"
        << "  --port <port>         HTTP listen port (default: 8081)\n"
        << "  -h, --help            Show this help message\n"
        << "\n"
        << "Required model files in <model-path>:\n"
        << "  encoder_model_htp.bin\n"
        << "  decoder_model_htp.bin\n"
        << "  vocab.bin\n"
        << "  libnnvad_model.so (or speech_float.eai)\n"
        << "\n"
        << "Endpoints:\n"
        << "  POST /generate        multipart/form-data, field 'audio' (WAV)\n"
        << "  POST /transcribe      alias for /generate\n"
        << "  POST /transcribe/raw  raw PCM body (16 kHz, 16-bit, mono)\n"
        << "  POST /v1/realtime/sessions\n"
        << "  POST /v1/realtime/sessions/{id}/audio\n"
        << "  POST /v1/realtime/sessions/{id}/finalize\n"
        << "  GET  /health          liveness check\n"
        << "  GET  /languages       supported language codes\n";
}

void ensureTrailingSlash(std::string& path) {
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
}

bool tryParsePort(const std::string& value, uint16_t& port) {
    try {
        const int parsed = std::stoi(value);
        if (parsed < 1 || parsed > 65535) {
            return false;
        }
        port = static_cast<uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

ParseResult parseCliArgs(int argc, char* argv[], CliOptions& options) {
    if (argc <= 1) {
        return ParseResult::kOk;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if ((arg == "--model-path" || arg == "-m") && i + 1 < argc) {
            options.model_path = argv[++i];
            ensureTrailingSlash(options.model_path);
        } else if (arg == "--vad-model-path" && i + 1 < argc) {
            options.vad_model_path = argv[++i];
        } else if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            const std::string port_arg = argv[++i];
            if (!tryParsePort(port_arg, options.port)) {
                std::cerr << "[ASR] Invalid port: " << port_arg << "\n";
                return ParseResult::kError;
            }
        } else if (arg == "-h" || arg == "--help") {
            return ParseResult::kHelp;
        } else {
            std::cerr << "[ASR] Unknown argument: " << arg << "\n";
            usage(argv[0]);
            return ParseResult::kError;
        }
    }

    return ParseResult::kOk;
}

void printCliConfig(const CliOptions& options) {
    std::cout << "[ASR] Model path : " << options.model_path << "\n";
    if (!options.vad_model_path.empty()) {
        std::cout << "[ASR] VAD path   : " << options.vad_model_path << "\n";
    }
    std::cout << "[ASR] HTTP port  : " << options.port << "\n";
}

bool printValidationErrors(const std::vector<std::string>& config_errors) {
    if (config_errors.empty()) {
        return false;
    }

    std::cerr << "[ASR] Invalid runtime configuration:\n";
    for (const auto& err : config_errors) {
        std::cerr << "  - " << err << "\n";
    }
    return true;
}

void printRuntimeConfig(const AsrRuntimeConfig& config) {
    std::cout << "[ASR] Max audio body: " << config.max_audio_body_bytes << " bytes\n";
    std::cout << "[ASR] Max PCM body  : " << config.max_pcm_body_bytes << " bytes\n";
    std::cout << "[ASR] Lock timeout  : " << config.engine_lock_timeout.count() << "s\n";
    std::cout << "[ASR] RT enabled    : "
              << (config.realtime_enabled ? "true" : "false") << "\n";
    std::cout << "[ASR] RT sessions   : " << config.realtime_max_sessions << "\n";
    std::cout << "[ASR] RT pending MB : "
              << (config.realtime_max_pending_pcm_bytes_per_session / (1024ULL * 1024ULL))
              << " (per-session)\n";
    std::cout << "[ASR] RT total MB   : "
              << (config.realtime_max_total_pending_pcm_bytes / (1024ULL * 1024ULL))
              << " (global)\n";
    std::cout << "[ASR] Legacy deprec : "
              << (config.legacy_deprecation_enabled ? "enabled" : "disabled") << "\n";
    std::cout << "[ASR] Legacy disable: "
              << (config.legacy_hard_disable ? "true" : "false") << "\n";
    std::cout << "[ASR] Legacy compat : "
              << (config.legacy_compat_response ? "true" : "false") << "\n";
    std::cout << "[ASR] Legacy sunset : " << config.legacy_sunset_rfc1123 << "\n";
    if (!config.legacy_migration_doc_url.empty()) {
        std::cout << "[ASR] Legacy guide  : " << config.legacy_migration_doc_url << "\n";
    }
}

int runService(const CliOptions& options) {
    try {
        const AsrRuntimeConfig config =
            AsrRuntimeConfig::fromInputs(options.model_path, options.port, options.vad_model_path);
        const std::vector<std::string> config_errors = config.validate();
        if (printValidationErrors(config_errors)) {
            return 1;
        }

        printRuntimeConfig(config);

        AsrService service(config);
        service.run();   // blocks until the server is stopped
    } catch (const std::exception& e) {
        std::cerr << "[ASR] Fatal error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    CliOptions options;
    const ParseResult parse_result = parseCliArgs(argc, argv, options);
    if (parse_result == ParseResult::kHelp) {
        usage(argv[0]);
        return 0;
    }
    if (parse_result == ParseResult::kError) {
        return 1;
    }

    printCliConfig(options);

    return runService(options);
}
