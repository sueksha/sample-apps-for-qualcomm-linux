#!/bin/bash
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Text-To-Image – single-command Docker build
#
# Handles automatically:
#   1. Optional SD2.1 runtime prep (legacy script path)
#   2. ubuntu:24.04         → downloads from cloud-images.ubuntu.com if missing
#   3. ubuntu-runtime:24.04 → minimal flat runtime base if missing
#   4. qairt-sdk/           → prepares repo-root QAIRT SDK slice if missing
#   5. genai-build-base     → shared C++ build base if missing
#   6. text-to-image:latest → final Text-To-Image service image
#      (also compiles core-services/text-to-image/deps/cpp_sd21_qnn/main_direct_qnn.cpp
#       into /usr/bin/sd21_qnn_cpp_direct inside the image)
#
# ─── Usage ───────────────────────────────────────────────────────────────────
#   bash build.sh
#
#   FORCE_REBUILD=1 bash build.sh
#
#   TEXT_TO_IMAGE_IMAGE=my-text-to-image:dev bash build.sh
#
# ─── Environment variables ───────────────────────────────────────────────────
#   BUILD_BASE_IMAGE   Builder base image (default: genai-build-base:latest)
#   RUNTIME_BASE_IMAGE Runtime base image (default: ubuntu-runtime:24.04)
#   TEXT_TO_IMAGE_IMAGE Output image name (default: text-to-image:latest)
#   IMAGEGEN_MODEL_DIR Unified host runtime dir for SD2.1 artifacts
#   IMAGEGEN_QAIRT_ROOT QAIRT root used to copy runtime libs
#   IMAGEGEN_PREPARE_MODEL Set 1 to run legacy runtime prep helper (default: 0)
#   IMAGEGEN_SKIP_DOWNLOAD Set 1 to disable AIHub/HF downloads
#   IMAGEGEN_REMOTE_HOST Optional remote host to push prepared runtime
#   IMAGEGEN_REMOTE_USER Remote SSH user (default: ubuntu)
#   IMAGEGEN_REMOTE_PASSWORD Remote SSH password (optional; uses sshpass)
#   IMAGEGEN_REMOTE_RUNTIME_DIR Remote runtime dir (defaults to IMAGEGEN_MODEL_DIR)
#   FORCE_REBUILD      Set to 1 to force rebuild everything
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
resolve_repo_root() {
    local d="${SCRIPT_DIR}"
    while [[ "${d}" != "/" ]]; do
        if [[ -f "${d}/Dockerfile.build-base" && -f "${d}/docker-compose.yml" ]]; then
            echo "${d}"
            return 0
        fi
        d="$(dirname "${d}")"
    done
    return 1
}
REPO_ROOT="$(resolve_repo_root || true)"
if [[ -z "${REPO_ROOT}" ]]; then
    echo "[build.sh] ERROR: failed to locate repository root from ${SCRIPT_DIR}" >&2
    exit 1
fi

BUILD_BASE_IMAGE="${BUILD_BASE_IMAGE:-genai-build-base:latest}"
RUNTIME_BASE_IMAGE="${RUNTIME_BASE_IMAGE:-ubuntu-runtime:24.04}"
TEXT_TO_IMAGE_IMAGE="${TEXT_TO_IMAGE_IMAGE:-text-to-image:latest}"
FORCE_REBUILD="${FORCE_REBUILD:-0}"
IMAGEGEN_PREPARE_MODEL="${IMAGEGEN_PREPARE_MODEL:-0}"
IMAGEGEN_SKIP_DOWNLOAD="${IMAGEGEN_SKIP_DOWNLOAD:-0}"
IMAGEGEN_QAIRT_ROOT="${IMAGEGEN_QAIRT_ROOT:-/opt/qairt/current}"
IMAGEGEN_REMOTE_HOST="${IMAGEGEN_REMOTE_HOST:-}"
IMAGEGEN_REMOTE_USER="${IMAGEGEN_REMOTE_USER:-ubuntu}"
IMAGEGEN_REMOTE_PASSWORD="${IMAGEGEN_REMOTE_PASSWORD:-}"
IMAGEGEN_REMOTE_RUNTIME_DIR="${IMAGEGEN_REMOTE_RUNTIME_DIR:-}"

DEFAULT_MODEL_DIR_NEW="/opt/genai-studio-models/text-to-image/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075"

runtime_dir_valid() {
    local runtime_dir="$1"
    local has_text=0
    local has_unet=0
    local has_vae=0
    [[ -f "${runtime_dir}/text_encoder.bin" || -f "${runtime_dir}/text_encoder_qairt_context.bin" || -f "${runtime_dir}/stable_diffusion_v2_1-text_encoder-qualcomm_qcs9075.bin" ]] && has_text=1
    [[ -f "${runtime_dir}/unet.bin" || -f "${runtime_dir}/unet_qairt_context.bin" || -f "${runtime_dir}/stable_diffusion_v2_1-unet-qualcomm_qcs9075.bin" ]] && has_unet=1
    [[ -f "${runtime_dir}/vae.bin" || -f "${runtime_dir}/vae_decoder.bin" || -f "${runtime_dir}/vae_qairt_context.bin" || -f "${runtime_dir}/stable_diffusion_v2_1-vae-qualcomm_qcs9075.bin" ]] && has_vae=1
    [[ "${has_text}" == "1" ]] && \
    [[ "${has_unet}" == "1" ]] && \
    [[ "${has_vae}" == "1" ]] && \
    [[ -f "${runtime_dir}/tokenizer/vocab.json" ]] && \
    [[ -f "${runtime_dir}/tokenizer/merges.txt" ]]
}

if [[ -z "${IMAGEGEN_MODEL_DIR:-}" ]]; then
    if runtime_dir_valid "${DEFAULT_MODEL_DIR_NEW}"; then
        IMAGEGEN_MODEL_DIR="${DEFAULT_MODEL_DIR_NEW}"
    else
        IMAGEGEN_MODEL_DIR="${DEFAULT_MODEL_DIR_NEW}"
    fi
fi

echo "============================================================"
echo " Text-To-Image – Single-Command Docker Build"
echo "============================================================"
echo " BUILD_BASE_IMAGE   : ${BUILD_BASE_IMAGE}"
echo " RUNTIME_BASE_IMAGE : ${RUNTIME_BASE_IMAGE}"
echo " OUTPUT_IMAGE       : ${TEXT_TO_IMAGE_IMAGE}"
echo " IMAGEGEN_MODEL_DIR : ${IMAGEGEN_MODEL_DIR}"
echo " IMAGEGEN_QAIRT_ROOT: ${IMAGEGEN_QAIRT_ROOT}"
echo " PREPARE_MODEL      : ${IMAGEGEN_PREPARE_MODEL}"
echo " SKIP_DOWNLOAD      : ${IMAGEGEN_SKIP_DOWNLOAD}"
echo " REMOTE_HOST        : ${IMAGEGEN_REMOTE_HOST:-<none>}"
echo " REMOTE_USER        : ${IMAGEGEN_REMOTE_USER}"
if [[ -n "${IMAGEGEN_REMOTE_RUNTIME_DIR}" ]]; then
    echo " REMOTE_RUNTIME_DIR : ${IMAGEGEN_REMOTE_RUNTIME_DIR}"
fi
echo " FORCE_REBUILD      : ${FORCE_REBUILD}"
echo "============================================================"
echo ""

LEGACY_PREP_SCRIPT="${SCRIPT_DIR}/model_generation_scripts/prepare_sd21_runtime.py"

echo "[1/6] Preparing SD2.1 runtime folder"
if [[ "${IMAGEGEN_PREPARE_MODEL}" == "1" ]]; then
    if [[ ! -f "${LEGACY_PREP_SCRIPT}" ]]; then
        echo "      ERROR: Legacy prep script not found at:"
        echo "             ${LEGACY_PREP_SCRIPT}"
        echo "      Disable prep with IMAGEGEN_PREPARE_MODEL=0, or restore the script."
        exit 1
    fi
    PREP_ARGS=(
        --runtime-dir "${IMAGEGEN_MODEL_DIR}"
        --qairt-root "${IMAGEGEN_QAIRT_ROOT}"
    )
    if [[ "${IMAGEGEN_SKIP_DOWNLOAD}" == "1" ]]; then
        PREP_ARGS+=(--no-download)
    fi
    if [[ -n "${IMAGEGEN_REMOTE_HOST}" ]]; then
        PREP_ARGS+=(--remote-host "${IMAGEGEN_REMOTE_HOST}")
        PREP_ARGS+=(--remote-user "${IMAGEGEN_REMOTE_USER}")
        if [[ -n "${IMAGEGEN_REMOTE_PASSWORD}" ]]; then
            PREP_ARGS+=(--remote-password "${IMAGEGEN_REMOTE_PASSWORD}")
        fi
        if [[ -n "${IMAGEGEN_REMOTE_RUNTIME_DIR}" ]]; then
            PREP_ARGS+=(--remote-runtime-dir "${IMAGEGEN_REMOTE_RUNTIME_DIR}")
        fi
    fi
    python3 "${LEGACY_PREP_SCRIPT}" "${PREP_ARGS[@]}"
else
    echo "      Skipped (IMAGEGEN_PREPARE_MODEL=${IMAGEGEN_PREPARE_MODEL})"
    echo "      Expected ready runtime folder at: ${IMAGEGEN_MODEL_DIR}"
fi
echo ""

echo "[2/6] Checking ubuntu:24.04 base image"
if docker image inspect ubuntu:24.04 &>/dev/null && [ "${FORCE_REBUILD}" != "1" ]; then
    echo "      ✓ ubuntu:24.04 already available"
    docker images ubuntu:24.04 --format "        ID={{.ID}}  Size={{.Size}}"
else
    echo "      ubuntu:24.04 not found. Downloading from cloud-images.ubuntu.com ..."
    bash "${REPO_ROOT}/scripts/pull-ubuntu-arm64.sh"
fi
echo ""

echo "[3/6] Checking minimal runtime base: ${RUNTIME_BASE_IMAGE}"
if docker image inspect "${RUNTIME_BASE_IMAGE}" &>/dev/null && [ "${FORCE_REBUILD}" != "1" ]; then
    echo "      ✓ ${RUNTIME_BASE_IMAGE} already exists"
    docker images "${RUNTIME_BASE_IMAGE}" --format "        ID={{.ID}}  Size={{.Size}}"
else
    echo "      Creating ${RUNTIME_BASE_IMAGE} ..."
    bash "${REPO_ROOT}/scripts/create-runtime-base.sh"
fi
echo ""

echo "[4/6] Checking repo-root qairt-sdk/ slice"
QAIRT_SDK_SLICE="${REPO_ROOT}/qairt-sdk"
if [[ "${FORCE_REBUILD}" == "1" ]] || [[ ! -d "${QAIRT_SDK_SLICE}" ]]; then
    echo "      qairt-sdk/ missing or FORCE_REBUILD=1. Preparing from local QAIRT install..."
    env \
        FORCE_DOWNLOAD="${FORCE_REBUILD}" \
        bash "${REPO_ROOT}/scripts/download-qairt-sdk.sh" --service base
else
    echo "      ✓ qairt-sdk/ already exists"
fi
if [[ ! -d "${QAIRT_SDK_SLICE}/include/Genie" ]] || [[ ! -d "${QAIRT_SDK_SLICE}/include/QNN" ]]; then
    echo "[build.sh] ERROR: qairt-sdk slice is incomplete under ${QAIRT_SDK_SLICE}" >&2
    exit 1
fi
echo ""

echo "[5/6] Checking build base image: ${BUILD_BASE_IMAGE}"
if docker image inspect "${BUILD_BASE_IMAGE}" &>/dev/null && [ "${FORCE_REBUILD}" != "1" ]; then
    echo "      ✓ ${BUILD_BASE_IMAGE} already exists"
    docker images "${BUILD_BASE_IMAGE}" --format "        ID={{.ID}}  Size={{.Size}}  Created={{.CreatedAt}}"
else
    echo "      Building ${BUILD_BASE_IMAGE} ..."
    DOCKER_BUILDKIT=1 docker build \
        --progress=plain \
        -f "${REPO_ROOT}/Dockerfile.build-base" \
        -t "${BUILD_BASE_IMAGE}" \
        "${REPO_ROOT}"
    echo "      ✓ ${BUILD_BASE_IMAGE} built"
fi
echo ""

echo "[6/6] Building Docker image: ${TEXT_TO_IMAGE_IMAGE}"
DOCKER_BUILDKIT=1 docker build \
    --progress=plain \
    -f "${SCRIPT_DIR}/Dockerfile" \
    -t "${TEXT_TO_IMAGE_IMAGE}" \
    "${REPO_ROOT}"

echo ""
echo "============================================================"
echo " Build complete!"
echo "============================================================"
docker images "${TEXT_TO_IMAGE_IMAGE}" --format "  {{.Repository}}:{{.Tag}}  {{.Size}}  {{.CreatedAt}}"
echo ""
echo "Run:"
echo "  MODEL_DIR=${IMAGEGEN_MODEL_DIR} bash run.sh"
echo ""
echo "Or with docker compose (starts all services):"
echo "  export IMAGEGEN_MODEL_DIR=${IMAGEGEN_MODEL_DIR}"
echo "  cd .. && docker compose up -d"
echo ""
echo "Test:"
echo "  curl http://localhost:8090/health"
echo "  curl -X POST http://localhost:8090/v1/images/generations -H 'Content-Type: application/json' -d '{\"model\":\"stable-diffusion-2-1\",\"prompt\":\"smoke\",\"size\":\"512x512\"}'"
