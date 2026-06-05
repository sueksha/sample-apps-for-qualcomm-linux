# openai-cpp + GenAI Studio Integration Guide

End-to-end guide: clone → build → run against Qualcomm GenAI Studio endpoints.

Tested on: openai-cpp `main`, GenAI Studio v2, host machine x86-64 Ubuntu 22.04.

---

## What This Library Does

`openai-cpp` is a lightweight, header-only C++ library that wraps the OpenAI
REST API using libcurl. Because GenAI Studio exposes an OpenAI-compatible API
through its orchestrator, the library works with GenAI Studio with **zero code
changes to the library itself** — only the model names in the example files
need updating, and two environment variables must be set.

**Supported GenAI Studio endpoints via this library:**

| Feature | Endpoint | Example file | Library call |
|---------|----------|--------------|--------------|
| List / retrieve models | `GET /v1/models` | `examples/01-model.cpp` | `openai::model().list()` |
| Chat completion | `POST /v1/chat/completions` | `examples/10-chat.cpp` | `openai::chat().create(...)` |
| STT transcription | `POST /v1/audio/transcriptions` | `examples/11-audio.cpp` | `openai::audio().transcribe(...)` |

**Not supported** (not in this library — added to OpenAI API after library was written):
- Text-To-Speech (`POST /v1/audio/speech`) — use `curl` or AnythingLLM instead

---

## Prerequisites

| Requirement | Check command |
|-------------|---------------|
| g++ with C++11 or later | `g++ --version` |
| libcurl runtime | `ldconfig -p \| grep libcurl` |
| libcurl headers | `find /usr/include -name curl.h 2>/dev/null` — on this Qualcomm server headers are in the ESDK sysroot (see Step 4) |
| `git` | `git --version` |
| GenAI Studio v2 running on QLI target | `curl http://<GENAI_IP>:8090/api/status` |

### Installing libcurl headers (if missing)

On Ubuntu/Debian:
```bash
sudo apt-get install libcurl4-openssl-dev
```

On systems without package access, point to an existing sysroot — see
[Build Without System curl Headers](#build-without-system-curl-headers) in Step 4.

---

## Step 1 — Clone the Repository

```bash
git clone https://github.com/olrea/openai-cpp.git
cd openai-cpp
```

The library is header-only. The only files you need are:
- `include/openai/openai.hpp` — the library
- `include/openai/nlohmann/json.hpp` — bundled JSON library

---

## Step 2 — Set Environment Variables

```bash
export OPENAI_API_KEY='sk-dummy'
export OPENAI_API_BASE='http://<GENAI_DEVICE_IP>:8090/v1'
```

Replace `<GENAI_DEVICE_IP>` with the IP of your QLI target (e.g. `10.92.165.205`).

- `OPENAI_API_KEY` — GenAI Studio does not validate the key; any non-empty string works.
- `OPENAI_API_BASE` — points the library at the GenAI Studio orchestrator instead
  of `api.openai.com`. The library appends a trailing `/` automatically **when
  the URL is read from this environment variable**. When passing the URL directly
  to `openai::start()` in code, you must include the trailing `/` yourself:

```cpp
// In code — trailing slash required
openai::start("sk-dummy", "", true, "http://<GENAI_DEVICE_IP>:8090/v1/");
```

---

## Step 3 — Verify GenAI Studio is Running

```bash
curl http://<GENAI_DEVICE_IP>:8090/api/status
curl http://<GENAI_DEVICE_IP>:8090/v1/models
```

`/api/status` should show all services as `"status":"ok"`.
`/v1/models` should return a list including `llama3.2-3B`, `whisper-tiny`,
`melo-tts-English`, and several image-generation models.

---

## Step 4 — Update Example Model Names

The upstream examples use hardcoded OpenAI model names that GenAI Studio
rejects. The three relevant examples in this repo have already been updated
with the correct GenAI Studio model names:

| File | Original model | Updated model |
|------|---------------|---------------|
| `examples/01-model.cpp` | `text-davinci-003` | `llama3.2-3B` |
| `examples/10-chat.cpp` | `gpt-3.5-turbo` | `genie` |
| `examples/11-audio.cpp` | `whisper-1` | `whisper-tiny` |

> **Model names for GenAI Studio:**
> - `genie` — orchestrator alias, routes to the active LLM (recommended for chat)
> - `llama3.2-3B` — calls Text-Generation directly (debug only)
> - `whisper-tiny` — STT model; `whisper-1` is rejected by GenAI Studio v2

---

## Step 5 — Build

### Standard build (system libcurl headers available)

```bash
g++ -std=c++11 -I include/openai examples/10-chat.cpp -o 10-chat -lcurl
g++ -std=c++11 -I include/openai examples/01-model.cpp -o 01-model -lcurl
g++ -std=c++11 -I include/openai examples/11-audio.cpp -o 11-audio -lcurl
```

### Build Without System curl Headers

If `libcurl-dev` is not installed but the runtime library exists at
`/lib/x86_64-linux-gnu/libcurl.so.4`, point to an existing sysroot.

On this Qualcomm development server the ESDK sysroot provides the headers:

```bash
CURL_INC=/local/mnt/workspace/mshree/Sarvam_TTS/qairt/ESDK/tmp/sysroots/x86_64/usr/include

g++ -std=c++17 -I include/openai -I ${CURL_INC} examples/10-chat.cpp -o 10-chat /lib/x86_64-linux-gnu/libcurl.so.4
g++ -std=c++17 -I include/openai -I ${CURL_INC} examples/01-model.cpp -o 01-model /lib/x86_64-linux-gnu/libcurl.so.4
g++ -std=c++17 -I include/openai -I ${CURL_INC} examples/11-audio.cpp -o 11-audio /lib/x86_64-linux-gnu/libcurl.so.4
```

### Build via CMake

```bash
CURL_INC=/local/mnt/workspace/mshree/Sarvam_TTS/qairt/ESDK/tmp/sysroots/x86_64/usr/include

mkdir build && cd build
cmake .. \
  -DCURL_INCLUDE_DIR="${CURL_INC}" \
  -DCURL_LIBRARY=/lib/x86_64-linux-gnu/libcurl.so.4
make -j$(nproc)
```

---

## Step 6 — Run

### List and retrieve models (`01-model`)

```bash
./01-model
```

Expected output (first two models from the list, then a retrieve of `llama3.2-3B`):

```json
{"created":...,"id":"llama3.2-3B","object":"model","owned_by":"qualcomm",...}
{"capabilities":["text","vision"],"id":"qwen2.5-vl-7b-instruct","object":"model",...}
{"created":...,"id":"llama3.2-3B","object":"model","owned_by":"qualcomm",...}
```

### Chat completion (`10-chat`)

```bash
./10-chat
```

Expected output:

```json
{
  "choices": [
    {
      "finish_reason": "stop",
      "index": 0,
      "message": {
        "content": "<non-deterministic LLM response>",
        "role": "assistant"
      }
    }
  ],
  "model": "llama3.2-3B",
  "object": "chat.completion",
  ...
}
```

The `content` field is non-deterministic — it will vary between runs.
The `model` field reflects the orchestrator's routing decision for `genie`.

### STT transcription (`11-audio`)

First generate a test WAV using the TTS endpoint:

```bash
curl -X POST http://<GENAI_DEVICE_IP>:8090/v1/audio/speech \
  -H 'Content-Type: application/json' \
  -o /tmp/test.wav \
  -d '{"model":"tts-1","voice":"alloy","input":"Hello from GenAI Studio."}'
```

> The `"file"` field in `11-audio.cpp` is opened from disk by libcurl. It must
> be a real, readable local file path. A missing file causes a libcurl read
> error, not a JSON error from the server.

Then run:

```bash
./11-audio
```

Expected output (transcription block):

```json
{
  "text": "",
  "x_timing": { ... }
}
```

> The transcript is empty for short TTS-generated clips — Whisper Tiny has low
> sensitivity on synthesized speech. The call still succeeds (HTTP 200, valid
> JSON with a `text` field). Use a real recorded voice WAV for non-empty output.

The translation block will also run but GenAI Studio does not support the
`/v1/audio/translations` endpoint — it will return an error response.

---

## Troubleshooting

**`fatal error: curl/curl.h: No such file or directory`**
`libcurl-dev` is not installed. Either install it (`sudo apt-get install libcurl4-openssl-dev`)
or use the sysroot path shown in Step 5.

**`cannot find -lcurl` during linking**
The system has only the versioned `.so.4` symlink, not the unversioned `.so`
that `-lcurl` requires. Link against the full path instead:
```bash
# Replace -lcurl with:
/lib/x86_64-linux-gnu/libcurl.so.4
```

**`Unsupported model 'gpt-3.5-turbo'` or `'whisper-1'`**
You are running an unpatched upstream example. Use the updated examples in this
repo which already have the correct model names (`genie`, `whisper-tiny`).

**`terminate called after throwing an instance of 'std::runtime_error'`**
The library throws on API errors by default. The exception message contains the
JSON error response from the server. To receive errors as JSON objects instead:
```cpp
openai::start("sk-dummy", "", false);  // throw_exception = false
```

**`Could NOT find CURL` during CMake**
Pass the hint variables explicitly:
```bash
cmake .. \
  -DCURL_INCLUDE_DIR=/path/to/curl/headers \
  -DCURL_LIBRARY=/lib/x86_64-linux-gnu/libcurl.so.4
```

---

## Quick Reference

```bash
# 1. Clone
git clone https://github.com/olrea/openai-cpp.git && cd openai-cpp

# 2. Set env vars
export OPENAI_API_KEY='sk-dummy'
export OPENAI_API_BASE='http://<GENAI_DEVICE_IP>:8090/v1'

# 3. Build (sysroot path — adjust if libcurl-dev is installed)
CURL_INC=/local/mnt/workspace/mshree/Sarvam_TTS/qairt/ESDK/tmp/sysroots/x86_64/usr/include
g++ -std=c++17 -I include/openai -I ${CURL_INC} examples/10-chat.cpp  -o 10-chat  /lib/x86_64-linux-gnu/libcurl.so.4
g++ -std=c++17 -I include/openai -I ${CURL_INC} examples/01-model.cpp -o 01-model /lib/x86_64-linux-gnu/libcurl.so.4
g++ -std=c++17 -I include/openai -I ${CURL_INC} examples/11-audio.cpp -o 11-audio /lib/x86_64-linux-gnu/libcurl.so.4

# 4. Generate test WAV for STT
curl -X POST http://<GENAI_DEVICE_IP>:8090/v1/audio/speech \
  -H 'Content-Type: application/json' -o /tmp/test.wav \
  -d '{"model":"tts-1","voice":"alloy","input":"Hello from GenAI Studio."}'

# 5. Run
./01-model
./10-chat
./11-audio
```

---

## Port Reference

| Port | Service | Use for |
|------|---------|---------|
| `8090` | Orchestrator | All client traffic — use this |
| `8088` | Text-Generation direct | Debug only |
| `8083` | TTS direct | Debug only |
| `8081` | STT direct | Debug only |

---

## Changes Made to the Upstream Repo

| File | Change |
|------|--------|
| `examples/10-chat.cpp` | Model `gpt-3.5-turbo` → `genie`; prompt updated |
| `examples/01-model.cpp` | Model `text-davinci-003` → `llama3.2-3B` in retrieve call |
| `examples/11-audio.cpp` | Model `whisper-1` → `whisper-tiny`; file paths updated to `/tmp/test.wav` |
| `GENAI_STUDIO_INTEGRATION.md` | This guide (new file) |
