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
#include <vector>

using namespace giodb;

// =============================================================================
// Test fixture
// =============================================================================

class AlterTableTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "giodb_test_alter_table";
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

    /// Helper: execute SQL, assert success, return the result.
    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return std::move(*result);
    }

    /// Helper: execute SQL, assert failure with the expected code.
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
// ADD COLUMN
// =============================================================================

TEST_F(AlterTableTest, AddColumnEmptyTable) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    auto qr = exec_ok("ALTER TABLE users ADD COLUMN bio VARCHAR");
    EXPECT_EQ(qr.message, "ALTER TABLE");

    // Verify the column exists by inserting a row with the new column.
    exec_ok("INSERT INTO users VALUES (1, 'alice', 'hello')");
    auto select = exec_ok("SELECT id, name, bio FROM users");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1);
    EXPECT_EQ(select.rows[0][1].as_string(), "alice");
    EXPECT_EQ(select.rows[0][2].as_string(), "hello");
}

TEST_F(AlterTableTest, AddColumnWithExistingRows) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO users VALUES (2, 'bob')");

    exec_ok("ALTER TABLE users ADD COLUMN bio VARCHAR");

    // Existing rows should have NULL for the new column.
    auto select = exec_ok("SELECT id, name, bio FROM users ORDER BY id");
    ASSERT_EQ(select.rows.size(), 2u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1);
    EXPECT_EQ(select.rows[0][1].as_string(), "alice");
    EXPECT_TRUE(select.rows[0][2].is_null());
    EXPECT_EQ(select.rows[1][0].as_int32(), 2);
    EXPECT_EQ(select.rows[1][1].as_string(), "bob");
    EXPECT_TRUE(select.rows[1][2].is_null());
}

TEST_F(AlterTableTest, AddColumnNewRowsUseNewSchema) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("ALTER TABLE users ADD COLUMN age INT");

    // New inserts should use all 3 columns.
    exec_ok("INSERT INTO users VALUES (2, 'bob', 25)");

    auto select = exec_ok("SELECT id, name, age FROM users ORDER BY id");
    ASSERT_EQ(select.rows.size(), 2u);
    EXPECT_TRUE(select.rows[0][2].is_null());    // alice - no age
    EXPECT_EQ(select.rows[1][2].as_int32(), 25); // bob - has age
}

TEST_F(AlterTableTest, AddColumnDuplicateError) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    exec_error("ALTER TABLE users ADD COLUMN name VARCHAR", StatusCode::ALREADY_EXISTS);
}

TEST_F(AlterTableTest, AddColumnTableNotFoundError) {
    exec_error("ALTER TABLE nonexistent ADD COLUMN x INT", StatusCode::NOT_FOUND);
}

TEST_F(AlterTableTest, AddColumnNotNullOnNonEmptyTableError) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_error("ALTER TABLE users ADD COLUMN age INT NOT NULL", StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(AlterTableTest, AddColumnNotNullOnEmptyTable) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    // NOT NULL on empty table should succeed.
    auto qr = exec_ok("ALTER TABLE users ADD COLUMN age INT NOT NULL");
    EXPECT_EQ(qr.message, "ALTER TABLE");
}

TEST_F(AlterTableTest, AddColumnNotNullWithDefaultOnNonEmptyTable) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO users VALUES (2, 'bob')");

    // NOT NULL + DEFAULT should succeed and backfill existing rows.
    exec_ok("ALTER TABLE users ADD COLUMN age INT NOT NULL DEFAULT 25");

    auto select = exec_ok("SELECT id, name, age FROM users ORDER BY id");
    ASSERT_EQ(select.rows.size(), 2u);
    EXPECT_EQ(select.rows[0][2].as_int32(), 25);
    EXPECT_EQ(select.rows[1][2].as_int32(), 25);
}

TEST_F(AlterTableTest, AddColumnNullableWithDefault) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("INSERT INTO t VALUES (2)");

    // Nullable column with DEFAULT should also backfill.
    exec_ok("ALTER TABLE t ADD COLUMN label VARCHAR DEFAULT 'unknown'");

    auto select = exec_ok("SELECT id, label FROM t ORDER BY id");
    ASSERT_EQ(select.rows.size(), 2u);
    EXPECT_EQ(select.rows[0][1].as_string(), "unknown");
    EXPECT_EQ(select.rows[1][1].as_string(), "unknown");
}

// =============================================================================
// DROP COLUMN
// =============================================================================

TEST_F(AlterTableTest, DropColumnEmptyTable) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR, age INT)");
    auto qr = exec_ok("ALTER TABLE users DROP COLUMN age");
    EXPECT_EQ(qr.message, "ALTER TABLE");

    // Verify the column is gone.
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    auto select = exec_ok("SELECT id, name FROM users");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1);
    EXPECT_EQ(select.rows[0][1].as_string(), "alice");
}

TEST_F(AlterTableTest, DropColumnWithExistingRows) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR, age INT)");
    exec_ok("INSERT INTO users VALUES (1, 'alice', 30)");
    exec_ok("INSERT INTO users VALUES (2, 'bob', 25)");

    exec_ok("ALTER TABLE users DROP COLUMN age");

    // Remaining columns should be intact.
    auto select = exec_ok("SELECT id, name FROM users ORDER BY id");
    ASSERT_EQ(select.rows.size(), 2u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1);
    EXPECT_EQ(select.rows[0][1].as_string(), "alice");
    EXPECT_EQ(select.rows[1][0].as_int32(), 2);
    EXPECT_EQ(select.rows[1][1].as_string(), "bob");
}

TEST_F(AlterTableTest, DropMiddleColumn) {
    exec_ok("CREATE TABLE t (a INT, b VARCHAR, c INT)");
    exec_ok("INSERT INTO t VALUES (1, 'x', 100)");
    exec_ok("INSERT INTO t VALUES (2, 'y', 200)");

    exec_ok("ALTER TABLE t DROP COLUMN b");

    auto select = exec_ok("SELECT a, c FROM t ORDER BY a");
    ASSERT_EQ(select.rows.size(), 2u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1);
    EXPECT_EQ(select.rows[0][1].as_int32(), 100);
    EXPECT_EQ(select.rows[1][0].as_int32(), 2);
    EXPECT_EQ(select.rows[1][1].as_int32(), 200);
}

TEST_F(AlterTableTest, DropColumnNotFoundError) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    exec_error("ALTER TABLE users DROP COLUMN nonexistent", StatusCode::NOT_FOUND);
}

TEST_F(AlterTableTest, DropColumnLastColumnError) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_error("ALTER TABLE t DROP COLUMN id", StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(AlterTableTest, DropColumnPrimaryKeyError) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR, PRIMARY KEY (id))");
    exec_error("ALTER TABLE users DROP COLUMN id", StatusCode::CONSTRAINT_VIOLATION);
}

// =============================================================================
// RENAME COLUMN
// =============================================================================

TEST_F(AlterTableTest, RenameColumn) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");

    auto qr = exec_ok("ALTER TABLE users RENAME COLUMN name TO full_name");
    EXPECT_EQ(qr.message, "ALTER TABLE");

    // Query should work with new name.
    auto select = exec_ok("SELECT id, full_name FROM users");
    ASSERT_EQ(select.rows.size(), 1u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1);
    EXPECT_EQ(select.rows[0][1].as_string(), "alice");
}

TEST_F(AlterTableTest, RenameColumnNotFoundError) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    exec_error("ALTER TABLE users RENAME COLUMN nonexistent TO new_name", StatusCode::NOT_FOUND);
}

TEST_F(AlterTableTest, RenameColumnDuplicateError) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    exec_error("ALTER TABLE users RENAME COLUMN name TO id", StatusCode::ALREADY_EXISTS);
}

// =============================================================================
// Multiple ALTER operations in sequence
// =============================================================================

TEST_F(AlterTableTest, MultipleAlterOperations) {
    exec_ok("CREATE TABLE t (a INT, b VARCHAR)");
    exec_ok("INSERT INTO t VALUES (1, 'hello')");

    // Add a column.
    exec_ok("ALTER TABLE t ADD COLUMN c INT");

    // Insert with new column.
    exec_ok("INSERT INTO t VALUES (2, 'world', 42)");

    // Rename column.
    exec_ok("ALTER TABLE t RENAME COLUMN b TO message");

    // Verify.
    auto select = exec_ok("SELECT a, message, c FROM t ORDER BY a");
    ASSERT_EQ(select.rows.size(), 2u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1);
    EXPECT_EQ(select.rows[0][1].as_string(), "hello");
    EXPECT_TRUE(select.rows[0][2].is_null());
    EXPECT_EQ(select.rows[1][0].as_int32(), 2);
    EXPECT_EQ(select.rows[1][1].as_string(), "world");
    EXPECT_EQ(select.rows[1][2].as_int32(), 42);
}

TEST_F(AlterTableTest, AddThenDropColumn) {
    exec_ok("CREATE TABLE t (a INT)");
    exec_ok("INSERT INTO t VALUES (1)");

    exec_ok("ALTER TABLE t ADD COLUMN b VARCHAR");
    exec_ok("INSERT INTO t VALUES (2, 'test')");

    exec_ok("ALTER TABLE t DROP COLUMN b");

    auto select = exec_ok("SELECT a FROM t ORDER BY a");
    ASSERT_EQ(select.rows.size(), 2u);
    EXPECT_EQ(select.rows[0][0].as_int32(), 1);
    EXPECT_EQ(select.rows[1][0].as_int32(), 2);
}
