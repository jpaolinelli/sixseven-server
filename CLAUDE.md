# SixSevenDB - Project Guide

SixSevenDB is a C++20 hybrid relational/graph/vector database with PostgreSQL wire protocol compatibility.

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

# Vision Document / Original Plan

./docs/sixseven-production-plan.md

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

# Dev tests only (fast, local development)
cmake --build build/debug --target sixseven_unit_tests
./build/debug/tests/unit/sixseven_unit_tests

# QA regression tests
cmake --build build/debug --target sixseven_qa_tests
./build/debug/tests/qa/sixseven_qa_tests

# QA tests for a specific ticket
./build/debug/tests/qa/sixseven_qa_tests --gtest_filter="*GDB258*"

# All tests via CTest (CI behavior)
ctest --test-dir build/debug --output-on-failure

# CTest by label
ctest --test-dir build/debug -L unit --output-on-failure
ctest --test-dir build/debug -L qa --output-on-failure

# Run server
./build/debug/src/sixseven-server [config.json]

# Run CLI
./build/debug/src/sixseven-cli
```

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
tools/                    — CLI and benchmark tools
docs/                     — Documentation
```

Modules: `common`, `storage`, `catalog`, `index`, `table`, `graph`, `vector`, `parser`, `planner`, `executor`, `txn`, `server`

## Coding Conventions

- **Functions/variables**: `snake_case`
- **Types/classes**: `PascalCase`
- **Constants/macros**: `UPPER_CASE`
- **Macro prefix**: `SIXSEVEN_`
- **Namespaces**: All code in `sixseven::` namespace
- **File naming**: `snake_case.h` / `snake_case.cpp`
- **Include guards**: Use `#pragma once`

## Error Handling

- Always use `Result<T>` (alias for `tl::expected<T, Error>`) — never throw exceptions
- Use `ok(value)` to return success, `make_error(StatusCode, message)` for errors
- StatusCode enum in `include/sixseven/common/status.h`
- Error includes `std::source_location` automatically

```cpp
Result<int> parse_number(const std::string& s) {
    if (s.empty()) return make_error(StatusCode::PARSE_ERROR, "empty input");
    return ok(std::stoi(s));
}
```

## Logging

Use project macros from `include/sixseven/common/logging.h`:

```cpp
SIXSEVEN_LOG_TRACE("detailed trace: {}", value);
SIXSEVEN_LOG_DEBUG("debug info");
SIXSEVEN_LOG_INFO("startup message");
SIXSEVEN_LOG_WARN("warning: {}", reason);
SIXSEVEN_LOG_ERROR("error occurred: {}", err.message);
SIXSEVEN_LOG_FATAL("unrecoverable error");
```

Initialize at startup: `sixseven::init_logging("info");`

## Testing

- Every new source file needs a corresponding `test_<name>.cpp` in `tests/unit/`
- QA regression tests go in `tests/qa/` with `test_qa_<ticket>.cpp` naming
- Use Google Test: `TEST(SuiteName, TestName) { ... }`
- Dev test target: `sixseven_unit_tests` (all `tests/unit/*.cpp`)
- QA test target: `sixseven_qa_tests` (all `tests/qa/*.cpp`)
- CTest labels: `unit` for dev tests, `qa` for QA tests, `integration` for integration tests

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
