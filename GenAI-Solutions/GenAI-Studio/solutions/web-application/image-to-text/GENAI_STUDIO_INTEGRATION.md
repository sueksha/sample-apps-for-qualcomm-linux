# streamlit-openai + GenAI Studio Integration Guide

End-to-end guide: clone → apply patch → run against Qualcomm GenAI Studio Responses API endpoint.

Tested on: streamlit-openai `main`, GenAI Studio v2, host machine x86-64 Ubuntu 22.04.

---

## What This App Does

`streamlit-openai` is a lightweight Streamlit web interface that calls the OpenAI Chat Completions API with support for vision models. Because GenAI Studio exposes an OpenAI-compatible `POST /v1/responses` endpoint through its orchestrator, the app works with GenAI Studio with **minimal code changes** — only the `OpenAI` client construction, the model name, and a single environment variable are modified by the patch.

**Supported GenAI Studio endpoint via this app:**

| Feature | Endpoint | App function | OpenAI SDK call |
|---------|----------|--------------|------------------|
| Vision (Image-to-Text) | `POST /v1/responses` | `Chat.run()` in `streamlit_openai/chat.py` | `client.responses.create(...)` |

---

## Prerequisites

| Requirement | Check command |
|---|---|
| Python 3.9 or later | `python3 --version` |
| pip | `pip --version` |
| git | `git --version` |
| GenAI Studio v2 running on QLI target | `curl http://<GENAI_IP>:8090/api/status` |


---

## Step 1 — Clone the Repository

```bash
git clone https://github.com/sbslee/streamlit-openai.git
```

Key files:

| File | Purpose |
|------|----------|
| `streamlit_openai/chat.py` | Main Chat class — `Chat.run()` method + vision payload handling |
| `streamlit_openai/__main__.py` | ...|
| `app.py` | Streamlit entry point — initializes Chat and runs the UI |

---

## Step 2 — Copy the Patch File

Copy the patch file into the cloned repository directory:

```bash
cp genai-studio-integration.patch streamlit-openai/
```

---

## Step 3 — Enter the Repository and Apply the Patch

```bash
cd streamlit-openai
git apply genai-studio-integration.patch
```

### What the Patch Changes

| Diff hunk | Original | Patched | Reason |
|-----------|----------|---------|--------|
| `import os` | — | `import os` added | required to read environment variable |
| `base_url` read | — | `base_url = os.environ["OPENAI_BASE_URL"]` | reads target device orchestrator URL at startup |
| `OpenAI` client construction | `openai.OpenAI(api_key=...)` | `client = openai.OpenAI(api_key=..., base_url=...)` | redirects all requests to GenAI Studio orchestrator |
| Model name | `gpt-4-vision` or similar | `qwen2.5-vl-7b-instruct` | GenAI Studio does not serve OpenAI models; use the loaded VLM |
| Vision payload | `file_id` references | `input_image.image_url` with `data:` URLs | GenAI Studio Responses API expects base64-encoded data URLs |
| Responses call | `client.chat.completions.create(...)` | `client.responses.create(...)` | uses the Responses API instead of Chat Completions |

> **Note:** The Streamlit UI still shows the original OpenAI API key input in the sidebar. GenAI Studio does not validate the key — the patched client always uses `"dummy"` regardless of what is entered.

---

## Step 4 — Set the Environment Variable

```bash
export OPENAI_BASE_URL='http://<GENAI_DEVICE_IP>:8090/v1'
```

Replace `<GENAI_DEVICE_IP>` with the IP address of your QLI target (e.g. `10.92.165.205`).

The resulting `base_url` passed to the `OpenAI` client will be:

```
http://<GENAI_DEVICE_IP>:8090/v1
```

---

## Step 5 — Verify GenAI Studio is Running

```bash
# Check all services are healthy
curl http://<GENAI_DEVICE_IP>:8090/api/status
# Confirm vision model is loaded
curl http://<GENAI_DEVICE_IP>:8090/v1/models
```

`/api/status` should return `"status":"ok"` for all services.

`/v1/models` should include `qwen2.5-vl-7b-instruct` (or your target VLM) in the model list.

You can also smoke-test the Responses endpoint directly with curl before running the app:

```bash
curl -sS -X POST http://127.0.0.1:8080/v1/responses \
  -H 'Content-Type: application/json' \
  -H 'X-Session-Id: agent-i2t' \
  -d '{"model":"qwen2.5-vl-7b-instruct","input":[{"role":"user","content":[{"type":"input_text","text":"Describe image briefly"},{"type":"input_image","image_url":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+tmn8AAAAASUVORK5CYII="}]}],"stream":false,"max_output_tokens":64}'

```

Expected: JSON object with `object: "response"` and non-empty `output_text`.

---

## Step 6 — Install Dependencies

Install the required Python packages:

```bash
pip install streamlit openai streamlit-openai
```

Key packages installed:

| Package | Purpose |
|---------|----------|
| `streamlit` | Web UI framework — renders the chat interface |
| `openai` | OpenAI Python SDK — `client.responses.create(...)` |

---

## Step 7 — Create the App Entry Point

Create a new file `app.py` in the repository root: (refer to the readme of streamlit-openai repo)

```python
import streamlit as st
import streamlit_openai

if "chat" not in st.session_state:
    st.session_state.chat = streamlit_openai.Chat(
        model="qwen2.5-vl-7b-instruct",
        allow_code_interpreter=False,
    )

st.session_state.chat.run()
```

## Step 8 — Run the App

Run the Streamlit app:

```bash
streamlit run app.py
```

Streamlit will start a local server and print a URL similar to:

```
You can now view your Streamlit app in your browser.
Local URL: http://localhost:8501
Network URL: http://<HOST_IP>:8501
```

Open the URL in your browser. The interface presents:

| UI Element | Description |
|------------|----------|
| **Chat input** | Type text messages or upload images (`.png`, `.jpg`, `.jpeg`, `.webp`, `.gif`) |
| **Chat history** | Displays conversation with the VLM |
| **Model selector** | Defaults to `qwen2.5-vl-7b-instruct` (patched) |

**Expected behaviour:**

- Text messages are sent to the VLM via `POST /v1/responses`.
- Uploaded images are converted to base64 data URLs and included in the request.
- The VLM responds with text output describing the image or answering the question.
- Response time depends on the complexity of the prompt and target device load.

**Supported image formats:** `.png`, `.jpg`, `.jpeg`, `.webp`, `.gif` (max file size depends on GenAI Studio configuration; typically 10–50 MB).

**Note on API key:** The sidebar still prompts for an OpenAI API key, but GenAI Studio does not validate it. You can enter any value (e.g., `dummy`) or leave it blank — the patched client always uses `"dummy"` internally.

---

## Troubleshooting

**`ConnectionRefusedError` or `httpx.ConnectError`**

GenAI Studio is not reachable at the configured IP and port. Verify:

```bash
curl http://<GENAI_DEVICE_IP>:8090/api/status
```

Also check that the `OPENAI_BASE_URL` environment variable is set correctly:

```bash
echo $OPENAI_BASE_URL
```

It should output: `http://<GENAI_DEVICE_IP>:8090/v1`

**`ModuleNotFoundError: No module named 'streamlit'`**

Dependencies are not installed in the active environment.

```bash
pip install -r requirements.txt
```

**`400 "'input' array is required"`**

Non-Responses payload is being sent. Ensure the patch was applied correctly and the client is calling `client.responses.create(...)` instead of `client.chat.completions.create(...)`.

**`400 "Only https:// and data: image_url sources are supported"`**

Vision payload has unsupported image source. Ensure uploaded images are converted to `data:image/...;base64,...` URLs.

**`400 "Image-To-Text is exposed only via POST /v1/responses"`**

Image request routed to chat-completions instead of responses. Verify the patch replaced the API call with `client.responses.create(...)`.

**Patch does not apply cleanly (`error: patch failed`)**

The upstream `streamlit_openai/chat.py` or `__main__.py` may have changed. Apply the changes manually:

1. Add `import os` to the imports section.
2. After the `openai.api_key` line, add:

```python
base_url = os.environ["OPENAI_BASE_URL"]
client = openai.OpenAI(
    api_key="dummy",
    base_url=base_url
)
```

3. In the chat method, replace `client.chat.completions.create(...)` with `client.responses.create(...)` and convert vision file references to `input_image.image_url` with base64 data URLs.

---

## Quick Reference

```bash
# 1. Clone
git clone https://github.com/sbslee/streamlit-openai.git
cd streamlit-openai

# 2. Copy patch into repo
cp /path/to/genai-studio-integration.patch .

# 3. Apply patch
git apply genai-studio-integration.patch

# 4. Set environment variable (replace <GENAI_DEVICE_IP> with actual IP)
export OPENAI_BASE_URL='http://<GENAI_DEVICE_IP>:8090/v1'

# 5. Verify environment variable is set
echo $OPENAI_BASE_URL

# 6. Install dependencies
pip install streamlit-openai streamlit openai

# 7. Verify GenAI Studio is running
curl http://<GENAI_DEVICE_IP>:8090/api/status

# 8. Make and run the app
streamlit run 
```

---

## Port Reference

| Port | Service | Use for |
|------|---------|----------|
| `8090` | Orchestrator | All client traffic — use this |
| `8088` | Text-Generation direct | Debug only |
| `8080` | Image-To-Text VLM direct | Debug only|

---

## Changes Made to the Upstream Repo

| File | Change |
|------|--------|
| `streamlit_openai/chat.py` | Added `import os`; read `OPENAI_BASE_URL` from environment; replaced module-level `openai.OpenAI(api_key=...)` with `client = openai.OpenAI(api_key="dummy", base_url=...)` using environment variable; converted vision file uploads to base64 data URLs; replaced `client.chat.completions.create(...)` with `client.responses.create(...)` |
| `streamlit_openai/__main__.py` | Added `import os`; read `OPENAI_BASE_URL` from environment; created `OpenAI` client instance pointing to GenAI Studio orchestrator |
| `GENAI_STUDIO_INTEGRATION.md` | This guide (new file) |

