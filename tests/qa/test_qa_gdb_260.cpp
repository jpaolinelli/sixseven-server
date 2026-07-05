/// @file test_qa_gdb_260.cpp
/// QA adversarial tests for GDB-260: Fix heap-buffer-overflow in ALTER TABLE
/// ADD COLUMN DEFAULT migration due to type mismatch.
///
/// The bug: the expression evaluator returns INT64 for integer literals, but
/// when the column is declared as a narrower type (e.g., INT/INT32 = 4 bytes),
/// the 8-byte INT64 value overflowed the 4-byte slot in
/// TupleSerializer::serialize(), causing a heap-buffer-overflow.
///
/// The fix: call fit_to_storage() to coerce the evaluated default value to the
/// column's declared type before appending it to migrated tuples.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "test_qa_helpers.h"

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class QA_GDB260 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_qa_gdb_260";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        bootstrap_qa_catalog(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    /// Execute SQL, assert success, return the result.
    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << " => " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    /// Execute SQL, assert failure with expected code.
    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected) << sql;
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// Original reproducer — the exact scenario from the bug report
// =============================================================================

TEST_F(QA_GDB260, OriginalReproducer_AddManyIntColumnsWithDefault) {
    // Exact reproduction from the bug report: CREATE TABLE, INSERT, then
    // add 15+ INT columns with DEFAULT values. Previously caused SIGSEGV
    // (without ASan) or heap-buffer-overflow (with ASan).
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");

    for (int i = 0; i < 20; ++i) {
        exec_ok("ALTER TABLE t ADD COLUMN col" + std::to_string(i) + " INT DEFAULT " +
                std::to_string(i * 10));
    }

    // Verify all values are correct.
    auto select = exec_ok("SELECT * FROM t");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1); // id
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(select.rows[0][static_cast<size_t>(i + 1)].as_int32(), i * 10)
            << "col" << i << " mismatch";
    }
}

// =============================================================================
// Type coercion for every narrower integer type
// =============================================================================

TEST_F(QA_GDB260, AddColumnDefaultTinyint) {
    // TINYINT = INT8 (1 byte). INT64 literal → INT8 is extreme narrowing.
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("ALTER TABLE t ADD COLUMN tiny TINYINT DEFAULT 42");

    auto select = exec_ok("SELECT tiny FROM t");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int8(), 42);
}

TEST_F(QA_GDB260, AddColumnDefaultSmallint) {
    // SMALLINT = INT16 (2 bytes). INT64 literal → INT16.
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("ALTER TABLE t ADD COLUMN sm SMALLINT DEFAULT 1000");

    auto select = exec_ok("SELECT sm FROM t");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int16(), 1000);
}

TEST_F(QA_GDB260, AddColumnDefaultBigint) {
    // BIGINT = INT64 (8 bytes). No narrowing needed — same type as evaluator output.
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("ALTER TABLE t ADD COLUMN big BIGINT DEFAULT 9999999999");

    auto select = exec_ok("SELECT big FROM t");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int64(), 9999999999);
}

// =============================================================================
// Float column with integer DEFAULT
// =============================================================================

TEST_F(QA_GDB260, AddColumnFloatWithIntDefault) {
    // FLOAT = FLOAT32 (4 bytes). Evaluator produces INT64 for integer literal.
    // fit_to_storage should convert INT64 → FLOAT32.
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("ALTER TABLE t ADD COLUMN f FLOAT DEFAULT 7");

    auto select = exec_ok("SELECT f FROM t");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_FLOAT_EQ(select.rows[0][0].as_float32(), 7.0f);
}

TEST_F(QA_GDB260, AddColumnDoubleWithIntDefault) {
    // DOUBLE = FLOAT64 (8 bytes). Evaluator produces INT64 for integer literal.
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("ALTER TABLE t ADD COLUMN d DOUBLE DEFAULT 42");

    auto select = exec_ok("SELECT d FROM t");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_DOUBLE_EQ(select.rows[0][0].as_float64(), 42.0);
}

// =============================================================================
// Float literal DEFAULT for float column (FLOAT64 → FLOAT32 narrowing)
// =============================================================================

TEST_F(QA_GDB260, AddColumnFloatWithFloatLiteralDefault) {
    // FLOAT column with a float literal DEFAULT. Evaluator produces FLOAT64 for
    // float literals; column expects FLOAT32. This is another narrowing path.
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("ALTER TABLE t ADD COLUMN f FLOAT DEFAULT 3.14");

    auto select = exec_ok("SELECT f FROM t");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_NEAR(select.rows[0][0].as_float32(), 3.14f, 0.001f);
}

// =============================================================================
// Multiple rows — ensure every row gets the correctly-typed default
// =============================================================================

TEST_F(QA_GDB260, AddColumnDefaultWithManyRows) {
    exec_ok("CREATE TABLE t (id INT)");
    for (int i = 0; i < 100; ++i) {
        exec_ok("INSERT INTO t VALUES (" + std::to_string(i) + ")");
    }

    exec_ok("ALTER TABLE t ADD COLUMN val INT DEFAULT 77");

    auto select = exec_ok("SELECT id, val FROM t ORDER BY id");
    ASSERT_EQ(select.rows.size(), 100u);
    for (size_t i = 0; i < 100; ++i) {
        EXPECT_EQ(select.rows[i][0].as_int32(), static_cast<int32_t>(i))
            << "row " << i << " id mismatch";
        EXPECT_EQ(select.rows[i][1].as_int32(), 77) << "row " << i << " val mismatch";
    }
}

// =============================================================================
// Mixed column types — table with VARCHAR columns (the "slack space" scenario)
// =============================================================================

TEST_F(QA_GDB260, AddColumnDefaultWithVarcharColumns) {
    // The bug report noted that VARCHAR columns can mask the overflow because the
    // variable-length offset table provides slack space. Verify correctness when
    // the table has both fixed and variable-length columns.
    exec_ok("CREATE TABLE t (id INT, name VARCHAR, info VARCHAR)");
    exec_ok("INSERT INTO t VALUES (1, 'alice', 'description1')");
    exec_ok("INSERT INTO t VALUES (2, 'bob', 'description2')");

    exec_ok("ALTER TABLE t ADD COLUMN score INT DEFAULT 100");

    auto select = exec_ok("SELECT id, name, info, score FROM t ORDER BY id");
    ASSERT_EQ(select.rows.size(), 2u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1);
    EXPECT_EQ(select.rows[0][1].as_string(), "alice");
    EXPECT_EQ(select.rows[0][2].as_string(), "description1");
    EXPECT_EQ(select.rows[0][3].as_int32(), 100);
    EXPECT_EQ(select.rows[1][0].as_int32(), 2);
    EXPECT_EQ(select.rows[1][1].as_string(), "bob");
    EXPECT_EQ(select.rows[1][2].as_string(), "description2");
    EXPECT_EQ(select.rows[1][3].as_int32(), 100);
}

// =============================================================================
// Boundary values — extreme integer defaults
// =============================================================================

TEST_F(QA_GDB260, AddColumnDefaultZero) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("ALTER TABLE t ADD COLUMN z INT DEFAULT 0");

    auto select = exec_ok("SELECT z FROM t");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 0);
}

TEST_F(QA_GDB260, AddColumnDefaultNegative) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("ALTER TABLE t ADD COLUMN neg INT DEFAULT -1");

    auto select = exec_ok("SELECT neg FROM t");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int32(), -1);
}

TEST_F(QA_GDB260, AddColumnDefaultSmallintBoundary) {
    // SMALLINT max is 32767. Test at the boundary.
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("ALTER TABLE t ADD COLUMN sm SMALLINT DEFAULT 32767");

    auto select = exec_ok("SELECT sm FROM t");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int16(), 32767);
}

TEST_F(QA_GDB260, AddColumnDefaultTinyintNegBoundary) {
    // TINYINT min is -128. Test at the boundary.
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("ALTER TABLE t ADD COLUMN tiny TINYINT DEFAULT -128");

    auto select = exec_ok("SELECT tiny FROM t");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int8(), -128);
}

// =============================================================================
// NULL default — ensure NULL still works after the fix
// =============================================================================

TEST_F(QA_GDB260, AddColumnNullableNoDefault) {
    // Adding a nullable column without DEFAULT should use NULL.
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("ALTER TABLE t ADD COLUMN opt INT");

    auto select = exec_ok("SELECT opt FROM t");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_TRUE(select.rows[0][0].is_null());
}

// =============================================================================
// Sequential ADD + INSERT — ensure new inserts work correctly after migration
// =============================================================================

TEST_F(QA_GDB260, InsertAfterAddColumnDefault) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("ALTER TABLE t ADD COLUMN x INT DEFAULT 50");

    // Insert a new row after migration.
    exec_ok("INSERT INTO t VALUES (2, 99)");

    auto select = exec_ok("SELECT id, x FROM t ORDER BY id");
    ASSERT_EQ(select.rows.size(), 2u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1);
    EXPECT_EQ(select.rows[0][1].as_int32(), 50); // migrated default
    EXPECT_EQ(select.rows[1][0].as_int32(), 2);
    EXPECT_EQ(select.rows[1][1].as_int32(), 99); // explicitly inserted
}

// =============================================================================
// Sequential ADD COLUMN operations with different types
// =============================================================================

TEST_F(QA_GDB260, AddMultipleColumnsOfDifferentTypes) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("INSERT INTO t VALUES (2)");

    // Add columns of different types, each requiring different coercion paths.
    exec_ok("ALTER TABLE t ADD COLUMN a TINYINT DEFAULT 10");
    exec_ok("ALTER TABLE t ADD COLUMN b SMALLINT DEFAULT 200");
    exec_ok("ALTER TABLE t ADD COLUMN c INT DEFAULT 30000");
    exec_ok("ALTER TABLE t ADD COLUMN d BIGINT DEFAULT 5000000000");
    exec_ok("ALTER TABLE t ADD COLUMN e FLOAT DEFAULT 2");
    exec_ok("ALTER TABLE t ADD COLUMN f DOUBLE DEFAULT 3");
    exec_ok("ALTER TABLE t ADD COLUMN g VARCHAR DEFAULT 'hello'");

    auto select = exec_ok("SELECT a, b, c, d, e, f, g FROM t ORDER BY id");
    ASSERT_EQ(select.rows.size(), 2u);

    // Check row 0.
    EXPECT_EQ(select.rows[0][0].as_int8(), 10);
    EXPECT_EQ(select.rows[0][1].as_int16(), 200);
    EXPECT_EQ(select.rows[0][2].as_int32(), 30000);
    EXPECT_EQ(select.rows[0][3].as_int64(), 5000000000);
    EXPECT_FLOAT_EQ(select.rows[0][4].as_float32(), 2.0f);
    EXPECT_DOUBLE_EQ(select.rows[0][5].as_float64(), 3.0);
    EXPECT_EQ(select.rows[0][6].as_string(), "hello");

    // Check row 1 — same defaults.
    EXPECT_EQ(select.rows[1][0].as_int8(), 10);
    EXPECT_EQ(select.rows[1][1].as_int16(), 200);
    EXPECT_EQ(select.rows[1][2].as_int32(), 30000);
    EXPECT_EQ(select.rows[1][3].as_int64(), 5000000000);
    EXPECT_FLOAT_EQ(select.rows[1][4].as_float32(), 2.0f);
    EXPECT_DOUBLE_EQ(select.rows[1][5].as_float64(), 3.0);
    EXPECT_EQ(select.rows[1][6].as_string(), "hello");
}

// =============================================================================
// UPDATE after ADD COLUMN with DEFAULT — ensure coerced column works in DML
// =============================================================================

TEST_F(QA_GDB260, UpdateAfterAddColumnWithDefault) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("INSERT INTO t VALUES (2)");

    exec_ok("ALTER TABLE t ADD COLUMN score INT DEFAULT 0");

    // Update a specific row's new column.
    exec_ok("UPDATE t SET score = 100 WHERE id = 1");

    auto select = exec_ok("SELECT id, score FROM t ORDER BY id");
    ASSERT_EQ(select.rows.size(), 2u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1);
    EXPECT_EQ(select.rows[0][1].as_int32(), 100); // updated
    EXPECT_EQ(select.rows[1][0].as_int32(), 2);
    EXPECT_EQ(select.rows[1][1].as_int32(), 0); // original default
}

// =============================================================================
// DELETE after ADD COLUMN with DEFAULT — ensure migrated data is queryable
// =============================================================================

TEST_F(QA_GDB260, DeleteAfterAddColumnWithDefault) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("INSERT INTO t VALUES (2)");
    exec_ok("INSERT INTO t VALUES (3)");

    exec_ok("ALTER TABLE t ADD COLUMN x INT DEFAULT 42");

    exec_ok("DELETE FROM t WHERE id = 2");

    auto select = exec_ok("SELECT id, x FROM t ORDER BY id");
    ASSERT_EQ(select.rows.size(), 2u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1);
    EXPECT_EQ(select.rows[0][1].as_int32(), 42);
    EXPECT_EQ(select.rows[1][0].as_int32(), 3);
    EXPECT_EQ(select.rows[1][1].as_int32(), 42);
}

// =============================================================================
// Stress: Repeated ADD COLUMN DEFAULT on many rows — the worst-case scenario
// =============================================================================

TEST_F(QA_GDB260, StressRepeatedAddColumnOnManyRows) {
    exec_ok("CREATE TABLE t (id INT)");
    for (int i = 0; i < 50; ++i) {
        exec_ok("INSERT INTO t VALUES (" + std::to_string(i) + ")");
    }

    // Add 10 INT columns each with a DEFAULT — each migration touches 50 rows.
    for (int c = 0; c < 10; ++c) {
        exec_ok("ALTER TABLE t ADD COLUMN c" + std::to_string(c) + " INT DEFAULT " +
                std::to_string(c + 1));
    }

    // Verify a subset of results.
    auto select = exec_ok("SELECT id, c0, c4, c9 FROM t ORDER BY id");
    ASSERT_EQ(select.rows.size(), 50u);
    EXPECT_EQ(select.rows[0][1].as_int32(), 1);  // c0
    EXPECT_EQ(select.rows[0][2].as_int32(), 5);  // c4
    EXPECT_EQ(select.rows[0][3].as_int32(), 10); // c9
    EXPECT_EQ(select.rows[49][1].as_int32(), 1);
    EXPECT_EQ(select.rows[49][2].as_int32(), 5);
    EXPECT_EQ(select.rows[49][3].as_int32(), 10);
}

// =============================================================================
// String DEFAULT on INT column — should produce a type error
// =============================================================================

TEST_F(QA_GDB260, AddColumnIntWithStringDefault) {
    // A STRING default value cannot be fit to INT32 — should produce an error.
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");

    auto result = engine_->execute("ALTER TABLE t ADD COLUMN bad INT DEFAULT 'hello'");
    EXPECT_FALSE(result.has_value()) << "INT column with STRING default should fail";
}

// =============================================================================
// NOT NULL + DEFAULT on multiple types — exercising the constraint path
// =============================================================================

TEST_F(QA_GDB260, AddNotNullSmallintWithDefault) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");

    exec_ok("ALTER TABLE t ADD COLUMN sm SMALLINT NOT NULL DEFAULT 500");

    auto select = exec_ok("SELECT sm FROM t");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int16(), 500);
}

TEST_F(QA_GDB260, AddNotNullTinyintWithDefault) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");

    exec_ok("ALTER TABLE t ADD COLUMN tiny TINYINT NOT NULL DEFAULT 99");

    auto select = exec_ok("SELECT tiny FROM t");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int8(), 99);
}

// =============================================================================
// Empty table — no migration needed but coercion should still be wired up
// =============================================================================

TEST_F(QA_GDB260, AddColumnDefaultOnEmptyTable) {
    exec_ok("CREATE TABLE t (id INT)");
    // No rows — migration loop doesn't execute, but schema must be updated.
    exec_ok("ALTER TABLE t ADD COLUMN x INT DEFAULT 42");

    // Insert a row that doesn't provide x — should use the schema default.
    exec_ok("INSERT INTO t (id) VALUES (1)");

    auto select = exec_ok("SELECT id, x FROM t");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1);
    EXPECT_EQ(select.rows[0][1].as_int32(), 42);
}

// =============================================================================
// WHERE clause on migrated default column
// =============================================================================

TEST_F(QA_GDB260, WhereOnMigratedDefaultColumn) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("INSERT INTO t VALUES (2)");
    exec_ok("INSERT INTO t VALUES (3)");

    exec_ok("ALTER TABLE t ADD COLUMN flag INT DEFAULT 1");

    // Query using the migrated column in WHERE.
    auto select = exec_ok("SELECT id FROM t WHERE flag = 1 ORDER BY id");
    ASSERT_EQ(select.rows.size(), 3u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1);
    EXPECT_EQ(select.rows[1][0].as_int32(), 2);
    EXPECT_EQ(select.rows[2][0].as_int32(), 3);
}

// =============================================================================
// Expression DEFAULT (not just literal) — e.g. DEFAULT 1 + 1
// =============================================================================

TEST_F(QA_GDB260, AddColumnDefaultExpression) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");

    // Default is an expression, not a simple literal.
    exec_ok("ALTER TABLE t ADD COLUMN computed INT DEFAULT 1 + 1");

    auto select = exec_ok("SELECT computed FROM t");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 2);
}

// =============================================================================
// ADD COLUMN followed by DROP COLUMN followed by ADD COLUMN — schema churn
// =============================================================================

TEST_F(QA_GDB260, AddDropAddColumnWithDefault) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");

    exec_ok("ALTER TABLE t ADD COLUMN a INT DEFAULT 10");
    exec_ok("ALTER TABLE t DROP COLUMN a");
    exec_ok("ALTER TABLE t ADD COLUMN b INT DEFAULT 20");

    auto select = exec_ok("SELECT id, b FROM t");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1);
    EXPECT_EQ(select.rows[0][1].as_int32(), 20);
}
