---
name: quality-checks
description: Use when you need to run code quality tools — clang-format, clang-tidy, building, running tests, or AddressSanitizer. Provides the exact commands and interpretation of results for the GioDB project.
user-invocable: false
---

# Quality Checks

## Build

```bash
export VCPKG_ROOT="$HOME/vcpkg" && cmake --preset default && cmake --build build/debug
```

Must complete with zero errors and zero warnings (`-Werror` is enabled).

## Tests (Developer)

Build and run developer unit tests only:

```bash
export VCPKG_ROOT="$HOME/vcpkg" && cmake --preset default && cmake --build build/debug --target giodb_unit_tests && ./build/debug/tests/unit/giodb_unit_tests
```

All dev tests must pass. If any fail, read the failure output and fix before proceeding.

## QA Tests

QA regression tests are a separate target. These are **not** run by implementers during development — they run in CI and during the QA process.

```bash
# Build QA tests
cmake --build build/debug --target giodb_qa_tests

# Run tests for a specific ticket only
./build/debug/tests/qa/giodb_qa_tests --gtest_filter="*GDB<N>*"
```

### Finding changed files to format

```bash
# Modified + staged files (filter to .h/.cpp under include/, src/, tests/)
git diff --name-only HEAD -- 'include/*.h' 'src/*.cpp' 'src/*.h' 'tests/*.cpp' 'tests/*.h'
git diff --name-only --cached -- 'include/*.h' 'src/*.cpp' 'src/*.h' 'tests/*.cpp' 'tests/*.h'

# Untracked new files
git ls-files --others --exclude-standard -- 'include/' 'src/' 'tests/' | grep -E '\.(h|cpp)$'
```

## AddressSanitizer

For deeper memory safety checks on developer tests:

```bash
export VCPKG_ROOT="$HOME/vcpkg" && cmake --preset asan
cmake --build build/asan --target giodb_unit_tests
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 ./build/asan/tests/unit/giodb_unit_tests
```

Detects: buffer overflows, use-after-free, memory leaks, stack overflows.

For QA tests under ASan (used by the QA process, not implementers):

```bash
cmake --build build/asan --target giodb_qa_tests
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 ./build/asan/tests/qa/giodb_qa_tests --gtest_filter="*GDB<N>*"
```

## Pre-Commit Checklist

Before every commit, run these in order:

1. Build the project (zero warnings)
2. Ticket-Specific Tests all PASS

Never commit if any of these steps fail.
