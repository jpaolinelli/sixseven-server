# GioDB - Project Guide

GioDB is a C++20 hybrid relational/graph/vector database with PostgreSQL wire protocol compatibility.

## Architecture

```
Client Libraries (Python, Node.js, Go, Rust, Java, .NET)
        │
PostgreSQL Wire Protocol (v3)
        │
Server (Event-Driven, Connection Pool)
        │
┌───────┼───────┐
Parser  Planner  Executor (Volcano Iterator)
        │
┌───────┼───────────┐
Table   Graph       Vector
Engine  Engine      Engine (HNSW)
        │
Storage Engine (Buffer Pool, WAL, B+ Trees)
```

## Build Commands

```bash
# Configure (requires VCPKG_ROOT env var)
cmake --preset default          # Debug build
cmake --preset release          # Release build
cmake --preset asan             # AddressSanitizer
cmake --preset tsan             # ThreadSanitizer

# Build
cmake --build build/debug
cmake --build build/release

# Run tests
cmake --build build/debug --target giodb_unit_tests
./build/debug/tests/unit/giodb_unit_tests
ctest --test-dir build/debug --output-on-failure

# Run server
./build/debug/src/giodb-server [config.json]

# Run CLI
./build/debug/src/giodb-cli
```

## Directory Layout

```
include/giodb/<module>/   — Public headers
src/<module>/             — Implementation files
tests/unit/               — Unit tests (Google Test)
tests/integration/        — Integration tests
tests/e2e/                — End-to-end tests
tests/fuzz/               — Fuzz tests
tests/benchmark/          — Benchmarks (Google Benchmark)
tools/                    — CLI and benchmark tools
docs/                     — Documentation
```

Modules: `common`, `storage`, `catalog`, `index`, `table`, `graph`, `vector`, `parser`, `planner`, `executor`, `txn`, `server`

## Coding Conventions

- **Functions/variables**: `snake_case`
- **Types/classes**: `PascalCase`
- **Constants/macros**: `UPPER_CASE`
- **Macro prefix**: `GIODB_`
- **Namespaces**: All code in `giodb::` namespace
- **File naming**: `snake_case.h` / `snake_case.cpp`
- **Include guards**: Use `#pragma once`

## Error Handling

- Always use `Result<T>` (alias for `tl::expected<T, Error>`) — never throw exceptions
- Use `ok(value)` to return success, `make_error(StatusCode, message)` for errors
- StatusCode enum in `include/giodb/common/status.h`
- Error includes `std::source_location` automatically

```cpp
Result<int> parse_number(const std::string& s) {
    if (s.empty()) return make_error(StatusCode::PARSE_ERROR, "empty input");
    return ok(std::stoi(s));
}
```

## Logging

Use project macros from `include/giodb/common/logging.h`:

```cpp
GIODB_LOG_TRACE("detailed trace: {}", value);
GIODB_LOG_DEBUG("debug info");
GIODB_LOG_INFO("startup message");
GIODB_LOG_WARN("warning: {}", reason);
GIODB_LOG_ERROR("error occurred: {}", err.message);
GIODB_LOG_FATAL("unrecoverable error");
```

Initialize at startup: `giodb::init_logging("info");`

## Testing

- Every new source file needs a corresponding `test_<name>.cpp` in `tests/unit/`
- Use Google Test: `TEST(SuiteName, TestName) { ... }`
- Add test files to `tests/unit/CMakeLists.txt`
- Test target: `giodb_unit_tests`

## Dependencies (vcpkg)

| Package | Purpose |
|---------|---------|
| spdlog | Structured logging |
| nlohmann-json | JSON parsing / config |
| gtest | Google Test framework |
| benchmark | Google Benchmark |
| tl-expected | `tl::expected<T,E>` error handling |

## Key Patterns

- **Volcano Iterator Model**: `open() → next() → close()` for query execution operators
- **Buffer Pool**: Pin/unpin protocol for page access, LRU-K eviction
- **WAL**: Write-ahead logging with group commit for crash recovery
- **MVCC**: Tuple versioning with xmin/xmax for transaction isolation
- **EMBEDDING type**: Native vector column type with auto-generation from source columns

## Type System (22 Types)

INT8, INT16, INT32, INT64, UINT8, UINT16, UINT32, UINT64, FLOAT32, FLOAT64, DECIMAL(p,s), BOOL, STRING, BLOB, DATE, TIME, TIMESTAMP, INTERVAL, POINT, JSON, UUID, EMBEDDING(dim, source, provider)

## Compiler Flags

`-Wall -Wextra -Wpedantic -Werror` — all warnings enabled, warnings are errors.
