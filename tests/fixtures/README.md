# Test Fixtures

## ONNX Model Fixtures

Model files are **not** checked into the repository (they are gitignored).
Download them before running integration/QA tests that require real models.

### Quick Setup

```bash
./tests/fixtures/download_models.sh
```

The script is idempotent — it skips files that already exist.

### Manual Download

#### all-MiniLM-L6-v2 (384-dim sentence embeddings)

```bash
mkdir -p tests/fixtures/models/all-MiniLM-L6-v2
curl -L -o tests/fixtures/models/all-MiniLM-L6-v2/model.onnx \
    "https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2/resolve/main/onnx/model.onnx"
cp tests/fixtures/tokenizer_minilm.json \
    tests/fixtures/models/all-MiniLM-L6-v2/tokenizer.json
```

### Test Behavior

QA tests that depend on model files use `GTEST_SKIP()` when the model
is not present, so a missing model never causes a test failure.
