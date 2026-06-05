// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------

#include "TtsApp.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>

static void usage(const char* prog) {
    std::cerr
        << "\nUsage: " << prog << " [options]\n\n"
        << "Required:\n"
        << "  --model-path <path>    Path to bundled .qnn model file OR directory\n"
        << "                         containing it (e.g. /opt/TTS_binary/MeloTTS/)\n"
        << "                         File is produced by qnn_model_generation.py:\n"
        << "                           melo_en.64_bit.qnn_v2.33.0.qnn  (English)\n"
        << "                           melo_es.64_bit.qnn_v2.33.0.qnn  (Spanish)\n"
        << "                           melo_zh.64_bit.qnn_v2.33.0.qnn  (Chinese)\n\n"
        << "Optional:\n"
        << "  --language <lang>      English | Spanish | Chinese  (default: English)\n"
        << "  --port <port>          HTTP listen port             (default: 8083)\n"
        << "  --speaking-rate <r>    Speaking rate [0.25-4.0]     (default: 1.0)\n"
        << "  --pitch <p>            Pitch [-20.0, 20.0]          (default: 0.0)\n"
        << "  --volume-gain <g>      Volume gain dB [-96.0, 16.0] (default: 0.0)\n"
        << "  --sample-rate <hz>     Output sample rate Hz        (default: 44100)\n"
        << "  -h, --help             Show this help\n\n"
        << "HTTP endpoints:\n"
        << "  GET  /health\n"
        << "  GET  /v1/models\n"
        << "  POST /generate              { \"text\": \"...\" }\n"
        << "  POST /v1/audio/speech       { \"input\": \"...\", \"speed\": 1.0 }\n\n"
        << "Examples:\n"
        << "  " << prog << " --model-path /opt/TTS_binary/MeloTTS/ --language English\n"
        << "  " << prog << " --model-path /opt/TTS_binary/MeloTTS/melo_en.64_bit.qnn_v2.33.0.qnn\n"
        << std::endl;
}

namespace {

struct AppOptions {
    std::string model_path;
    std::string language      = "English";
    int         port          = 8083;
    float       speaking_rate = 1.0f;
    float       pitch         = 0.0f;
    float       volume_gain   = 0.0f;
    int         sample_rate   = 44100;
};

bool takeValue(int& i, int argc, char* argv[], const std::string& option_name,
               std::string& value, std::string& error) {
    if (i + 1 >= argc) {
        error = "Error: " + option_name + " requires a value";
        return false;
    }
    value = argv[++i];
    return true;
}

template <typename T, typename ParseFn>
bool parseAndStoreValue(int& i, int argc, char* argv[], const std::string& option_name,
                        T& target, ParseFn parse_fn, std::string& error) {
    std::string raw;
    if (!takeValue(i, argc, argv, option_name, raw, error)) {
        return false;
    }
    try {
        target = parse_fn(raw);
        return true;
    } catch (const std::exception&) {
        error = "Error: invalid value for " + option_name + ": " + raw;
        return false;
    }
}

bool parseArgs(int argc, char* argv[], AppOptions& options, bool& show_help,
               std::string& error) {
    using Handler = std::function<bool(int&, int, char*[], AppOptions&, std::string&)>;
    static const std::unordered_map<std::string, Handler> handlers = {
        {"--model-path", [](int& i, int argc, char* argv[], AppOptions& opts, std::string& err) {
            return takeValue(i, argc, argv, "--model-path", opts.model_path, err);
        }},
        {"-m", [](int& i, int argc, char* argv[], AppOptions& opts, std::string& err) {
            return takeValue(i, argc, argv, "-m", opts.model_path, err);
        }},
        {"--language", [](int& i, int argc, char* argv[], AppOptions& opts, std::string& err) {
            return takeValue(i, argc, argv, "--language", opts.language, err);
        }},
        {"-l", [](int& i, int argc, char* argv[], AppOptions& opts, std::string& err) {
            return takeValue(i, argc, argv, "-l", opts.language, err);
        }},
        {"--port", [](int& i, int argc, char* argv[], AppOptions& opts, std::string& err) {
            return parseAndStoreValue(i, argc, argv, "--port", opts.port, [](const std::string& v) {
                return std::stoi(v);
            }, err);
        }},
        {"-p", [](int& i, int argc, char* argv[], AppOptions& opts, std::string& err) {
            return parseAndStoreValue(i, argc, argv, "-p", opts.port, [](const std::string& v) {
                return std::stoi(v);
            }, err);
        }},
        {"--speaking-rate", [](int& i, int argc, char* argv[], AppOptions& opts, std::string& err) {
            return parseAndStoreValue(i, argc, argv, "--speaking-rate", opts.speaking_rate, [](const std::string& v) {
                return std::stof(v);
            }, err);
        }},
        {"--pitch", [](int& i, int argc, char* argv[], AppOptions& opts, std::string& err) {
            return parseAndStoreValue(i, argc, argv, "--pitch", opts.pitch, [](const std::string& v) {
                return std::stof(v);
            }, err);
        }},
        {"--volume-gain", [](int& i, int argc, char* argv[], AppOptions& opts, std::string& err) {
            return parseAndStoreValue(i, argc, argv, "--volume-gain", opts.volume_gain, [](const std::string& v) {
                return std::stof(v);
            }, err);
        }},
        {"--sample-rate", [](int& i, int argc, char* argv[], AppOptions& opts, std::string& err) {
            return parseAndStoreValue(i, argc, argv, "--sample-rate", opts.sample_rate, [](const std::string& v) {
                return std::stoi(v);
            }, err);
        }},
    };

    show_help = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            show_help = true;
            return true;
        }
        const auto handler = handlers.find(arg);
        if (handler == handlers.end()) {
            error = "Unknown argument: " + arg;
            return false;
        }
        if (!handler->second(i, argc, argv, options, error)) {
            return false;
        }
    }
    if (options.model_path.empty()) {
        error = "Error: --model-path is required";
        return false;
    }
    return true;
}

void clampOptions(AppOptions& options) {
    if (options.speaking_rate < 0.25f) options.speaking_rate = 0.25f;
    if (options.speaking_rate > 4.0f)  options.speaking_rate = 4.0f;
    if (options.pitch < -20.0f)        options.pitch = -20.0f;
    if (options.pitch > 20.0f)         options.pitch = 20.0f;
    if (options.volume_gain < -96.0f)  options.volume_gain = -96.0f;
    if (options.volume_gain > 16.0f)   options.volume_gain = 16.0f;
}

void printOptions(const AppOptions& options) {
    std::cout
        << "============================================================\n"
        << " MeloTTS Service (melo_sdk)\n"
        << "============================================================\n"
        << "  Model path    : " << options.model_path    << "\n"
        << "  Language      : " << options.language      << "\n"
        << "  Port          : " << options.port          << "\n"
        << "  Speaking rate : " << options.speaking_rate << "\n"
        << "  Pitch         : " << options.pitch         << "\n"
        << "  Volume gain   : " << options.volume_gain   << " dB\n"
        << "  Sample rate   : " << options.sample_rate   << " Hz\n"
        << "============================================================\n";
}

} // namespace

int main(int argc, char* argv[]) {
    AppOptions options;
    bool show_help = false;
    std::string parse_error;

    if (!parseArgs(argc, argv, options, show_help, parse_error)) {
        std::cerr << parse_error << "\n";
        usage(argv[0]);
        return 1;
    }
    if (show_help) {
        usage(argv[0]);
        return 0;
    }

    clampOptions(options);
    printOptions(options);

    try {
        TtsService::TtsApp app(
            options.model_path,
            options.language,
            options.speaking_rate,
            options.pitch,
            options.volume_gain,
            options.sample_rate,
            options.port
        );
        app.run();
    } catch (const std::exception& e) {
        std::cerr << "\n[FATAL] " << e.what() << "\n";
        return 1;
    }

    return 0;
}
