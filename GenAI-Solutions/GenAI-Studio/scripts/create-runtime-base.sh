#!/bin/bash
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# create-runtime-base.sh
#
# Creates a minimal Ubuntu 24.04 ARM64 runtime base image for GenAI Studio.
# Target size: < 300 MB (vs 1.25 GB cloud image + 908 MB apt layer = 2.15 GB)
#
# ─── Why this is needed ───────────────────────────────────────────────────────
# The ubuntu:24.04 from cloud-images.ubuntu.com is a FULL server image:
#   linux-firmware          547 MB  ← firmware blobs (loaded by HOST kernel)
#   linux-firmware-dragonwing 311 MB  ← Qualcomm firmware (loaded by HOST kernel)
#   snapd                   120 MB  ← snap package manager (not needed)
#   python3-botocore         92 MB  ← AWS SDK (not needed)
#   vim-runtime, git, etc.  ~80 MB  ← dev tools (not needed)
#
# qcom-fastrpc1 pulls in linux-firmware as a dependency, adding 858 MB.
# None of this firmware is needed INSIDE the container — it's loaded by the
# host kernel. We strip it all out using docker export + docker import.
#
# ─── Technique: docker export + docker import ─────────────────────────────────
# Unlike multi-stage builds (which stack layers), docker export creates a
# FLAT single-layer image with only the files that exist at export time.
# This is the only way to truly reduce image size in Docker.
#
# ─── Usage ───────────────────────────────────────────────────────────────────
#   bash scripts/create-runtime-base.sh
#
# ─── Output ──────────────────────────────────────────────────────────────────
#   ubuntu-runtime:24.04  — minimal Ubuntu + qcom-fastrpc1 (no firmware)
#   Expected size: ~250-350 MB (vs 2.15 GB previously)
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

RUNTIME_IMAGE="ubuntu-runtime:24.04"
BUILDER_CONTAINER="genai-runtime-builder-$$"

echo "============================================================"
echo " Creating minimal Ubuntu runtime base: ${RUNTIME_IMAGE}"
echo "============================================================"
echo " Technique : docker run → strip → docker export → docker import"
echo " Target    : < 300 MB (removes firmware, snapd, docs, locales)"
echo "============================================================"
echo ""

# ─── Check if already exists ─────────────────────────────────────────────────
if docker image inspect "${RUNTIME_IMAGE}" &>/dev/null; then
    echo "✓ ${RUNTIME_IMAGE} already exists"
    docker images "${RUNTIME_IMAGE}" --format "  ID={{.ID}}  Size={{.Size}}"
    exit 0
fi

# ─── Ensure ubuntu:24.04 base exists ─────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if ! docker image inspect ubuntu:24.04 &>/dev/null; then
    echo "ubuntu:24.04 not found. Downloading..."
    bash "${SCRIPT_DIR}/pull-ubuntu-arm64.sh"
fi

# ─── Cleanup on exit ─────────────────────────────────────────────────────────
cleanup() {
    docker rm -f "${BUILDER_CONTAINER}" 2>/dev/null || true
}
trap cleanup EXIT

echo "[1/3] Building minimal runtime environment in container..."
echo "      Installing: qcom-fastrpc1, qcom-libdmabufheap, libstdc++6, libatomic1, curl"
echo "      Removing  : linux-firmware (547 MB), linux-firmware-dragonwing (311 MB),"
echo "                  snapd (120 MB), python3-botocore (92 MB), docs, locales"
echo ""

docker run --name "${BUILDER_CONTAINER}" ubuntu:24.04 bash -c '
    set -e
    export DEBIAN_FRONTEND=noninteractive

    # Install required runtime packages
    apt-get update -qq
    apt-get install -y --no-install-recommends \
        libstdc++6 \
        libgcc-s1 \
        libatomic1 \
        ca-certificates \
        curl \
        bash \
        software-properties-common

    # Add Qualcomm PPA and install FastRPC runtime
    apt-add-repository -s ppa:ubuntu-qcom-iot/qcom-ppa
    apt-get update -qq
    apt-get install -y --no-install-recommends \
        qcom-fastrpc1 \
        qcom-libdmabufheap

    # Remove software-properties-common (no longer needed)
    apt-get remove -y --purge software-properties-common
    apt-get autoremove -y

    # ── AGGRESSIVE CLEANUP ────────────────────────────────────────────────────
    # Remove firmware packages (858 MB) — firmware is loaded by HOST kernel,
    # NOT needed inside the container
    dpkg --remove --force-depends \
        linux-firmware \
        linux-firmware-dragonwing \
        2>/dev/null || true

    # Remove other large unnecessary packages
    dpkg --remove --force-depends \
        snapd \
        python3-botocore \
        python3-twisted \
        python3-urllib3 \
        python3-dateutil \
        python3-jmespath \
        vim-runtime \
        git \
        git-man \
        locales \
        iso-codes \
        systemd \
        udev \
        perl \
        perl-modules-5.38 \
        libperl5.38t64 \
        libpython3.12-stdlib \
        libpython3.12t64 \
        libicu74 \
        iptables \
        2>/dev/null || true

    apt-get autoremove -y 2>/dev/null || true
    apt-get clean

    # Remove firmware files directly (in case dpkg remove did not get them all)
    rm -rf /lib/firmware /usr/lib/firmware /usr/share/firmware

    # Remove documentation, man pages, locale data
    rm -rf \
        /usr/share/doc \
        /usr/share/man \
        /usr/share/locale \
        /usr/share/info \
        /usr/share/lintian \
        /usr/share/games \
        /usr/share/sounds \
        /usr/share/zoneinfo \
        /usr/share/i18n \
        /usr/share/bug

    # Remove APT cache and lists
    rm -rf \
        /var/cache/apt \
        /var/lib/apt/lists \
        /var/log/apt \
        /var/log/*.log

    # Remove temp files
    rm -rf /tmp/* /var/tmp/* /root/.cache

    echo "=== Remaining large packages ==="
    dpkg-query -W -f="${Package}\t${Installed-Size}\n" 2>/dev/null | sort -k2 -rn | head -10 || true
    echo "=== Disk usage ==="
    du -sh / --exclude=/proc 2>/dev/null || true
'

echo ""
echo "[2/3] Exporting flat filesystem and importing as ${RUNTIME_IMAGE}..."
echo "      (docker export creates a single flat layer — no layer overhead)"
echo ""

docker export "${BUILDER_CONTAINER}" | docker import \
    --change 'ENV DEBIAN_FRONTEND=noninteractive' \
    --change 'ENV LD_LIBRARY_PATH=/opt/genie_bundle:/usr/lib' \
    --change 'CMD ["/bin/bash"]' \
    - "${RUNTIME_IMAGE}"

echo ""
echo "[3/3] Verifying..."
docker rm -f "${BUILDER_CONTAINER}" 2>/dev/null || true

echo ""
echo "============================================================"
echo " ${RUNTIME_IMAGE} created!"
echo "============================================================"
docker images "${RUNTIME_IMAGE}" --format "  Size={{.Size}}  ID={{.ID}}  Created={{.CreatedAt}}"
echo ""
echo "Next step:"
echo "  DOCKER_BUILDKIT=1 docker build -t text-to-text:latest core-services/text-to-text/"
