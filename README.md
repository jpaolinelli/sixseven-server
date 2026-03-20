# SixSevenDB

A hybrid relational, graph, and vector database built in C++20 with PostgreSQL wire protocol compatibility.

## Features

- **Relational** — Full SQL support with B+ tree and hash indexes, joins, CTEs, window functions, and subqueries
- **Graph** — Native edge types, BFS traversal, Cypher-style pattern matching with variable-length paths, inline predicates, weighted shortest path, and path selectors
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
./build/debug/src/sixseven-server

# Connect with psql (default port 6767, trust auth)
psql -h localhost -p 6767
```

## Documentation

- [Server Configuration](docs/configuration.md) — config.json options, replication setup
- [SQL Reference](docs/sql-reference.md) — data types, DDL, DML, queries, aggregates, window functions
- [Graph Queries](docs/graph-queries.md) — edge types, traverse, pattern matching, shortest path
- [Vector Search](docs/vector-search.md) — EMBEDDING columns, NEAREST queries, k-tuning, performance tips
- [Embedding Providers](docs/embedding-providers.md) — OpenAI, Ollama, ONNX, builtin providers
- [Admin Commands](docs/admin-commands.md) — SHOW, EXPLAIN
- [Development](docs/development.md) — build, testing, seed data, formatting, CI pipeline

## Contributing

Contributions are welcome. Please open an issue or pull request.
