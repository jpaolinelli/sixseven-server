# Vector Search Guide

SixSevenDB provides native vector search through `EMBEDDING` columns and the `NEAREST` predicate. Vectors are indexed automatically using HNSW (Hierarchical Navigable Small World) graphs for fast approximate nearest neighbor lookup.

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

When a row is inserted or updated, SixSevenDB automatically generates the embedding from the source column text using the configured provider. Embeddings are produced asynchronously — the column may be `NULL` briefly until the worker pool completes the embedding job.

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

## NEAREST Predicate Reference

### Syntax

`NEAREST` is a `WHERE` predicate inside a normal `SELECT`:

```
SELECT <cols> FROM <table>
WHERE NEAREST(<column>, <k>) TO <target>
    [WITHIN TRAVERSE <edge> FROM <table>(<pk>) DIRECTION <dir> MAX_DEPTH <n>]
    [USING COSINE | L2 | DOT]
    [AND <residual predicate>];
```

The clauses after `TO <target>` are ordered: optional `WITHIN TRAVERSE ...`, then optional `USING <metric>`, then an optional `AND <predicate>` that further filters the matched rows.

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `column` | Yes | EMBEDDING column to search (within the `FROM` table) |
| `k` | Yes | Number of nearest neighbors to return (positive integer) |
| `TO target` | Yes | Query vector — either a text string (auto-embedded) or a vector literal `[0.1, 0.2, ...]` |
| `WITHIN TRAVERSE` | No | Restrict search to nodes reachable via graph traversal |
| `USING` | No | Distance metric (default: `COSINE`) |
| `AND <pred>` | No | Residual filter applied to the matched rows |

### Output

`SELECT *` returns the table's columns. The nearest-first ordering is implicit, so results come back closest-first. To expose the distance, add the `_distance` pseudo-column (`FLOAT64`, lower = nearer) to the select list and optionally `ORDER BY _distance`.

```sql
SELECT id, title, _distance
FROM articles
WHERE NEAREST(title_vec, 3) TO 'machine learning'
ORDER BY _distance;
-- Returns: id | title | _distance
--          5  | ...   | 0.123
--          2  | ...   | 0.234
--          8  | ...   | 0.345
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
- Using an `AND` residual filter that may reject some nearest neighbors
- You need diversity in results (e.g., recommendations)

**Decrease k** when:
- Only the single best match matters (e.g., deduplication)
- Feeding results to an LLM with limited context
- Query latency is critical

### k with Residual Filters

When an `AND` residual filter is present, SixSevenDB over-fetches from the HNSW index (retrieving `k * 4` candidates) then post-filters to the requested `k`. This means some queries may return fewer than `k` results if the filter is very selective. If you consistently get fewer results than expected, increase `k` to compensate.

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
SELECT * FROM articles WHERE NEAREST(body_vec, 5) TO 'neural networks' USING COSINE;
```

### L2

Squared Euclidean distance. Range: `[0, +inf)` where 0 means identical vectors.

Best for: **spatial proximity** — when vector magnitude matters (e.g., geographic data, image features).

```sql
SELECT * FROM articles WHERE NEAREST(body_vec, 5) TO 'neural networks' USING L2;
```

### DOT

Dot product (negated internally so lower = better). Higher raw dot product means more similar.

Best for: **normalized vectors** where providers already L2-normalize their output (e.g., OpenAI embeddings).

```sql
SELECT * FROM articles WHERE NEAREST(body_vec, 5) TO 'neural networks' USING DOT;
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

When an HNSW index exists and contains vectors, SixSevenDB uses approximate nearest neighbor search. This is the default path since a companion index is auto-created with every `EMBEDDING` column.

- **Time complexity:** O(log n) per query
- **Recall:** >95% at default settings (compared to exact brute-force)
- **Trade-off:** Approximate — may occasionally miss the true nearest neighbor

### Brute-Force Fallback

If the HNSW index is empty (e.g., embeddings not yet generated), SixSevenDB falls back to sequential scan:

- Scans every row in the table
- Computes distance for each non-NULL embedding
- Returns the exact top-k
- **Time complexity:** O(n) per query

### Graph-Scoped Search

Use `WITHIN TRAVERSE` to restrict vector search to a subgraph:

```sql
-- Only search among articles reachable from user 1 via "authored" edges
SELECT * FROM articles
WHERE NEAREST(body_vec, 5) TO 'data science'
WITHIN TRAVERSE authored FROM users(1) DIRECTION OUT MAX_DEPTH 2;
```

This first performs a BFS traversal to find reachable node IDs, then passes them as a filter to the HNSW search. Only vectors belonging to reachable nodes are considered.

## Examples

### Basic Text Search

```sql
-- Find the 5 articles most similar to "machine learning"
SELECT * FROM articles WHERE NEAREST(title_vec, 5) TO 'machine learning';
```

The text string is automatically embedded using the column's configured provider before searching.

### Filtered Search

```sql
-- Find similar articles, but only in the 'tech' category
SELECT * FROM articles
WHERE NEAREST(body_vec, 10) TO 'distributed databases'
AND category = 'tech';
```

### Explicit Vector

```sql
-- Search with a pre-computed vector
SELECT * FROM products WHERE NEAREST(embedding, 5) TO [0.1, 0.2, 0.3, 0.4];
```

### Different Metrics

```sql
-- Euclidean distance for spatial data
SELECT * FROM locations WHERE NEAREST(geo_vec, 10) TO 'downtown restaurant' USING L2;

-- Dot product for normalized embeddings
SELECT * FROM articles WHERE NEAREST(body_vec, 10) TO 'query' USING DOT;
```

### Graph-Scoped

```sql
-- Relevant articles among a user's social graph
SELECT * FROM articles
WHERE NEAREST(body_vec, 5) TO 'data science'
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

### Residual Filter Interaction

With an `AND` residual predicate, SixSevenDB retrieves `k * 4` candidates from the HNSW index, then post-filters. Very selective filters may cause fewer than `k` results. Increase `k` if needed.

### Re-embedding

After changing a provider or model, regenerate all embeddings:

```sql
REEMBED TABLE articles;
```

This clears the HNSW index, re-embeds all rows using the current provider, and rebuilds the index.
