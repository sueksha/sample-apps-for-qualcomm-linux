// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
//
// StableDiffusionEngine.cpp  –  Stable Diffusion 2.1 on Qualcomm HTP (NPU)
//
// Key fix (v2): use QnnSystemContext_getBinaryInfo to obtain the exact tensor
// IDs and tensor version (V2) that the compiled model binary expects, instead
// of hard-coding V1 tensors with ID=0.
// ---------------------------------------------------------------------

#include "StableDiffusionEngine.hpp"

#include "QnnInterface.h"
#include "QnnTypes.h"
#include "QnnBackend.h"
#include "QnnContext.h"
#include "QnnGraph.h"
#include "QnnTensor.h"
#include "QnnLog.h"
#include "QnnDevice.h"
#include "QnnProperty.h"
#include "System/QnnSystemInterface.h"
#include "System/QnnSystemContext.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// ---------------------------------------------------------------------------
// QNN function pointer table
// ---------------------------------------------------------------------------
struct StableDiffusionEngine::QnnFnPtrs {
    QnnBackend_CreateFn_t              backendCreate          = nullptr;
    QnnBackend_FreeFn_t                backendFree            = nullptr;
    QnnBackend_GetBuildIdFn_t          backendGetBuildId      = nullptr;
    QnnLog_CreateFn_t                  logCreate              = nullptr;
    QnnLog_FreeFn_t                    logFree                = nullptr;
    QnnDevice_CreateFn_t               deviceCreate           = nullptr;
    QnnDevice_FreeFn_t                 deviceFree             = nullptr;
    QnnProperty_HasCapabilityFn_t      propertyHasCapability  = nullptr;
    QnnContext_CreateFromBinaryFn_t    contextCreateFromBinary= nullptr;
    QnnContext_FreeFn_t                contextFree            = nullptr;
    QnnGraph_RetrieveFn_t              graphRetrieve          = nullptr;
    QnnGraph_ExecuteFn_t               graphExecute           = nullptr;
    QnnSystemContext_CreateFn_t        sysContextCreate       = nullptr;
    QnnSystemContext_GetBinaryInfoFn_t sysContextGetBinaryInfo= nullptr;
    QnnSystemContext_GetMetaDataFn_t   sysContextGetMetaData  = nullptr;
    QnnSystemContext_FreeFn_t          sysContextFree         = nullptr;
};

// ---------------------------------------------------------------------------
// File-scope helpers
// ---------------------------------------------------------------------------
static void* loadSymbol(void* handle, const char* name) {
    void* sym = dlsym(handle, name);
    if (!sym)
        throw std::runtime_error(std::string("dlsym failed for ") + name + ": " + dlerror());
    return sym;
}

static std::vector<uint8_t> readBinaryFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open model file: " + path);
    const auto size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

// float32 ↔ float16 conversion
static uint16_t f32_to_f16(float v) {
    uint32_t x; std::memcpy(&x, &v, 4);
    uint16_t sign = (x >> 31) & 0x1;
    int32_t  exp  = ((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = (x >> 13) & 0x3FF;
    if (exp <= 0)  return (sign << 15);
    if (exp >= 31) return static_cast<uint16_t>((sign << 15) | (0x1F << 10));
    return static_cast<uint16_t>((sign << 15) | (exp << 10) | mant);
}

static float f16_to_f32(uint16_t h) {
    uint32_t sign = (h >> 15) & 0x1;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t x;
    if (exp == 0)       x = (sign << 31) | (mant << 13);
    else if (exp == 31) x = (sign << 31) | (0xFF << 23) | (mant << 13);
    else                x = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    float v; std::memcpy(&v, &x, 4);
    return v;
}

// ---------------------------------------------------------------------------
// Tensor utility helpers
// ---------------------------------------------------------------------------

// Compute element size in bytes from QNN data type.
// QNN encodes bit-width in BCD in the low byte:
//   0x0216 (FLOAT_16) → low=0x16 → BCD→16 bits → 2 bytes
//   0x0232 (FLOAT_32) → low=0x32 → BCD→32 bits → 4 bytes
//   0x0416 (any 16-bit) → low=0x16 → 2 bytes
static size_t qnnDataTypeBytes(Qnn_DataType_t dtype) {
    uint32_t low = static_cast<uint32_t>(dtype) & 0xFF;
    uint32_t bits = (low >> 4) * 10 + (low & 0xF);
    if (bits == 0) bits = 8;
    return (bits + 7) / 8;
}

static size_t tensorNumElements(const Qnn_Tensor_t& t) {
    uint32_t rank = 0;
    const uint32_t* dims = nullptr;
    if (t.version == QNN_TENSOR_VERSION_2) { rank = t.v2.rank; dims = t.v2.dimensions; }
    else                                   { rank = t.v1.rank; dims = t.v1.dimensions; }
    if (!dims || rank == 0) return 0;
    size_t n = 1;
    for (uint32_t i = 0; i < rank; ++i) n *= dims[i];
    return n;
}

static Qnn_DataType_t tensorDataType(const Qnn_Tensor_t& t) {
    return (t.version == QNN_TENSOR_VERSION_2) ? t.v2.dataType : t.v1.dataType;
}

static void* tensorClientBuf(Qnn_Tensor_t& t) {
    return (t.version == QNN_TENSOR_VERSION_2) ? t.v2.clientBuf.data : t.v1.clientBuf.data;
}

static const void* tensorClientBufConst(const Qnn_Tensor_t& t) {
    return (t.version == QNN_TENSOR_VERSION_2) ? t.v2.clientBuf.data : t.v1.clientBuf.data;
}

static const char* tensorName(const Qnn_Tensor_t& t) {
    return (t.version == QNN_TENSOR_VERSION_2) ? t.v2.name : t.v1.name;
}

struct TensorQuantInfo {
    bool has = false;
    float scale = 1.0f;
    int32_t offset = 0;
};

static TensorQuantInfo tensorQuantInfo(const Qnn_Tensor_t& t) {
    TensorQuantInfo qi;
    const Qnn_QuantizeParams_t* q =
        (t.version == QNN_TENSOR_VERSION_2) ? &t.v2.quantizeParams : &t.v1.quantizeParams;
    if (!q) return qi;
    if (q->quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET) {
        qi.has = true;
        qi.scale = q->scaleOffsetEncoding.scale;
        qi.offset = q->scaleOffsetEncoding.offset;
    }
    return qi;
}

// Deep-copy a tensor from binary info: copies dimensions array, resets client buf.
static Qnn_Tensor_t deepCopyTensor(const Qnn_Tensor_t& src) {
    Qnn_Tensor_t dst = src;
    if (src.version == QNN_TENSOR_VERSION_2) {
        if (src.v2.rank > 0 && src.v2.dimensions) {
            auto* d = new uint32_t[src.v2.rank];
            std::memcpy(d, src.v2.dimensions, src.v2.rank * sizeof(uint32_t));
            dst.v2.dimensions = d;
        }
        dst.v2.isDynamicDimensions = nullptr;  // don't carry dangling pointer
        dst.v2.memType            = QNN_TENSORMEMTYPE_RAW;
        dst.v2.clientBuf.data     = nullptr;
        dst.v2.clientBuf.dataSize = 0;
    } else {
        if (src.v1.rank > 0 && src.v1.dimensions) {
            auto* d = new uint32_t[src.v1.rank];
            std::memcpy(d, src.v1.dimensions, src.v1.rank * sizeof(uint32_t));
            dst.v1.dimensions = d;
        }
        dst.v1.memType            = QNN_TENSORMEMTYPE_RAW;
        dst.v1.clientBuf.data     = nullptr;
        dst.v1.clientBuf.dataSize = 0;
    }
    return dst;
}

// Allocate and zero-fill the client buffer for a tensor.
static void allocClientBuf(Qnn_Tensor_t& t) {
    size_t n         = tensorNumElements(t);
    size_t byte_size = n * qnnDataTypeBytes(tensorDataType(t));
    void*  buf       = (byte_size > 0) ? std::malloc(byte_size) : nullptr;
    if (buf) std::memset(buf, 0, byte_size);
    if (t.version == QNN_TENSOR_VERSION_2) {
        t.v2.clientBuf.data     = buf;
        t.v2.clientBuf.dataSize = static_cast<uint32_t>(byte_size);
    } else {
        t.v1.clientBuf.data     = buf;
        t.v1.clientBuf.dataSize = static_cast<uint32_t>(byte_size);
    }
}

// Find a tensor by name; returns -1 if not found.
static int findTensorByName(const Qnn_Tensor_t* tensors, uint32_t n, const char* name) {
    for (uint32_t i = 0; i < n; ++i) {
        const char* tn = tensorName(tensors[i]);
        if (tn && std::strcmp(tn, name) == 0) return static_cast<int>(i);
    }
    return -1;
}

static bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

static bool envEnabled(const char* name) {
    const char* value = std::getenv(name);
    if (!value) return false;
    const std::string v(value);
    return v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "YES";
}

static std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out += "'";
    return out;
}

static bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

static bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string pickFirstBinByPrefixes(const std::string& model_dir,
                                          std::initializer_list<const char*> prefixes) {
    std::vector<std::string> hits;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(model_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (!endsWith(name, ".bin")) continue;
        for (const char* prefix : prefixes) {
            if (prefix && startsWith(name, prefix)) {
                hits.push_back(name);
                break;
            }
        }
    }
    if (hits.empty()) return {};
    std::sort(hits.begin(), hits.end());
    return hits.front();
}

static std::string pickRunnerContextArg(const std::string& model_dir,
                                        const char* env_name,
                                        std::initializer_list<const char*> candidates,
                                        std::initializer_list<const char*> wildcard_prefixes) {
    if (const char* forced = std::getenv(env_name)) {
        if (forced[0] != '\0') {
            return std::string(forced);
        }
    }
    for (const char* candidate : candidates) {
        if (!candidate || candidate[0] == '\0') continue;
        const std::string p = model_dir + candidate;
        if (fileExists(p)) {
            return std::string(candidate);
        }
    }
    const std::string wildcard_match = pickFirstBinByPrefixes(model_dir, wildcard_prefixes);
    if (!wildcard_match.empty()) {
        return wildcard_match;
    }
    // Backward-compatible default if nothing exists.
    return std::string(*candidates.begin());
}

static std::string mergedEnvPath(char delimiter,
                                 std::initializer_list<std::string> preferred_paths,
                                 const char* inherited) {
    std::vector<std::string> ordered;
    auto add = [&](const std::string& value) {
        if (value.empty()) return;
        if (std::find(ordered.begin(), ordered.end(), value) == ordered.end()) {
            ordered.push_back(value);
        }
    };

    for (const std::string& p : preferred_paths) add(p);

    if (inherited && inherited[0] != '\0') {
        std::stringstream ss(inherited);
        std::string item;
        while (std::getline(ss, item, delimiter)) {
            if (!item.empty()) add(item);
        }
    }

    std::ostringstream out;
    for (size_t i = 0; i < ordered.size(); ++i) {
        if (i > 0) out << delimiter;
        out << ordered[i];
    }
    return out.str();
}

static std::string firstNonEmptyEnv(std::initializer_list<const char*> names) {
    for (const char* name : names) {
        if (!name) continue;
        if (const char* value = std::getenv(name)) {
            if (value[0] != '\0') return std::string(value);
        }
    }
    return {};
}

static std::string pickModelBinaryPath(
    const std::string& model_dir,
    std::initializer_list<const char*> candidates,
    std::initializer_list<const char*> wildcard_prefixes) {
    for (const char* name : candidates) {
        if (!name || name[0] == '\0') continue;
        const std::string path = model_dir + name;
        if (fileExists(path)) return path;
    }

    const std::string wildcard = pickFirstBinByPrefixes(model_dir, wildcard_prefixes);
    if (!wildcard.empty()) return model_dir + wildcard;

    std::string tried;
    for (const char* name : candidates) {
        if (!name || name[0] == '\0') continue;
        if (!tried.empty()) tried += ", ";
        tried += model_dir + name;
    }
    if (wildcard_prefixes.size() > 0) {
        tried += ", wildcard prefixes: ";
        bool first = true;
        for (const char* prefix : wildcard_prefixes) {
            if (!first) tried += "|";
            tried += std::string(prefix ? prefix : "") + "*.bin";
            first = false;
        }
    }
    throw std::runtime_error("Missing model binary; tried: " + tried);
}

struct DirectRunnerConfig {
    int num_steps = 20;
    std::string qnn_lib_dir;
    std::string qnn_adsp_lib_dir;
    std::string text_context;
    std::string unet_context;
    std::string vae_context;
    std::string ld_env;
    std::string adsp_env;
};

static DirectRunnerConfig buildDirectRunnerConfig(const std::string& model_dir,
                                                  int requested_steps) {
    DirectRunnerConfig cfg;
    cfg.num_steps = std::max(1, std::min(50, requested_steps));
    cfg.qnn_lib_dir = firstNonEmptyEnv({"QAIRT_LIB_DIR", "QNN_LIB_DIR"});
    if (cfg.qnn_lib_dir.empty()) cfg.qnn_lib_dir = model_dir;

    cfg.qnn_adsp_lib_dir = firstNonEmptyEnv({"QAIRT_ADSP_LIB_DIR", "QNN_ADSP_LIB_DIR"});
    if (cfg.qnn_adsp_lib_dir.empty()) cfg.qnn_adsp_lib_dir = cfg.qnn_lib_dir;

    cfg.text_context = pickRunnerContextArg(
        model_dir,
        "SD21_TEXT_CONTEXT",
        {"text_encoder.bin",
         "text_encoder_qairt_context.bin",
         "stable_diffusion_v2_1-text_encoder-qualcomm_qcs9075.bin"},
        {"text_encoder"});
    cfg.unet_context = pickRunnerContextArg(
        model_dir,
        "SD21_UNET_CONTEXT",
        {"unet.bin",
         "unet_qairt_context.bin",
         "stable_diffusion_v2_1-unet-qualcomm_qcs9075.bin"},
        {"unet"});
    cfg.vae_context = pickRunnerContextArg(
        model_dir,
        "SD21_VAE_CONTEXT",
        {"vae.bin",
         "vae_decoder.bin",
         "vae_qairt_context.bin",
         "stable_diffusion_v2_1-vae-qualcomm_qcs9075.bin"},
        {"vae"});

    cfg.ld_env = mergedEnvPath(':', {cfg.qnn_lib_dir, model_dir}, std::getenv("LD_LIBRARY_PATH"));
    cfg.adsp_env = mergedEnvPath(';',
                                 {cfg.qnn_adsp_lib_dir, cfg.qnn_lib_dir, model_dir},
                                 std::getenv("ADSP_LIBRARY_PATH"));
    return cfg;
}

static std::string buildDirectRunnerCommand(const std::string& runner,
                                            const std::string& model_dir,
                                            const std::string& tokenizer_dir,
                                            const DirectRunnerConfig& cfg,
                                            const GenerationRequest& req,
                                            const std::string& out_ppm,
                                            const std::string& out_log) {
    std::ostringstream cmd;
    cmd << "LD_LIBRARY_PATH=" << shellQuote(cfg.ld_env)
        << " ADSP_LIBRARY_PATH=" << shellQuote(cfg.adsp_env) << " "
        << shellQuote(runner)
        << " --runtime_dir " << shellQuote(model_dir)
        << " --qnn_lib_dir " << shellQuote(cfg.qnn_lib_dir)
        << " --tokenizer_dir " << shellQuote(tokenizer_dir)
        << " --text_context " << shellQuote(cfg.text_context)
        << " --unet_context " << shellQuote(cfg.unet_context)
        << " --vae_context " << shellQuote(cfg.vae_context)
        << " --prompt " << shellQuote(req.prompt)
        << " --negative_prompt " << shellQuote(req.negative_prompt)
        << " --steps " << cfg.num_steps
        << " --guidance " << req.guidance_scale
        << " --seed " << req.seed
        << " --output " << shellQuote(out_ppm)
        << " --log_file " << shellQuote(out_log);

    const std::string prediction_type_env = firstNonEmptyEnv({"SD21_PREDICTION_TYPE"});
    const std::string vae_scaling_factor_env = firstNonEmptyEnv({"SD21_VAE_SCALING_FACTOR"});

    // Optional explicit tuning overrides for controlled A/B runs.
    // Request-level overrides take priority over process env overrides.
    if (!req.prediction_type_override.empty()) {
        cmd << " --prediction_type " << shellQuote(req.prediction_type_override);
    } else if (!prediction_type_env.empty()) {
        cmd << " --prediction_type " << shellQuote(prediction_type_env);
    }
    if (req.vae_scaling_factor_override > 0.0f) {
        cmd << " --vae_scaling_factor " << req.vae_scaling_factor_override;
    } else if (!vae_scaling_factor_env.empty()) {
        cmd << " --vae_scaling_factor " << shellQuote(vae_scaling_factor_env);
    }

    cmd << " >/dev/null 2>&1";
    return cmd.str();
}

// Extract first-graph metadata from binary info (handles V1/V2/V3).
static bool extractGraphMeta(const QnnSystemContext_BinaryInfo_t* bin_info,
                             const char** graph_name,
                             const Qnn_Tensor_t** inputs,
                             uint32_t* num_in,
                             const Qnn_Tensor_t** outputs,
                             uint32_t* num_out) {
    if (!bin_info) return false;

    auto handle_graph = [&](const QnnSystemContext_GraphInfo_t& gi) {
        if (gi.version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1) {
            *graph_name = gi.graphInfoV1.graphName;
            *inputs  = gi.graphInfoV1.graphInputs;  *num_in  = gi.graphInfoV1.numGraphInputs;
            *outputs = gi.graphInfoV1.graphOutputs; *num_out = gi.graphInfoV1.numGraphOutputs;
        } else if (gi.version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2) {
            *graph_name = gi.graphInfoV2.graphName;
            *inputs  = gi.graphInfoV2.graphInputs;  *num_in  = gi.graphInfoV2.numGraphInputs;
            *outputs = gi.graphInfoV2.graphOutputs; *num_out = gi.graphInfoV2.numGraphOutputs;
        } else if (gi.version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_3) {
            *graph_name = gi.graphInfoV3.graphName;
            *inputs  = gi.graphInfoV3.graphInputs;  *num_in  = gi.graphInfoV3.numGraphInputs;
            *outputs = gi.graphInfoV3.graphOutputs; *num_out = gi.graphInfoV3.numGraphOutputs;
        }
    };

    if (bin_info->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1) {
        const auto& v = bin_info->contextBinaryInfoV1;
        if (v.numGraphs > 0) { handle_graph(v.graphs[0]); return true; }
    } else if (bin_info->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2) {
        const auto& v = bin_info->contextBinaryInfoV2;
        if (v.numGraphs > 0) { handle_graph(v.graphs[0]); return true; }
    } else if (bin_info->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_3) {
        const auto& v = bin_info->contextBinaryInfoV3;
        if (v.numGraphs > 0) { handle_graph(v.graphs[0]); return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
StableDiffusionEngine::StableDiffusionEngine(const std::string& model_dir, const std::string& tokenizer_dir)
    : model_dir_(model_dir), tokenizer_dir_(tokenizer_dir), qnn_(std::make_unique<QnnFnPtrs>())
{
    if (!model_dir_.empty() && model_dir_.back() != '/')
        model_dir_ += '/';
    if (!tokenizer_dir_.empty() && tokenizer_dir_.back() != '/')
        tokenizer_dir_ += '/';
}

StableDiffusionEngine::~StableDiffusionEngine() { destroy(); }

std::string StableDiffusionEngine::runnerBinaryPath() const {
    if (const char* forced = std::getenv("SD21_RUNNER_PATH")) {
        if (forced[0] != '\0' && fileExists(forced)) {
            return std::string(forced);
        }
    }

    const std::string runtime_runner = model_dir_ + "cpp_sd21_qnn_direct/sd21_qnn_cpp_direct";
    const std::string image_runner = "/usr/bin/sd21_qnn_cpp_direct";

    const bool prefer_image = envEnabled("SD21_PREFER_IMAGE_RUNNER");
    if (prefer_image) {
        if (fileExists(image_runner)) return image_runner;
        if (fileExists(runtime_runner)) return runtime_runner;
    } else {
        // Default to runtime-mounted runner first because it is often paired
        // with model-export-specific context metadata fixes.
        if (fileExists(runtime_runner)) return runtime_runner;
        if (fileExists(image_runner)) return image_runner;
    }

    return {};
}

// ---------------------------------------------------------------------------
// initialize
// ---------------------------------------------------------------------------
void StableDiffusionEngine::initialize() {
    if (use_external_runner_) {
        const std::string runner = runnerBinaryPath();
        if (!runner.empty()) {
            if (!fileExists(tokenizer_dir_ + "vocab.json") ||
                !fileExists(tokenizer_dir_ + "merges.txt")) {
                throw std::runtime_error("Missing tokenizer files in: " + tokenizer_dir_);
            }
            ready_ = true;
            std::cout << "[SD] External direct-runner mode enabled: " << runner << "\n";
            return;
        }
        std::cout << "[SD] Direct runner not found, falling back to in-process QNN execution: "
                  << runner << "\n";
        use_external_runner_ = false;
    }

    std::cout << "[SD] Initializing Stable Diffusion engine\n"
              << "[SD] Model dir: " << model_dir_ << "\n"
              << "[SD] Init thread id: " << std::this_thread::get_id() << "\n";
    tokenizer_.load(tokenizer_dir_);
    loadQnnBackend();
    createQnnBackend();
    createQnnDevice();
    loadModels();
    ready_ = true;
    std::cout << "[SD] Engine ready\n";
}

// ---------------------------------------------------------------------------
// loadQnnBackend
// ---------------------------------------------------------------------------
void StableDiffusionEngine::loadQnnBackend() {
    const char* htp_lib = "libQnnHtp.so";
    backend_lib_handle_ = dlopen(htp_lib, RTLD_NOW | RTLD_LOCAL);
    if (!backend_lib_handle_)
        throw std::runtime_error(std::string("Cannot load ") + htp_lib + ": " + dlerror());

    using GetProvidersFn = Qnn_ErrorHandle_t (*)(const QnnInterface_t***, uint32_t*);
    auto getProviders = reinterpret_cast<GetProvidersFn>(
        loadSymbol(backend_lib_handle_, "QnnInterface_getProviders"));

    const QnnInterface_t** providers = nullptr;
    uint32_t num_providers = 0;
    if (QNN_SUCCESS != getProviders(&providers, &num_providers) || num_providers == 0)
        throw std::runtime_error("QnnInterface_getProviders returned no providers");

    const QnnInterface_t& iface = *providers[0];
    qnn_->backendCreate          = iface.QNN_INTERFACE_VER_NAME.backendCreate;
    qnn_->backendFree            = iface.QNN_INTERFACE_VER_NAME.backendFree;
    qnn_->backendGetBuildId      = iface.QNN_INTERFACE_VER_NAME.backendGetBuildId;
    qnn_->logCreate              = iface.QNN_INTERFACE_VER_NAME.logCreate;
    qnn_->logFree                = iface.QNN_INTERFACE_VER_NAME.logFree;
    qnn_->deviceCreate           = iface.QNN_INTERFACE_VER_NAME.deviceCreate;
    qnn_->deviceFree             = iface.QNN_INTERFACE_VER_NAME.deviceFree;
    qnn_->propertyHasCapability  = iface.QNN_INTERFACE_VER_NAME.propertyHasCapability;
    qnn_->contextCreateFromBinary= iface.QNN_INTERFACE_VER_NAME.contextCreateFromBinary;
    qnn_->contextFree            = iface.QNN_INTERFACE_VER_NAME.contextFree;
    qnn_->graphRetrieve          = iface.QNN_INTERFACE_VER_NAME.graphRetrieve;
    qnn_->graphExecute           = iface.QNN_INTERFACE_VER_NAME.graphExecute;

    system_lib_handle_ = dlopen("libQnnSystem.so", RTLD_NOW | RTLD_LOCAL);
    if (!system_lib_handle_)
        throw std::runtime_error(std::string("Cannot load libQnnSystem.so: ") + dlerror());

    using GetSysProvidersFn = Qnn_ErrorHandle_t (*)(const QnnSystemInterface_t***, uint32_t*);
    auto getSysProviders = reinterpret_cast<GetSysProvidersFn>(
        loadSymbol(system_lib_handle_, "QnnSystemInterface_getProviders"));

    const QnnSystemInterface_t** sys_providers = nullptr;
    uint32_t num_sys = 0;
    if (QNN_SUCCESS != getSysProviders(&sys_providers, &num_sys) || num_sys == 0)
        throw std::runtime_error("QnnSystemInterface_getProviders returned no providers");

    const QnnSystemInterface_t& siface = *sys_providers[0];
    qnn_->sysContextCreate        = siface.QNN_SYSTEM_INTERFACE_VER_NAME.systemContextCreate;
    qnn_->sysContextGetBinaryInfo = siface.QNN_SYSTEM_INTERFACE_VER_NAME.systemContextGetBinaryInfo;
    qnn_->sysContextGetMetaData   = siface.QNN_SYSTEM_INTERFACE_VER_NAME.systemContextGetMetaData;
    qnn_->sysContextFree          = siface.QNN_SYSTEM_INTERFACE_VER_NAME.systemContextFree;

    std::cout << "[SD] QNN backend loaded\n";
}

// ---------------------------------------------------------------------------
// createQnnBackend
// ---------------------------------------------------------------------------
void StableDiffusionEngine::createQnnBackend() {
    if (qnn_->logCreate)
        qnn_->logCreate(nullptr, QNN_LOG_LEVEL_ERROR,
                        reinterpret_cast<Qnn_LogHandle_t*>(&qnn_log_handle_));

    if (QNN_BACKEND_NO_ERROR !=
        qnn_->backendCreate(reinterpret_cast<Qnn_LogHandle_t>(qnn_log_handle_),
                            nullptr,
                            reinterpret_cast<Qnn_BackendHandle_t*>(&qnn_backend_handle_)))
        throw std::runtime_error("QNN backendCreate failed");

    std::cout << "[SD] QNN backend created\n";
}

// ---------------------------------------------------------------------------
// createQnnDevice
// ---------------------------------------------------------------------------
void StableDiffusionEngine::createQnnDevice() {
    if (!qnn_->deviceCreate) return;

    if (qnn_->propertyHasCapability) {
        if (QNN_PROPERTY_NOT_SUPPORTED == qnn_->propertyHasCapability(QNN_PROPERTY_GROUP_DEVICE)) {
            std::cout << "[SD] Device property not supported, skipping\n";
            return;
        }
    }

    auto status = qnn_->deviceCreate(
        reinterpret_cast<Qnn_LogHandle_t>(qnn_log_handle_),
        nullptr,
        reinterpret_cast<Qnn_DeviceHandle_t*>(&qnn_device_handle_));

    if (QNN_SUCCESS != status && QNN_DEVICE_ERROR_UNSUPPORTED_FEATURE != status)
        throw std::runtime_error("QNN deviceCreate failed");

    std::cout << "[SD] QNN device created\n";
}

// ---------------------------------------------------------------------------
// loadModels
// ---------------------------------------------------------------------------
void StableDiffusionEngine::loadModels() {
    const std::string model_names[NUM_MODELS] = {
        pickModelBinaryPath(model_dir_,
                            {"text_encoder.bin",
                             "text_encoder_qairt_context.bin",
                             "stable_diffusion_v2_1-text_encoder-qualcomm_qcs9075.bin"},
                            {"text_encoder"}),
        pickModelBinaryPath(model_dir_,
                            {"unet.bin",
                             "unet_qairt_context.bin",
                             "stable_diffusion_v2_1-unet-qualcomm_qcs9075.bin"},
                            {"unet"}),
        pickModelBinaryPath(model_dir_,
                            {"vae.bin",
                             "vae_decoder.bin",
                             "vae_qairt_context.bin",
                             "stable_diffusion_v2_1-vae-qualcomm_qcs9075.bin"},
                            {"vae"})
    };

    for (int i = 0; i < NUM_MODELS; ++i) {
        loadSingleModel(i, model_names[i]);
    }
}

void StableDiffusionEngine::loadSingleModel(int idx, const std::string& model_name) {
    if (idx < 0 || idx >= NUM_MODELS) {
        throw std::invalid_argument("Invalid model index: " + std::to_string(idx));
    }

    std::cout << "[SD] Loading model: " << model_name << "\n";

    context_binaries_[idx] = readBinaryFile(model_name);
    auto& buf = context_binaries_[idx];

    QnnSystemContext_Handle_t sys_ctx = nullptr;
    auto freeSysCtx = [&]() {
        if (sys_ctx) {
            qnn_->sysContextFree(sys_ctx);
            sys_ctx = nullptr;
        }
    };

    if (QNN_SUCCESS != qnn_->sysContextCreate(&sys_ctx))
        throw std::runtime_error("sysContextCreate failed for " + model_name);

    const QnnSystemContext_BinaryInfo_t* bin_info = nullptr;
    Qnn_ContextBinarySize_t bin_info_size = 0;
    Qnn_ErrorHandle_t info_status = QNN_SYSTEM_CONTEXT_ERROR_OPERATION_FAILED;
    if (qnn_->sysContextGetBinaryInfo) {
        info_status = qnn_->sysContextGetBinaryInfo(
            sys_ctx, buf.data(), buf.size(), &bin_info, &bin_info_size);
    }
    if ((info_status != QNN_SUCCESS || !bin_info) && qnn_->sysContextGetMetaData) {
        info_status = qnn_->sysContextGetMetaData(sys_ctx, buf.data(), buf.size(), &bin_info);
    }
    if (info_status != QNN_SUCCESS || !bin_info) {
        freeSysCtx();
        throw std::runtime_error(
            "systemContextGetBinaryInfo/systemContextGetMetaData failed for " + model_name);
    }

    Qnn_ContextHandle_t ctx = nullptr;
    if (QNN_SUCCESS != qnn_->contextCreateFromBinary(
            reinterpret_cast<Qnn_BackendHandle_t>(qnn_backend_handle_),
            reinterpret_cast<Qnn_DeviceHandle_t>(qnn_device_handle_),
            nullptr,
            buf.data(), buf.size(),
            &ctx, nullptr)) {
        freeSysCtx();
        throw std::runtime_error("contextCreateFromBinary failed for " + model_name);
    }
    qnn_context_[idx] = ctx;

    const char* graph_name = nullptr;
    const Qnn_Tensor_t* graph_inputs = nullptr;
    const Qnn_Tensor_t* graph_outputs = nullptr;
    uint32_t num_in = 0;
    uint32_t num_out = 0;
    if (!extractGraphMeta(bin_info, &graph_name, &graph_inputs, &num_in, &graph_outputs, &num_out) ||
        !graph_name || !*graph_name) {
        freeSysCtx();
        throw std::runtime_error("Could not extract graph metadata for " + model_name);
    }
    const std::string graph_name_owned(graph_name);

    Qnn_GraphHandle_t graph = nullptr;
    if (QNN_SUCCESS != qnn_->graphRetrieve(ctx, graph_name_owned.c_str(), &graph)) {
        freeSysCtx();
        throw std::runtime_error(std::string("graphRetrieve failed for ") + graph_name_owned);
    }
    graph_handle_[idx] = graph;

    setupTensors(idx, bin_info);
    freeSysCtx();

    std::cout << "[SD] Model " << idx << " loaded graph='" << graph_name_owned << "' ("
              << num_inputs_[idx] << " inputs, "
              << num_outputs_[idx] << " outputs)\n";
}

// ---------------------------------------------------------------------------
// setupTensors  –  deep-copy tensors from binary info, allocate client buffers
// ---------------------------------------------------------------------------
void StableDiffusionEngine::setupTensors(int idx,
                                          const QnnSystemContext_BinaryInfo_t* bin_info) {
    const char* graph_name = nullptr;
    const Qnn_Tensor_t* graph_inputs  = nullptr;
    const Qnn_Tensor_t* graph_outputs = nullptr;
    uint32_t num_in = 0, num_out = 0;

    if (!extractGraphMeta(bin_info, &graph_name, &graph_inputs, &num_in, &graph_outputs, &num_out)
        || !graph_inputs || !graph_outputs || num_in == 0 || num_out == 0) {
        throw std::runtime_error(
            "Failed to extract tensor info from binary info for model " + std::to_string(idx));
    }

    num_inputs_[idx]  = num_in;
    num_outputs_[idx] = num_out;

    auto* inputs  = new Qnn_Tensor_t[num_in];
    auto* outputs = new Qnn_Tensor_t[num_out];

    for (uint32_t i = 0; i < num_in; ++i) {
        inputs[i] = deepCopyTensor(graph_inputs[i]);
        allocClientBuf(inputs[i]);
        const char* name = tensorName(inputs[i]);
        uint32_t id = (inputs[i].version == QNN_TENSOR_VERSION_2)
                      ? inputs[i].v2.id : inputs[i].v1.id;
        std::cout << "[SD]   in[" << i << "] id=" << id
                  << " name=" << (name ? name : "?")
                  << " elems=" << tensorNumElements(inputs[i])
                  << " dtype=0x" << std::hex
                  << static_cast<uint32_t>(tensorDataType(inputs[i]))
                  << std::dec;
        const auto q = tensorQuantInfo(inputs[i]);
        if (q.has) {
            std::cout << " q(scale=" << q.scale << ",offset=" << q.offset << ")";
        }
        std::cout << "\n";
    }

    for (uint32_t i = 0; i < num_out; ++i) {
        outputs[i] = deepCopyTensor(graph_outputs[i]);
        allocClientBuf(outputs[i]);
        const char* name = tensorName(outputs[i]);
        uint32_t id = (outputs[i].version == QNN_TENSOR_VERSION_2)
                      ? outputs[i].v2.id : outputs[i].v1.id;
        std::cout << "[SD]   out[" << i << "] id=" << id
                  << " name=" << (name ? name : "?")
                  << " elems=" << tensorNumElements(outputs[i])
                  << " dtype=0x" << std::hex
                  << static_cast<uint32_t>(tensorDataType(outputs[i]))
                  << std::dec;
        const auto q = tensorQuantInfo(outputs[i]);
        if (q.has) {
            std::cout << " q(scale=" << q.scale << ",offset=" << q.offset << ")";
        }
        std::cout << "\n";
    }

    input_tensors_[idx]  = inputs;
    output_tensors_[idx] = outputs;
}

// ---------------------------------------------------------------------------
// freeTensors
// ---------------------------------------------------------------------------
void StableDiffusionEngine::freeTensors(int idx) {
    auto free_array = [](void* ptr, uint32_t n) {
        if (!ptr) return;
        auto* t = reinterpret_cast<Qnn_Tensor_t*>(ptr);
        for (uint32_t i = 0; i < n; ++i) {
            if (t[i].version == QNN_TENSOR_VERSION_2) {
                if (t[i].v2.clientBuf.data) std::free(t[i].v2.clientBuf.data);
                delete[] t[i].v2.dimensions;
            } else {
                if (t[i].v1.clientBuf.data) std::free(t[i].v1.clientBuf.data);
                delete[] t[i].v1.dimensions;
            }
        }
        delete[] t;
    };
    free_array(input_tensors_[idx],  num_inputs_[idx]);
    free_array(output_tensors_[idx], num_outputs_[idx]);
    input_tensors_[idx]  = nullptr;
    output_tensors_[idx] = nullptr;
}

// ---------------------------------------------------------------------------
// copyFloatToTensor  –  float32 → tensor (handles float32 and any 16-bit type)
// ---------------------------------------------------------------------------
void StableDiffusionEngine::copyFloatToTensor(const float* src, size_t count,
                                               Qnn_Tensor_t* dst) {
    void* buf = tensorClientBuf(*dst);
    if (!buf) return;
    const auto q = tensorQuantInfo(*dst);
    switch (tensorDataType(*dst)) {
        case QNN_DATATYPE_FLOAT_32: {
            std::memcpy(buf, src, count * sizeof(float));
            break;
        }
        case QNN_DATATYPE_FLOAT_16: {
            auto* d16 = reinterpret_cast<uint16_t*>(buf);
            for (size_t i = 0; i < count; ++i) d16[i] = f32_to_f16(src[i]);
            break;
        }
        case QNN_DATATYPE_INT_32: {
            auto* d = reinterpret_cast<int32_t*>(buf);
            for (size_t i = 0; i < count; ++i) d[i] = static_cast<int32_t>(src[i]);
            break;
        }
        case QNN_DATATYPE_UINT_32: {
            auto* d = reinterpret_cast<uint32_t*>(buf);
            for (size_t i = 0; i < count; ++i) d[i] = static_cast<uint32_t>(std::max(0.0f, src[i]));
            break;
        }
        case QNN_DATATYPE_UFIXED_POINT_16: {
            auto* d = reinterpret_cast<uint16_t*>(buf);
            for (size_t i = 0; i < count; ++i) {
                float qf = src[i];
                if (q.has && q.scale > 0.0f) qf = qf / q.scale - static_cast<float>(q.offset);
                const long long qv = static_cast<long long>(std::llround(qf));
                d[i] = static_cast<uint16_t>(std::clamp<long long>(qv, 0, 65535));
            }
            break;
        }
        case QNN_DATATYPE_SFIXED_POINT_16: {
            auto* d = reinterpret_cast<int16_t*>(buf);
            for (size_t i = 0; i < count; ++i) {
                float qf = src[i];
                if (q.has && q.scale > 0.0f) qf = qf / q.scale - static_cast<float>(q.offset);
                const long long qv = static_cast<long long>(std::llround(qf));
                d[i] = static_cast<int16_t>(
                    std::clamp<long long>(qv, std::numeric_limits<int16_t>::min(),
                                          std::numeric_limits<int16_t>::max()));
            }
            break;
        }
        default:
            throw std::runtime_error("Unsupported input tensor datatype");
    }
}

// ---------------------------------------------------------------------------
// copyTensorToFloat  –  tensor → float32
// ---------------------------------------------------------------------------
void StableDiffusionEngine::copyTensorToFloat(const Qnn_Tensor_t* src,
                                               float* dst, size_t count) {
    const void* buf = tensorClientBufConst(*src);
    if (!buf) return;
    const auto q = tensorQuantInfo(*src);
    switch (tensorDataType(*src)) {
        case QNN_DATATYPE_FLOAT_32: {
            std::memcpy(dst, buf, count * sizeof(float));
            break;
        }
        case QNN_DATATYPE_FLOAT_16: {
            const auto* s16 = reinterpret_cast<const uint16_t*>(buf);
            for (size_t i = 0; i < count; ++i) dst[i] = f16_to_f32(s16[i]);
            break;
        }
        case QNN_DATATYPE_INT_32: {
            const auto* s = reinterpret_cast<const int32_t*>(buf);
            for (size_t i = 0; i < count; ++i) dst[i] = static_cast<float>(s[i]);
            break;
        }
        case QNN_DATATYPE_UINT_32: {
            const auto* s = reinterpret_cast<const uint32_t*>(buf);
            for (size_t i = 0; i < count; ++i) dst[i] = static_cast<float>(s[i]);
            break;
        }
        case QNN_DATATYPE_UFIXED_POINT_16: {
            const auto* s = reinterpret_cast<const uint16_t*>(buf);
            for (size_t i = 0; i < count; ++i) {
                const float raw = static_cast<float>(s[i]);
                dst[i] = q.has ? (raw + static_cast<float>(q.offset)) * q.scale : raw;
            }
            break;
        }
        case QNN_DATATYPE_SFIXED_POINT_16: {
            const auto* s = reinterpret_cast<const int16_t*>(buf);
            for (size_t i = 0; i < count; ++i) {
                const float raw = static_cast<float>(s[i]);
                dst[i] = q.has ? (raw + static_cast<float>(q.offset)) * q.scale : raw;
            }
            break;
        }
        default:
            throw std::runtime_error("Unsupported output tensor datatype");
    }
}

// ---------------------------------------------------------------------------
// runTextEncoder
// ---------------------------------------------------------------------------
std::vector<float> StableDiffusionEngine::runTextEncoder(
    const std::vector<float>& tokens) {

    auto* inputs  = reinterpret_cast<Qnn_Tensor_t*>(input_tensors_[0]);
    auto* outputs = reinterpret_cast<Qnn_Tensor_t*>(output_tensors_[0]);

    copyFloatToTensor(tokens.data(), tokens.size(), &inputs[0]);

    const auto status = qnn_->graphExecute(
        reinterpret_cast<Qnn_GraphHandle_t>(graph_handle_[0]),
        inputs,  num_inputs_[0],
        outputs, num_outputs_[0],
        nullptr, nullptr);
    if (status != QNN_SUCCESS) {
        throw std::runtime_error("text_encoder graphExecute failed: " + std::to_string(status));
    }

    const size_t out_count = tensorNumElements(outputs[0]);
    std::vector<float> emb(out_count);
    copyTensorToFloat(&outputs[0], emb.data(), out_count);
    return emb;
}

// ---------------------------------------------------------------------------
// runUnet  –  finds inputs by name to handle any ordering
// The UNet model takes a scalar timestep value (not a 1280-dim embedding).
// The model computes the sinusoidal embedding internally.
// ---------------------------------------------------------------------------
std::vector<float> StableDiffusionEngine::runUnet(
    const std::vector<float>& latent_nhwc,
    float                     timestep_val,
    const std::vector<float>& text_emb) {

    auto* inputs  = reinterpret_cast<Qnn_Tensor_t*>(input_tensors_[1]);
    auto* outputs = reinterpret_cast<Qnn_Tensor_t*>(output_tensors_[1]);

    // Find inputs by name; fall back to positional order if names differ
    int li = findTensorByName(inputs, num_inputs_[1], "latent");
    int ti = findTensorByName(inputs, num_inputs_[1], "timestep");
    int ei = findTensorByName(inputs, num_inputs_[1], "text_emb");
    if (li < 0) li = 0;
    if (ti < 0) ti = 1;
    if (ei < 0) ei = 2;

    // timestep is a single scalar value (model computes embedding internally)
    copyFloatToTensor(latent_nhwc.data(), latent_nhwc.size(), &inputs[li]);
    copyFloatToTensor(&timestep_val, 1,                        &inputs[ti]);
    copyFloatToTensor(text_emb.data(), text_emb.size(),        &inputs[ei]);

    const auto status = qnn_->graphExecute(
        reinterpret_cast<Qnn_GraphHandle_t>(graph_handle_[1]),
        inputs,  num_inputs_[1],
        outputs, num_outputs_[1],
        nullptr, nullptr);
    if (status != QNN_SUCCESS) {
        throw std::runtime_error("unet graphExecute failed: " + std::to_string(status));
    }

    const size_t out_count = tensorNumElements(outputs[0]);
    std::vector<float> noise(out_count);
    copyTensorToFloat(&outputs[0], noise.data(), out_count);
    return noise;
}

// ---------------------------------------------------------------------------
// runVaeDecoder
// ---------------------------------------------------------------------------
std::vector<float> StableDiffusionEngine::runVaeDecoder(
    const std::vector<float>& latent_nhwc) {

    auto* inputs  = reinterpret_cast<Qnn_Tensor_t*>(input_tensors_[2]);
    auto* outputs = reinterpret_cast<Qnn_Tensor_t*>(output_tensors_[2]);

    copyFloatToTensor(latent_nhwc.data(), latent_nhwc.size(), &inputs[0]);

    const auto status = qnn_->graphExecute(
        reinterpret_cast<Qnn_GraphHandle_t>(graph_handle_[2]),
        inputs,  num_inputs_[2],
        outputs, num_outputs_[2],
        nullptr, nullptr);
    if (status != QNN_SUCCESS) {
        throw std::runtime_error("vae graphExecute failed: " + std::to_string(status));
    }

    const size_t out_count = tensorNumElements(outputs[0]);
    std::vector<float> pixels(out_count);
    copyTensorToFloat(&outputs[0], pixels.data(), out_count);
    return pixels;
}

// ---------------------------------------------------------------------------
// encodePng
// ---------------------------------------------------------------------------
/*static*/
std::vector<uint8_t> StableDiffusionEngine::encodePng(
    const float* pixels, int width, int height, int channels) {

    std::vector<uint8_t> img(width * height * channels);
    for (size_t i = 0; i < img.size(); ++i) {
        float v = std::max(0.0f, std::min(1.0f, pixels[i]));
        img[i] = static_cast<uint8_t>(v * 255.0f + 0.5f);
    }

    std::vector<uint8_t> png;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            auto* out = reinterpret_cast<std::vector<uint8_t>*>(ctx);
            const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
            out->insert(out->end(), p, p + size);
        },
        &png, width, height, channels, img.data(), width * channels);

    return png;
}

/*static*/
std::vector<uint8_t> StableDiffusionEngine::encodePngFromU8(
    const uint8_t* pixels, int width, int height, int channels) {
    std::vector<uint8_t> png;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            auto* out = reinterpret_cast<std::vector<uint8_t>*>(ctx);
            const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
            out->insert(out->end(), p, p + size);
        },
        &png, width, height, channels, pixels, width * channels);
    return png;
}

GenerationResult StableDiffusionEngine::generateViaDirectBinary(const GenerationRequest& req) {
    GenerationResult result;
    const auto t_start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point t_runner_start{};
    std::chrono::steady_clock::time_point t_runner_end{};
    std::chrono::steady_clock::time_point t_ppm_start{};
    std::chrono::steady_clock::time_point t_ppm_end{};
    std::chrono::steady_clock::time_point t_png_start{};
    std::chrono::steady_clock::time_point t_png_end{};

    try {
        const std::string runner = runnerBinaryPath();
        const std::string tokenizer_dir = tokenizer_dir_;
        if (runner.empty()) {
            throw std::runtime_error(
                "Direct runner binary not found in runtime dir or image (/usr/bin/sd21_qnn_cpp_direct)");
        }

        const DirectRunnerConfig runner_cfg = buildDirectRunnerConfig(model_dir_, req.num_steps);
        const auto nonce = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        const std::string out_ppm = "/tmp/sd21_server_" + std::to_string(nonce) + ".ppm";
        const std::string out_log = "/tmp/sd21_server_" + std::to_string(nonce) + ".log";
        const std::string command = buildDirectRunnerCommand(
            runner, model_dir_, tokenizer_dir, runner_cfg, req, out_ppm, out_log);

        t_runner_start = std::chrono::steady_clock::now();
        const int rc = std::system(command.c_str());
        t_runner_end = std::chrono::steady_clock::now();
        if (rc != 0) {
            throw std::runtime_error("Direct runner command failed with rc=" + std::to_string(rc));
        }

        t_ppm_start = std::chrono::steady_clock::now();
        std::ifstream ppm(out_ppm, std::ios::binary);
        if (!ppm) throw std::runtime_error("Failed to open output PPM: " + out_ppm);

        auto readToken = [&ppm]() -> std::string {
            std::string tok;
            while (ppm >> tok) {
                if (!tok.empty() && tok[0] == '#') {
                    std::string rest;
                    std::getline(ppm, rest);
                    continue;
                }
                return tok;
            }
            return {};
        };

        const std::string magic = readToken();
        if (magic != "P6") throw std::runtime_error("Unsupported PPM format, expected P6");
        const int width = std::stoi(readToken());
        const int height = std::stoi(readToken());
        const int maxval = std::stoi(readToken());
        if (width <= 0 || height <= 0 || maxval != 255) {
            throw std::runtime_error("Invalid PPM header in runner output");
        }
        ppm.get();  // consume single whitespace after maxval
        std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
        ppm.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
        if (ppm.gcount() != static_cast<std::streamsize>(rgb.size())) {
            throw std::runtime_error("PPM payload size mismatch");
        }
        t_ppm_end = std::chrono::steady_clock::now();

        t_png_start = std::chrono::steady_clock::now();
        result.png_bytes = encodePngFromU8(rgb.data(), width, height, 3);
        t_png_end = std::chrono::steady_clock::now();
        result.success = true;

        std::error_code ec;
        std::filesystem::remove(out_ppm, ec);
        std::filesystem::remove(out_log, ec);
    } catch (const std::exception& e) {
        result.success = false;
        result.error = e.what();
    }

    const auto t_end = std::chrono::steady_clock::now();
    result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
    result.timing.total_ms = result.elapsed_ms;
    if (t_runner_end > t_runner_start) {
        result.timing.runner_exec_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t_runner_end - t_runner_start).count();
    }
    if (t_ppm_end > t_ppm_start) {
        result.timing.ppm_read_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t_ppm_end - t_ppm_start).count();
    }
    if (t_png_end > t_png_start) {
        result.timing.png_encode_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t_png_end - t_png_start).count();
    }
    return result;
}

// ---------------------------------------------------------------------------
// generate
// ---------------------------------------------------------------------------
GenerationResult StableDiffusionEngine::generate(const GenerationRequest& req) {
    std::lock_guard<std::mutex> lock(inference_mutex_);

    if (use_external_runner_) {
        if (!ready_) {
            GenerationResult r;
            r.success = false;
            r.error = "Engine not initialized";
            return r;
        }
        return generateViaDirectBinary(req);
    }

    GenerationResult result;
    const auto t_start = std::chrono::steady_clock::now();
    int64_t unet_ms_acc = 0;

    try {
        if (!ready_) throw std::runtime_error("Engine not initialized");

        const int num_steps = std::max(1, std::min(50, req.num_steps));
        std::cout << "[SD] Generating: \"" << req.prompt << "\""
                  << " seed=" << req.seed
                  << " steps=" << num_steps
                  << " cfg=" << req.guidance_scale
                  << " thread=" << std::this_thread::get_id() << "\n";

        // 1. Tokenize
        const auto t_tokenize_start = std::chrono::steady_clock::now();
        const auto cond_tokens   = tokenizer_.tokenizeAsFloat(req.prompt);
        const auto uncond_tokens = tokenizer_.tokenizeAsFloat(req.negative_prompt);
        const auto t_tokenize_end = std::chrono::steady_clock::now();
        result.timing.tokenize_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t_tokenize_end - t_tokenize_start).count();

        // 2. Text encoding
        std::cout << "[SD] Running text encoder...\n";
        const auto t_text_encode_start = std::chrono::steady_clock::now();
        const auto uncond_emb = runTextEncoder(uncond_tokens);
        const auto cond_emb   = runTextEncoder(cond_tokens);
        const auto t_text_encode_end = std::chrono::steady_clock::now();
        result.timing.text_encode_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t_text_encode_end - t_text_encode_start).count();

        // 3. Random latent (NCHW → NHWC)
        const auto t_latent_start = std::chrono::steady_clock::now();
        std::mt19937_64 rng(static_cast<uint64_t>(req.seed));
        std::normal_distribution<float> normal(0.0f, 1.0f);

        std::vector<float> latent_nchw(1 * 4 * 64 * 64);
        for (auto& v : latent_nchw) v = normal(rng);

        std::vector<float> latent(1 * 64 * 64 * 4);
        for (int c = 0; c < 4; ++c)
            for (int h = 0; h < 64; ++h)
                for (int w = 0; w < 64; ++w)
                    latent[h * 64 * 4 + w * 4 + c] = latent_nchw[c * 64 * 64 + h * 64 + w];
        const auto t_latent_end = std::chrono::steady_clock::now();
        result.timing.latent_init_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t_latent_end - t_latent_start).count();

        // 4. Denoising loop
        scheduler_.setTimesteps(num_steps);
        const auto t_denoise_start = std::chrono::steady_clock::now();

        for (int step = 0; step < num_steps; ++step) {
            const int32_t ts = scheduler_.timestep(step);
            std::cout << "[SD] Step " << step + 1 << "/" << num_steps
                      << " (t=" << ts << ")\n";

            // Pass scalar timestep – model computes sinusoidal embedding internally
            const auto t_unet_start = std::chrono::steady_clock::now();
            const float ts_val = static_cast<float>(ts);
            const auto uncond_noise = runUnet(latent, ts_val, uncond_emb);
            const auto cond_noise   = runUnet(latent, ts_val, cond_emb);
            const auto t_unet_end = std::chrono::steady_clock::now();
            unet_ms_acc += std::chrono::duration_cast<std::chrono::milliseconds>(
                t_unet_end - t_unet_start).count();

            latent = scheduler_.step(step, latent, uncond_noise, cond_noise,
                                     req.guidance_scale);
        }
        const auto t_denoise_end = std::chrono::steady_clock::now();
        result.timing.denoise_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t_denoise_end - t_denoise_start).count();
        result.timing.unet_ms = unet_ms_acc;

        // 5. VAE decode
        std::cout << "[SD] Running VAE decoder...\n";
        const auto t_vae_start = std::chrono::steady_clock::now();
        const auto pixels = runVaeDecoder(latent);
        const auto t_vae_end = std::chrono::steady_clock::now();
        result.timing.vae_decode_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t_vae_end - t_vae_start).count();

        // 6. Encode PNG
        const auto t_png_start = std::chrono::steady_clock::now();
        result.png_bytes = encodePng(pixels.data(), 512, 512, 3);
        const auto t_png_end = std::chrono::steady_clock::now();
        result.timing.png_encode_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t_png_end - t_png_start).count();
        result.success   = true;

    } catch (const std::exception& e) {
        result.success = false;
        result.error   = e.what();
        std::cerr << "[SD] Error: " << e.what() << "\n";
    }

    const auto t_end = std::chrono::steady_clock::now();
    result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t_end - t_start).count();
    result.timing.total_ms = result.elapsed_ms;

    std::cout << "[SD] Done in " << result.elapsed_ms << " ms\n";
    return result;
}

// ---------------------------------------------------------------------------
// destroy
// ---------------------------------------------------------------------------
void StableDiffusionEngine::destroy() {
    if (!ready_) return;
    ready_ = false;

    for (int i = 0; i < NUM_MODELS; ++i) {
        freeTensors(i);
        if (qnn_context_[i] && qnn_->contextFree) {
            qnn_->contextFree(reinterpret_cast<Qnn_ContextHandle_t>(qnn_context_[i]), nullptr);
            qnn_context_[i] = nullptr;
        }
    }

    if (qnn_device_handle_ && qnn_->deviceFree) {
        qnn_->deviceFree(reinterpret_cast<Qnn_DeviceHandle_t>(qnn_device_handle_));
        qnn_device_handle_ = nullptr;
    }
    if (qnn_backend_handle_ && qnn_->backendFree) {
        qnn_->backendFree(reinterpret_cast<Qnn_BackendHandle_t>(qnn_backend_handle_));
        qnn_backend_handle_ = nullptr;
    }
    if (qnn_log_handle_ && qnn_->logFree) {
        qnn_->logFree(reinterpret_cast<Qnn_LogHandle_t>(qnn_log_handle_));
        qnn_log_handle_ = nullptr;
    }
    if (backend_lib_handle_) {
        dlclose(backend_lib_handle_);
        backend_lib_handle_ = nullptr;
    }
    if (system_lib_handle_) {
        dlclose(system_lib_handle_);
        system_lib_handle_ = nullptr;
    }
    std::cout << "[SD] Engine destroyed\n";
}

void StableDiffusionEngine::freeQnnResources() { destroy(); }
