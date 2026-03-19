# Embedding Providers

SixSevenDB supports four embedding provider types. The provider name format is `"type/model"`.

> See the [Vector Search Guide](vector-search.md) for EMBEDDING column syntax, NEAREST queries, and performance tips.

## OpenAI

Uses the OpenAI Embeddings API. Requires an API key.

```sql
-- Set your API key at runtime
SET embedding_api_key = 'sk-...';

-- Use in an EMBEDDING column
CREATE TABLE docs (
    id INT PRIMARY KEY,
    content TEXT,
    vec EMBEDDING(1536, source='content', provider='openai/text-embedding-3-small')
);
```

| Parameter | Required | Example |
|-----------|----------|---------|
| API key | Yes | `sk-...` (set via `SET embedding_api_key`) |
| Model | Yes (in provider name) | `text-embedding-3-small`, `text-embedding-3-large` |
| Base URL | No (default: `https://api.openai.com`) | Override via `SET embedding_provider_url` |

Supports native batch embedding.

## Ollama

Uses a local Ollama server for embedding generation. No API key required.

```sql
-- Default URL is http://localhost:11434
SET embedding_provider_url = 'http://localhost:11434';

CREATE TABLE docs (
    id INT PRIMARY KEY,
    content TEXT,
    vec EMBEDDING(384, source='content', provider='ollama/all-minilm')
);
```

| Parameter | Required | Example |
|-----------|----------|---------|
| Base URL | Yes | `http://localhost:11434` (default) |
| Model | Yes (in provider name) | `all-minilm`, `nomic-embed-text` |

Start Ollama first: `ollama serve` then `ollama pull all-minilm`.

## ONNX (Offline / Network-Free)

Runs a local ONNX model for embedding inference. No network access required after model download — ideal for air-gapped environments, CI pipelines, and local development.

```sql
-- Point to a model directory (recommended — auto-discovers model + tokenizer)
CREATE TABLE docs (
    id INT PRIMARY KEY,
    content TEXT,
    vec EMBEDDING(384, source='content', provider='onnx/models/all-MiniLM-L6-v2')
);
```

| Parameter | Required | Example |
|-----------|----------|---------|
| Model path | Yes (in provider name) | Directory path or direct `.onnx` file path |

### Model Directory Format

The recommended layout is a directory containing the model and tokenizer:

```
models/all-MiniLM-L6-v2/
    model.onnx          # ONNX model file (or onnx/model.onnx)
    tokenizer.json      # Hugging Face tokenizer config
```

When the provider path points to a directory, SixSevenDB auto-discovers:
1. The model file (`model.ort`, `model.onnx`, or `onnx/model.onnx`)
2. The tokenizer (`tokenizer.json` in the directory root)

**Backward compatibility:** Pointing directly to a `.onnx` file still works. If a `tokenizer.json` exists alongside the model file, it will be loaded automatically. Without a tokenizer file, SixSevenDB falls back to a hash-based tokenizer.

### Tokenizer Support

When a `tokenizer.json` is found, SixSevenDB loads the pretrained tokenizer for full semantic quality. Supported tokenizer types:

| Algorithm | Models | Description |
|-----------|--------|-------------|
| WordPiece | BERT, MiniLM, BGE, most sentence-transformers | Greedy longest-match subword tokenization |
| BPE | GPT-2, RoBERTa, nomic-embed | Byte-pair encoding with learned merge rules |

The tokenizer handles text normalization (lowercasing, accent stripping, whitespace cleanup), pre-tokenization (punctuation/whitespace splitting), and subword segmentation using the model's vocabulary.

### Downloading ONNX Models

Any ONNX-exported transformer embedding model that accepts `input_ids` and `attention_mask` inputs will work. The recommended approach is to download pre-converted models from Hugging Face.

**Option 1 — Download a pre-converted model (recommended):**

```bash
# Install the Hugging Face CLI
pip install huggingface-hub

# all-MiniLM-L6-v2 (384 dimensions, ~180 MB) — best balance of size and quality
hf download onnx-community/all-MiniLM-L6-v2-ONNX \
    --local-dir models/all-MiniLM-L6-v2
rm -rf models/all-MiniLM-L6-v2/.cache

# bge-small-en-v1.5 (384 dimensions, ~130 MB)
hf download onnx-community/bge-small-en-v1.5-ONNX \
    --local-dir models/bge-small-en-v1.5
rm -rf models/bge-small-en-v1.5/.cache
```

**Option 2 — Export any Hugging Face model to ONNX yourself:**

```bash
pip install optimum onnxruntime sentence-transformers

# Export to ONNX format
optimum-cli export onnx \
    --model sentence-transformers/all-MiniLM-L6-v2 \
    models/all-MiniLM-L6-v2
```

This produces `model.onnx` (and optionally `model.onnx_data`) in the output directory along with `tokenizer.json`.

### Recommended Models

| Model | Dimensions | Size | Tokenizer | Notes |
|-------|-----------|------|-----------|-------|
| `all-MiniLM-L6-v2` | 384 | ~80 MB | WordPiece | Best for general-purpose semantic search |
| `all-MiniLM-L12-v2` | 384 | ~120 MB | WordPiece | Higher quality, slightly slower |
| `bge-small-en-v1.5` | 384 | ~130 MB | WordPiece | Strong retrieval performance |
| `nomic-embed-text-v1.5` | 768 | ~550 MB | BPE | High quality, larger dimension |

### Usage

```sql
-- Directory path (recommended — auto-discovers model.onnx + tokenizer.json)
CREATE TABLE articles (
    id INT PRIMARY KEY,
    title TEXT NOT NULL,
    title_vec EMBEDDING(384, source='title', provider='onnx/models/all-MiniLM-L6-v2')
);

-- Direct .onnx file path (tokenizer.json loaded from same directory if present)
CREATE TABLE articles (
    id INT PRIMARY KEY,
    title TEXT NOT NULL,
    title_vec EMBEDDING(384, source='title', provider='onnx/models/all-MiniLM-L6-v2/onnx/model.onnx')
);

-- Absolute path
CREATE TABLE articles (
    id INT PRIMARY KEY,
    title TEXT NOT NULL,
    title_vec EMBEDDING(384, source='title', provider='onnx//home/user/models/model.onnx')
);
```

### Current Limitations

- **Sequence length**: Max 128 tokens (longer text is truncated).
- **Batch size**: Inference runs one input at a time (no batched GPU inference).

## Builtin

A deterministic hash-projection provider for testing. No network, no model files.

```sql
CREATE TABLE docs (
    id INT PRIMARY KEY,
    content TEXT,
    vec EMBEDDING(384, source='content', provider='builtin/384')
);
```

| Parameter | Required | Example |
|-----------|----------|---------|
| Dimension | Yes (in provider name) | Any positive integer: `384`, `768`, etc. |

The builtin provider is based on word-overlap hashing. It does not capture semantic meaning like neural models, but is useful for testing and offline development.

## Re-embedding

After changing a provider or model, regenerate all embeddings:

```sql
REEMBED TABLE articles;
```
