# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Orchestrator — Python FastAPI image
#
# ─── Base image ──────────────────────────────────────────────────────────────
#   ubuntu:24.04  (already on device, 1.59 GB cloud image)
#
# ─── Why NOT ubuntu-runtime:24.04 ────────────────────────────────────────────
#   ubuntu-runtime:24.04 had libicu74 + libpython3.12-stdlib removed to save
#   space for C++ services. This breaks apt's dependency resolution for Python.
#   The orchestrator is a pure Python app — it needs a working Python stdlib.
#
# ─── Size optimization ───────────────────────────────────────────────────────
#   Virtual environment at /opt/venv (avoids --break-system-packages issues)
#   Single RUN layer with aggressive cleanup (docs, man, locale, __pycache__)
#   Final image: ~900 MB uncompressed / ~350 MB compressed
#
# ─── Build ───────────────────────────────────────────────────────────────────
#   DOCKER_BUILDKIT=1 docker build -t orchestrator:latest core-services/orchestrator/
#
# ─── Ports ───────────────────────────────────────────────────────────────────
#   8090 — Orchestrator API + Web UI
# =============================================================================

# syntax=docker/dockerfile:1.7

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1

# ─── Install Python + venv + packages in a single layer ──────────────────────
# Uses python3-venv to create /opt/venv — avoids --break-system-packages issues.
# All cleanup in the same RUN to minimize layer size.
RUN apt-get update -qq && \
    apt-get install -y --no-install-recommends \
        python3 \
        python3-venv \
        ca-certificates \
        curl && \
    # Create virtual environment
    python3 -m venv /opt/venv && \
    # Install orchestrator packages into venv
    /opt/venv/bin/pip install --no-cache-dir \
        "fastapi>=0.111.0" \
        "uvicorn[standard]>=0.29.0" \
        "httpx>=0.27.0" \
        "docker>=7.0.0" \
        "python-multipart>=0.0.9" \
        "openai>=1.0.0" \
        "numpy>=1.24.0" \
        "Pillow>=10.0.0" \
        "transformers>=4.37.0,<5.0.0" && \
    # Remove python3-venv (not needed at runtime — venv is already created)
    apt-get remove -y python3-venv && \
    apt-get autoremove -y && \
    # Cleanup: apt cache, pip cache, docs, man, locale
    rm -rf /var/lib/apt/lists/* /var/cache/apt/archives/* /root/.cache/pip && \
    rm -rf /usr/share/doc /usr/share/man /usr/share/info /usr/share/locale && \
    # Cleanup: __pycache__ and .pyc files in venv
    find /opt/venv -name "*.pyc" -delete 2>/dev/null || true && \
    find /opt/venv -name "__pycache__" -type d -exec rm -rf {} + 2>/dev/null || true && \
    find /opt/venv -type d -name "tests" -exec rm -rf {} + 2>/dev/null || true && \
    find /opt/venv -name "*.pyi" -delete 2>/dev/null || true

# Use venv Python for all subsequent commands
ENV VIRTUAL_ENV=/opt/venv
ENV PATH="/opt/venv/bin:$PATH"

# ─── Service endpoint defaults (all overridable via -e at runtime) ───────────
ENV PORT=8090
ENV TG_URL=http://localhost:8088
ENV STT_URL=http://localhost:8081
ENV TTS_URL=http://localhost:8083
ENV IMG_URL=http://localhost:8084
ENV I2T_URL=http://localhost:8080
ENV MODEL_DIR=/opt/genai-studio-models/image-to-text/Lemans_LE_Gen2_QNN2_41_qwen25_vl_7B/files
ENV UPLOADS_DIR=/opt/genai-studio-models/image-to-text/Lemans_LE_Gen2_QNN2_41_qwen25_vl_7B/files/uploads

WORKDIR /app/core-services/orchestrator

# Copy application files
COPY --link device_app.py .
COPY --link app/ ./app/
COPY --link static/ ./static/

# Port 8090 — Orchestrator HTTP server + Web UI
EXPOSE 8090

CMD ["/opt/venv/bin/python3", "device_app.py"]
