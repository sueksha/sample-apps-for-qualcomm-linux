// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// DpmScheduler
//
// C++ implementation of DPMSolverMultistepScheduler matching the Python
// diffusers scheduler configuration used for SD2.1 inference:
//
//   DPMSolverMultistepScheduler(
//       num_train_timesteps = 1000,
//       beta_start          = 0.00085,
//       beta_end            = 0.012,
//       beta_schedule       = "scaled_linear"
//   )
//
// Usage:
//   DpmScheduler sched;
//   sched.setTimesteps(20);                    // prepare for 20-step inference
//   int ts = sched.timestep(step);             // get timestep for step i
//   void getTimestepEmbedding(ts, 1280, buf);  // sinusoidal embedding
//   latent = sched.step(step, latent,          // advance latent
//                       uncond_noise, cond_noise, guidance_scale);
// ---------------------------------------------------------------------------
class DpmScheduler {
public:
    // Noise schedule parameters (match Python defaults)
    static constexpr int   NUM_TRAIN_TIMESTEPS = 1000;
    static constexpr float BETA_START          = 0.00085f;
    static constexpr float BETA_END            = 0.012f;

    DpmScheduler();

    // Prepare timesteps for `num_steps` inference steps.
    // Must be called before step() / timestep().
    void setTimesteps(int num_steps);

    // Return the i-th timestep (0-indexed).
    int32_t timestep(int step) const;

    // Number of inference steps configured.
    int numSteps() const { return static_cast<int>(timesteps_.size()); }

    // Compute sinusoidal timestep embedding.
    // Output: embedding_dim floats written to `out` (caller allocates).
    static void getTimestepEmbedding(int32_t timestep,
                                     int     embedding_dim,
                                     float*  out);

    // Advance the latent by one denoising step.
    //
    // step          : current step index (0 … num_steps-1)
    // latent        : current latent  [1 × 64 × 64 × 4] = 16384 floats (NHWC)
    // uncond_noise  : unconditional noise prediction (same shape)
    // cond_noise    : conditional noise prediction   (same shape)
    // guidance_scale: classifier-free guidance scale (default 7.5)
    //
    // Returns the next latent (same shape).
    std::vector<float> step(int                       step,
                            const std::vector<float>& latent,
                            const std::vector<float>& uncond_noise,
                            const std::vector<float>& cond_noise,
                            float                     guidance_scale = 7.5f);

    // Access precomputed schedule arrays (for debugging / advanced use)
    const std::vector<float>& alphasCumprod() const { return alphas_cumprod_; }
    const std::vector<float>& sigmas()        const { return sigmas_; }

private:
    void buildSchedule();

    std::vector<float>   betas_;
    std::vector<float>   alphas_;
    std::vector<float>   alphas_cumprod_;
    std::vector<float>   sigmas_;          // sqrt((1-ac)/ac)
    std::vector<int32_t> timesteps_;       // inference timesteps (descending)

    // DPM-Solver++ 2nd-order multistep state
    std::vector<float>   prev_model_output_;  // model output from previous step
    bool                 has_prev_ = false;
};
