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

using namespace sixseven;

// =============================================================================
// Test fixture for per-connection database context (GDB-184)
// =============================================================================

class DatabaseContextTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_dbctx";
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

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, expected);
    }

    /// Switch engine to a different database by name.
    void use_database(const std::string& name) {
        auto db = catalog_.get_database(name);
        ASSERT_TRUE(db.has_value()) << "database '" << name << "' not found";
        engine_->set_current_database(db->database_id);
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// Default database context
// =============================================================================

TEST_F(DatabaseContextTest, DefaultDatabaseIsSixseven) {
    EXPECT_EQ(engine_->current_database_id(), default_database_id);
}

TEST_F(DatabaseContextTest, DDLUsesDefaultDatabase) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");

    auto qr = exec_ok("SELECT * FROM users");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "alice");
}

// =============================================================================
// set_current_database / current_database_id
// =============================================================================

TEST_F(DatabaseContextTest, SetCurrentDatabase) {
    exec_ok("CREATE DATABASE mydb");
    auto db = catalog_.get_database("mydb");
    ASSERT_TRUE(db.has_value());

    engine_->set_current_database(db->database_id);
    EXPECT_EQ(engine_->current_database_id(), db->database_id);
}

// =============================================================================
// Database-scoped DDL (CREATE TABLE / DROP TABLE)
// =============================================================================

TEST_F(DatabaseContextTest, CreateTableInNonDefaultDatabase) {
    exec_ok("CREATE DATABASE mydb");
    use_database("mydb");

    exec_ok("CREATE TABLE items (id INT, name VARCHAR)");
    exec_ok("INSERT INTO items VALUES (1, 'widget')");

    auto qr = exec_ok("SELECT * FROM items");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "widget");
}

TEST_F(DatabaseContextTest, DropTableInNonDefaultDatabase) {
    exec_ok("CREATE DATABASE mydb");
    use_database("mydb");

    exec_ok("CREATE TABLE items (id INT)");
    exec_ok("DROP TABLE items");

    exec_error("SELECT * FROM items", StatusCode::NOT_FOUND);
}

// =============================================================================
// Cross-database isolation
// =============================================================================

TEST_F(DatabaseContextTest, TablesAreIsolatedAcrossDatabases) {
    // Create table in default database.
    exec_ok("CREATE TABLE shared_name (id INT, source VARCHAR)");
    exec_ok("INSERT INTO shared_name VALUES (1, 'default_db')");

    // Switch to a new database and create a table with the same name.
    exec_ok("CREATE DATABASE otherdb");
    use_database("otherdb");

    exec_ok("CREATE TABLE shared_name (id INT, source VARCHAR)");
    exec_ok("INSERT INTO shared_name VALUES (2, 'other_db')");

    // Query in otherdb should see the otherdb data.
    auto qr = exec_ok("SELECT * FROM shared_name");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 2);
    EXPECT_EQ(qr.rows[0][1].as_string(), "other_db");

    // Switch back to default database — should see default data.
    use_database("sixseven");

    auto qr2 = exec_ok("SELECT * FROM shared_name");
    ASSERT_EQ(qr2.rows.size(), 1u);
    EXPECT_EQ(qr2.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr2.rows[0][1].as_string(), "default_db");
}

TEST_F(DatabaseContextTest, TableNotVisibleFromOtherDatabase) {
    // Create table only in default database.
    exec_ok("CREATE TABLE users (id INT)");

    // Switch to a new database.
    exec_ok("CREATE DATABASE otherdb");
    use_database("otherdb");

    // Table 'users' should not be found in otherdb.
    exec_error("SELECT * FROM users", StatusCode::NOT_FOUND);
}

TEST_F(DatabaseContextTest, InsertIsolatedAcrossDatabases) {
    exec_ok("CREATE TABLE accounts (id INT, balance INT)");
    exec_ok("INSERT INTO accounts VALUES (1, 100)");

    exec_ok("CREATE DATABASE bankdb");
    use_database("bankdb");

    exec_ok("CREATE TABLE accounts (id INT, balance INT)");
    exec_ok("INSERT INTO accounts VALUES (1, 999)");

    // bankdb sees 999.
    auto qr = exec_ok("SELECT * FROM accounts");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 999);

    // default db sees 100.
    use_database("sixseven");
    auto qr2 = exec_ok("SELECT * FROM accounts");
    ASSERT_EQ(qr2.rows.size(), 1u);
    EXPECT_EQ(qr2.rows[0][1].as_int32(), 100);
}

TEST_F(DatabaseContextTest, UpdateIsolatedAcrossDatabases) {
    exec_ok("CREATE TABLE counters (id INT, val INT)");
    exec_ok("INSERT INTO counters VALUES (1, 10)");

    exec_ok("CREATE DATABASE db2");
    use_database("db2");

    exec_ok("CREATE TABLE counters (id INT, val INT)");
    exec_ok("INSERT INTO counters VALUES (1, 20)");

    // Update only in db2.
    exec_ok("UPDATE counters SET val = 99 WHERE id = 1");

    auto qr = exec_ok("SELECT * FROM counters");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 99);

    // Default db should still have 10.
    use_database("sixseven");
    auto qr2 = exec_ok("SELECT * FROM counters");
    ASSERT_EQ(qr2.rows.size(), 1u);
    EXPECT_EQ(qr2.rows[0][1].as_int32(), 10);
}

TEST_F(DatabaseContextTest, DeleteIsolatedAcrossDatabases) {
    exec_ok("CREATE TABLE items (id INT)");
    exec_ok("INSERT INTO items VALUES (1)");
    exec_ok("INSERT INTO items VALUES (2)");

    exec_ok("CREATE DATABASE db2");
    use_database("db2");

    exec_ok("CREATE TABLE items (id INT)");
    exec_ok("INSERT INTO items VALUES (3)");
    exec_ok("INSERT INTO items VALUES (4)");

    // Delete in db2.
    exec_ok("DELETE FROM items WHERE id = 3");

    auto qr = exec_ok("SELECT * FROM items");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 4);

    // Default db should still have both rows.
    use_database("sixseven");
    auto qr2 = exec_ok("SELECT * FROM items");
    ASSERT_EQ(qr2.rows.size(), 2u);
}

// =============================================================================
// Error messages include database name
// =============================================================================

TEST_F(DatabaseContextTest, ErrorMessageIncludesDatabaseName) {
    auto result = engine_->execute("SELECT * FROM nonexistent_table");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
    // Error should reference the database name 'sixseven'.
    EXPECT_NE(result.error().message.find("'sixseven'"), std::string::npos)
        << "Error message should include database name: " << result.error().message;
}

TEST_F(DatabaseContextTest, ErrorMessageIncludesNonDefaultDatabaseName) {
    exec_ok("CREATE DATABASE mydb");
    use_database("mydb");

    auto result = engine_->execute("SELECT * FROM nonexistent_table");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
    // Error should reference the database name 'mydb'.
    EXPECT_NE(result.error().message.find("'mydb'"), std::string::npos)
        << "Error message should include database name: " << result.error().message;
}

// =============================================================================
// Full CRUD in non-default database
// =============================================================================

TEST_F(DatabaseContextTest, FullCRUDInNonDefaultDatabase) {
    exec_ok("CREATE DATABASE appdb");
    use_database("appdb");

    // CREATE TABLE
    exec_ok("CREATE TABLE products (id INT, name VARCHAR, price INT)");

    // INSERT
    auto ins = exec_ok("INSERT INTO products VALUES (1, 'widget', 100)");
    EXPECT_EQ(ins.affected_rows, 1);

    exec_ok("INSERT INTO products VALUES (2, 'gadget', 200)");

    // SELECT
    auto sel = exec_ok("SELECT * FROM products");
    EXPECT_EQ(sel.rows.size(), 2u);

    // UPDATE
    auto upd = exec_ok("UPDATE products SET price = 150 WHERE name = 'widget'");
    EXPECT_EQ(upd.affected_rows, 1);

    auto check = exec_ok("SELECT * FROM products WHERE name = 'widget'");
    ASSERT_EQ(check.rows.size(), 1u);
    EXPECT_EQ(check.rows[0][2].as_int32(), 150);

    // DELETE
    auto del = exec_ok("DELETE FROM products WHERE price > 150");
    EXPECT_EQ(del.affected_rows, 1);

    auto remaining = exec_ok("SELECT * FROM products");
    EXPECT_EQ(remaining.rows.size(), 1u);

    // DROP TABLE
    exec_ok("DROP TABLE products");
    exec_error("SELECT * FROM products", StatusCode::NOT_FOUND);
}

// =============================================================================
// JOIN across tables in the same non-default database
// =============================================================================

TEST_F(DatabaseContextTest, JoinInNonDefaultDatabase) {
    exec_ok("CREATE DATABASE joindb");
    use_database("joindb");

    exec_ok("CREATE TABLE employees (id INT, name VARCHAR, dept_id INT)");
    exec_ok("CREATE TABLE departments (id INT, dept_name VARCHAR)");

    exec_ok("INSERT INTO employees VALUES (1, 'alice', 10)");
    exec_ok("INSERT INTO employees VALUES (2, 'bob', 20)");

    exec_ok("INSERT INTO departments VALUES (10, 'engineering')");
    exec_ok("INSERT INTO departments VALUES (20, 'sales')");

    auto qr = exec_ok("SELECT employees.name, departments.dept_name "
                      "FROM employees JOIN departments ON employees.dept_id = departments.id");

    ASSERT_EQ(qr.rows.size(), 2u);
}

// =============================================================================
// CREATE DATABASE does not change current context
// =============================================================================

TEST_F(DatabaseContextTest, CreateDatabaseDoesNotChangeContext) {
    exec_ok("CREATE TABLE t1 (id INT)");
    exec_ok("INSERT INTO t1 VALUES (1)");

    exec_ok("CREATE DATABASE newdb");

    // We should still be in the default database.
    EXPECT_EQ(engine_->current_database_id(), default_database_id);

    auto qr = exec_ok("SELECT * FROM t1");
    ASSERT_EQ(qr.rows.size(), 1u);
}
