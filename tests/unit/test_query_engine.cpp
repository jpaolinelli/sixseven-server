#include "giodb/executor/query_engine.h"

#include "giodb/catalog/catalog.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace giodb;

// =============================================================================
// Test fixture
// =============================================================================

class QueryEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ =
            std::filesystem::temp_directory_path() / "giodb_test_qe";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_ / "tables");

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

    /// Helper: create the standard test table.
    void create_test_table() {
        exec_ok("CREATE TABLE users (id INT, name VARCHAR, age INT)");
    }

    /// Helper: insert standard test data (3 rows).
    void insert_test_data() {
        exec_ok("INSERT INTO users VALUES (1, 'alice', 30)");
        exec_ok("INSERT INTO users VALUES (2, 'bob', 25)");
        exec_ok("INSERT INTO users VALUES (3, 'charlie', 35)");
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// DDL tests
// =============================================================================

TEST_F(QueryEngineTest, CreateTable) {
    auto qr = exec_ok("CREATE TABLE users (id INT, name VARCHAR, age INT)");
    EXPECT_EQ(qr.message, "CREATE TABLE");
}

TEST_F(QueryEngineTest, CreateTableIfNotExists) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    // Second CREATE with IF NOT EXISTS should succeed silently.
    auto qr =
        exec_ok("CREATE TABLE IF NOT EXISTS users (id INT, name VARCHAR)");
    EXPECT_EQ(qr.message, "CREATE TABLE");
}

TEST_F(QueryEngineTest, CreateTableDuplicateError) {
    exec_ok("CREATE TABLE users (id INT)");
    exec_error("CREATE TABLE users (id INT)", StatusCode::ALREADY_EXISTS);
}

TEST_F(QueryEngineTest, DropTable) {
    create_test_table();
    auto qr = exec_ok("DROP TABLE users");
    EXPECT_EQ(qr.message, "DROP TABLE");
    // Table should no longer exist.
    exec_error("SELECT * FROM users", StatusCode::NOT_FOUND);
}

TEST_F(QueryEngineTest, DropTableIfExists) {
    // DROP on non-existent table with IF EXISTS should succeed.
    auto qr = exec_ok("DROP TABLE IF EXISTS nonexistent");
    EXPECT_EQ(qr.message, "DROP TABLE");
}

TEST_F(QueryEngineTest, DropTableNotFound) {
    exec_error("DROP TABLE nonexistent", StatusCode::NOT_FOUND);
}

// =============================================================================
// INSERT tests
// =============================================================================

TEST_F(QueryEngineTest, InsertSingleRow) {
    create_test_table();
    auto qr = exec_ok("INSERT INTO users VALUES (1, 'alice', 30)");
    EXPECT_EQ(qr.affected_rows, 1);
}

TEST_F(QueryEngineTest, InsertMultipleRows) {
    create_test_table();
    exec_ok("INSERT INTO users VALUES (1, 'alice', 30)");
    exec_ok("INSERT INTO users VALUES (2, 'bob', 25)");
    exec_ok("INSERT INTO users VALUES (3, 'charlie', 35)");

    auto qr = exec_ok("SELECT * FROM users");
    EXPECT_EQ(qr.rows.size(), 3u);
}

TEST_F(QueryEngineTest, InsertAndSelect) {
    create_test_table();
    exec_ok("INSERT INTO users VALUES (1, 'alice', 30)");

    auto qr = exec_ok("SELECT * FROM users");
    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.column_names.size(), 3u);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "name");
    EXPECT_EQ(qr.column_names[2], "age");
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "alice");
    EXPECT_EQ(qr.rows[0][2].as_int32(), 30);
}

// =============================================================================
// SELECT tests
// =============================================================================

TEST_F(QueryEngineTest, SelectAll) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT * FROM users");
    EXPECT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.column_names.size(), 3u);
    EXPECT_EQ(qr.affected_rows, -1); // This is a query, not DML.
}

TEST_F(QueryEngineTest, SelectWithWhere) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT * FROM users WHERE age > 28");
    EXPECT_EQ(qr.rows.size(), 2u);
    // alice (30) and charlie (35).
}

TEST_F(QueryEngineTest, SelectSpecificColumns) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT name, age FROM users");
    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "name");
    EXPECT_EQ(qr.column_names[1], "age");
    EXPECT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
}

TEST_F(QueryEngineTest, SelectWithOrderBy) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT * FROM users ORDER BY age DESC");
    ASSERT_EQ(qr.rows.size(), 3u);
    // charlie(35), alice(30), bob(25)
    EXPECT_EQ(qr.rows[0][2].as_int32(), 35);
    EXPECT_EQ(qr.rows[1][2].as_int32(), 30);
    EXPECT_EQ(qr.rows[2][2].as_int32(), 25);
}

TEST_F(QueryEngineTest, SelectWithLimit) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT * FROM users LIMIT 2");
    EXPECT_EQ(qr.rows.size(), 2u);
}

TEST_F(QueryEngineTest, SelectWithLimitOffset) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT * FROM users ORDER BY id LIMIT 2 OFFSET 1");
    ASSERT_EQ(qr.rows.size(), 2u);
    // Skipped id=1, got id=2 and id=3.
    EXPECT_EQ(qr.rows[0][0].as_int32(), 2);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 3);
}

TEST_F(QueryEngineTest, SelectOrderByAndLimit) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("SELECT * FROM users ORDER BY age LIMIT 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "bob"); // youngest
}

TEST_F(QueryEngineTest, SelectEmpty) {
    create_test_table();

    auto qr = exec_ok("SELECT * FROM users");
    EXPECT_EQ(qr.rows.size(), 0u);
    EXPECT_EQ(qr.column_names.size(), 3u);
}

// =============================================================================
// UPDATE tests
// =============================================================================

TEST_F(QueryEngineTest, UpdateAllRows) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("UPDATE users SET age = 99");
    EXPECT_EQ(qr.affected_rows, 3);
}

TEST_F(QueryEngineTest, UpdateWithWhere) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("UPDATE users SET age = 99 WHERE name = 'alice'");
    EXPECT_EQ(qr.affected_rows, 1);
}

TEST_F(QueryEngineTest, UpdateAndSelectVerify) {
    create_test_table();
    insert_test_data();

    exec_ok("UPDATE users SET age = 50 WHERE name = 'bob'");

    auto qr = exec_ok("SELECT * FROM users WHERE name = 'bob'");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][2].as_int32(), 50);
}

// =============================================================================
// DELETE tests
// =============================================================================

TEST_F(QueryEngineTest, DeleteAllRows) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("DELETE FROM users");
    EXPECT_EQ(qr.affected_rows, 3);
}

TEST_F(QueryEngineTest, DeleteWithWhere) {
    create_test_table();
    insert_test_data();

    auto qr = exec_ok("DELETE FROM users WHERE age < 30");
    EXPECT_EQ(qr.affected_rows, 1); // Only bob (25).
}

TEST_F(QueryEngineTest, DeleteAndSelectVerify) {
    create_test_table();
    insert_test_data();

    exec_ok("DELETE FROM users WHERE name = 'charlie'");

    auto qr = exec_ok("SELECT * FROM users");
    EXPECT_EQ(qr.rows.size(), 2u);
}

// =============================================================================
// Integration / pipeline tests
// =============================================================================

TEST_F(QueryEngineTest, FullCRUDPipeline) {
    // CREATE TABLE
    exec_ok("CREATE TABLE items (id INT, name VARCHAR, price INT)");

    // INSERT
    auto ins = exec_ok("INSERT INTO items VALUES (1, 'widget', 100)");
    EXPECT_EQ(ins.affected_rows, 1);

    exec_ok("INSERT INTO items VALUES (2, 'gadget', 200)");
    exec_ok("INSERT INTO items VALUES (3, 'doohickey', 50)");

    // SELECT all
    auto sel = exec_ok("SELECT * FROM items");
    EXPECT_EQ(sel.rows.size(), 3u);

    // UPDATE
    auto upd = exec_ok("UPDATE items SET price = 150 WHERE name = 'widget'");
    EXPECT_EQ(upd.affected_rows, 1);

    // Verify update
    auto check = exec_ok("SELECT * FROM items WHERE name = 'widget'");
    ASSERT_EQ(check.rows.size(), 1u);
    EXPECT_EQ(check.rows[0][2].as_int32(), 150);

    // DELETE
    auto del = exec_ok("DELETE FROM items WHERE price < 100");
    EXPECT_EQ(del.affected_rows, 1);

    // Verify remaining
    auto remaining = exec_ok("SELECT * FROM items ORDER BY id");
    EXPECT_EQ(remaining.rows.size(), 2u);

    // DROP TABLE
    exec_ok("DROP TABLE items");
    exec_error("SELECT * FROM items", StatusCode::NOT_FOUND);
}

TEST_F(QueryEngineTest, ErrorOnNonExistentTable) {
    exec_error("SELECT * FROM does_not_exist", StatusCode::NOT_FOUND);
}
