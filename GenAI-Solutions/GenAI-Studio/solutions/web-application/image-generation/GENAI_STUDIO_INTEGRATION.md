# openai-image-generation + GenAI Studio Integration Guide

End-to-end guide: clone → apply patch → run against Qualcomm GenAI Studio Image Generation endpoint.

Tested on: openai-image-generation `main`, GenAI Studio v2, host machine x86-64 Ubuntu 22.04.

---

## What This App Does

`openai-image-generation` is a lightweight Streamlit web interface that calls
the OpenAI Image Generation API. Because GenAI Studio exposes an
OpenAI-compatible `POST /v1/images/generations` endpoint through its
orchestrator, the app works with GenAI Studio with **minimal code changes** —
only the `OpenAI` client construction, the model name, and a single environment
variable are modified by the patch.

**Supported GenAI Studio endpoint via this app:**

| Feature | Endpoint | App function | OpenAI SDK call |
|---------|----------|--------------|-----------------|
| Image generation | `POST /v1/images/generations` | `generate_image()` in `app.py` | `client.images.generate(...)` |

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
git clone https://github.com/amjadraza/openai-image-generation.git
```

Key files:

| File | Purpose |
|------|---------|
| `app.py` | Main Streamlit app — `generate_image()` function + UI layout |
| `background.jpg` | Background image used by the Streamlit UI |
| `requirements.txt` | Python dependencies (`streamlit`, `openai`, `Pillow`) |

---

## Step 2 — Copy the Patch File

Copy the patch file into the cloned repository directory:

```bash
cp genai-studio-intergration.patch openai-image-generation/
```

---

## Step 3 — Enter the Repository and Apply the Patch

```bash
cd openai-image-generation
git apply genai-studio-intergration.patch
```

### What the Patch Changes

| Diff hunk | Original | Patched | Reason |
|-----------|----------|---------|--------|
| `import os` | — | `import os` added | required to read environment variable |
| `base_ip` read | — | `base_ip = os.environ["OPENAI_BASE_IP"]` | reads target device IP at startup |
| `OpenAI` client construction | `openai.images.generate(...)` called on module directly | `client = openai.OpenAI(base_url=f"http://{base_ip}:8090/v1", api_key="dummy")` | redirects all requests to GenAI Studio orchestrator |
| Model name | `dall-e-2` | `stable-diffusion-2-1` | GenAI Studio does not serve `dall-e-2`; use the loaded SD model |
| `size` parameter | `size="256x256"` | removed | GenAI Studio image endpoint does not accept this parameter |
| Generate call | `openai.images.generate(...)` | `client.images.generate(...)` | uses the patched client instance |

> **Note:** The Streamlit UI still shows the original OpenAI API key input in
> the sidebar. GenAI Studio does not validate the key — the patched client
> always uses `"dummy"` regardless of what is entered.

---

## Step 4 — Set the Environment Variable

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

## Step 5 — Verify GenAI Studio is Running

```bash
# Check all services are healthy
curl http://<GENAI_DEVICE_IP>:8090/api/status

# Confirm image generation model is loaded
curl http://<GENAI_DEVICE_IP>:8090/v1/models
```

`/api/status` should return `"status":"ok"` for all services.
`/v1/models` should include `stable-diffusion-2-1` in the model list.

You can also smoke-test the image generation endpoint directly with curl
before running the app:

```bash
curl -X POST http://<GENAI_DEVICE_IP>:8090/v1/images/generations \
  -H 'Content-Type: application/json' \
  -o /tmp/test.png \
  -d '{"model":"stable-diffusion-2-1","prompt":"a sunset over mountains","n":1,"quality":"hd"}'

# Verify the image file was written
file /tmp/test.png
```

---

## Step 6 — Install Dependencies

```bash
pip install -r requirements.txt
```

Key packages installed:

| Package | Purpose |
|---------|---------|
| `streamlit` | Web UI framework — renders the interface |
| `openai` | OpenAI Python SDK — `client.images.generate(...)` |
| `Pillow` | Image loading and display (`PIL.Image`) |

---

## Step 7 — Run

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
|------------|-------------|
| **OpenAI API Key** sidebar input | Enter any non-empty string (e.g. `dummy`) — GenAI Studio does not validate it |
| **Image Description** text input | The prompt describing the image to generate |
| **Generate Image** button | Submits the request; the generated image appears below |

**Expected behaviour:**
- The generated image is downloaded to `img.png` in the working directory and
  displayed inline in the browser.
- Response time depends on the complexity of the prompt and target device load.
- The API key sidebar input is a UX guard only — the patched client always uses
  `"dummy"`.

---

## Troubleshooting

**`Please enter your OpenAI API key` warning in UI**
The API key sidebar input is empty. Enter any non-empty string (e.g. `dummy`) —
GenAI Studio does not validate it.

**`ConnectionRefusedError` or `httpx.ConnectError`**
GenAI Studio is not reachable at the configured IP and port. Verify:
```bash
curl http://<GENAI_DEVICE_IP>:8090/api/status
```

**`ModuleNotFoundError: No module named 'streamlit'`**
Dependencies are not installed in the active environment.
```bash
pip install -r requirements.txt
```

**`Unsupported model 'dall-e-2'`**
You are running the unpatched upstream `app.py`. Apply the patch as described
in Step 3.

**Patch does not apply cleanly (`error: patch failed`)**
The upstream `app.py` may have changed. Apply the changes manually:
1. Add `import os` to the imports section.
2. After the `openai.api_key` line, add:
```python
base_ip = os.environ["OPENAI_BASE_IP"]

client = openai.OpenAI(
        base_url=f"http://{base_ip}:8090/v1",
        api_key="dummy"
        )
```
3. In `generate_image()`, replace `openai.images.generate(model="dall-e-2", ..., size="256x256", ...)` with:
```python
client.images.generate(
    model="stable-diffusion-2-1",
    prompt=prompt,
    quality="hd",
    n=1,
)
```

---

## Quick Reference

```bash
# 1. Clone
git clone https://github.com/amjadraza/openai-image-generation.git

# 2. Copy patch into repo
cp genai-studio-intergration.patch openai-image-generation/

# 3. Enter repo and apply patch
cd openai-image-generation
git apply genai-studio-intergration.patch

# 4. Set environment variable
export OPENAI_BASE_IP='<GENAI_DEVICE_IP>'

# 5. Install dependencies
pip install -r requirements.txt

# 6. Run
streamlit run app.py
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
| `app.py` | Added `import os`; read `OPENAI_BASE_IP` from environment; replaced module-level `openai.images.generate(model="dall-e-2", size="256x256", ...)` with `client.images.generate(model="stable-diffusion-2-1", ...)` using a new `OpenAI` client instance pointing to GenAI Studio orchestrator |
| `GENAI_STUDIO_INTEGRATION.md` | This guide (new file) |