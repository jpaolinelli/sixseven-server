# Vector Search Guide

GioDB provides native vector search through `EMBEDDING` columns and the `NEAREST` query. Vectors are indexed automatically using HNSW (Hierarchical Navigable Small World) graphs for fast approximate nearest neighbor lookup.

## EMBEDDING Columns

Define an `EMBEDDING` column to auto-generate vector embeddings from one or more source columns:

```sql
EMBEDDING(dimension, source='column_name', provider='type/model')
```

| Parameter | Description | Example |
|-----------|-------------|---------|
| `dimension` | Vector length (must match provider output) | `384`, `1536`, `3072` |
| `source` | Column(s) to embed (comma-separated for multiple) | `'title'`, `'title,body'` |
| `provider` | Embedding provider in `type/model` format | `'openai/text-embedding-3-small'` |

When a row is inserted or updated, GioDB automatically generates the embedding from the source column text using the configured provider. Embeddings are produced asynchronously — the column may be `NULL` briefly until the worker pool completes the embedding job.

A companion HNSW index is created automatically for each `EMBEDDING` column (named `hnsw_<table>_<column>`).

```sql
CREATE TABLE articles (
    id INT PRIMARY KEY,
    title TEXT NOT NULL,
    body TEXT,
    title_vec EMBEDDING(384, source='title', provider='builtin/384'),
    body_vec EMBEDDING(1536, source='body', provider='openai/text-embedding-3-small')
);

INSERT INTO articles (id, title, body)
VALUES (1, 'Introduction to AI', 'Artificial intelligence is transforming...');
-- title_vec and body_vec are generated automatically
```

To inspect registered embedding columns:

```sql
SHOW EMBEDDINGS;
```

## NEAREST Query Reference

### Syntax

```
NEAREST <k> FROM <table>.<column> TO <target>
    [WHERE <condition>]
    [WITHIN TRAVERSE <edge> FROM <table>(<pk>) DIRECTION <dir> MAX_DEPTH <n>]
    [USING COSINE | L2 | DOT];
```

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `k` | Yes | Number of nearest neighbors to return (positive integer) |
| `table.column` | Yes | Table name and EMBEDDING column, dot-separated |
| `TO target` | Yes | Query vector — either a text string (auto-embedded) or a vector literal `[0.1, 0.2, ...]` |
| `WHERE` | No | Post-filter applied to results after the vector search |
| `WITHIN TRAVERSE` | No | Restrict search to nodes reachable via graph traversal |
| `USING` | No | Distance metric (default: `COSINE`) |

### Output

Results include all columns from the table plus a `_distance` pseudo-column (`FLOAT64`), sorted by distance ascending (closest first).

```sql
NEAREST 3 FROM articles.title_vec TO 'machine learning';
-- Returns: id | title | body | title_vec | body_vec | _distance
--          5  | ...   | ...  | ...       | ...      | 0.123
--          2  | ...   | ...  | ...       | ...      | 0.234
--          8  | ...   | ...  | ...       | ...      | 0.345
```

## Choosing k

The `k` parameter controls how many nearest neighbors are returned. It directly affects the tradeoff between precision (relevance of each result) and recall (coverage of relevant items).

### Recommended Values

| Use Case | k | Rationale |
|----------|---|-----------|
| Chat context retrieval | 3-5 | High precision — feed only the most relevant context to LLMs |
| Search results | 5-10 | Balanced — enough variety for a results page |
| Recommendations | 10-20 | Broader recall — surface diverse related items |
| Clustering / analysis | 10-50 | Wide net — capture neighborhood structure |

### When to Adjust k

**Increase k** when:
- Results feel too narrow or repetitive
- Using a `WHERE` filter that may reject some nearest neighbors
- You need diversity in results (e.g., recommendations)

**Decrease k** when:
- Only the single best match matters (e.g., deduplication)
- Feeding results to an LLM with limited context
- Query latency is critical

### k with WHERE Filters

When a `WHERE` clause is present, GioDB over-fetches from the HNSW index (retrieving `k * 4` candidates) then post-filters to the requested `k`. This means some queries may return fewer than `k` results if the filter is very selective. If you consistently get fewer results than expected, increase `k` to compensate.

### Edge Cases

- `k = 0` returns an empty result set
- `k` larger than the number of rows returns all rows (sorted by distance)
- Rows with `NULL` embeddings (not yet generated) are skipped

## Distance Metrics

Specify the metric with the `USING` clause. All metrics are computed using SIMD-optimized routines (NEON on Apple Silicon, AVX2/AVX-512 on x86).

### COSINE (default)

Measures the angle between vectors, ignoring magnitude. Range: `[0, 2]` where 0 means identical direction.

Best for: **semantic similarity** — comparing meaning regardless of text length.

```sql
NEAREST 5 FROM articles.body_vec TO 'neural networks' USING COSINE;
```

### L2

Squared Euclidean distance. Range: `[0, +inf)` where 0 means identical vectors.

Best for: **spatial proximity** — when vector magnitude matters (e.g., geographic data, image features).

```sql
NEAREST 5 FROM articles.body_vec TO 'neural networks' USING L2;
```

### DOT

Dot product (negated internally so lower = better). Higher raw dot product means more similar.

Best for: **normalized vectors** where providers already L2-normalize their output (e.g., OpenAI embeddings).

```sql
NEAREST 5 FROM articles.body_vec TO 'neural networks' USING DOT;
```

### Selection Guide

| Scenario | Metric |
|----------|--------|
| General text similarity | `COSINE` |
| Pre-normalized embeddings (OpenAI, most providers) | `COSINE` or `DOT` |
| Spatial / coordinate data | `L2` |
| Not sure | `COSINE` (default) |

## Execution Strategies

### HNSW Index (default)

When an HNSW index exists and contains vectors, GioDB uses approximate nearest neighbor search. This is the default path since a companion index is auto-created with every `EMBEDDING` column.

- **Time complexity:** O(log n) per query
- **Recall:** >95% at default settings (compared to exact brute-force)
- **Trade-off:** Approximate — may occasionally miss the true nearest neighbor

### Brute-Force Fallback

If the HNSW index is empty (e.g., embeddings not yet generated), GioDB falls back to sequential scan:

- Scans every row in the table
- Computes distance for each non-NULL embedding
- Returns the exact top-k
- **Time complexity:** O(n) per query

### Graph-Scoped Search

Use `WITHIN TRAVERSE` to restrict vector search to a subgraph:

```sql
-- Only search among articles reachable from user 1 via "authored" edges
NEAREST 5 FROM articles.body_vec TO 'data science'
WITHIN TRAVERSE authored FROM users(1) DIRECTION OUT MAX_DEPTH 2;
```

This first performs a BFS traversal to find reachable node IDs, then passes them as a filter to the HNSW search. Only vectors belonging to reachable nodes are considered.

## Examples

### Basic Text Search

```sql
-- Find the 5 articles most similar to "machine learning"
NEAREST 5 FROM articles.title_vec TO 'machine learning';
```

The text string is automatically embedded using the column's configured provider before searching.

### Filtered Search

```sql
-- Find similar articles, but only in the 'tech' category
NEAREST 10 FROM articles.body_vec TO 'distributed databases'
WHERE category = 'tech';
```

### Explicit Vector

```sql
-- Search with a pre-computed vector
NEAREST 5 FROM products.embedding TO [0.1, 0.2, 0.3, 0.4];
```

### Different Metrics

```sql
-- Euclidean distance for spatial data
NEAREST 10 FROM locations.geo_vec TO 'downtown restaurant' USING L2;

-- Dot product for normalized embeddings
NEAREST 10 FROM articles.body_vec TO 'query' USING DOT;
```

### Graph-Scoped

```sql
-- Relevant articles among a user's social graph
NEAREST 5 FROM articles.body_vec TO 'data science'
WITHIN TRAVERSE follows FROM users(42) DIRECTION OUT MAX_DEPTH 2;
```

## Performance Tips

### HNSW Parameters

The auto-created HNSW index uses these defaults:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `M` | 16 | Max neighbors per node per layer. Higher = better recall, more memory |
| `ef_construction` | 200 | Search width during index build. Higher = better recall, slower inserts |
| `ef_search` | 64 | Search width during queries. Higher = better recall, slower queries |

### ONNX Tokenization

When using ONNX models with a `tokenizer.json`, the tokenizer pipeline runs locally before inference:

1. **Normalization** — lowercasing, accent stripping, control character removal (configured per model)
2. **Pre-tokenization** — splitting on whitespace and/or punctuation boundaries
3. **Subword tokenization** — WordPiece (BERT-family) or BPE (GPT-family) segmentation
4. **Special tokens** — CLS/SEP wrapping, padding, and truncation to the max sequence length

This ensures token IDs match the pretrained model's vocabulary for full semantic embedding quality. Models without a `tokenizer.json` fall back to a hash-based tokenizer that provides functional but reduced-quality embeddings.

### Dimension Impact

Higher-dimension embeddings (e.g., 1536 vs 384) require more computation per distance calculation. If latency matters, prefer smaller models when accuracy is acceptable.

| Provider / Model | Dimension | Relative Speed |
|------------------|-----------|----------------|
| `builtin/384` | 384 | Fastest (no network, hash-based) |
| `ollama/all-minilm` | 384 | Fast (local inference) |
| `openai/text-embedding-3-small` | 1536 | Moderate (network call) |
| `openai/text-embedding-3-large` | 3072 | Slower (network call, larger vectors) |

### WHERE Filter Interaction

With a `WHERE` clause, GioDB retrieves `k * 4` candidates from the HNSW index, then post-filters. Very selective filters may cause fewer than `k` results. Increase `k` if needed.

### Re-embedding

After changing a provider or model, regenerate all embeddings:

```sql
REEMBED TABLE articles;
```

This clears the HNSW index, re-embeds all rows using the current provider, and rebuilds the index.
