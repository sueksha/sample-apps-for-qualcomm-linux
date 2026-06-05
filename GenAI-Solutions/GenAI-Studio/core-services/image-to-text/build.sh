#!/bin/bash
# =============================================================================
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# =============================================================================
#
# build.sh – Build image-to-text:responses-v1 Docker image
#
# Handles EVERYTHING automatically (5 steps):
#   1. ubuntu:24.04        → downloads from cloud-images.ubuntu.com if missing
#   2. ubuntu-runtime:24.04 → creates stripped runtime base if missing
#   3. qairt-sdk/ (repo root) → copies QAIRT SDK slice if missing
#   4. genai-build-base:latest → builds shared C++ builder base (with SDK baked in)
#   5. image-to-text:responses-v1   → builds the final service image
#
# The QAIRT SDK is baked into genai-build-base:latest at /opt/qairt/.
# No per-service qairt-sdk/ directory is needed — all services share one copy.
#
# Usage:
#   QAIRT_SDK_ROOT=/path/to/qairt/<version> bash build.sh
#   FORCE_REBUILD=1 bash build.sh   # force rebuild everything
#
# Environment variables:
#   QAIRT_SDK_ROOT    Path to local QAIRT SDK (skips download if set)
#   QAIRT_VERSION     SDK version to download (default from versions.env)
#   FORCE_REBUILD     Set to 1 to force rebuild of all images
# =============================================================================

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

# Canonical version source: repo-root versions.env (if present)
if [ -f "${REPO_ROOT}/versions.env" ]; then
    set -a
    # shellcheck source=/dev/null
    source "${REPO_ROOT}/versions.env"
    set +a
fi

QAIRT_VERSION="${QAIRT_VERSION:-2.45.0.260326}"
QAIRT_SDK_ROOT="${QAIRT_SDK_ROOT:-}"
BUILD_BASE_IMAGE="${BUILD_BASE_IMAGE:-genai-build-base:latest}"
IMAGE_TO_TEXT_IMAGE="${IMAGE_TO_TEXT_IMAGE:-image-to-text:responses-v1}"
FORCE_REBUILD="${FORCE_REBUILD:-0}"

echo "============================================================"
echo " Image-To-Text – Single-Command Docker Build"
echo "============================================================"
echo " BUILD_BASE_IMAGE : ${BUILD_BASE_IMAGE}"
echo " OUTPUT_IMAGE     : ${IMAGE_TO_TEXT_IMAGE}"
echo " QAIRT_VERSION    : ${QAIRT_VERSION}"
if [ -n "${QAIRT_SDK_ROOT}" ]; then
    echo " QAIRT_SDK_ROOT   : ${QAIRT_SDK_ROOT} (local)"
else
    echo " QAIRT_SDK_ROOT   : (unset, using scripts/download-qairt-sdk.sh)"
fi
echo "============================================================"
echo ""

# ─── Step 1: Ensure ubuntu:24.04 exists ──────────────────────────────────────
echo "[1/5] Checking ubuntu:24.04 base image"

if docker image inspect ubuntu:24.04 &>/dev/null && [ "${FORCE_REBUILD}" != "1" ]; then
    echo "      ✓ ubuntu:24.04 already available"
    docker images ubuntu:24.04 --format "        ID={{.ID}}  Size={{.Size}}"
else
    echo "      ubuntu:24.04 not found. Downloading from cloud-images.ubuntu.com ..."
    echo "      (Docker Hub is blocked on Qualcomm network — using Ubuntu cloud images)"
    echo ""
    bash "${REPO_ROOT}/scripts/pull-ubuntu-arm64.sh"
fi
echo ""

# ─── Step 2: Ensure ubuntu-runtime:24.04 exists ──────────────────────────────
echo "[2/5] Checking minimal runtime base: ubuntu-runtime:24.04"

if docker image inspect "ubuntu-runtime:24.04" &>/dev/null && [ "${FORCE_REBUILD}" != "1" ]; then
    echo "      ✓ ubuntu-runtime:24.04 already exists"
    docker images "ubuntu-runtime:24.04" --format "        ID={{.ID}}  Size={{.Size}}"
else
    echo "      Creating ubuntu-runtime:24.04 (strips firmware/snapd/docs)..."
    echo ""
    bash "${REPO_ROOT}/scripts/create-runtime-base.sh"
fi
echo ""

# ─── Step 3: Prepare QAIRT SDK slice at repo root ────────────────────────────
# MUST happen BEFORE building genai-build-base, because Dockerfile.build-base
# does: COPY qairt-sdk/ /opt/qairt/
# The SDK is baked into genai-build-base:latest — no per-service copy needed.
echo "[3/5] Preparing QAIRT SDK slice (repo root → baked into genai-build-base)"

QAIRT_DEST="${REPO_ROOT}/qairt-sdk"
NEED_BASE_REBUILD=0

if [ -d "${QAIRT_DEST}" ] && [ "${FORCE_REBUILD}" != "1" ]; then
    echo "      ✓ qairt-sdk/ already exists at repo root ($(du -sh "${QAIRT_DEST}" | cut -f1))"
else
    echo "      Running download-qairt-sdk.sh --service base ..."
    echo ""
    env \
        QAIRT_VERSION="${QAIRT_VERSION}" \
        QAIRT_SDK_ROOT="${QAIRT_SDK_ROOT}" \
        FORCE_DOWNLOAD="${FORCE_REBUILD}" \
        bash "${REPO_ROOT}/scripts/download-qairt-sdk.sh" --service base

    echo ""
    echo "      ✓ qairt-sdk/ ready at repo root ($(du -sh "${QAIRT_DEST}" | cut -f1))"
    NEED_BASE_REBUILD=1   # SDK was just created → must rebuild genai-build-base
fi
echo ""

# ─── Step 4: Ensure genai-build-base:latest exists (with SDK baked in) ───────
echo "[4/5] Checking build base image: ${BUILD_BASE_IMAGE}"

if docker image inspect "${BUILD_BASE_IMAGE}" &>/dev/null \
   && [ "${FORCE_REBUILD}" != "1" ] \
   && [ "${NEED_BASE_REBUILD}" = "0" ]; then
    echo "      ✓ ${BUILD_BASE_IMAGE} already exists (with QAIRT SDK at /opt/qairt/)"
    docker images "${BUILD_BASE_IMAGE}" --format "        ID={{.ID}}  Size={{.Size}}  Created={{.CreatedAt}}"
else
    echo "      Building ${BUILD_BASE_IMAGE} ..."
    echo "      (cmake + gcc + Qualcomm dev headers + nlohmann + httplib + QAIRT SDK)"
    echo ""
    DOCKER_BUILDKIT=1 docker build \
        --progress=plain \
        -f "${REPO_ROOT}/Dockerfile.build-base" \
        -t "${BUILD_BASE_IMAGE}" \
        "${REPO_ROOT}"
    echo ""
    echo "      ✓ ${BUILD_BASE_IMAGE} built (QAIRT SDK baked in at /opt/qairt/)"
fi
echo ""

# ─── Step 5: Build image-to-text:responses-v1 ────────────────────────────────
echo "[5/5] Building Docker image: ${IMAGE_TO_TEXT_IMAGE}"
echo "      Builder : ${BUILD_BASE_IMAGE} (QAIRT SDK at /opt/qairt/)"
echo "      Runtime : ubuntu-runtime:24.04 (flat layer, no firmware)"
echo ""

DOCKER_BUILDKIT=1 docker build \
    --progress=plain \
    -t "${IMAGE_TO_TEXT_IMAGE}" \
    "${SCRIPT_DIR}"

echo ""
echo "============================================================"
echo " Build complete!"
echo "============================================================"
docker images "${IMAGE_TO_TEXT_IMAGE}" --format "  {{.Repository}}:{{.Tag}}  {{.Size}}  {{.CreatedAt}}"
echo ""
echo "Run:"
echo "  docker compose up -d   (from repo root)"
echo ""
echo "Test:"
echo "  curl http://localhost:8080/health"
