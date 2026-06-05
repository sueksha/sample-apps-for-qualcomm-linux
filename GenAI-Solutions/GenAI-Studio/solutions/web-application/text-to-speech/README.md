# Text-to-Speech Web Application for GenAI Studio

This folder contains the OpenAI-compatible text-to-speech web app integration assets for GenAI Studio.

## GitHub Repository

- Upstream project (openai-tts): https://github.com/arham-kk/openai-tts

## Documentation

- Full integration guide: [GENAI_STUDIO_INTEGRATION.md](./GENAI_STUDIO_INTEGRATION.md)
- Patch file: [genai-studio-integration.patch](./genai-studio-integration.patch)

## Quick Setup

1. Clone the upstream repository: `git clone https://github.com/arham-kk/openai-tts.git`
2. Apply patch: `git apply genai-studio-integration.patch`
3. Set target IP: `export OPENAI_BASE_IP=<GENAI_DEVICE_IP>`
4. Install dependencies: `pip install -r requirements.txt`
5. Run app: `gradio app.py`

The full command-level flow and troubleshooting are documented in `GENAI_STUDIO_INTEGRATION.md`.

## Demo Video

- [Text-to-Speech Web App Demo (GenAI Studio)](./text-to-speech-web-app-genai_studio.mp4)

If inline playback is not available in your Git viewer, download the video from the link above and play it locally.

## File List

- `README.md`
- `GENAI_STUDIO_INTEGRATION.md`
- `genai-studio-integration.patch`
- `text-to-speech-web-app-genai_studio.mp4`
