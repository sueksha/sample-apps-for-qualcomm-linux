# openai-tts + GenAI Studio Integration Guide

End-to-end guide: clone → apply patch → run against Qualcomm GenAI Studio TTS endpoint.

Tested on: openai-tts `main`, GenAI Studio v2, host machine x86-64 Ubuntu 22.04.

---

## What This App Does

`openai-tts` is a lightweight Gradio web interface that calls the OpenAI
Text-To-Speech (TTS) API. Because GenAI Studio exposes an OpenAI-compatible
`POST /v1/audio/speech` endpoint through its orchestrator, the app works with
GenAI Studio with **minimal code changes** — only the `OpenAI` client
construction and a single environment variable are modified by the patch.

**Supported GenAI Studio endpoint via this app:**

| Feature | Endpoint | App module | OpenAI SDK call |
|---------|----------|------------|-----------------|
| Text-To-Speech synthesis | `POST /v1/audio/speech` | `tts()` in `app.py` | `client.audio.speech.create(...)` |

---

## Prerequisites

| Requirement | Check command |
|-------------|---------------|
| Python 3.8 or later | `python3 --version` |
| pip | `pip --version` |
| git | `git --version` |
| GenAI Studio v2 running on QLI target | `curl http://<GENAI_IP>:8090/api/status` |

---

## Step 1 — Clone the Repository

```bash
git clone https://github.com/arham-kk/openai-tts.git
cd openai-tts
```

Key files:

| File | Purpose |
|------|---------|
| `app.py` | Main Gradio app — `tts()` function + interface definition |
| `requirements.txt` | Python dependencies (`gradio`, `openai`) |

---

## Step 2 — Apply the Patch

```bash
git apply genai.patch
```

### What the Patch Changes

| Diff hunk | Original | Patched | Reason |
|-----------|----------|---------|--------|
| `base_ip` read | — | `base_ip = os.environ["OPENAI_BASE_IP"]` | reads target device IP at startup |
| `OpenAI` client construction | `OpenAI(api_key=api_key)` — points to `api.openai.com` | `OpenAI(base_url=f"http://{base_ip}:8090/v1", api_key="dummy")` | redirects all requests to GenAI Studio orchestrator |

> **Note:** The Gradio UI still shows the original model dropdown values (`tts-1`,
> `tts-1-hd`) and voice options (`alloy`, `echo`, etc.). GenAI Studio routes all
> `/v1/audio/speech` requests to the loaded TTS model (`melo-tts-English`)
> regardless of the model field value.
---

## Step 3 — Set the Environment Variable

```bash
export OPENAI_BASE_IP='<GENAI_DEVICE_IP>'
```

Replace `<GENAI_DEVICE_IP>` with the IP address of your QLI target
(e.g. `10.92.165.205`).

The resulting `base_url` passed to the `OpenAI` client will be:

```
http://<GENAI_DEVICE_IP>:8090/v1
```

---

## Step 4 — Verify GenAI Studio is Running

```bash
# Check all services are healthy
curl http://<GENAI_DEVICE_IP>:8090/api/status

# Confirm TTS model is loaded
curl http://<GENAI_DEVICE_IP>:8090/v1/models
```

`/api/status` should return `"status":"ok"` for all services.
`/v1/models` should include `melo-tts-English` in the model list.

You can also do a quick smoke-test of the TTS endpoint directly with curl
before running the app:

```bash
curl -X POST http://<GENAI_DEVICE_IP>:8090/v1/audio/speech \
  -H 'Content-Type: application/json' \
  -o /tmp/test.wav \
  -d '{"model":"tts-1","voice":"alloy","input":"Hello from GenAI Studio."}'

# Verify the WAV file was written
file /tmp/test.wav
```

---

## Step 5 — Install Dependencies

```bash
pip install -r requirements.txt
```

Key packages installed:

| Package | Purpose |
|---------|---------|
| `gradio` | Web UI framework — renders the interface |
| `openai` | OpenAI Python SDK — `client.audio.speech.create(...)` |

---

## Step 6 — Run

```bash
gradio app.py
```

Gradio will start a local server and print a URL similar to:

```
Running on local URL:  http://127.0.0.1:7860
```

Open the URL in your browser. The interface presents:

| UI Element | Description |
|------------|-------------|
| **OpenAI API Key** textbox | Enter any non-empty string (e.g. `dummy`) — GenAI Studio does not validate it |
| **Model** dropdown | `tts-1` or `tts-1-hd` — GenAI Studio routes both to `melo-tts-English` |
| **Voice** dropdown | `alloy`, `echo`, `fable`, `onyx`, `nova`, `shimmer` |
| **Text** input | The text you want synthesised into speech |
| **Generate** button | Submits the request; audio output appears below |

**Expected behaviour:**
- The audio player renders the synthesised speech inline in the browser.
- Response time depends on the length of the input text and target device load.
- The API key textbox is a UX guard only — the patched client always uses `"dummy"`.

---

## Troubleshooting

**`Please enter your OpenAI API Key` error in UI**
The API key textbox is empty. Enter any non-empty string (e.g. `dummy`) —
GenAI Studio does not validate it.

**`ConnectionRefusedError` or `httpx.ConnectError`**
GenAI Studio is not reachable at the configured IP and port. Verify:
```bash
curl http://<GENAI_DEVICE_IP>:8090/api/status
```

**`ModuleNotFoundError: No module named 'gradio'`**
Dependencies are not installed in the active environment.
```bash
pip install -r requirements.txt
```

**Patch does not apply cleanly (`error: patch failed`)**
The upstream `app.py` may have changed. Apply the changes manually:
1. Add `base_ip = os.environ["OPENAI_BASE_IP"]` after the imports.
2. Replace `client = OpenAI(api_key=api_key)` with:
```python
client = OpenAI(
        base_url=f"http://{base_ip}:8090/v1",
        api_key="dummy"
        )
```

---

## Quick Reference

```bash
# 1. Clone
git clone https://github.com/arham-kk/openai-tts.git && cd openai-tts

# 2. Apply patch
git apply genai.patch

# 3. Set environment variable
export OPENAI_BASE_IP='<GENAI_DEVICE_IP>'

# 4. Install dependencies
pip install -r requirements.txt

# 6. Run
gradio app.py
```

---

## Port Reference

| Port | Service | Use for |
|------|---------|---------|
| `8090` | Orchestrator | All client traffic — use this |
| `8083` | TTS direct | Debug only |

---

## Changes Made to the Upstream Repo

| File | Change |
|------|--------|
| `app.py` | Read `OPENAI_BASE_IP` from environment; replaced `OpenAI(api_key=api_key)` with `OpenAI(base_url=..., api_key="dummy")` pointing to GenAI Studio orchestrator |

