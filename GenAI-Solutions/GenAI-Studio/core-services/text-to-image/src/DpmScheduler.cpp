// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "DpmScheduler.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Constructor – build the full noise schedule
// ---------------------------------------------------------------------------
DpmScheduler::DpmScheduler() {
    buildSchedule();
}

// ---------------------------------------------------------------------------
// buildSchedule
//
// Replicates Python:
//   betas = torch.linspace(sqrt(beta_start), sqrt(beta_end), 1000) ** 2
//   alphas = 1 - betas
//   alphas_cumprod = torch.cumprod(alphas, dim=0)
//   sigmas = sqrt((1 - alphas_cumprod) / alphas_cumprod)
// ---------------------------------------------------------------------------
void DpmScheduler::buildSchedule() {
    const int N = NUM_TRAIN_TIMESTEPS;
    betas_.resize(N);
    alphas_.resize(N);
    alphas_cumprod_.resize(N);
    sigmas_.resize(N);

    const float sqrt_start = std::sqrt(BETA_START);
    const float sqrt_end   = std::sqrt(BETA_END);

    // scaled_linear betas
    for (int i = 0; i < N; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(N - 1);
        float sq = sqrt_start + t * (sqrt_end - sqrt_start);
        betas_[i]  = sq * sq;
        alphas_[i] = 1.0f - betas_[i];
    }

    // cumulative product of alphas
    float cumprod = 1.0f;
    for (int i = 0; i < N; ++i) {
        cumprod *= alphas_[i];
        alphas_cumprod_[i] = cumprod;
        sigmas_[i] = std::sqrt((1.0f - cumprod) / cumprod);
    }
}

// ---------------------------------------------------------------------------
// setTimesteps
//
// Replicates DPMSolverMultistepScheduler.set_timesteps(num_steps).
// Timesteps are evenly spaced in [0, 999] (descending), matching diffusers.
// ---------------------------------------------------------------------------
void DpmScheduler::setTimesteps(int num_steps) {
    if (num_steps <= 0)
        throw std::invalid_argument("num_steps must be > 0");

    has_prev_ = false;
    prev_model_output_.clear();

    timesteps_.resize(num_steps);
    const int N = NUM_TRAIN_TIMESTEPS;

    // Evenly spaced timesteps in descending order (same as diffusers)
    for (int i = 0; i < num_steps; ++i) {
        // Python: timesteps = np.arange(0, num_train_timesteps)[::-1][::step_ratio]
        // Simplified: evenly spaced from N-1 down to 0
        float frac = (num_steps == 1)
                         ? 0.0f
                         : static_cast<float>(num_steps - 1 - i) /
                               static_cast<float>(num_steps - 1);
        int ts = static_cast<int>(std::round(frac * static_cast<float>(N - 1)));
        ts = std::max(0, std::min(N - 1, ts));
        timesteps_[i] = static_cast<int32_t>(ts);
    }

    std::cout << "[DpmScheduler] " << num_steps << " steps, timesteps[0]="
              << timesteps_[0] << " timesteps[-1]=" << timesteps_.back() << "\n";
}

// ---------------------------------------------------------------------------
// timestep
// ---------------------------------------------------------------------------
int32_t DpmScheduler::timestep(int step) const {
    if (step < 0 || step >= static_cast<int>(timesteps_.size()))
        throw std::out_of_range("step out of range");
    return timesteps_[step];
}

// ---------------------------------------------------------------------------
// getTimestepEmbedding
//
// Sinusoidal timestep embedding matching diffusers get_timestep_embedding():
//   half_dim = embedding_dim // 2
//   emb = log(10000) / (half_dim - 1)
//   emb = exp(arange(half_dim) * -emb)
//   emb = timestep * emb
//   emb = [sin(emb), cos(emb)]
// ---------------------------------------------------------------------------
/*static*/
void DpmScheduler::getTimestepEmbedding(int32_t ts, int embedding_dim, float* out) {
    const int half = embedding_dim / 2;
    const float log10000 = std::log(10000.0f);
    const float scale = log10000 / static_cast<float>(half - 1);

    for (int i = 0; i < half; ++i) {
        float freq = std::exp(-scale * static_cast<float>(i));
        float arg  = static_cast<float>(ts) * freq;
        out[i]        = std::sin(arg);
        out[i + half] = std::cos(arg);
    }

    // If embedding_dim is odd, zero the last element
    if (embedding_dim % 2 != 0)
        out[embedding_dim - 1] = 0.0f;
}

// ---------------------------------------------------------------------------
// step
//
// DPM-Solver++ 2nd-order multistep denoising step.
//
// Algorithm:
//   1. Compute guided noise: noise = uncond + guidance * (cond - uncond)
//   2. Convert noise prediction to "model output" (x0 prediction):
//        x0 = (latent - sigma_t * noise) / alpha_t
//   3. First step: use 1st-order (DDIM-like) update
//      Subsequent steps: use 2nd-order correction
//
// This matches the DPMSolverMultistepScheduler in diffusers.
// ---------------------------------------------------------------------------
std::vector<float> DpmScheduler::step(int                       step_idx,
                                       const std::vector<float>& latent,
                                       const std::vector<float>& uncond_noise,
                                       const std::vector<float>& cond_noise,
                                       float                     guidance_scale) {
    const size_t N = latent.size();
    assert(uncond_noise.size() == N);
    assert(cond_noise.size()   == N);

    const int num_steps = static_cast<int>(timesteps_.size());
    if (step_idx < 0 || step_idx >= num_steps)
        throw std::out_of_range("step_idx out of range");

    // ---- 1. Classifier-free guidance ----
    std::vector<float> noise_pred(N);
    for (size_t i = 0; i < N; ++i)
        noise_pred[i] = uncond_noise[i] + guidance_scale * (cond_noise[i] - uncond_noise[i]);

    // ---- 2. Get alpha / sigma for current and previous timestep ----
    const int32_t t_cur  = timesteps_[step_idx];
    const float   ac_cur = alphas_cumprod_[t_cur];
    const float   alpha_cur = std::sqrt(ac_cur);
    const float   sigma_cur = std::sqrt(1.0f - ac_cur);

    // Previous timestep (t-1): use next entry or 0
    float alpha_prev, sigma_prev;
    if (step_idx + 1 < num_steps) {
        const int32_t t_prev = timesteps_[step_idx + 1];
        const float   ac_prev = alphas_cumprod_[t_prev];
        alpha_prev = std::sqrt(ac_prev);
        sigma_prev = std::sqrt(1.0f - ac_prev);
    } else {
        // Final step: target is clean image (alpha=1, sigma=0)
        alpha_prev = 1.0f;
        sigma_prev = 0.0f;
    }

    // ---- 3. Convert noise prediction to x0 prediction ----
    // x0 = (latent - sigma_cur * noise_pred) / alpha_cur
    std::vector<float> x0_pred(N);
    for (size_t i = 0; i < N; ++i)
        x0_pred[i] = (latent[i] - sigma_cur * noise_pred[i]) / alpha_cur;

    // ---- 4. DPM-Solver++ update ----
    std::vector<float> next_latent(N);

    if (!has_prev_) {
        // 1st-order (DDIM) step
        for (size_t i = 0; i < N; ++i)
            next_latent[i] = alpha_prev * x0_pred[i] + sigma_prev * noise_pred[i];
    } else {
        // 2nd-order correction using previous model output
        // DPM-Solver++ formula:
        //   D = (x0_pred - prev_x0_pred) / 2
        //   next = alpha_prev * (x0_pred + D) + sigma_prev * noise_pred
        // where prev_x0_pred is stored from the previous step.
        for (size_t i = 0; i < N; ++i) {
            float d = (x0_pred[i] - prev_model_output_[i]) * 0.5f;
            next_latent[i] = alpha_prev * (x0_pred[i] + d) + sigma_prev * noise_pred[i];
        }
    }

    // ---- 5. Save model output for next step ----
    prev_model_output_ = x0_pred;
    has_prev_ = true;

    return next_latent;
}
