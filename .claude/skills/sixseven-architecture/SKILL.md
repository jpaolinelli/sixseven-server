---
name: sixseven-architecture
description: Use when you need to understand the SixSevenDB project structure, module layout, or system architecture. Provides the module→path map, SQL pipeline stages, dependency graph, and key abstractions.
user-invocable: false
---

# SixSevenDB Architecture

> The high-level diagram, module list, directory layout, and 23-type list are in the project `CLAUDE.md`, which is always loaded. This skill carries only the detail `CLAUDE.md` does not: the SQL pipeline stages with file paths, the Iterator contract, the build-order dependency graph, and the module→path map. Read it when you need to trace a query path or locate a module's files.

## SQL Pipeline

A SQL query flows through these stages in order:

```
SQL string
  -> Lexer     (src/parser/lexer.cpp)         -> token stream
  -> Parser    (src/parser/parser.cpp)         -> AST (ast.h)
  -> Binder    (src/planner/binder.cpp)        -> BoundStatement (semantic validation)
  -> Planner   (src/executor/planner.cpp)      -> Iterator tree (physical plan)
  -> Executor  (src/executor/query_engine.cpp) -> open/next/close drain -> QueryResult
```

DDL statements (CREATE TABLE, DROP TABLE) are dispatched before binding — the table must exist in the catalog before the binder can validate references to it.

## Module → Path Map

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
