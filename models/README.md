# ONNX Models

This directory holds ONNX embedding models for local inference. Models are not checked into version control (see `.gitignore`).

## Directory Layout

Each model is a directory containing the ONNX model file and a Hugging Face tokenizer config:

```
models/
    all-MiniLM-L6-v2/
        model.onnx          # or onnx/model.onnx
        tokenizer.json       # Hugging Face tokenizer config
    bge-small-en-v1.5/
        model.onnx
        tokenizer.json
```

## Downloading Models

Install the Hugging Face CLI:

```bash
pip install huggingface-hub
```

### all-MiniLM-L6-v2 (384 dimensions, WordPiece tokenizer)

Best balance of size (~80 MB) and quality for general-purpose semantic search.

```bash
huggingface-cli download onnx-community/all-MiniLM-L6-v2-ONNX \
    --local-dir models/all-MiniLM-L6-v2
```

### bge-small-en-v1.5 (384 dimensions, WordPiece tokenizer)

Strong retrieval performance (~130 MB).

```bash
huggingface-cli download onnx-community/bge-small-en-v1.5-ONNX \
    --local-dir models/bge-small-en-v1.5
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

SixSevenDB auto-discovers `model.onnx` and `tokenizer.json` in the directory.
