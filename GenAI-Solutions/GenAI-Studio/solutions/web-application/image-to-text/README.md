# Image-to-Text Web Application for GenAI Studio

This folder contains the OpenAI-compatible image-to-text (vision) web app integration assets for GenAI Studio.

## GitHub Repository

- Upstream project (streamlit-openai): https://github.com/sbslee/streamlit-openai

## Documentation

- Full integration guide: [GENAI_STUDIO_INTEGRATION.md](./GENAI_STUDIO_INTEGRATION.md)
- Patch file: [genai-studio-integration.patch](./genai-studio-integration.patch)

## Quick Setup

1. Clone the upstream repository: `git clone https://github.com/sbslee/streamlit-openai.git`
2. Copy patch: `cp genai-studio-integration.patch streamlit-openai/`
3. Apply patch: `cd streamlit-openai && git apply genai-studio-integration.patch`
4. Set target URL: `export OPENAI_BASE_URL=http://<GENAI_DEVICE_IP>:8090/v1`
5. Install dependencies: `pip install streamlit-openai streamlit openai`
6. Create app entry point: `app.py` (see `GENAI_STUDIO_INTEGRATION.md` for details)
7. Run app: `streamlit run app.py`

The full command-level flow and troubleshooting are documented in `GENAI_STUDIO_INTEGRATION.md`.

## File List

- `README.md`
- `GENAI_STUDIO_INTEGRATION.md`
- `genai-studio-integration.patch`
