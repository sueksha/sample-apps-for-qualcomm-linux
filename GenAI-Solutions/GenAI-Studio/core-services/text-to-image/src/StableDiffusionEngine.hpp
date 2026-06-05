// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ClipTokenizer.hpp"
#include "DpmScheduler.hpp"

// QNN types (needed for Qnn_Tensor_t definition)
#include "QnnTypes.h"
#include "System/QnnSystemContext.h"

// ---------------------------------------------------------------------------
// GenerationRequest – parameters for one image generation call
// ---------------------------------------------------------------------------
struct GenerationRequest {
    std::string prompt;
    std::string negative_prompt =
        "lowres, text, error, cropped, worst quality, low quality, "
        "normal quality, jpeg artifacts, signature, watermark, blurry";
    int64_t seed          = 42;
    int     num_steps     = 20;
    float   guidance_scale = 7.5f;
    int     width         = 512;   // current SD2.1 export supports 512 only
    int     height        = 512;   // current SD2.1 export supports 512 only
    // Optional direct-runner quality overrides used for A/B experiments.
    // Empty/<=0 values mean "use service defaults + runner auto-tune".
    std::string prediction_type_override;
    float       vae_scaling_factor_override = 0.0f;
};

// ---------------------------------------------------------------------------
// GenerationResult
// ---------------------------------------------------------------------------
struct GenerationTiming {
    int64_t tokenize_ms    = 0;
    int64_t text_encode_ms = 0;
    int64_t latent_init_ms = 0;
    int64_t denoise_ms     = 0;
    int64_t unet_ms        = 0;
    int64_t vae_decode_ms  = 0;
    int64_t png_encode_ms  = 0;
    int64_t runner_exec_ms = 0;
    int64_t ppm_read_ms    = 0;
    int64_t total_ms       = 0;
};

struct GenerationResult {
    bool                     success = false;
    std::vector<uint8_t>     png_bytes;   // PNG-encoded image
    std::string              error;
    int64_t                  elapsed_ms = 0;
    GenerationTiming         timing;
};

// ---------------------------------------------------------------------------
// StableDiffusionEngine
// ---------------------------------------------------------------------------
class StableDiffusionEngine {
public:
    explicit StableDiffusionEngine(const std::string& model_dir, const std::string& tokenizer_dir);
    ~StableDiffusionEngine();

    StableDiffusionEngine(const StableDiffusionEngine&)            = delete;
    StableDiffusionEngine& operator=(const StableDiffusionEngine&) = delete;

    void initialize();
    bool isReady() const noexcept { return ready_; }
    GenerationResult generate(const GenerationRequest& req);
    void destroy();

private:
    void loadQnnBackend();
    void createQnnBackend();
    void createQnnDevice();
    void loadModels();
    void loadSingleModel(int idx, const std::string& model_name);
    void freeQnnResources();

    // bin_info must remain valid for the duration of this call
    void setupTensors(int graph_idx, const QnnSystemContext_BinaryInfo_t* bin_info);
    void freeTensors(int graph_idx);

    void copyFloatToTensor(const float* src, size_t count, Qnn_Tensor_t* dst);
    void copyTensorToFloat(const Qnn_Tensor_t* src, float* dst, size_t count);

    std::vector<float> runTextEncoder(const std::vector<float>& tokens);
    std::vector<float> runUnet(const std::vector<float>& latent_nhwc,
                               float                     timestep_val,
                               const std::vector<float>& text_emb);
    std::vector<float> runVaeDecoder(const std::vector<float>& latent_nhwc);
    GenerationResult generateViaDirectBinary(const GenerationRequest& req);
    std::string runnerBinaryPath() const;

    static std::vector<uint8_t> encodePng(const float* pixels,
                                          int width, int height, int channels);
    static std::vector<uint8_t> encodePngFromU8(const uint8_t* pixels,
                                                int width, int height, int channels);

    std::string model_dir_;
    std::string tokenizer_dir_;

    void* backend_lib_handle_  = nullptr;
    void* system_lib_handle_   = nullptr;
    void* qnn_backend_handle_  = nullptr;
    void* qnn_device_handle_   = nullptr;
    void* qnn_log_handle_      = nullptr;

    static constexpr int NUM_MODELS = 3;
    void* qnn_context_[NUM_MODELS]  = {nullptr, nullptr, nullptr};
    std::vector<uint8_t> context_binaries_[NUM_MODELS];

    void*  graph_handle_[NUM_MODELS]  = {nullptr, nullptr, nullptr};
    void*  input_tensors_[NUM_MODELS] = {nullptr, nullptr, nullptr};
    void*  output_tensors_[NUM_MODELS]= {nullptr, nullptr, nullptr};
    uint32_t num_inputs_[NUM_MODELS]  = {0, 0, 0};
    uint32_t num_outputs_[NUM_MODELS] = {0, 0, 0};

    struct QnnFnPtrs;
    std::unique_ptr<QnnFnPtrs> qnn_;

    ClipTokenizer tokenizer_;
    DpmScheduler  scheduler_;
    bool use_external_runner_ = true;

    bool ready_ = false;
    std::mutex inference_mutex_;
};
