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

# Start the server (default port 6767)
./build/debug/src/sixseven-server

# Connect with the built-in CLI
./build/debug/src/sixseven-cli

# Connect with psql
psql -h localhost -p 6767 -U demo
```

## CLI

`sixseven-cli` is a native pg-wire v3 client. It connects directly to a running SixSevenDB server over TCP, executes SQL, and prints tabular results.

```bash
# Connect with defaults (localhost:6767, user=sixseven, db=sixseven)
./build/debug/src/sixseven-cli

# Connect to a specific host/port/user/database
./build/debug/src/sixseven-cli -h myhost -p 6767 -U alice -d mydb

# Execute a single SQL statement and exit (scriptable)
./build/debug/src/sixseven-cli -c "SELECT * FROM users LIMIT 5;"
```

REPL meta-commands: `\q` to quit, `\help` for help. SQL statements must end with `;`.

**Note:** The server defaults to `scram-sha-256` authentication. Configure `auth_method = "trust"` in `config.json` for local development, or run with a pg-wire client that supports SCRAM.

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
