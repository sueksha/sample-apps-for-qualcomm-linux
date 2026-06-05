#!/usr/bin/env python3
# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
"""
GenAI Studio v2 — Orchestrator (Device Edition)

This file now focuses on app wiring.
Use-case logic is split into dedicated modules under orchestrator/app/.
"""

import os
import warnings

from fastapi import FastAPI, Request
from fastapi.middleware.cors import CORSMiddleware

from app.common import _ms_since, _now
from app.context import (
    I2T_URL,
    STT_URL,
    TG_URL,
    TTS_URL,
    UPLOADS_DIR,
    close_http_clients,
)
from app.i2t import router as i2t_router
from app.imagegen import router as imagegen_router
from app.openai import router as openai_router
from app.stt import router as stt_router
from app.system import router as system_router
from app.tg import router as tg_router

warnings.filterwarnings("ignore")

app = FastAPI(title="GenAI Studio Orchestrator (Device)", version="2.0.0")
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.on_event("shutdown")
async def _shutdown() -> None:
    await close_http_clients()


@app.middleware("http")
async def add_request_timing_header(request: Request, call_next):
    t0 = _now()
    response = await call_next(request)
    response.headers["X-Orchestrator-Time-Ms"] = str(_ms_since(t0))
    return response


app.include_router(system_router)
app.include_router(tg_router)
app.include_router(stt_router)
app.include_router(imagegen_router)
app.include_router(openai_router)
app.include_router(i2t_router)


if __name__ == "__main__":
    import uvicorn

    port = int(os.getenv("PORT", "8080"))
    print(f"\n{'=' * 60}")
    print("  GenAI Studio (Device, OpenAI Package)")
    print(f"  UI             ->  http://0.0.0.0:{port}")
    print(f"  Text-Gen       ->  {TG_URL}/v1")
    print(f"  Speech-To-Text ->  {STT_URL}/v1")
    print(f"  Image-To-Text  ->  {I2T_URL}/v1/responses")
    print(f"  Uploads dir    ->  {UPLOADS_DIR}")
    print(f"{'=' * 60}\n")
    uvicorn.run("device_app:app", host="0.0.0.0", port=port, reload=False)
