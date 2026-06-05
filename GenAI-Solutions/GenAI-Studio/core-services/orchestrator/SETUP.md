# Orchestrator Dependency Setup (Reference)

For comprehensive setup guidance, refer to the following documentation:

- Primary onboarding: [`README.md`](README.md)
- Orchestrator configuration: [`core-services/orchestrator/README.md`](core-services/orchestrator/README.md)

The orchestrator does not generate or host native inference models.
It routes to backend services and needs these mounted dependencies:

- backend services up on ports `8088`, `8081`, `8083`, `8084`, `8080`
- I2T model mount path for preprocess outputs:
  - `/opt/genai-studio-models/image-to-text/Lemans_LE_Gen2_QNN2_41_qwen25_vl_7B/files`
- Hugging Face processor cache for Qwen2.5-VL preprocess:
  - `${HF_CACHE_HOST_DIR:-/opt/genai-studio-cache/huggingface}:/root/.cache/huggingface`

If backend model folders are correct and mounted, orchestrator requires no additional model-generation step.
