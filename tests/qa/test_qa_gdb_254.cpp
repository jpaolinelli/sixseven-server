#include "giodb/catalog/catalog.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/query_engine.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

using namespace giodb;

// =============================================================================
// QA Adversarial Test Fixture
// =============================================================================

class QA_AlterTable : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "giodb_test_qa_gdb_254";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
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
        if (!result.has_value()) {
            ADD_FAILURE() << "SQL failed: " << sql << "\n  Error: " << result.error().message;
            return {};
        }
        return std::move(*result);
    }

    /// Execute SQL, assert failure with the expected code.
    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, expected);
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// ADD COLUMN — Boundary & Edge Cases
// =============================================================================

TEST_F(QA_AlterTable, AddColumnCaseInsensitiveDuplicate) {
    exec_ok("CREATE TABLE t (id INT, Name VARCHAR)");
    // Adding column with same name but different case should fail.
    exec_error("ALTER TABLE t ADD COLUMN name VARCHAR", StatusCode::ALREADY_EXISTS);
    exec_error("ALTER TABLE t ADD COLUMN NAME VARCHAR", StatusCode::ALREADY_EXISTS);
}

TEST_F(QA_AlterTable, AddColumnNotNullNoDefaultManyRows) {
    exec_ok("CREATE TABLE t (id INT)");
    // Insert 100 rows.
    for (int i = 0; i < 100; ++i) {
        exec_ok("INSERT INTO t VALUES (" + std::to_string(i) + ")");
    }
    // NOT NULL without DEFAULT should fail on non-empty table.
    exec_error("ALTER TABLE t ADD COLUMN x INT NOT NULL", StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(QA_AlterTable, AddColumnNotNullNoDefaultExactlyTwoRows) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("INSERT INTO t VALUES (2)");
    // NOT NULL without DEFAULT on 2-row table must fail.
    exec_error("ALTER TABLE t ADD COLUMN x INT NOT NULL", StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(QA_AlterTable, AddColumnThenSelectStar) {
    exec_ok("CREATE TABLE t (a INT, b VARCHAR)");
    exec_ok("INSERT INTO t VALUES (1, 'hello')");
    exec_ok("ALTER TABLE t ADD COLUMN c INT");

    auto qr = exec_ok("SELECT * FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    // Should have 3 columns.
    ASSERT_EQ(qr.rows[0].size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "hello");
    EXPECT_TRUE(qr.rows[0][2].is_null());
}

TEST_F(QA_AlterTable, AddMultipleColumnsSequentially) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");

    exec_ok("ALTER TABLE t ADD COLUMN a VARCHAR");
    exec_ok("ALTER TABLE t ADD COLUMN b INT");
    exec_ok("ALTER TABLE t ADD COLUMN c BOOLEAN");

    auto qr = exec_ok("SELECT * FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.rows[0].size(), 4u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_TRUE(qr.rows[0][1].is_null()); // a
    EXPECT_TRUE(qr.rows[0][2].is_null()); // b
    EXPECT_TRUE(qr.rows[0][3].is_null()); // c

    // Insert with all columns.
    exec_ok("INSERT INTO t VALUES (2, 'x', 42, TRUE)");
    auto qr2 = exec_ok("SELECT id, a, b, c FROM t ORDER BY id");
    ASSERT_EQ(qr2.rows.size(), 2u);
    EXPECT_EQ(qr2.rows[1][1].as_string(), "x");
    EXPECT_EQ(qr2.rows[1][2].as_int32(), 42);
    EXPECT_TRUE(qr2.rows[1][3].as_bool());
}

TEST_F(QA_AlterTable, AddColumnWithArithmeticDefault) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("INSERT INTO t VALUES (2)");

    exec_ok("ALTER TABLE t ADD COLUMN x INT DEFAULT 10 + 5");

    auto qr = exec_ok("SELECT id, x FROM t ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 15);
    EXPECT_EQ(qr.rows[1][1].as_int32(), 15);
}

TEST_F(QA_AlterTable, AddColumnDefaultBackfillAllRows) {
    exec_ok("CREATE TABLE t (id INT)");
    // Insert 50 rows.
    for (int i = 0; i < 50; ++i) {
        exec_ok("INSERT INTO t VALUES (" + std::to_string(i) + ")");
    }

    exec_ok("ALTER TABLE t ADD COLUMN tag VARCHAR DEFAULT 'migrated'");

    auto qr = exec_ok("SELECT id, tag FROM t ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 50u);
    for (size_t i = 0; i < 50; ++i) {
        EXPECT_EQ(qr.rows[i][1].as_string(), "migrated") << "Row " << i << " missing default value";
    }
}

TEST_F(QA_AlterTable, AddColumnOnNonExistentTable) {
    exec_error("ALTER TABLE ghost ADD COLUMN x INT", StatusCode::NOT_FOUND);
}

// =============================================================================
// DROP COLUMN — Boundary & Edge Cases
// =============================================================================

TEST_F(QA_AlterTable, DropFirstColumn) {
    exec_ok("CREATE TABLE t (a INT, b VARCHAR, c INT)");
    exec_ok("INSERT INTO t VALUES (1, 'x', 100)");
    exec_ok("INSERT INTO t VALUES (2, 'y', 200)");

    exec_ok("ALTER TABLE t DROP COLUMN a");

    auto qr = exec_ok("SELECT b, c FROM t ORDER BY c");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "x");
    EXPECT_EQ(qr.rows[0][1].as_int32(), 100);
    EXPECT_EQ(qr.rows[1][0].as_string(), "y");
    EXPECT_EQ(qr.rows[1][1].as_int32(), 200);
}

TEST_F(QA_AlterTable, DropFirstColumnThenInsert) {
    exec_ok("CREATE TABLE t (a INT, b VARCHAR, c INT)");
    exec_ok("INSERT INTO t VALUES (1, 'x', 100)");

    exec_ok("ALTER TABLE t DROP COLUMN a");

    // Insert new row with remaining schema.
    exec_ok("INSERT INTO t VALUES ('new', 300)");

    auto qr = exec_ok("SELECT b, c FROM t ORDER BY c");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "x");
    EXPECT_EQ(qr.rows[0][1].as_int32(), 100);
    EXPECT_EQ(qr.rows[1][0].as_string(), "new");
    EXPECT_EQ(qr.rows[1][1].as_int32(), 300);
}

TEST_F(QA_AlterTable, DropColumnCaseInsensitive) {
    exec_ok("CREATE TABLE t (Id INT, Name VARCHAR, Age INT)");
    exec_ok("INSERT INTO t VALUES (1, 'alice', 30)");

    // Drop using different case.
    exec_ok("ALTER TABLE t DROP COLUMN AGE");

    auto qr = exec_ok("SELECT * FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.rows[0].size(), 2u);
}

TEST_F(QA_AlterTable, DropColumnWithNulls) {
    exec_ok("CREATE TABLE t (id INT, name VARCHAR, bio VARCHAR)");
    exec_ok("INSERT INTO t VALUES (1, 'alice', NULL)");
    exec_ok("INSERT INTO t VALUES (2, 'bob', 'hello')");
    exec_ok("INSERT INTO t VALUES (3, 'charlie', NULL)");

    exec_ok("ALTER TABLE t DROP COLUMN bio");

    auto qr = exec_ok("SELECT id, name FROM t ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "alice");
    EXPECT_EQ(qr.rows[1][0].as_int32(), 2);
    EXPECT_EQ(qr.rows[1][1].as_string(), "bob");
    EXPECT_EQ(qr.rows[2][0].as_int32(), 3);
    EXPECT_EQ(qr.rows[2][1].as_string(), "charlie");
}

TEST_F(QA_AlterTable, DropColumnCompositePk) {
    exec_ok("CREATE TABLE t (a INT, b INT, c VARCHAR, PRIMARY KEY (a, b))");
    // Cannot drop a PK column from a composite PK.
    exec_error("ALTER TABLE t DROP COLUMN a", StatusCode::CONSTRAINT_VIOLATION);
    exec_error("ALTER TABLE t DROP COLUMN b", StatusCode::CONSTRAINT_VIOLATION);
    // Dropping non-PK column should succeed.
    exec_ok("ALTER TABLE t DROP COLUMN c");
}

TEST_F(QA_AlterTable, DropColumnThenAddSameName) {
    exec_ok("CREATE TABLE t (a INT, b VARCHAR, c INT)");
    exec_ok("INSERT INTO t VALUES (1, 'x', 100)");

    exec_ok("ALTER TABLE t DROP COLUMN b");
    exec_ok("ALTER TABLE t ADD COLUMN b BOOLEAN");

    exec_ok("INSERT INTO t VALUES (2, 200, TRUE)");

    auto qr = exec_ok("SELECT a, c, b FROM t ORDER BY a");
    ASSERT_EQ(qr.rows.size(), 2u);
    // First row: b was dropped then re-added, so NULL.
    EXPECT_TRUE(qr.rows[0][2].is_null());
    // Second row: new insert with b = TRUE.
    EXPECT_TRUE(qr.rows[1][2].as_bool());
}

TEST_F(QA_AlterTable, DropColumnNotFoundError) {
    exec_ok("CREATE TABLE t (id INT, name VARCHAR)");
    exec_error("ALTER TABLE t DROP COLUMN ghost", StatusCode::NOT_FOUND);
}

TEST_F(QA_AlterTable, DropOnNonExistentTable) {
    exec_error("ALTER TABLE ghost DROP COLUMN x", StatusCode::NOT_FOUND);
}

TEST_F(QA_AlterTable, DropAllColumnsButOneSequentially) {
    exec_ok("CREATE TABLE t (a INT, b VARCHAR, c INT, d BOOLEAN)");
    exec_ok("INSERT INTO t VALUES (1, 'x', 100, TRUE)");

    exec_ok("ALTER TABLE t DROP COLUMN d");
    exec_ok("ALTER TABLE t DROP COLUMN c");
    exec_ok("ALTER TABLE t DROP COLUMN b");
    // Only 'a' remains.
    exec_error("ALTER TABLE t DROP COLUMN a", StatusCode::CONSTRAINT_VIOLATION);

    auto qr = exec_ok("SELECT a FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
}

TEST_F(QA_AlterTable, DropColumnManyRows) {
    exec_ok("CREATE TABLE t (a INT, b VARCHAR, c INT)");
    for (int i = 0; i < 100; ++i) {
        exec_ok("INSERT INTO t VALUES (" + std::to_string(i) + ", 'row" + std::to_string(i) +
                "', " + std::to_string(i * 10) + ")");
    }

    exec_ok("ALTER TABLE t DROP COLUMN b");

    auto qr = exec_ok("SELECT a, c FROM t ORDER BY a");
    ASSERT_EQ(qr.rows.size(), 100u);
    // Spot-check first and last.
    EXPECT_EQ(qr.rows[0][0].as_int32(), 0);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 0);
    EXPECT_EQ(qr.rows[99][0].as_int32(), 99);
    EXPECT_EQ(qr.rows[99][1].as_int32(), 990);
}

// =============================================================================
// RENAME COLUMN — Boundary & Edge Cases
// =============================================================================

TEST_F(QA_AlterTable, RenameColumnCaseInsensitiveSource) {
    exec_ok("CREATE TABLE t (Id INT, Name VARCHAR)");
    exec_ok("INSERT INTO t VALUES (1, 'alice')");

    // Rename using different case for source.
    exec_ok("ALTER TABLE t RENAME COLUMN NAME TO full_name");

    auto qr = exec_ok("SELECT Id, full_name FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "alice");
}

TEST_F(QA_AlterTable, RenameColumnToExistingCaseInsensitive) {
    exec_ok("CREATE TABLE t (a INT, b VARCHAR)");
    // Renaming to same name different case should fail.
    exec_error("ALTER TABLE t RENAME COLUMN a TO B", StatusCode::ALREADY_EXISTS);
    exec_error("ALTER TABLE t RENAME COLUMN a TO b", StatusCode::ALREADY_EXISTS);
}

TEST_F(QA_AlterTable, RenameColumnOldNameQueryFails) {
    exec_ok("CREATE TABLE t (id INT, name VARCHAR)");
    exec_ok("INSERT INTO t VALUES (1, 'alice')");

    exec_ok("ALTER TABLE t RENAME COLUMN name TO full_name");

    // Old name should no longer work.
    exec_error("SELECT name FROM t", StatusCode::NOT_FOUND);
}

TEST_F(QA_AlterTable, RenamePrimaryKeyColumn) {
    exec_ok("CREATE TABLE t (id INT, name VARCHAR, PRIMARY KEY (id))");
    exec_ok("INSERT INTO t VALUES (1, 'alice')");
    exec_ok("INSERT INTO t VALUES (2, 'bob')");

    exec_ok("ALTER TABLE t RENAME COLUMN id TO user_id");

    // Table should still work with new PK column name.
    auto qr = exec_ok("SELECT user_id, name FROM t ORDER BY user_id");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 2);

    // New inserts should also work.
    exec_ok("INSERT INTO t VALUES (3, 'charlie')");
    auto qr2 = exec_ok("SELECT user_id, name FROM t ORDER BY user_id");
    ASSERT_EQ(qr2.rows.size(), 3u);
    EXPECT_EQ(qr2.rows[2][0].as_int32(), 3);
}

TEST_F(QA_AlterTable, RenameCompositePkColumn) {
    exec_ok("CREATE TABLE t (a INT, b INT, c VARCHAR, PRIMARY KEY (a, b))");
    exec_ok("INSERT INTO t VALUES (1, 2, 'x')");

    exec_ok("ALTER TABLE t RENAME COLUMN a TO x");

    // Query should use new name.
    auto qr = exec_ok("SELECT x, b, c FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 2);
    EXPECT_EQ(qr.rows[0][2].as_string(), "x");

    // New inserts should work.
    exec_ok("INSERT INTO t VALUES (3, 4, 'y')");
    auto qr2 = exec_ok("SELECT x, b, c FROM t ORDER BY x");
    ASSERT_EQ(qr2.rows.size(), 2u);
    EXPECT_EQ(qr2.rows[1][0].as_int32(), 3);
}

TEST_F(QA_AlterTable, RenameColumnThenAddOldName) {
    exec_ok("CREATE TABLE t (id INT, name VARCHAR)");
    exec_ok("INSERT INTO t VALUES (1, 'alice')");

    exec_ok("ALTER TABLE t RENAME COLUMN name TO full_name");
    // Old name should now be available.
    exec_ok("ALTER TABLE t ADD COLUMN name INT");

    exec_ok("INSERT INTO t VALUES (2, 'bob', 42)");

    auto qr = exec_ok("SELECT id, full_name, name FROM t ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "alice");
    EXPECT_TRUE(qr.rows[0][2].is_null());
    EXPECT_EQ(qr.rows[1][1].as_string(), "bob");
    EXPECT_EQ(qr.rows[1][2].as_int32(), 42);
}

TEST_F(QA_AlterTable, RenameOnNonExistentTable) {
    exec_error("ALTER TABLE ghost RENAME COLUMN a TO b", StatusCode::NOT_FOUND);
}

TEST_F(QA_AlterTable, RenameNonExistentColumn) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_error("ALTER TABLE t RENAME COLUMN ghost TO new_name", StatusCode::NOT_FOUND);
}

TEST_F(QA_AlterTable, RenameMultipleColumns) {
    exec_ok("CREATE TABLE t (a INT, b VARCHAR, c INT)");
    exec_ok("INSERT INTO t VALUES (1, 'x', 100)");

    exec_ok("ALTER TABLE t RENAME COLUMN a TO alpha");
    exec_ok("ALTER TABLE t RENAME COLUMN b TO beta");
    exec_ok("ALTER TABLE t RENAME COLUMN c TO gamma");

    auto qr = exec_ok("SELECT alpha, beta, gamma FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "x");
    EXPECT_EQ(qr.rows[0][2].as_int32(), 100);
}

// =============================================================================
// Compound & Stress Scenarios
// =============================================================================

TEST_F(QA_AlterTable, AddDropAddSameColumn) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");

    exec_ok("ALTER TABLE t ADD COLUMN x VARCHAR");
    exec_ok("INSERT INTO t VALUES (2, 'hello')");

    exec_ok("ALTER TABLE t DROP COLUMN x");
    exec_ok("ALTER TABLE t ADD COLUMN x INT");

    exec_ok("INSERT INTO t VALUES (3, 42)");

    auto qr = exec_ok("SELECT id, x FROM t ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u);
    // Row 1: x was added as VARCHAR (NULL), dropped, re-added as INT (NULL).
    EXPECT_TRUE(qr.rows[0][1].is_null());
    // Row 2: originally had VARCHAR 'hello', dropped, re-added as INT (NULL).
    EXPECT_TRUE(qr.rows[1][1].is_null());
    // Row 3: new insert with x=42.
    EXPECT_EQ(qr.rows[2][1].as_int32(), 42);
}

TEST_F(QA_AlterTable, AddManyColumns) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");

    // Add 20 columns.
    for (int i = 0; i < 20; ++i) {
        exec_ok("ALTER TABLE t ADD COLUMN col" + std::to_string(i) + " INT DEFAULT " +
                std::to_string(i * 10));
    }

    auto qr = exec_ok("SELECT * FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    // id + 20 columns = 21.
    ASSERT_EQ(qr.rows[0].size(), 21u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    // Verify DEFAULT values.
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(qr.rows[0][static_cast<size_t>(i + 1)].as_int32(), i * 10)
            << "col" << i << " has wrong default";
    }
}

TEST_F(QA_AlterTable, RenameThenDropRenamed) {
    exec_ok("CREATE TABLE t (a INT, b VARCHAR, c INT)");
    exec_ok("INSERT INTO t VALUES (1, 'x', 100)");

    exec_ok("ALTER TABLE t RENAME COLUMN b TO beta");
    exec_ok("ALTER TABLE t DROP COLUMN beta");

    auto qr = exec_ok("SELECT a, c FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 100);
}

TEST_F(QA_AlterTable, DropThenRenameRemaining) {
    exec_ok("CREATE TABLE t (a INT, b VARCHAR, c INT)");
    exec_ok("INSERT INTO t VALUES (1, 'x', 100)");

    exec_ok("ALTER TABLE t DROP COLUMN b");
    exec_ok("ALTER TABLE t RENAME COLUMN c TO value");

    auto qr = exec_ok("SELECT a, value FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 100);
}

TEST_F(QA_AlterTable, AlterEmptyTableAllOperations) {
    exec_ok("CREATE TABLE t (a INT, b VARCHAR)");

    exec_ok("ALTER TABLE t ADD COLUMN c INT");
    exec_ok("ALTER TABLE t RENAME COLUMN b TO beta");
    exec_ok("ALTER TABLE t DROP COLUMN a");

    // Now table has: beta VARCHAR, c INT.
    exec_ok("INSERT INTO t VALUES ('hello', 42)");

    auto qr = exec_ok("SELECT beta, c FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "hello");
    EXPECT_EQ(qr.rows[0][1].as_int32(), 42);
}

TEST_F(QA_AlterTable, AddColumnWithDefaultFalse) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");

    exec_ok("ALTER TABLE t ADD COLUMN active BOOLEAN DEFAULT FALSE");

    auto qr = exec_ok("SELECT id, active FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_FALSE(qr.rows[0][1].as_bool());
}

TEST_F(QA_AlterTable, AddColumnWithDefaultStringExpression) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");

    exec_ok("ALTER TABLE t ADD COLUMN label VARCHAR DEFAULT 'N/A'");

    auto qr = exec_ok("SELECT id, label FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "N/A");
}

// =============================================================================
// WHERE clause with altered columns
// =============================================================================

TEST_F(QA_AlterTable, WhereClauseAfterAddColumn) {
    exec_ok("CREATE TABLE t (id INT, name VARCHAR)");
    exec_ok("INSERT INTO t VALUES (1, 'alice')");
    exec_ok("INSERT INTO t VALUES (2, 'bob')");

    exec_ok("ALTER TABLE t ADD COLUMN active BOOLEAN DEFAULT TRUE");

    // WHERE on the new column.
    auto qr = exec_ok("SELECT id, name FROM t WHERE active = TRUE ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 2);
}

TEST_F(QA_AlterTable, WhereClauseOnNullAddedColumn) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("INSERT INTO t VALUES (2)");

    exec_ok("ALTER TABLE t ADD COLUMN val INT");

    // NULL comparison: WHERE val IS NULL.
    auto qr = exec_ok("SELECT id FROM t WHERE val IS NULL ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 2);
}

TEST_F(QA_AlterTable, UpdateAfterAddColumn) {
    exec_ok("CREATE TABLE t (id INT, name VARCHAR)");
    exec_ok("INSERT INTO t VALUES (1, 'alice')");
    exec_ok("INSERT INTO t VALUES (2, 'bob')");

    exec_ok("ALTER TABLE t ADD COLUMN score INT DEFAULT 0");

    exec_ok("UPDATE t SET score = 100 WHERE id = 1");

    auto qr = exec_ok("SELECT id, name, score FROM t ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][2].as_int32(), 100);
    EXPECT_EQ(qr.rows[1][2].as_int32(), 0);
}

TEST_F(QA_AlterTable, DeleteAfterDropColumn) {
    exec_ok("CREATE TABLE t (id INT, name VARCHAR, age INT)");
    exec_ok("INSERT INTO t VALUES (1, 'alice', 30)");
    exec_ok("INSERT INTO t VALUES (2, 'bob', 25)");
    exec_ok("INSERT INTO t VALUES (3, 'charlie', 35)");

    exec_ok("ALTER TABLE t DROP COLUMN age");

    exec_ok("DELETE FROM t WHERE id = 2");

    auto qr = exec_ok("SELECT id, name FROM t ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "alice");
    EXPECT_EQ(qr.rows[1][0].as_int32(), 3);
    EXPECT_EQ(qr.rows[1][1].as_string(), "charlie");
}

// =============================================================================
// Deserialization resilience — silent skip on bad data
// =============================================================================
// Note: We cannot force deserialization failure in a black-box test, but we can
// verify that migration preserves data integrity by running multiple ADD/DROP
// cycles and verifying data survives each migration.

TEST_F(QA_AlterTable, MigrationIntegrityMultipleCycles) {
    exec_ok("CREATE TABLE t (id INT, name VARCHAR)");
    for (int i = 0; i < 10; ++i) {
        exec_ok("INSERT INTO t VALUES (" + std::to_string(i) + ", 'name" + std::to_string(i) +
                "')");
    }

    // Cycle: add column, verify, drop column, verify.
    for (int cycle = 0; cycle < 5; ++cycle) {
        std::string col = "extra" + std::to_string(cycle);
        exec_ok("ALTER TABLE t ADD COLUMN " + col + " INT DEFAULT " + std::to_string(cycle));

        auto qr = exec_ok("SELECT * FROM t ORDER BY id");
        ASSERT_EQ(qr.rows.size(), 10u) << "Cycle " << cycle << " ADD: row count mismatch";

        exec_ok("ALTER TABLE t DROP COLUMN " + col);

        auto qr2 = exec_ok("SELECT id, name FROM t ORDER BY id");
        ASSERT_EQ(qr2.rows.size(), 10u) << "Cycle " << cycle << " DROP: row count mismatch";
        // Spot-check data integrity.
        EXPECT_EQ(qr2.rows[0][0].as_int32(), 0);
        EXPECT_EQ(qr2.rows[0][1].as_string(), "name0");
        EXPECT_EQ(qr2.rows[9][0].as_int32(), 9);
        EXPECT_EQ(qr2.rows[9][1].as_string(), "name9");
    }
}
