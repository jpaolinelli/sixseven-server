#!/usr/bin/env bash
# Download ONNX model fixtures for integration/QA tests.
# Usage: ./tests/fixtures/download_models.sh
#
# Models are gitignored. This script is idempotent — it skips
# files that already exist.

set -euo pipefail

FIXTURES_DIR="$(cd "$(dirname "$0")" && pwd)"
MODELS_DIR="${FIXTURES_DIR}/models"

# --- all-MiniLM-L6-v2 (384-dim sentence embeddings) ---
MINILM_DIR="${MODELS_DIR}/all-MiniLM-L6-v2"
MINILM_ONNX="${MINILM_DIR}/model.onnx"
MINILM_ORT="${MINILM_DIR}/model.ort"
MINILM_TOKENIZER="${MINILM_DIR}/tokenizer.json"

mkdir -p "${MINILM_DIR}"

if [ -f "${MINILM_ORT}" ]; then
    echo "model.ort already exists, skipping."
else
    # Download the ONNX model first.
    if [ ! -f "${MINILM_ONNX}" ]; then
        echo "Downloading all-MiniLM-L6-v2 model.onnx (~86 MB)..."
        curl -L -o "${MINILM_ONNX}" \
            "https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2/resolve/main/onnx/model.onnx"
    fi

    # Convert to ORT format (avoids ONNX schema validation issues in debug builds).
    echo "Converting to ORT format..."
    python3 -c "
import onnxruntime as ort
opts = ort.SessionOptions()
opts.optimized_model_filepath = '${MINILM_ORT}'
opts.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
opts.add_session_config_entry('session.save_model_format', 'ORT')
ort.InferenceSession('${MINILM_ONNX}', opts)
print('Saved model.ort')
"
    echo "Done."
fi

if [ -f "${MINILM_TOKENIZER}" ]; then
    echo "tokenizer.json already exists, skipping."
else
    echo "Copying tokenizer.json from fixtures..."
    cp "${FIXTURES_DIR}/tokenizer_minilm.json" "${MINILM_TOKENIZER}"
    echo "Done."
fi

echo "All model fixtures ready in ${MODELS_DIR}"
