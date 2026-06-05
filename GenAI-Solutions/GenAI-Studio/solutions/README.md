# GenAI Studio — Solutions

A collection of examples showing how to point OpenAI-compatible open-source clients and applications at **GenAI Studio v2** endpoints with minimal or no code changes.

Each sub-folder contains a drop-in integration guide and any required patch files.

---

## GenAI Studio

GenAI Studio v2 runs on a Qualcomm QLI target device and exposes an OpenAI-compatible REST API through its orchestrator on port `8090`. Most OpenAI clients work against GenAI Studio by changing just the **IP** and the **port number**.

---

## Solutions

| Folder | Repo | Language / Framework | What it demonstrates |
|--------|------|---------------------|----------------------|
| [`android-application/maid`](./android-application/maid/GENAI_STUDIO_INTEGRATION.md) | [Mobile-Artificial-Intelligence/maid](https://github.com/Mobile-Artificial-Intelligence/maid) | React Native / Expo (Android) | Streaming chat completions and model listing via the OpenAI provider — no code changes needed |
| [`anything-llm`](./anything-llm/GENAI_STUDIO_INTEGRATION.md) | [Mintplex-Labs/anything-llm](https://github.com/Mintplex-Labs/anything-llm) | Docker · Windows app · Ubuntu app | Full RAG assistant with chat, TTS, and STT all routed through GenAI Studio — deployable via Docker or the native desktop app |
| [`cpp-application`](./cpp-application/GENAI_STUDIO_INTEGRATION.md) | [olrea/openai-cpp](https://github.com/olrea/openai-cpp) | C++ (header-only) | Chat completions, model listing, and STT transcription using the `openai-cpp` library |
| [`web-application/image-generation`](./web-application/image-generation/GENAI_STUDIO_INTEGRATION.md) | [prashver/text-to-image-generation-with-DALL-E](https://github.com/prashver/text-to-image-generation-with-DALL-E) | Python / Streamlit | Text-to-image generation via `stable-diffusion-2-1` replacing DALL-E in the ImagiCraft app |
| [`web-application/image-to-text`](./web-application/image-to-text/GENAI_STUDIO_INTEGRATION.md) | [sbslee/streamlit-openai](https://github.com/sbslee/streamlit-openai) | Python / Streamlit | Vision model (image-to-text) via `qwen2.5-vl-7b-instruct` using the OpenAI Responses API for image analysis and description |
| [`web-application/text-to-speech`](./web-application/text-to-speech/GENAI_STUDIO_INTEGRATION.md) | [arham-kk/openai-tts](https://github.com/arham-kk/openai-tts) | Python / Gradio | Text-to-speech via `melo-tts-English` replacing OpenAI TTS in the openai-tts app |

---

## Supported Endpoints

- `GET /v1/models`
- `GET /v1/models/{id}`
- `POST /v1/chat/completions`
- `POST /v1/audio/transcriptions`
- `GET /v1/audio/transcriptions/stream`
- `POST /v1/audio/transcriptions/stream`
- `POST /v1/audio/translations`
- `GET /v1/realtime`
- `POST /v1/realtime/sessions`
- `GET /v1/realtime/sessions/{id}`
- `DELETE /v1/realtime/sessions/{id}`
- `POST /v1/realtime/sessions/{id}/audio`
- `POST /v1/realtime/sessions/{id}/finalize`
- `POST /v1/audio/speech`
- `POST /v1/images/generations`
- `GET /v1/images/generations/params`
- `POST /v1/responses`
- `POST /v1/session/reset`

---

## Quick Start

1. Confirm GenAI Studio is running on your QLI target:
   ```bash
   curl http://<GENAI_DEVICE_IP>:8090/api/status
   curl http://<GENAI_DEVICE_IP>:8090/v1/models
   ```

2. Pick a solution folder and follow its `GENAI_STUDIO_INTEGRATION.md`.

---

## Port Reference

| Port | Service |
|------|---------|
| `8090` | Orchestrator — use this for all client traffic |
| `8088` | Text-Generation (direct, debug only) |
| `8083` | Text-To-Speech (direct, debug only) |
| `8081` | Speech-To-Text (direct, debug only) |
| `8084` | Image-Generation (direct, debug only) |
| `8080` | Image-To-Text (direct, debug only) |

---

## Tested On

- GenAI Studio v2 · QLI target `IQ9 / QCS9075`
- Host machine: x86-64 Ubuntu 22.04