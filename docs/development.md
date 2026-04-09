# Development

## Required Tools

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

## Build

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

## Testing

Tests are split into three categories, each with its own CMake target and CTest label:

| Category | Target | Directory | CTest Label |
|----------|--------|-----------|-------------|
| Dev unit tests | `sixseven_unit_tests` | `tests/unit/` | `unit` |
| QA regression tests | `sixseven_qa_tests` | `tests/qa/` | `qa` |
| Integration tests | `sixseven_integration_tests` | `tests/integration/` | `integration` |

```bash
# Build and run all tests
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure

# Run only dev unit tests (fastest, use during development)
cmake --build build/debug --target sixseven_unit_tests
ctest --test-dir build/debug -L unit --output-on-failure

# Run only QA regression tests
cmake --build build/debug --target sixseven_qa_tests
ctest --test-dir build/debug -L qa --output-on-failure

# Run only integration tests
cmake --build build/debug --target sixseven_integration_tests
ctest --test-dir build/debug -L integration --output-on-failure

# Run a specific test by name (supports wildcards)
./build/debug/tests/unit/sixseven_unit_tests --gtest_filter="BufferPool*"
./build/debug/tests/qa/sixseven_qa_tests --gtest_filter="*GDB258*"

# Run a specific test suite
./build/debug/tests/unit/sixseven_unit_tests --gtest_filter="ExprEvaluator.*"

# List all available tests without running them
./build/debug/tests/unit/sixseven_unit_tests --gtest_list_tests
./build/debug/tests/qa/sixseven_qa_tests --gtest_list_tests
```

New test files are auto-detected by CMake — just add them to the correct directory:
- `tests/unit/test_<name>.cpp` for dev tests
- `tests/qa/test_qa_<ticket>.cpp` for QA regression tests

## Seed Data

Generate realistic seed data that exercises every feature (relational, graph, vector, transactions, admin). Three scales are available:

| Scale | Data Rows | Edge Links | SQL Lines | File Size |
|-------|-----------|------------|-----------|-----------|
| `small` | ~7K | ~7.5K | ~15K | ~3 MB |
| `medium` | ~73K | ~75K | ~149K | ~26 MB |
| `large` | ~725K | ~750K | ~1.5M | ~272 MB |

```bash
# Generate seed SQL (requires Python 3, no external dependencies)
python3 tools/generate_seed_data.py --scale small  > tools/seed_small.sql
python3 tools/generate_seed_data.py --scale medium > tools/seed_medium.sql
python3 tools/generate_seed_data.py --scale large  > tools/seed_large.sql
python3 tools/generate_loadtest_data.py --scale stress > tools/seed_stress.sql
python3 tools/generate_loadtest_data.py --scale massive > tools/seed_massive.sql
python3 tools/generate_loadtest_data.py --scale extreme > tools/seed_extreme.sql

# Load into a running server
psql -h localhost -p 6767 -f tools/seed_small.sql
```

Pre-generated files are checked in at `tools/seed_small.sql`, `tools/seed_medium.sql`, and `tools/seed_large.sql`.

The seed data creates 8 tables, 4 edge types, 9 indexes, and 2 EMBEDDING columns (using the ONNX provider). It includes verification queries for JOINs, CTEs, window functions, subqueries, set operations, graph traversal, pattern matching (variable-length paths, inline predicates, path selectors, weighted shortest path), vector search, graph algorithms, and admin commands.

**Note:** Embedding columns use the ONNX provider (`onnx/models/all-MiniLM-L6-v2`). Install the model first — see [models/README.md](../models/README.md).

## Pre-commit Hooks

Install the project's pre-commit hooks to automatically check formatting before each commit:

```bash
git config core.hooksPath .githooks
```

The hook checks `clang-format-20` on staged `.cpp` and `.h` files. It warns if a different major version is detected and skips gracefully if clang-format is not installed.

## Formatting and Static Analysis

CMake targets are provided to reproduce CI checks locally:

```bash
# Check formatting (dry-run, mirrors CI)
cmake --build build/debug --target format-check

# Auto-fix formatting in-place
cmake --build build/debug --target format-fix

# Run clang-tidy with CI flags
cmake --build build/debug --target tidy
```

## CI Pipeline

The CI pipeline runs on every push to `main` and every pull request:

1. **Format Check** - Verifies all source files match `clang-format-20` output
2. **Clang-Tidy** - Static analysis with `clang-tidy-20` (select warnings as errors)
3. **Build Matrix** - Compiles and tests on Linux GCC 13, Linux Clang 20, and macOS Apple Clang
