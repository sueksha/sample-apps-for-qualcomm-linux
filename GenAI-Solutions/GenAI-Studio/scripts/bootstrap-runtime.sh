#!/bin/bash
# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

bash "${REPO_ROOT}/scripts/create-runtime-base.sh"
DOCKER_BUILDKIT=1 docker build --progress=plain -f "${REPO_ROOT}/Dockerfile.runtime" -t genai-runtime:latest "${REPO_ROOT}"
