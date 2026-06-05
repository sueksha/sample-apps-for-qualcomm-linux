# Image Generation Web Application for GenAI Studio

This folder contains the OpenAI-compatible image generation web app integration assets for GenAI Studio.

## GitHub Repository

- Upstream project (openai-image-generation): https://github.com/amjadraza/openai-image-generation

## Documentation

- Full integration guide: [GENAI_STUDIO_INTEGRATION.md](./GENAI_STUDIO_INTEGRATION.md)
- Patch file: [genai-studio-intergration.patch](./genai-studio-intergration.patch)

## Quick Setup

1. Clone the upstream repository: `git clone https://github.com/amjadraza/openai-image-generation.git`
2. Copy patch: `cp genai-studio-intergration.patch openai-image-generation/`
3. Apply patch: `cd openai-image-generation && git apply genai-studio-intergration.patch`
4. Set target IP: `export OPENAI_BASE_IP=<GENAI_DEVICE_IP>`
5. Install dependencies: `pip install -r requirements.txt`
6. Run app: `streamlit run app.py`

The full command-level flow and troubleshooting are documented in `GENAI_STUDIO_INTEGRATION.md`.

## File List

- `README.md`
- `GENAI_STUDIO_INTEGRATION.md`
- `genai-studio-intergration.patch`
