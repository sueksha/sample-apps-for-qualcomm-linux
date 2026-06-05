# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
#
# docker-bake.hcl – Build all GenAI Studio images in parallel
#
# ─── What is Docker Bake? ─────────────────────────────────────────────────────
# docker buildx bake is the modern replacement for docker-compose build.
# It reads this HCL file and builds all targets concurrently, sharing the
# BuildKit cache between them.  This is significantly faster than building
# services one-by-one because:
#   • The genai-build-base layer is built once and reused by all C++ services
#   • All service builds run in parallel (limited only by CPU/RAM)
#   • BuildKit cache mounts (apt, pip) persist across builds
#
# ─── Prerequisites ────────────────────────────────────────────────────────────
#   1. genai-base:latest    (docker build -f Dockerfile.base -t genai-base:latest .)
#   2. genai-runtime:latest (docker build -f Dockerfile.runtime -t genai-runtime:latest .)
#   3. genai-build-base:latest (docker build -f Dockerfile.build-base -t genai-build-base:latest .)
#   4. SDK slices in each service directory (created by each service's build.sh)
#
# ─── Usage ───────────────────────────────────────────────────────────────────
#   # Build ALL services in parallel:
#   DOCKER_BUILDKIT=1 docker buildx bake
#
#   # Build a single service:
#   DOCKER_BUILDKIT=1 docker buildx bake text-to-text
#
#   # Build only C++ services (skip orchestrator):
#   DOCKER_BUILDKIT=1 docker buildx bake cpp-services
#
#   # Dry-run (print what would be built):
#   docker buildx bake --print
#
# ─── Cache strategy ──────────────────────────────────────────────────────────
#   Local cache is stored in /tmp/.buildx-cache.
#   On CI, replace with registry cache:
#     cache-from = ["type=registry,ref=myregistry/genai-studio:cache"]
#     cache-to   = ["type=registry,ref=myregistry/genai-studio:cache,mode=max"]

# ── Groups ────────────────────────────────────────────────────────────────────

# Default: build everything
group "default" {
  targets = ["text-to-text", "image-to-text", "speech-to-text", "orchestrator"]
}

# C++ inference services only
group "cpp-services" {
  targets = ["text-to-text", "image-to-text", "speech-to-text"]
}

# ── Common variables ──────────────────────────────────────────────────────────

variable "CACHE_DIR" {
  default = "/tmp/.buildx-cache"
}

variable "TAG" {
  default = "latest"
}

# ── Base images (build these first, manually, before running bake) ────────────
# These are not bake targets because they require special bootstrapping
# (genai-base needs the Qualcomm PPA; genai-runtime is derived from genai-base).
# See: scripts/bootstrap-base.sh, scripts/bootstrap-runtime.sh

# ── Service targets ───────────────────────────────────────────────────────────

target "text-to-text" {
  context    = "./core-services/text-to-text"
  dockerfile = "Dockerfile"
  tags       = ["text-to-text:${TAG}"]
  # BuildKit inline cache: cache metadata is stored inside the image layer
  cache-from = ["type=local,src=${CACHE_DIR}/text-to-text"]
  cache-to   = ["type=local,dest=${CACHE_DIR}/text-to-text,mode=max"]
  # Pass DOCKER_BUILDKIT=1 or use `docker buildx bake` (BuildKit is always on)
  args = {}
}

target "image-to-text" {
  context    = "./core-services/image-to-text"
  dockerfile = "Dockerfile"
  tags       = ["image-to-text:${TAG}"]
  cache-from = ["type=local,src=${CACHE_DIR}/image-to-text"]
  cache-to   = ["type=local,dest=${CACHE_DIR}/image-to-text,mode=max"]
  args = {}
}

target "speech-to-text" {
  context    = "./core-services/speech-to-text"
  dockerfile = "Dockerfile"
  tags       = ["speech-to-text:${TAG}"]
  cache-from = ["type=local,src=${CACHE_DIR}/speech-to-text"]
  cache-to   = ["type=local,dest=${CACHE_DIR}/speech-to-text,mode=max"]
  args = {}
}

target "orchestrator" {
  context    = "./core-services/orchestrator"
  dockerfile = "Dockerfile"
  tags       = ["orchestrator:${TAG}"]
  cache-from = ["type=local,src=${CACHE_DIR}/orchestrator"]
  cache-to   = ["type=local,dest=${CACHE_DIR}/orchestrator,mode=max"]
  args = {}
}
