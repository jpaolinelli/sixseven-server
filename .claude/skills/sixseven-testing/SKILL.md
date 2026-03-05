---
name: sixseven-testing
description: Use when writing or reviewing unit tests for the SixSevenDB project. Provides test patterns, fixture conventions, and quality requirements.
user-invocable: false
---

# SixSevenDB Testing Guide

## Framework

Google Test (gtest). Dev tests live in `tests/unit/test_<name>.cpp`. QA regression tests live in `tests/qa/test_qa_<ticket>.cpp`.

## Test Targets

The project has two separate test executables:

| Target | Directory | File Pattern | CTest Label | Purpose |
|--------|-----------|-------------|-------------|---------|
| `sixseven_unit_tests` | `tests/unit/` | `test_<name>.cpp` | `unit` | Developer functional tests |
| `sixseven_qa_tests` | `tests/qa/` | `test_qa_gdb_<N>.cpp` | `qa` | QA regression / adversarial tests |

> **Important**: Developer tests MUST NOT be added to `sixseven_qa_tests`. QA tests MUST NOT be added to `sixseven_unit_tests`. Each target has a strict ownership boundary.

## File Registration

Test files are auto-detected by CMake via `file(GLOB CONFIGURE_DEPENDS ...)`. No manual registration is needed — just add the file to the correct directory:

- `tests/unit/` for dev tests → `sixseven_unit_tests` target (CTest label: `unit`)
- `tests/qa/` for QA regression tests → `sixseven_qa_tests` target (CTest label: `qa`)

## Test Naming

Use `TEST(SuiteName, TestName)` or `TEST_F(FixtureName, TestName)`:

```cpp
TEST(ExprEvaluator, LiteralInt)    { ... }
TEST_F(QueryEngineTest, SelectAll) { ... }
```

- **Suite name**: The component under test, PascalCase.
- **Test name**: What is being tested, PascalCase. Be specific — `InsertSingleRow`, not `Test1`.

## Fixture Pattern

Use a test fixture when multiple tests share setup/teardown:

```cpp
class MyOperatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create temp dirs, initialize storage, etc.
    }
    void TearDown() override {
        // Clean up temp dirs, reset state.
    }

    // Shared test helpers
    void insert_test_data() { ... }

    // Shared members
    DiskManager dm_;
    Catalog catalog_;
};
```

For tests that need disk-backed storage (table heap, buffer pool), use a temp directory:

```cpp
void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_<name>";
    std::filesystem::remove_all(data_dir_);
    std::filesystem::create_directories(data_dir_ / "tables");
    // ... create DiskManager, BufferPoolManager, TableHeap, etc.
}

void TearDown() override {
    // ... reset smart pointers in reverse order
    std::filesystem::remove_all(data_dir_);
}
```

## Assertion Patterns

- `EXPECT_*` for non-fatal checks (test continues).
- `ASSERT_*` for fatal checks (test stops) — use when subsequent lines depend on this.

```cpp
// Check Result<T> success
auto result = engine_->execute(sql);
ASSERT_TRUE(result.has_value()) << result.error().message;

// Check Result<T> failure
auto result = engine_->execute(bad_sql);
ASSERT_FALSE(result.has_value());
EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);

// Check container sizes before indexing
ASSERT_EQ(qr.rows.size(), 3u);
EXPECT_EQ(qr.rows[0][0].as_int32(), 1);

// Check string values
EXPECT_EQ(qr.rows[0][1].as_string(), "alice");
```

## Test Quality Requirements

Every test MUST:

1. **Assert something substantive** — never create a test that only calls a function without checking results.
2. **Test one specific behavior** — not a grab-bag of unrelated checks.
3. **Have a clear name** describing what is verified.

Tests SHOULD cover:

- **Happy path**: Normal inputs produce expected outputs.
- **Edge cases**: Empty inputs, NULL values, zero/one/many, boundary values.
- **Error conditions**: Invalid inputs return the correct `StatusCode`.
- **Integration**: At least one test exercising the full pipeline end-to-end.

## Operator Testing Pattern

For Volcano iterator operators, follow this pattern:

```cpp
// 1. Build the child data (schema + heap with test rows)
// 2. Create the operator
// 3. Call open()
// 4. Pull all tuples via next() in a loop
// 5. Assert tuple contents
// 6. Call close()

auto op = std::make_unique<SomeOperator>(/* ... */);
auto open_result = op->open();
ASSERT_TRUE(open_result.has_value());

std::vector<Tuple> rows;
while (true) {
    auto next_result = op->next();
    ASSERT_TRUE(next_result.has_value());
    if (!next_result->has_value()) break;
    rows.push_back(std::move(**next_result));
}
op->close();

ASSERT_EQ(rows.size(), expected_count);
EXPECT_EQ(rows[0].values[col_idx].as_int32(), expected_value);
```

## Build and Run

Build and run developer unit tests (default for implementers):

```bash
cmake --build build/debug --target sixseven_unit_tests
./build/debug/tests/unit/sixseven_unit_tests
```

To run via CTest with the `unit` label only:

```bash
ctest --test-dir build/debug -L unit --output-on-failure
```
