#!/bin/bash
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Text-Generation – single-command build script
#
# Handles EVERYTHING automatically:
#   1. ubuntu:24.04    → downloads from cloud-images.ubuntu.com if missing
#   2. genai-build-base → builds from Dockerfile.build-base if missing
#   3. QAIRT SDK slice  → downloads/copies from local SDK if missing
#   4. text-to-text:latest → builds the final service image
#
# ─── Usage ───────────────────────────────────────────────────────────────────
#   # Fresh device — everything automatic:
#   bash build.sh
#
#   # Use existing local QAIRT SDK (skip download):
#   QAIRT_SDK_ROOT=/path/to/qairt/2.45.0.260326 bash build.sh
#
#   # Specific QAIRT version:
#   QAIRT_VERSION=2.45.0.260326 bash build.sh
#
#   # Force rebuild everything:
#   FORCE_REBUILD=1 bash build.sh
#
# ─── Environment variables ───────────────────────────────────────────────────
#   QAIRT_SDK_ROOT       Local QAIRT SDK path (skips download if set)
#   QAIRT_VERSION        SDK version to use (default: loaded from versions.env)
#   QAIRT_DOWNLOAD_URL   Override download URL
#   BUILD_BASE_IMAGE     Builder base image (default: genai-build-base:latest)
#   TEXT_TO_TEXT_IMAGE   Output image name  (default: text-to-text:latest)
#   FORCE_REBUILD        Set to 1 to force rebuild everything
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

# Load canonical versions (if present).
if [[ -f "${REPO_ROOT}/versions.env" ]]; then
    set -a
    # shellcheck disable=SC1091
    source "${REPO_ROOT}/versions.env"
    set +a
fi

# ─── Configuration ────────────────────────────────────────────────────────────
QAIRT_VERSION="${QAIRT_VERSION:-2.45.0.260326}"
QAIRT_SDK_ROOT="${QAIRT_SDK_ROOT:-}"
BUILD_BASE_IMAGE="${BUILD_BASE_IMAGE:-genai-build-base:latest}"
TEXT_TO_TEXT_IMAGE="${TEXT_TO_TEXT_IMAGE:-text-to-text:latest}"
FORCE_REBUILD="${FORCE_REBUILD:-0}"

echo "============================================================"
echo " Text-Generation – Single-Command Docker Build"
echo "============================================================"
echo " BUILD_BASE_IMAGE : ${BUILD_BASE_IMAGE}"
echo " OUTPUT_IMAGE     : ${TEXT_TO_TEXT_IMAGE}"
echo " QAIRT_VERSION    : ${QAIRT_VERSION}"
if [ -n "${QAIRT_SDK_ROOT}" ]; then
    echo " QAIRT_SDK_ROOT   : ${QAIRT_SDK_ROOT} (local)"
else
    echo " QAIRT_SDK_ROOT   : (auto: resolve from /opt/qairt/current or /opt/qairt)"
fi
echo "============================================================"
echo ""

# ─── Step 1: Ensure ubuntu:24.04 exists ──────────────────────────────────────
echo "[1/4] Checking ubuntu:24.04 base image"

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
    echo "      Creating ubuntu-runtime:24.04 (strips firmware/snapd/docs, target <300 MB)..."
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

# ─── Step 5: Build text-to-text:latest ───────────────────────────────────────
echo "[5/5] Building Docker image: ${TEXT_TO_TEXT_IMAGE}"
echo "      Builder : ${BUILD_BASE_IMAGE}"
echo "      Runtime : ubuntu-runtime:24.04 (flat layer, no firmware, target <500 MB)"
echo ""

DOCKER_BUILDKIT=1 docker build \
    --progress=plain \
    -t "${TEXT_TO_TEXT_IMAGE}" \
    "${SCRIPT_DIR}"

echo ""
echo "============================================================"
echo " Build complete!"
echo "============================================================"
docker images "${TEXT_TO_TEXT_IMAGE}" --format "  {{.Repository}}:{{.Tag}}  {{.Size}}  {{.CreatedAt}}"
echo ""
echo "Run:"
echo "  bash run.sh"
echo ""
echo "Or with docker compose (starts all services):"
echo "  cd .. && docker compose up -d"
echo ""
echo "Test:"
echo "  curl http://localhost:8088/health"
