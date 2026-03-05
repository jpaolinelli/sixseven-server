---
name: sixseven-architecture
description: Use when you need to understand the SixSevenDB project structure, module layout, or system architecture. Provides the module map, dependency graph, and key abstractions.
user-invocable: false
---

# SixSevenDB Architecture

## System Overview

SixSevenDB is a C++20 hybrid relational/graph/vector database with PostgreSQL wire protocol compatibility.

```
Client Libraries (Python, Node.js, Go, Rust, Java, .NET)
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

## SQL Pipeline

A SQL query flows through these stages in order:

```
SQL string
  -> Lexer     (src/parser/lexer.cpp)       -> token stream
  -> Parser    (src/parser/parser.cpp)       -> AST (ast.h)
  -> Binder    (src/planner/binder.cpp)      -> BoundStatement (semantic validation)
  -> Planner   (src/executor/planner.cpp)    -> Iterator tree (physical plan)
  -> Executor  (src/executor/query_engine.cpp) -> open/next/close drain -> QueryResult
```

DDL statements (CREATE TABLE, DROP TABLE) are dispatched before binding — the table must exist in the catalog before the binder can validate references to it.

## Module Map

| Module | Headers | Source | Purpose |
|--------|---------|--------|---------|
| `common` | `include/sixseven/common/` | `src/common/` | Result<T>, StatusCode, Error, Value, types, logging, coercion |
| `storage` | `include/sixseven/storage/` | `src/storage/` | DiskManager, BufferPoolManager, page I/O, LRU-K eviction |
| `catalog` | `include/sixseven/catalog/` | `src/catalog/` | Catalog, TableSchema, column metadata, table ID management |
| `table` | `include/sixseven/table/` | `src/table/` | TableHeap, TableIterator, TupleSerializer, slotted pages |
| `parser` | `include/sixseven/parser/` | `src/parser/` | Lexer, Token, Parser, AST nodes (expressions + statements) |
| `planner` | `include/sixseven/planner/` | `src/planner/` | Binder (semantic analysis), TypeResolver |
| `executor` | `include/sixseven/executor/` | `src/executor/` | Volcano iterators, expression evaluator, planner, QueryEngine |
| `index` | `include/sixseven/index/` | `src/index/` | B+ tree index (Phase 3) |
| `graph` | `include/sixseven/graph/` | `src/graph/` | Graph engine (Phase 4+) |
| `vector` | `include/sixseven/vector/` | `src/vector/` | HNSW vector index (Phase 4+) |
| `txn` | `include/sixseven/txn/` | `src/txn/` | Transactions, MVCC (Phase 3+) |
| `server` | `include/sixseven/server/` | `src/server/` | PostgreSQL wire protocol (Phase 4+) |

## Dependency Graph (build order)

```
common
  <- storage
       <- catalog
            <- table
                 <- parser
                      <- planner
                           <- executor
```

## Key Abstractions

### Volcano Iterator Model

All query operators implement the `Iterator` interface:

```cpp
class Iterator {
    virtual Result<void> open() = 0;
    virtual Result<std::optional<Tuple>> next() = 0;
    virtual void close() = 0;
    virtual const OutputSchema& output_schema() const = 0;
};
```

Operators compose into a pull-based tree: `SeqScan -> Project -> Sort -> Limit`.

### Storage Layer

- **DiskManager**: File-level I/O (read/write pages).
- **BufferPoolManager**: In-memory page cache with LRU-K eviction, pin/unpin protocol.
- **TableHeap**: Slotted-page tuple storage with insert/update/delete.
- **StorageManager**: Per-table file + BPM + heap lifecycle.

### Catalog

- **Catalog**: In-memory registry of table schemas (table_id -> TableSchema).
- **TableSchema**: Table name, columns (name, type, nullable), auto-increment table IDs.

### Type System

22 types: INT8, INT16, INT32, INT64, UINT8, UINT16, UINT32, UINT64, FLOAT32, FLOAT64, DECIMAL, BOOL, STRING, BLOB, DATE, TIME, TIMESTAMP, INTERVAL, POINT, JSON, UUID, EMBEDDING.

## Directory Layout

```
include/sixseven/<module>/   — Public headers
src/<module>/             — Implementation files
tests/unit/               — Dev unit tests (Google Test)
tests/qa/                 — QA regression tests (test_qa_*.cpp)
tests/integration/        — Integration tests
tests/e2e/                — End-to-end tests
tests/fuzz/               — Fuzz tests
tests/benchmark/          — Benchmarks (Google Benchmark)
```
