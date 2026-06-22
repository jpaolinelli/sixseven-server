# SixSevenDB Production — Complete Build Plan

## Context

We built a successful proof-of-concept for SixSevenDB — a C++ database that unifies relational tables, graph edges (network model), and vector search (HNSW). The POC validated the core idea: SQL as the interface language, edge types with properties connecting tables, BFS traversal with enriched FETCH/WHERE, and graph-scoped vector search.

Now we rebuild from scratch at production quality. Key learnings from the POC:
- The hybrid SQL model works — users write familiar SQL plus `CREATE EDGE TYPE`, `LINK`, `TRAVERSE`, `NEAREST`
- The EMBEDDING type (auto-generated vector column tied to source data) is the killer feature — every row can optionally have a vector representation
- The custom TCP protocol should be replaced with PostgreSQL wire protocol compatibility for ecosystem leverage
- Storage needs WAL, buffer pool, MVCC for real durability and concurrency

After this plan is approved, we will create tickets for each work item.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                PostgreSQL Wire Protocol (v3)                 │
├─────────────────────────────────────────────────────────────┤
│                    Server (Event-Driven)                      │
│  Connection Pool · Auth · Session · SSL/TLS                  │
├──────────┬──────────┬───────────────────────────────────────┤
│  Parser  │ Planner  │            Executor                    │
│  Lexer   │ Optimizer│  Volcano Iterator · JIT (future)       │
│  AST     │ Cost     │  Hash/Sort-Merge/NL Joins              │
│          │ Model    │  Aggregates · Windows · Subqueries      │
├──────────┴──────────┴───────────────────────────────────────┤
│              Transaction Manager (MVCC)                      │
│  Snapshot Isolation · Lock Manager · Deadlock Detection       │
├──────────┬──────────────────────┬───────────────────────────┤
│  Table   │   Graph Engine       │   Vector Engine            │
│  Engine  │   Edge Storage       │   HNSW (persistent)        │
│  B+Tree  │   Dual Adjacency     │   EMBEDDING type           │
│  Indexes │   Traversal/Patterns │   Embedding Providers       │
├──────────┴──────────────────────┴───────────────────────────┤
│                    Storage Engine                             │
│  Buffer Pool · WAL · Disk Manager · Page Layout              │
└─────────────────────────────────────────────────────────────┘
```

---

## Type System (23 Types)

| Category | Types |
|----------|-------|
| Integer | `INT8`, `INT16`, `INT32`, `INT64` |
| Unsigned | `UINT8`, `UINT16`, `UINT32`, `UINT64` |
| Float | `FLOAT32`, `FLOAT64` |
| Decimal | `DECIMAL(p,s)` (fixed-point, 128-bit) |
| Boolean | `BOOL` |
| String | `STRING` (variable-length, UTF-8) |
| Binary | `BLOB` (variable-length binary) |
| Temporal | `DATE`, `TIME`, `TIMESTAMP`, `INTERVAL` |
| Spatial | `POINT` (2D, future: full GIS) |
| JSON | `JSON` (binary-encoded, indexable paths) |
| UUID | `UUID` (128-bit, auto-gen default) |
| Vector | `EMBEDDING(dim, source_cols, provider)` |

### The EMBEDDING Type — Killer Feature

```sql
CREATE TABLE articles (
    id UUID PRIMARY KEY DEFAULT gen_uuid(),
    title STRING NOT NULL,
    body STRING,
    content_vector EMBEDDING(384, source='title || body', provider='ollama/all-minilm')
);

-- Insert — embedding auto-generated from source columns:
INSERT INTO articles (title, body) VALUES ('Hello World', 'My first article');
-- content_vector is automatically populated by the embedding provider

-- Search — transparent vector similarity:
NEAREST 10 FROM articles.content_vector TO 'search query text'
  WHERE published = true;
-- Text query auto-embedded, then HNSW search with predicate filter

-- Graph-scoped vector search:
NEAREST 5 FROM articles.content_vector TO 'machine learning'
  WITHIN TRAVERSE cites FROM articles('abc-123') DIRECTION OUT MAX_DEPTH 3;
```

**EMBEDDING column behavior:**
- `source` expression defines which columns feed the embedding (concatenated with ` || `)
- `provider` specifies the embedding model: `ollama/<model>`, `openai/<model>`, `onnx/<model>`
- On INSERT/UPDATE of source columns, embedding is (re)generated asynchronously
- Stored as a fixed-dimension float array in a companion HNSW index
- Text queries to NEAREST are auto-embedded via the same provider
- NULL if source columns are all NULL
- Provider configuration is global: `SET embedding_provider_url = 'http://localhost:11434'`

**Provider architecture:**
- `EmbeddingProvider` interface: `embed(text) -> vector<float>`, `embed_batch(texts) -> vector<vector<float>>`
- Built-in providers: `OllamaProvider`, `OpenAIProvider`, `OnnxProvider` (local ONNX runtime, no network)
- Async pipeline: INSERT completes immediately with NULL embedding, background worker generates and updates
- `REEMBED TABLE articles` — admin command to regenerate all embeddings (e.g., after model change)

---

## SQL Language Specification

### DDL

```sql
-- Tables
CREATE TABLE name (col type [constraints], ..., PRIMARY KEY(col [, col2]));
ALTER TABLE name ADD COLUMN col type [constraints];
ALTER TABLE name DROP COLUMN col;
ALTER TABLE name RENAME COLUMN old TO new;
DROP TABLE name;

-- Indexes
CREATE INDEX name ON table(col [, col2]) [USING BTREE|HASH];
CREATE UNIQUE INDEX name ON table(col);
DROP INDEX name;

-- Edge types (graph)
CREATE EDGE TYPE name (prop type, ...) FROM table TO table;
DROP EDGE TYPE name;

-- Constraints
NOT NULL, UNIQUE, DEFAULT expr, CHECK(expr)
FOREIGN KEY (col) REFERENCES table(col)  -- standard relational FK
```

### DML

```sql
-- Standard CRUD
INSERT INTO table (cols) VALUES (vals), (vals), ...;
INSERT INTO table (cols) SELECT ...;
UPDATE table SET col=val, ... WHERE expr;
DELETE FROM table WHERE expr;

-- Graph operations
LINK table(pk) TO table(pk) VIA edge_type (prop=val, ...);
UNLINK table(pk) FROM table(pk) VIA edge_type [WHERE expr];
```

### Queries

```sql
-- Standard SQL
SELECT [DISTINCT] cols FROM table
  [JOIN table ON expr]
  [WHERE expr]
  [GROUP BY cols [HAVING expr]]
  [ORDER BY cols [ASC|DESC]]
  [LIMIT n [OFFSET m]];

-- Subqueries
SELECT * FROM table WHERE col IN (SELECT ...);
SELECT * FROM (SELECT ...) AS sub;

-- Window functions
SELECT col, ROW_NUMBER() OVER (PARTITION BY col ORDER BY col) FROM table;
-- Supported: ROW_NUMBER, RANK, DENSE_RANK, LAG, LEAD, SUM, AVG, COUNT, MIN, MAX

-- Aggregates: COUNT, SUM, AVG, MIN, MAX, COUNT(DISTINCT)

-- CTEs
WITH name AS (SELECT ...) SELECT * FROM name;
```

### Graph Traversal

```sql
-- BFS traversal (existing + enriched)
TRAVERSE edge_type FROM table(pk)
  [DIRECTION IN|OUT|BOTH]
  [MAX_DEPTH n]
  [WHERE expr]
  [FETCH];

-- Shortest path (new)
SHORTEST PATH FROM table(pk) TO table(pk)
  VIA edge_type
  [DIRECTION IN|OUT|BOTH]
  [MAX_DEPTH n];

-- Pattern matching (new — Cypher-inspired but SQL syntax)
MATCH (a:users)-[r:knows]->(b:users)-[s:knows]->(c:users)
  WHERE a.id = 1 AND c.age > 30
  RETURN a.name, b.name, c.name, r.since;
```

### Vector Search

```sql
-- Basic nearest neighbor
NEAREST k FROM table.embedding_col TO [1.0, 2.0, ...]
  [WHERE expr]
  [USING COSINE|L2|DOT];

-- Text-based search (auto-embeds the query)
NEAREST k FROM table.embedding_col TO 'search text'
  [WHERE expr];

-- Graph-scoped
NEAREST k FROM table.embedding_col TO 'search text'
  WITHIN TRAVERSE edge FROM table(pk) DIRECTION OUT MAX_DEPTH n;
```

### Introspection

```sql
SHOW TABLES;
SHOW COLUMNS FROM table;
SHOW EDGE TYPES;
SHOW INDEXES;
DESCRIBE table;
EXPLAIN [ANALYZE] query;
```

### Admin

```sql
SET parameter = value;
SHOW parameter;
BEGIN; COMMIT; ROLLBACK; SAVEPOINT name; ROLLBACK TO name;
REEMBED TABLE table;  -- regenerate all embeddings
VACUUM [table];        -- reclaim space
ANALYZE [table];       -- update statistics
```

---

## Project Structure

```
sixseven/
├── CMakeLists.txt
├── vcpkg.json
├── include/sixseven/
│   ├── common/
│   │   ├── types.h              — Type system (TypeId enum, Value variant, type traits)
│   │   ├── result.h             — Result<T, Error> monad
│   │   ├── status.h             — Status codes
│   │   ├── config.h             — Server configuration
│   │   └── logging.h            — Structured logging (spdlog)
│   ├── storage/
│   │   ├── page.h               — Page layout (slotted pages, 8KB)
│   │   ├── buffer_pool.h        — LRU-K buffer pool manager
│   │   ├── disk_manager.h       — File I/O abstraction
│   │   ├── wal.h                — Write-ahead log
│   │   ├── wal_record.h         — WAL record types
│   │   └── serialization.h      — Row serialization/deserialization
│   ├── catalog/
│   │   ├── catalog.h            — Schema metadata store
│   │   ├── schema.h             — TableSchema, ColumnDef, EdgeTypeDef
│   │   └── system_catalog.h     — System tables (pg_class equivalent)
│   ├── index/
│   │   ├── btree.h              — B+ tree index
│   │   ├── hash_index.h         — Hash index
│   │   └── index_manager.h      — Index lifecycle
│   ├── table/
│   │   ├── table_heap.h         — Heap file (tuple storage)
│   │   ├── table_iterator.h     — Sequential scan iterator
│   │   └── tuple.h              — Tuple layout and access
│   ├── graph/
│   │   ├── edge_storage.h       — Edge table (dual adjacency B+ trees)
│   │   ├── traversal.h          — BFS, DFS, shortest path
│   │   ├── pattern_matcher.h    — MATCH pattern engine
│   │   └── edge_properties.h    — Edge property storage
│   ├── vector/
│   │   ├── hnsw_index.h         — Persistent HNSW index
│   │   ├── embedding_column.h   — EMBEDDING type manager
│   │   ├── embedding_provider.h — Provider interface
│   │   ├── ollama_provider.h    — Ollama API client
│   │   ├── openai_provider.h    — OpenAI API client
│   │   ├── onnx_provider.h      — Local ONNX runtime
│   │   └── distance.h           — Distance functions (L2, cosine, dot)
│   ├── parser/
│   │   ├── lexer.h              — SQL tokenizer
│   │   ├── token.h              — Token types
│   │   ├── parser.h             — Recursive descent parser
│   │   ├── ast.h                — AST node types
│   │   └── ast_visitor.h        — Visitor pattern for AST
│   ├── planner/
│   │   ├── binder.h             — Name resolution, type checking
│   │   ├── logical_plan.h       — Logical operators
│   │   ├── optimizer.h          — Rule + cost-based optimizer
│   │   ├── physical_plan.h      — Physical operators
│   │   ├── statistics.h         — Table/column statistics
│   │   └── cost_model.h         — Cost estimation
│   ├── executor/
│   │   ├── executor.h           — Plan executor
│   │   ├── iterator.h           — Volcano iterator interface
│   │   ├── operators/           — SeqScan, IndexScan, HashJoin, etc.
│   │   └── expression.h         — Expression evaluator
│   ├── txn/
│   │   ├── transaction.h        — Transaction state
│   │   ├── txn_manager.h        — Begin/Commit/Rollback
│   │   ├── mvcc.h               — Multi-version concurrency control
│   │   ├── lock_manager.h       — Row/table locks
│   │   └── deadlock_detector.h  — Wait-for graph cycle detection
│   └── server/
│       ├── server.h             — Event-driven TCP server
│       ├── connection.h         — Per-connection state machine
│       ├── pg_protocol.h        — PostgreSQL v3 wire protocol
│       ├── auth.h               — Authentication (md5, SCRAM-SHA-256)
│       └── session.h            — Session variables, prepared stmts
├── src/                         — (mirrors include/ structure)
├── tests/
│   ├── unit/                    — Per-module unit tests
│   ├── integration/             — Cross-module integration tests
│   ├── e2e/                     — End-to-end with PG protocol
│   ├── fuzz/                    — AFL/libFuzzer targets
│   ├── benchmark/               — Google Benchmark microbenchmarks
│   └── sql/                     — SQL test scripts (.sql + .expected)
├── tools/
│   ├── sixseven-cli/            — Interactive CLI (readline)
│   └── sixseven-bench/          — Custom benchmark driver
└── docs/
    ├── sql-reference.md
    ├── embedding-guide.md
    ├── graph-queries.md
    └── architecture.md
```

---

## Phase 1: Storage Foundation (Weeks 1–6)

### 1.1 Type System
- `TypeId` enum with all 23 types
- `Value` — tagged union / `std::variant` for all types
- Type traits: `size_of()`, `alignment()`, `is_numeric()`, `is_comparable()`
- Type coercion rules matrix
- Serialization: `serialize(Value) -> bytes`, `deserialize(bytes, TypeId) -> Value`
- Comparison operators for all types (for B+ tree ordering)

### 1.2 Page Layout
- **8KB pages** (configurable, power-of-2)
- Slotted page format: header (page_id, slot_count, free_space_offset, lsn) + slot directory + tuple data growing from end
- Page types: `DATA`, `BTREE_INTERNAL`, `BTREE_LEAF`, `OVERFLOW`, `FREE_LIST`, `HNSW_NODE`
- Tuple format: null bitmap + fixed-size fields + variable-length fields (offset/length pairs)
- Large values (>~2KB): overflow pages with TOAST-style chunking

### 1.3 Disk Manager
- File abstraction: one file per table heap, one per index, one per WAL segment
- Direct I/O option (O_DIRECT) for bypassing OS page cache
- Page read/write with checksum verification (CRC32C)
- File growth: extend by 1MB chunks (128 pages at 8KB each)
- Crash safety: fsync after WAL writes, fdatasync for data files

### 1.4 Buffer Pool Manager
- **LRU-K replacement** (K=2) with clock-sweep approximation for scalability
- Configurable size (default 256MB = 32K pages)
- Page table: `page_id -> frame_id` (concurrent hash map)
- Pin/unpin protocol: pinned pages cannot be evicted
- Dirty page tracking and background flusher thread
- Double-write buffer for torn page protection
- Pre-fetching hints for sequential scans

### 1.5 Write-Ahead Log (WAL)
- WAL record types: `INSERT`, `UPDATE`, `DELETE`, `PAGE_SPLIT`, `COMMIT`, `ABORT`, `CHECKPOINT`, `CREATE_TABLE`, `DROP_TABLE`
- Log sequence number (LSN) — monotonically increasing 64-bit
- WAL segments: 16MB files, rotated on fill
- Group commit: batch fsync every 10ms or N records (configurable)
- Recovery: scan WAL forward from last checkpoint, redo committed, undo aborted
- Checkpoint: flush all dirty pages, write checkpoint record, truncate old WAL segments

### 1.6 B+ Tree Index
- Order determined by page size and key size
- Internal nodes: sorted keys + child page pointers
- Leaf nodes: sorted keys + tuple RIDs (page_id + slot_id) + sibling pointers
- Operations: point lookup, range scan, insert (with split), delete (with merge/redistribute)
- Concurrency: latch crabbing (top-down, release parent when child is safe)
- Bulk loading: sorted insert for initial index build
- Support for composite keys (multi-column)
- Support for unique constraint enforcement

---

## Phase 2: Catalog, Parser & Basic Executor (Weeks 4–10)

### 2.1 Catalog
- System tables stored as regular heap tables:
  - `sys_tables(table_id, name, column_count, pk_columns, ...)`
  - `sys_columns(table_id, ordinal, name, type_id, nullable, default_expr, ...)`
  - `sys_indexes(index_id, table_id, name, type, columns, unique, ...)`
  - `sys_edge_types(edge_id, name, source_table, target_table, properties, ...)`
  - `sys_embedding_columns(table_id, column_id, dimension, source_expr, provider, ...)`
- In-memory cache with invalidation on DDL
- Schema versioning for online DDL (future)

### 2.2 Lexer & Tokens
- ~100 token types: keywords (SELECT, CREATE, TRAVERSE, NEAREST, MATCH, EMBEDDING, ...), operators, literals, identifiers
- String/number/UUID literal parsing
- Array literal parsing `[1.0, 2.0, 3.0]` for vector constants
- Comment handling (`--` line, `/* */` block)
- Position tracking for error messages (line, column)

### 2.3 Parser (Recursive Descent)
- Statement types (~30):
  - DDL: CreateTable, AlterTable, DropTable, CreateIndex, DropIndex, CreateEdgeType, DropEdgeType
  - DML: Insert, Update, Delete, Link, Unlink
  - Query: Select (with full clause support), TraverseStmt, NearestStmt, MatchStmt, ShortestPathStmt
  - TCL: Begin, Commit, Rollback, Savepoint
  - Admin: Set, Show, Explain, Describe, Vacuum, Analyze, Reembed
- Expression parser: arithmetic, comparison, logical, IS NULL, IN, BETWEEN, LIKE, CASE, function calls, cast
- Full precedence climbing for expressions
- Error recovery: sync to next semicolon on parse error, report multiple errors

### 2.4 Binder (Semantic Analysis)
- Name resolution: table aliases, column references, star expansion
- Type checking: operator type compatibility, implicit coercion insertion
- Aggregate validation: columns in SELECT must be in GROUP BY or aggregated
- Subquery correlation analysis
- Edge type validation: ensure FROM/TO tables match schema

### 2.5 Basic Executor (Volcano Iterator Model)
- `Iterator` interface: `open()`, `next() -> optional<Tuple>`, `close()`
- Initial operators:
  - `SeqScan` — full table scan via table heap iterator
  - `Filter` — predicate evaluation
  - `Project` — column projection and expression evaluation
  - `Sort` — in-memory sort (external sort in Phase 4)
  - `Limit` — limit + offset
  - `Insert`, `Update`, `Delete` — DML operators
- Expression evaluator: arithmetic, comparisons, string ops, NULL handling, type coercion

---

## Phase 3: Joins, Aggregation & Graph Engine (Weeks 8–16)

### 3.1 Join Operators
- **Nested Loop Join** — simple, supports all join types and predicates
- **Hash Join** — build hash table on smaller input, probe with larger; grace hash join for when build side exceeds memory (partition to disk, recursive)
- **Sort-Merge Join** — for pre-sorted inputs or when sort is needed anyway
- Join types: INNER, LEFT, RIGHT, FULL OUTER, CROSS, SEMI, ANTI
- Join order optimization in planner (dynamic programming for small join counts, greedy for large)

### 3.2 Aggregation
- **Hash Aggregate** — hash table keyed on GROUP BY columns, accumulate aggregate functions
- **Sort Aggregate** — for pre-sorted input
- Aggregate functions: COUNT, SUM, AVG, MIN, MAX, COUNT(DISTINCT), STRING_AGG
- HAVING clause evaluation post-aggregation
- Grouping sets (future): ROLLUP, CUBE

### 3.3 Window Functions
- `WindowOperator` — partitions input, evaluates window functions per partition
- Supported: ROW_NUMBER, RANK, DENSE_RANK, NTILE, LAG, LEAD, FIRST_VALUE, LAST_VALUE
- Window aggregates: SUM, AVG, COUNT, MIN, MAX with frame specification
- Frame types: ROWS BETWEEN, RANGE BETWEEN

### 3.4 Subqueries
- Correlated subqueries: decorrelation into joins where possible
- EXISTS/NOT EXISTS: rewrite to semi/anti joins
- IN subqueries: rewrite to semi join
- Scalar subqueries in SELECT and WHERE

### 3.5 Edge Storage (Graph Engine)
- Edge table per edge type: stores `(source_table, source_pk, target_table, target_pk, properties...)`
- **Dual B+ tree adjacency indexes**:
  - Forward index: `(source_pk) -> [(target_pk, edge_row_id), ...]`
  - Reverse index: `(target_pk) -> [(source_pk, edge_row_id), ...]`
- Edge property storage: regular columns in the edge table, accessed by edge_row_id
- LINK/UNLINK operations: insert/delete in edge table + update both adjacency indexes
- Duplicate edge prevention: composite unique index on `(source_pk, target_pk)` per edge type (configurable)

### 3.6 Graph Traversal
- **BFS** — existing algorithm from POC, production-hardened:
  - Memory-bounded visited set (configurable max nodes)
  - Depth limit (MAX_DEPTH)
  - Direction control (IN/OUT/BOTH)
  - FETCH enrichment (join with row data)
  - WHERE post-filter
  - Full edge collection for visualization (META EDGES protocol)
- **Shortest Path** — bidirectional BFS with path reconstruction
- **Pattern Matching (MATCH)** — compile graph patterns to join plans:
  - Single-hop: rewrite to edge table join
  - Multi-hop: chain of joins with optional WHERE on each node/edge
  - Variable-length patterns `[*1..3]`: iterative deepening or BFS

---

## Phase 4: Vector Engine & EMBEDDING Type (Weeks 12–20)

### 4.1 Persistent HNSW Index
- Replace in-memory HNSW with buffer-pool-backed persistent index
- HNSW page types: node pages (neighbors list), vector data pages
- Layer structure stored in index metadata page
- Insert: allocate node, connect to neighbors, update pages, log to WAL
- Search: traverse layers using buffer pool reads (hot nodes cached automatically)
- Delete: lazy tombstone + periodic compaction
- Parameters: M=16, ef_construction=200, ef_search=64 (configurable per index)

### 4.2 Distance Functions
- L2 (Euclidean), Cosine similarity, Dot product, Inner product
- SIMD-accelerated: AVX2/AVX-512 on x86, NEON on ARM
- Compile-time dispatch based on CPU features
- Batch distance computation for scan operations

### 4.3 EMBEDDING Column Implementation
- Schema: `EMBEDDING(dimension, source='expr', provider='name')`
- Catalog storage: `sys_embedding_columns` table
- On CREATE TABLE with EMBEDDING column:
  1. Create companion HNSW index automatically
  2. Register embedding generation trigger
- On INSERT: store NULL in vector column, enqueue async embedding job
- On UPDATE of source columns: enqueue re-embedding job
- Background worker thread pool (configurable concurrency):
  - Dequeue pending embeddings
  - Call provider API (batch when possible)
  - UPDATE the vector column + HNSW index insert
  - Retry with exponential backoff on provider failure

### 4.4 Embedding Providers
- `EmbeddingProvider` abstract interface:
  ```
  embed(text: string) -> vector<float>
  embed_batch(texts: vector<string>) -> vector<vector<float>>
  dimension() -> size_t
  name() -> string
  ```
- **OllamaProvider**: HTTP client to Ollama REST API, configurable model and URL
- **OpenAIProvider**: HTTP client to OpenAI embeddings API, API key auth, rate limiting
- **OnnxProvider**: Local ONNX Runtime inference, no network dependency, bundled models
- Provider registry: configured via `SET embedding_provider_url`, `SET embedding_api_key`
- Health check: validate provider connectivity and dimension match on CREATE TABLE

### 4.5 NEAREST Query Execution
- Parse: `NEAREST k FROM table.col TO vector_or_text [WHERE pred] [USING metric]`
- If target is text string: auto-embed via the column's provider
- Execute HNSW search (optionally filtered by WHERE predicate)
- Graph-scoped: run BFS first, then `search_filtered()` with reachable node IDs
- Result: rows from the source table + distance column, sorted by distance ASC

### 4.6 REEMBED Command
- `REEMBED TABLE t` — regenerate all embeddings for table `t`
- Sequential scan, batch embed, bulk update
- Progress reporting (row count)
- Useful after changing embedding model or source expression

---

## Phase 5: Transactions, Optimizer & Advanced Features (Weeks 16–28)

### 5.1 MVCC (Multi-Version Concurrency Control)
- Tuple header: `xmin` (creating txn), `xmax` (deleting txn), `t_ctid` (tuple chain pointer)
- Snapshot: list of active transaction IDs at snapshot time
- Visibility rules:
  - Visible if: `xmin` committed && (`xmax` not set || `xmax` aborted || `xmax` > snapshot)
  - Invisible if: `xmin` aborted || `xmin` > snapshot || `xmax` committed && `xmax` <= snapshot
- Isolation levels:
  - **Read Committed** (default) — new snapshot per statement
  - **Snapshot Isolation** — snapshot at BEGIN, detects write-write conflicts
  - **Serializable** — SSI (Serializable Snapshot Isolation) with rw-dependency tracking
- Garbage collection: vacuum process removes dead tuple versions

### 5.2 Lock Manager
- Lock modes: Shared (S), Exclusive (X), Intent Shared (IS), Intent Exclusive (IX)
- Lock granularity: row-level (primary), table-level (for DDL)
- Lock table: concurrent hash map of lock queues
- Wait-for graph: detect deadlocks, abort the youngest transaction

### 5.3 Cost-Based Optimizer
- **Statistics**:
  - Per-table: row count, page count, average row width
  - Per-column: distinct count, min/max, most-common values (MCV list), histogram (equi-depth)
  - ANALYZE command to gather/refresh statistics
- **Cost model**:
  - SeqScan cost: `pages * seq_page_cost + rows * cpu_tuple_cost`
  - IndexScan cost: `selectivity * pages * random_page_cost + matching_rows * cpu_tuple_cost`
  - HashJoin cost: `build_cost + probe_cost + hash_cost`
  - SortMergeJoin cost: `sort_cost(left) + sort_cost(right) + merge_cost`
  - NestLoopJoin cost: `outer_rows * inner_cost`
- **Plan enumeration**:
  - Single table: choose between SeqScan and available IndexScans based on selectivity
  - Joins: dynamic programming (for <=10 tables), greedy heuristic otherwise
  - Interesting orderings: track sort orders through plan tree
- **Rule-based rewrites** (applied before cost-based):
  - Predicate pushdown
  - Projection pushdown
  - Constant folding
  - Subquery decorrelation (EXISTS → semi join, IN → semi join)
  - Join predicate extraction from WHERE

### 5.4 Index Scan Operator
- B+ tree range scan with predicate pushdown
- Index-only scans when all requested columns are in the index
- Bitmap index scan: collect RIDs, sort by page, fetch (reduces random I/O)

### 5.5 External Sort
- Two-phase external merge sort:
  - Phase 1: sort runs that fit in memory, write to temp files
  - Phase 2: K-way merge of sorted runs
- Configurable work memory (`SET work_mem = '64MB'`)
- Used by ORDER BY, Sort-Merge Join, Sort Aggregate, DISTINCT

### 5.6 Hash Index
- Extensible hashing for equality lookups
- Faster than B+ tree for exact match (no range scan support)
- Used by: hash join build side, hash aggregate, `USING HASH` indexes

### 5.7 EXPLAIN / EXPLAIN ANALYZE
- `EXPLAIN` — show logical and physical plan tree with estimated costs
- `EXPLAIN ANALYZE` — execute and show actual row counts, timing per operator
- Output format: text tree (default), JSON

---

## Phase 6: Server & Protocol (Weeks 22–36)

### 6.1 Event-Driven TCP Server
- Single acceptor thread + thread pool for query execution
- Epoll (Linux) / kqueue (macOS) event loop for connection management
- Connection states: `INIT → AUTH → READY → QUERY → READY → ... → CLOSED`
- Graceful shutdown: drain active queries, close connections, flush WAL, checkpoint
- Configuration: `config.json` file — port, data directory, buffer pool size, WAL settings, max connections, embedding provider settings

### 6.2 PostgreSQL Wire Protocol (v3)
- Startup: `StartupMessage` → `AuthenticationOk` / `AuthenticationMD5Password`
- Simple query: `Query` → `RowDescription` + `DataRow`* + `CommandComplete` + `ReadyForQuery`
- Extended query: `Parse` → `Bind` → `Describe` → `Execute` → `Sync` (prepared statements)
- Error protocol: `ErrorResponse` with SQLSTATE codes
- COPY protocol: `CopyInResponse` / `CopyOutResponse` for bulk data (future)
- SSL/TLS: `SSLRequest` → upgrade to TLS connection
- Cancel: `CancelRequest` with backend PID and secret key

**Why PG protocol?** Instant compatibility with psql, pgAdmin, DataGrip, DBeaver, Grafana, and every PG driver. Users can connect with existing tools on day one. Our custom SQL extensions (TRAVERSE, NEAREST, MATCH, EMBEDDING) pass through as regular SQL strings — the protocol doesn't care about SQL dialect.

### 6.3 Authentication
- Trust (no auth) — for development
- MD5 password — `md5(md5(password + username) + salt)`
- SCRAM-SHA-256 — production-grade, PG-compatible
- User management: `CREATE USER name WITH PASSWORD 'pass'`, `DROP USER name`, `GRANT/REVOKE`

### 6.4 Session Management
- Session variables: `SET variable = value`, `SHOW variable`
- Prepared statements: `PREPARE name AS SELECT ...`, `EXECUTE name(params)`
- Cursors: `DECLARE name CURSOR FOR SELECT ...`, `FETCH n FROM name` (future)

> **Note:** Client libraries and web admin have been moved to the [client repository](https://github.com/SixSeven/client).

### 7.2 Documentation
- SQL Reference: complete syntax for all statements with examples
- Embedding Guide: setup providers, create EMBEDDING columns, search patterns
- Graph Queries: traversal, pattern matching, shortest path examples
- Architecture: storage layout, MVCC, WAL, index structures

---

## Phase 8: Testing, Benchmarks & Hardening (Weeks 36–48)

### 8.1 Test Infrastructure
- **Unit tests**: one test file per source file, Google Test framework
- **Integration tests**: cross-module tests (parser → planner → executor)
- **E2E tests**: connect via PG protocol, execute SQL, verify results
  - SQL test runner: `.sql` input + `.expected` output files
  - Covers all SQL features, edge cases, error messages
- **Fuzz testing**: AFL/libFuzzer on parser (malformed SQL), protocol (malformed packets)
- **Crash recovery tests**: inject crashes at random WAL positions, verify recovery
- **Concurrency tests**: thread sanitizer, multiple concurrent transactions, deadlock scenarios

### 8.2 Benchmarks
- **Microbenchmarks** (Google Benchmark):
  - Buffer pool: page fetch latency, hit rate under load
  - B+ tree: insert/lookup/range scan throughput
  - HNSW: insert/search latency vs. recall
  - Expression evaluation: arithmetic, string, comparison throughput
- **Macro benchmarks**:
  - TPC-H subset (analytical queries): measure optimizer quality
  - Graph benchmark: social network traversal (1M nodes, 10M edges)
  - Vector benchmark: ANN recall@10 vs. latency at various dataset sizes
  - Mixed workload: concurrent OLTP + graph traversal + vector search

### 8.3 Hardening
- Memory safety: run all tests under AddressSanitizer, MemorySanitizer
- Thread safety: ThreadSanitizer for all concurrent code paths
- Undefined behavior: UBSan for all arithmetic and type operations
- Valgrind: leak detection on integration tests
- Static analysis: clang-tidy, cppcheck
- CI pipeline: build + test on Linux (GCC, Clang) and macOS (Apple Clang)

---

## Implementation Order Summary

| Weeks | Phase | Description |
|-------|-------|-------------|
| 1–6 | Phase 1 | Storage: types, pages, disk, buffer pool, WAL, B+ tree |
| 4–10 | Phase 2 | Catalog, parser, binder, basic executor (CRUD + scans) |
| 8–16 | Phase 3 | Joins, aggregation, windows, graph engine (edges, traversal, patterns) |
| 12–20 | Phase 4 | Vector engine: persistent HNSW, EMBEDDING type, providers |
| 16–28 | Phase 5 | MVCC, lock manager, optimizer, EXPLAIN, external sort |
| 22–36 | Phase 6 | PG wire protocol server, auth |
| 30–42 | Phase 7 | Documentation, polish |
| 36–48 | Phase 8 | Testing, fuzz, crash recovery, benchmarks, hardening |

Phases overlap intentionally — each phase starts before the previous one ends, but depends on the critical path from the prior phase being complete.

---

## Verification (End-to-End Smoke Test)

After all phases, this sequence validates the complete system:

```bash
# 1. Start server
./sixseven-server config.json

# 2. Connect with psql (PG protocol compatibility)
psql -h localhost -p 6767 -U demo demo
```

```sql
-- 3. Create schema
CREATE TABLE users (
    id UUID PRIMARY KEY DEFAULT gen_uuid(),
    name STRING NOT NULL,
    age INT32,
    bio STRING,
    bio_vec EMBEDDING(384, source='name || bio', provider='ollama/all-minilm')
);

CREATE TABLE posts (
    id UUID PRIMARY KEY DEFAULT gen_uuid(),
    author_id UUID NOT NULL,
    title STRING,
    body STRING,
    body_vec EMBEDDING(384, source='title || body', provider='ollama/all-minilm')
);

CREATE EDGE TYPE follows FROM users TO users;
CREATE EDGE TYPE authored FROM users TO posts;
CREATE INDEX idx_posts_author ON posts(author_id);

-- 4. Insert data
INSERT INTO users (name, age, bio) VALUES ('Alice', 30, 'ML engineer');
INSERT INTO users (name, age, bio) VALUES ('Bob', 25, 'Data scientist');
INSERT INTO posts (author_id, title, body) VALUES ('<alice-uuid>', 'Hello', 'First post');
LINK users('<alice-uuid>') TO users('<bob-uuid>') VIA follows;
LINK users('<alice-uuid>') TO posts('<post-uuid>') VIA authored;

-- 5. Relational query
SELECT u.name, COUNT(p.id) AS post_count
FROM users u
LEFT JOIN posts p ON u.id = p.author_id
GROUP BY u.name
ORDER BY post_count DESC;

-- 6. Graph traversal
TRAVERSE follows FROM users('<alice-uuid>') DIRECTION OUT MAX_DEPTH 3 FETCH;

-- 7. Vector search
NEAREST 5 FROM users.bio_vec TO 'machine learning expert' WHERE age > 20;

-- 8. Graph-scoped vector search
NEAREST 5 FROM posts.body_vec TO 'data analysis'
  WITHIN TRAVERSE authored FROM users('<alice-uuid>') DIRECTION OUT MAX_DEPTH 1;

-- 9. Pattern matching
MATCH (a:users)-[f:follows]->(b:users)-[au:authored]->(p:posts)
  WHERE a.name = 'Alice'
  RETURN b.name, p.title;

-- 10. Explain
EXPLAIN ANALYZE SELECT * FROM users WHERE age > 25;
```

```bash
# 11. Web admin & client libraries — see client repository
```
