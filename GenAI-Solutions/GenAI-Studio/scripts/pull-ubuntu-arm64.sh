#!/bin/bash
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# pull-ubuntu-arm64.sh
#
# Downloads ubuntu:24.04 ARM64 directly on the target device.
# Works WITHOUT Docker Hub access (Docker Hub is blocked on Qualcomm network).
#
# Method: Downloads the official Ubuntu 24.04 ARM64 rootfs tarball from
#         cloud-images.ubuntu.com (accessible on Qualcomm network) and
#         imports it into Docker with `docker import`.
#
# ─── Usage ───────────────────────────────────────────────────────────────────
#   bash scripts/pull-ubuntu-arm64.sh
#
# ─── What it does ────────────────────────────────────────────────────────────
#   1. Checks if ubuntu:24.04 already exists (skips if so)
#   2. Downloads noble-server-cloudimg-arm64-root.tar.xz (~210 MB)
#      from https://cloud-images.ubuntu.com/noble/current/
#   3. Imports it as ubuntu:24.04 via docker import
#   4. Deletes the downloaded file
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

UBUNTU_VERSION="24.04"
UBUNTU_CODENAME="noble"
ROOTFS_URL="https://cloud-images.ubuntu.com/${UBUNTU_CODENAME}/current/${UBUNTU_CODENAME}-server-cloudimg-arm64-root.tar.xz"
ROOTFS_FILE="/tmp/ubuntu-${UBUNTU_CODENAME}-arm64-root.tar.xz"

echo "============================================================"
echo " Ubuntu ${UBUNTU_VERSION} ARM64 — Docker Image Setup"
echo "============================================================"
echo " Source : ${ROOTFS_URL}"
echo " Method : curl + docker import (no Docker Hub needed)"
echo "============================================================"
echo ""

# ─── Check if already available ──────────────────────────────────────────────
if docker image inspect "ubuntu:${UBUNTU_VERSION}" &>/dev/null; then
    echo "✓ ubuntu:${UBUNTU_VERSION} already available"
    docker images "ubuntu:${UBUNTU_VERSION}" --format "  ID={{.ID}}  Size={{.Size}}  Created={{.CreatedAt}}"
    exit 0
fi

# ─── Download rootfs ─────────────────────────────────────────────────────────
echo "[1/3] Downloading Ubuntu ${UBUNTU_VERSION} ARM64 rootfs (~210 MB)"
echo "      URL: ${ROOTFS_URL}"
echo ""

curl -L \
    --progress-bar \
    --retry 3 \
    --connect-timeout 30 \
    -o "${ROOTFS_FILE}" \
    "${ROOTFS_URL}"

FILESIZE=$(du -sh "${ROOTFS_FILE}" | cut -f1)
echo ""
echo "      ✓ Downloaded: ${ROOTFS_FILE} (${FILESIZE})"
echo ""

# ─── Import into Docker ───────────────────────────────────────────────────────
echo "[2/3] Importing as ubuntu:${UBUNTU_VERSION} ..."
docker import "${ROOTFS_FILE}" "ubuntu:${UBUNTU_VERSION}"
echo "      ✓ Imported"
echo ""

# ─── Cleanup ─────────────────────────────────────────────────────────────────
echo "[3/3] Cleaning up ..."
rm -f "${ROOTFS_FILE}"
echo "      ✓ Deleted ${ROOTFS_FILE}"
echo ""

# ─── Verify ──────────────────────────────────────────────────────────────────
echo "============================================================"
echo " ubuntu:${UBUNTU_VERSION} ready!"
echo "============================================================"
docker images "ubuntu:${UBUNTU_VERSION}" --format "  ID={{.ID}}  Size={{.Size}}  Created={{.CreatedAt}}"
echo ""
echo "Next step:"
echo "  DOCKER_BUILDKIT=1 docker build -f Dockerfile.build-base -t genai-build-base:latest ."
