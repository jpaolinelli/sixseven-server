# ONNX Models

This directory holds ONNX embedding models for local inference. Models are not checked into version control (see `.gitignore`).

## Quick Start

```bash
pip install huggingface-hub

# Download the recommended model (~180 MB total)
hf download onnx-community/all-MiniLM-L6-v2-ONNX \
    --local-dir models/all-MiniLM-L6-v2

# Clean up the download cache
rm -rf models/all-MiniLM-L6-v2/.cache
```

## Directory Layout

The Hugging Face download creates a directory with the model files in an `onnx/` subdirectory and the tokenizer at the root:

```
models/
    all-MiniLM-L6-v2/
        onnx/
            model.onnx          # Full-precision model
            model.onnx_data     # Model weights (external data)
            model_fp16.onnx     # Half-precision variant
            model_q4.onnx       # 4-bit quantized variant
        tokenizer.json          # Hugging Face tokenizer config
        config.json             # Model config
        vocab.txt               # Vocabulary
```

SixSevenDB auto-discovers `model.onnx` (or `model.ort`) inside the `onnx/` subdirectory and `tokenizer.json` at the directory root.

## Downloading Models

### all-MiniLM-L6-v2 (384 dimensions, WordPiece tokenizer)

Best balance of size and quality for general-purpose semantic search.

```bash
hf download onnx-community/all-MiniLM-L6-v2-ONNX \
    --local-dir models/all-MiniLM-L6-v2
rm -rf models/all-MiniLM-L6-v2/.cache
```

### bge-small-en-v1.5 (384 dimensions, WordPiece tokenizer)

Strong retrieval performance.

```bash
hf download onnx-community/bge-small-en-v1.5-ONNX \
    --local-dir models/bge-small-en-v1.5
rm -rf models/bge-small-en-v1.5/.cache
```

### Export Your Own Model

```bash
pip install optimum onnxruntime sentence-transformers

optimum-cli export onnx \
    --model sentence-transformers/all-MiniLM-L6-v2 \
    models/all-MiniLM-L6-v2
```

## Supported Tokenizer Types

| Algorithm | Used By | Description |
|-----------|---------|-------------|
| WordPiece | BERT, MiniLM, BGE, most sentence-transformers | Greedy longest-match subword tokenization (prefix `##`) |
| BPE | GPT-2, RoBERTa, nomic-embed | Byte-pair encoding with learned merge rules |

## Usage in SQL

```sql
-- Point provider to the model directory
CREATE TABLE articles (
    id INT PRIMARY KEY,
    title TEXT NOT NULL,
    title_vec EMBEDDING(384, source='title', provider='onnx/models/all-MiniLM-L6-v2')
);
```
