# GioDB

A hybrid relational, graph, and vector database built in C++20 with PostgreSQL wire protocol compatibility.

## Features

- **Relational** — Full SQL support with ACID transactions, B+ tree and hash indexes, joins, CTEs, window functions, and subqueries
- **Graph** — Native edge types, BFS traversal, Cypher-style pattern matching, and shortest path queries
- **Vector** — HNSW indexes with pluggable embedding providers (OpenAI, Ollama, ONNX, builtin) and automatic embedding generation
- **PostgreSQL Compatible** — Wire protocol v3 lets you connect with `psql`, DBeaver, DataGrip, or any PostgreSQL client library

## Architecture

```
Client (psql, DBeaver, any PG driver)
        |
PostgreSQL Wire Protocol (v3)
        |
Server (Event-Driven, Connection Pool)
        |
+-------+-------+
Parser  Planner  Executor (Volcano Iterator)
        |
+-------+-----------+
Table   Graph       Vector
Engine  Engine      Engine (HNSW)
        |
Storage Engine (Buffer Pool, WAL, B+ Trees)
```

## Quick Start

```bash
# Build (requires VCPKG_ROOT env var)
cmake --preset default
cmake --build build/debug

# Start the server
./build/debug/src/giodb-server

# Connect with psql (default port 6767, trust auth)
psql -h localhost -p 6767
```

## Server Configuration

Create a `config.json` file and pass it as the first argument:

```bash
./build/debug/src/giodb-server config.json
```

All fields are optional and fall back to defaults:

```json
{
  "data_dir": "./data",
  "port": 6767,
  "log_level": "info",
  "buffer_pool_size_mb": 256,
  "wal_segment_size_mb": 16,
  "max_connections": 100,
  "auth_method": "trust",
  "shutdown_timeout_s": 30,
  "archive_enabled": false,
  "archive_cleanup_policy": "keep_all",
  "replication_max_wal_senders": 10,
  "replication_keepalive_interval_ms": 10000,
  "replication_sender_timeout_ms": 60000
}
```

| Field | Default | Description |
|-------|---------|-------------|
| `data_dir` | `./data` | Storage directory for database files |
| `port` | `6767` | TCP listen port |
| `log_level` | `info` | Logging level: `trace`, `debug`, `info`, `warn`, `error`, `fatal` |
| `buffer_pool_size_mb` | `256` | Buffer pool size in MB |
| `wal_segment_size_mb` | `16` | Write-ahead log segment size in MB |
| `max_connections` | `100` | Maximum concurrent client connections |
| `auth_method` | `trust` | Authentication: `trust`, `md5`, or `scram-sha-256` |
| `shutdown_timeout_s` | `30` | Seconds to wait for active queries on graceful shutdown |

### Replication

GioDB supports primary/standby replication. Start a standby with `--standby` or set `standby_mode` in config:

| Field | Default | Description |
|-------|---------|-------------|
| `standby_mode` | `false` | Run as read-only standby |
| `replication_primary_host` | `""` | Primary server hostname |
| `replication_primary_port` | `6767` | Primary server port |
| `replication_synchronous_mode` | `off` | `off`, `remote_write`, `remote_flush`, `remote_apply` |

## SQL Reference

### Data Types

| Type | Description |
|------|-------------|
| `TINYINT`, `SMALLINT`, `INT`/`INTEGER`, `BIGINT` | Signed integers (8, 16, 32, 64-bit) |
| `FLOAT`, `DOUBLE` | IEEE floating point (32, 64-bit) |
| `DECIMAL(p, s)`, `NUMERIC(p, s)` | Fixed-point decimal |
| `BOOLEAN` | Boolean |
| `CHAR(n)`, `VARCHAR(n)`, `TEXT` | Text |
| `BLOB` | Binary data |
| `DATE`, `TIME`, `TIMESTAMP`, `INTERVAL` | Temporal types |
| `POINT` | 2D spatial point |
| `JSON` | JSON documents |
| `UUID` | Universally unique identifier |
| `EMBEDDING(dim, source, provider)` | Vector embedding (auto-generated) |

### Databases

```sql
CREATE DATABASE analytics;
CREATE DATABASE analytics IF NOT EXISTS;
DROP DATABASE analytics;
DROP DATABASE analytics IF EXISTS CASCADE;
```

### Tables

```sql
CREATE TABLE users (
    id UUID PRIMARY KEY DEFAULT gen_uuid(),
    name TEXT NOT NULL,
    age INT CHECK (age > 0),
    email VARCHAR(255) UNIQUE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    deleted_at TIMESTAMP
);

ALTER TABLE users ADD COLUMN bio TEXT;
ALTER TABLE users DROP COLUMN bio;
ALTER TABLE users RENAME COLUMN email TO email_address;

DROP TABLE users;
DROP TABLE users IF EXISTS CASCADE;
```

### Indexes

```sql
CREATE INDEX idx_email ON users(email);
CREATE UNIQUE INDEX idx_username ON users(username);
CREATE INDEX idx_compound ON users(department, salary);
CREATE INDEX idx_hash ON users(id) USING HASH;

DROP INDEX idx_email;
```

### Insert, Update, Delete

```sql
INSERT INTO users (name, age) VALUES ('Alice', 30), ('Bob', 25);

INSERT INTO users (name, age)
SELECT name, age FROM temp_users WHERE status = 'active';

UPDATE users SET age = 31 WHERE name = 'Alice';

DELETE FROM users WHERE age < 18;
```

### Queries

```sql
-- Basic select with filtering and sorting
SELECT id, name, age FROM users
WHERE age > 25
ORDER BY name ASC
LIMIT 10 OFFSET 20;

-- Aggregates and grouping
SELECT department, COUNT(*) AS cnt, AVG(salary) AS avg_sal
FROM employees
GROUP BY department
HAVING COUNT(*) > 5
ORDER BY avg_sal DESC;

-- Joins (INNER, LEFT, RIGHT, FULL, CROSS)
SELECT u.name, p.title
FROM users u
INNER JOIN posts p ON u.id = p.author_id;

SELECT u.name, COUNT(o.id) AS orders
FROM users u
LEFT JOIN orders o ON u.id = o.user_id
GROUP BY u.name;

-- Subqueries
SELECT name FROM users
WHERE id IN (SELECT user_id FROM posts WHERE published = true);

SELECT name FROM users
WHERE EXISTS (SELECT 1 FROM posts WHERE posts.user_id = users.id);

-- CTEs (Common Table Expressions)
WITH active_users AS (
    SELECT id, name FROM users WHERE status = 'active'
),
user_posts AS (
    SELECT user_id, COUNT(*) AS post_count FROM posts GROUP BY user_id
)
SELECT u.name, COALESCE(p.post_count, 0) AS posts
FROM active_users u
LEFT JOIN user_posts p ON u.id = p.user_id;

-- Window functions
SELECT name, salary,
       ROW_NUMBER() OVER (PARTITION BY department ORDER BY salary DESC) AS rank,
       LAG(salary) OVER (ORDER BY salary) AS prev_salary,
       SUM(salary) OVER (PARTITION BY department) AS dept_total
FROM employees;

-- Set operations
SELECT name FROM users WHERE age > 30
UNION
SELECT name FROM contractors WHERE active = true;

SELECT id FROM table1 INTERSECT SELECT id FROM table2;
SELECT id FROM table1 EXCEPT SELECT id FROM table2;

-- Type casting
SELECT price::INT FROM products;
SELECT CAST(created_at AS DATE) FROM events;

-- CASE expressions
SELECT name,
    CASE
        WHEN age < 18 THEN 'minor'
        WHEN age < 65 THEN 'adult'
        ELSE 'senior'
    END AS category
FROM users;
```

### Aggregate Functions

`COUNT(*)`, `COUNT(col)`, `COUNT(DISTINCT col)`, `SUM`, `AVG`, `MIN`, `MAX`, `STRING_AGG`

### Window Functions

**Ranking:** `ROW_NUMBER()`, `RANK()`, `DENSE_RANK()`, `NTILE(n)`

**Offset:** `LAG(col, offset, default)`, `LEAD(col, offset, default)`, `FIRST_VALUE(col)`, `LAST_VALUE(col)`

**Aggregate:** `SUM`, `AVG`, `COUNT`, `MIN`, `MAX` with `OVER (PARTITION BY ... ORDER BY ... ROWS/RANGE BETWEEN ...)`

### Transactions

```sql
BEGIN;
SAVEPOINT sp1;
-- ... queries ...
ROLLBACK TO sp1;
COMMIT;

BEGIN;
-- ... queries ...
ROLLBACK;
```

---

## Graph Queries

### Edge Types

Define relationships between tables:

```sql
CREATE EDGE TYPE follows FROM users TO users;
CREATE EDGE TYPE authored FROM users TO posts;
CREATE EDGE TYPE rated (score DOUBLE, review TEXT) FROM users TO products;

DROP EDGE TYPE follows;
```

### Link and Unlink

Create and remove edges between rows:

```sql
LINK users(1) TO users(2) VIA follows;
LINK users(1) TO products(10) VIA rated (score=4.5, review='Excellent!');

UNLINK users(1) FROM users(2) VIA follows;
UNLINK users(1) FROM products(1) VIA rated WHERE score < 2.0;
```

### Traverse (BFS)

Walk edges from a starting node using breadth-first search:

```sql
-- All outgoing follows from user 1
TRAVERSE follows FROM users(1) DIRECTION OUT;

-- Incoming edges (who follows user 1?)
TRAVERSE follows FROM users(1) DIRECTION IN;

-- Both directions, limited depth, with row data
TRAVERSE follows FROM users(1)
    DIRECTION BOTH
    MAX_DEPTH 3
    FETCH;

-- Filter by traversal metadata
TRAVERSE follows FROM users(1)
    DIRECTION OUT
    WHERE __depth > 1
    FETCH;
```

**Direction options:** `OUT` (default), `IN`, `BOTH`. `BOTH` is only supported on edges between the same table (homogeneous edges).

#### TRAVERSE in SELECT

TRAVERSE can be used as a FROM source in SELECT queries with full projection, filtering, aliasing, and join support:

```sql
-- Projection and ordering
SELECT name, __depth
FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH AS t
ORDER BY name
LIMIT 10;

-- Join traverse results with other tables
SELECT u.name, p.title
FROM users u
JOIN TRAVERSE authored FROM users(1) DIRECTION OUT AS posts_t
  ON u.id = posts_t.__source;
```

#### Output Modes

**MODE NODES** (default) — one row per discovered node:

| Column | Description |
|--------|-------------|
| *target table columns* | Full row data from the discovered node (requires `FETCH`) |
| `__node` | Primary key of the discovered node |
| `__depth` | BFS depth from the starting node |
| `__source` | PK of the previous node in the BFS tree (NULL for depth-0) |
| *edge properties* | Properties from the edge used to reach this node |

**MODE EDGES** — one row per edge, including cross-edges and bidirectional edges (useful for graph visualization):

| Column | Description |
|--------|-------------|
| `__from` | Source node primary key |
| `__to` | Target node primary key |
| `__depth` | Depth of the target node in the BFS tree |
| *edge properties* | Properties of the edge |

```sql
-- Node-centric output (default)
TRAVERSE follows FROM users(1) DIRECTION OUT MODE NODES FETCH;

-- Edge-centric output for graph rendering
TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES;
```

#### Edge Properties

Edge types can carry typed properties. These appear as additional columns in traverse output:

```sql
CREATE EDGE TYPE rated (score DOUBLE, review TEXT) FROM users TO products;
LINK users(1) TO products(10) VIA rated (score=4.5, review='Excellent!');

-- Access edge properties (qualified or unqualified)
SELECT name, rated.score, rated.review
FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t;

-- Star expansion includes edge properties
SELECT * FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH;
-- Returns: [product columns...], __node, __depth, __source, score, review
```

#### Heterogeneous Edge Traversal

Edges connecting different tables (e.g., `users → posts`) expose the opposite endpoint's columns:

```sql
CREATE EDGE TYPE authored FROM users TO posts;

-- OUT from source → target table columns (posts)
SELECT title FROM TRAVERSE authored FROM users(1) DIRECTION OUT FETCH;

-- IN from target → source table columns (users)
SELECT name FROM TRAVERSE authored FROM posts(10) DIRECTION IN FETCH;
```

### Match (Pattern Matching)

Cypher-style pattern matching over graph edges:

```sql
-- Direct follow relationships
MATCH (a:users)-[r:follows]->(b:users)
RETURN a.name, b.name;

-- Incoming edges
MATCH (a:users)<-[r:follows]-(b:users)
RETURN a.name, b.name;

-- Multi-hop: users who follow someone who authored a post
MATCH (a:users)-[f:follows]->(b:users)-[w:authored]->(p:posts)
WHERE a.name = 'Alice'
RETURN b.name, p.title;

-- Undirected edges
MATCH (a:users)-[r:follows]-(b:users)
RETURN a.name, b.name;
```

### Shortest Path

```sql
SHORTEST PATH FROM users(1) TO users(100) VIA follows;

SHORTEST PATH FROM users(1) TO users(100)
    VIA follows
    DIRECTION OUT
    MAX_DEPTH 10;
```

---

## Vector Search

> See the [Vector Search Guide](docs/vector-search.md) for detailed usage, k-tuning advice, and performance tips.

### EMBEDDING Columns

The `EMBEDDING` type auto-generates vector embeddings from source columns when rows are inserted or updated. A companion HNSW index is created automatically for each embedding column.

```sql
CREATE TABLE articles (
    id INT PRIMARY KEY,
    title TEXT NOT NULL,
    body TEXT,
    title_vec EMBEDDING(384, source='title', provider='builtin/384'),
    body_vec EMBEDDING(1536, source='body', provider='openai/text-embedding-3-small')
);

-- Embeddings are generated automatically on insert
INSERT INTO articles (id, title, body)
VALUES (1, 'AI in 2025', 'Latest trends in artificial intelligence');
```

### NEAREST (Similarity Search)

Find the `k` most similar rows to a query. The first argument controls how many neighbors to return — use lower k (3-5) for precision, higher k (10-20) for broader recall. Results are sorted by distance ascending and include a `_distance` column.

```sql
-- Text query (auto-embedded using the column's provider)
NEAREST 5 FROM articles.title_vec TO 'machine learning';

-- With filter
NEAREST 10 FROM articles.body_vec TO 'distributed systems'
WHERE id > 0;

-- Explicit vector
NEAREST 5 FROM products.embedding TO [0.1, 0.2, 0.3, ...];

-- Distance metric (default: COSINE)
NEAREST 10 FROM articles.body_vec TO 'query text' USING L2;
NEAREST 10 FROM articles.body_vec TO 'query text' USING DOT;
NEAREST 10 FROM articles.body_vec TO 'query text' USING COSINE;
```

### Graph-Scoped Vector Search

Combine graph traversal with vector search. Find the most relevant articles among those authored by people user 1 follows:

```sql
NEAREST 5 FROM articles.body_vec TO 'data science'
WITHIN TRAVERSE authored FROM users(1) DIRECTION OUT MAX_DEPTH 2;
```

---

## Embedding Providers

GioDB supports four embedding provider types. The provider name format is `"type/model"`.

### OpenAI

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

### Ollama

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

### ONNX (Offline / Network-Free)

Runs a local ONNX model for embedding inference. No network access required after model download — ideal for air-gapped environments, CI pipelines, and local development.

```sql
CREATE TABLE docs (
    id INT PRIMARY KEY,
    content TEXT,
    vec EMBEDDING(384, source='content', provider='onnx/models/all-MiniLM-L6-v2/model.onnx')
);
```

| Parameter | Required | Example |
|-----------|----------|---------|
| Model path | Yes (in provider name) | Relative or absolute path to `.onnx` file |

#### Downloading ONNX Models

Any ONNX-exported transformer embedding model that accepts `input_ids` and `attention_mask` inputs will work. The recommended approach is to download pre-converted models from Hugging Face.

**Option 1 — Download a pre-converted model (recommended):**

```bash
# Install the Hugging Face CLI
pip install huggingface-hub

# all-MiniLM-L6-v2 (384 dimensions, ~80 MB) — best balance of size and quality
huggingface-cli download onnx-community/all-MiniLM-L6-v2-ONNX \
    --local-dir models/all-MiniLM-L6-v2

# bge-small-en-v1.5 (384 dimensions, ~130 MB)
huggingface-cli download onnx-community/bge-small-en-v1.5-ONNX \
    --local-dir models/bge-small-en-v1.5
```

**Option 2 — Export any Hugging Face model to ONNX yourself:**

```bash
pip install optimum onnxruntime sentence-transformers

# Export to ONNX format
optimum-cli export onnx \
    --model sentence-transformers/all-MiniLM-L6-v2 \
    models/all-MiniLM-L6-v2
```

This produces `model.onnx` (and optionally `model.onnx_data`) in the output directory.

#### Recommended Models

| Model | Dimensions | Size | Notes |
|-------|-----------|------|-------|
| `all-MiniLM-L6-v2` | 384 | ~80 MB | Best for general-purpose semantic search |
| `all-MiniLM-L12-v2` | 384 | ~120 MB | Higher quality, slightly slower |
| `bge-small-en-v1.5` | 384 | ~130 MB | Strong retrieval performance |
| `nomic-embed-text-v1.5` | 768 | ~550 MB | High quality, larger dimension |

#### Usage

Point the provider to the `.onnx` file inside the downloaded model directory:

```sql
-- Relative path (from server working directory)
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

#### Current Limitations

- **Tokenizer**: Uses a hash-based tokenizer (not WordPiece/BPE). Token IDs are deterministic and consistent, enabling reliable word-overlap similarity, but they do not match the pretrained model's vocabulary. This means embeddings are functional for fuzzy matching but do not capture the full semantic quality of the original model. A proper tokenizer integration is planned.
- **Sequence length**: Max 128 tokens (longer text is truncated).
- **Batch size**: Inference runs one input at a time (no batched GPU inference).

### Builtin

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

### Re-embedding

After changing a provider or model, regenerate all embeddings:

```sql
REEMBED TABLE articles;
```

---

## Admin Commands

```sql
SHOW DATABASES;
SHOW TABLES;
SHOW COLUMNS FROM users;
SHOW EDGE TYPES;
SHOW INDEXES;
SHOW EMBEDDINGS;
SHOW PROVIDERS;

DESCRIBE users;

EXPLAIN SELECT * FROM users WHERE age > 25;
EXPLAIN ANALYZE SELECT * FROM users WHERE age > 25;

SET max_connections = 200;
SET log_level = 'debug';
SHOW PARAMETER max_connections;

VACUUM;
ANALYZE;
```

---

## Development

### Required Tools

This project requires **LLVM/Clang 20** for formatting and static analysis (must match CI).

**macOS:**

```bash
brew install llvm@20
```

Homebrew installs LLVM 20 as keg-only. The binaries are at `/opt/homebrew/opt/llvm@20/bin/`. To make them available system-wide, add to your shell profile:

```bash
export PATH="/opt/homebrew/opt/llvm@20/bin:$PATH"
```

**Ubuntu:**

```bash
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc
sudo add-apt-repository -y "deb http://apt.llvm.org/$(lsb_release -cs)/ llvm-toolchain-$(lsb_release -cs)-20 main"
sudo apt-get update
sudo apt-get install -y clang-format-20 clang-tidy-20
```

### Build

```bash
# Configure (requires VCPKG_ROOT env var)
cmake --preset default          # Debug build
cmake --preset release          # Release build
cmake --preset asan             # AddressSanitizer
cmake --preset tsan             # ThreadSanitizer

# Build
cmake --build build/debug
cmake --build build/release

# Run all tests
ctest --test-dir build/debug --output-on-failure
```

### Testing

Tests are split into three categories, each with its own CMake target and CTest label:

| Category | Target | Directory | CTest Label |
|----------|--------|-----------|-------------|
| Dev unit tests | `giodb_unit_tests` | `tests/unit/` | `unit` |
| QA regression tests | `giodb_qa_tests` | `tests/qa/` | `qa` |
| Integration tests | `giodb_integration_tests` | `tests/integration/` | `integration` |

```bash
# Build and run all tests
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure

# Run only dev unit tests (fastest, use during development)
cmake --build build/debug --target giodb_unit_tests
ctest --test-dir build/debug -L unit --output-on-failure

# Run only QA regression tests
cmake --build build/debug --target giodb_qa_tests
ctest --test-dir build/debug -L qa --output-on-failure

# Run only integration tests
cmake --build build/debug --target giodb_integration_tests
ctest --test-dir build/debug -L integration --output-on-failure

# Run a specific test by name (supports wildcards)
./build/debug/tests/unit/giodb_unit_tests --gtest_filter="BufferPool*"
./build/debug/tests/qa/giodb_qa_tests --gtest_filter="*GDB258*"

# Run a specific test suite
./build/debug/tests/unit/giodb_unit_tests --gtest_filter="ExprEvaluator.*"

# List all available tests without running them
./build/debug/tests/unit/giodb_unit_tests --gtest_list_tests
./build/debug/tests/qa/giodb_qa_tests --gtest_list_tests
```

New test files are auto-detected by CMake — just add them to the correct directory:
- `tests/unit/test_<name>.cpp` for dev tests
- `tests/qa/test_qa_<ticket>.cpp` for QA regression tests

### Pre-commit hooks

Install the project's pre-commit hooks to automatically check formatting before each commit:

```bash
git config core.hooksPath .githooks
```

The hook checks `clang-format-20` on staged `.cpp` and `.h` files. It warns if a different major version is detected and skips gracefully if clang-format is not installed.

### Formatting and Static Analysis

CMake targets are provided to reproduce CI checks locally:

```bash
# Check formatting (dry-run, mirrors CI)
cmake --build build/debug --target format-check

# Auto-fix formatting in-place
cmake --build build/debug --target format-fix

# Run clang-tidy with CI flags
cmake --build build/debug --target tidy
```

### CI Pipeline

The CI pipeline runs on every push to `main` and every pull request:

1. **Format Check** - Verifies all source files match `clang-format-20` output
2. **Clang-Tidy** - Static analysis with `clang-tidy-20` (select warnings as errors)
3. **Build Matrix** - Compiles and tests on Linux GCC 13, Linux Clang 20, and macOS Apple Clang

## Contributing

Contributions are welcome. Please open an issue or pull request.
