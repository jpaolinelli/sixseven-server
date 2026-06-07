#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/result.h"
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
namespace fs = std::filesystem;

// =============================================================================
// Test fixture — full-stack SQL engine with filesystem verification
// =============================================================================

class MultiDatabaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = fs::temp_directory_path() / "sixseven_test_multidb";
        fs::remove_all(data_dir_);
        fs::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        fs::remove_all(data_dir_);
    }

    /// Execute SQL and assert success.
    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return std::move(*result);
    }

    /// Execute SQL on a specific engine and assert success.
    static QueryResult exec_ok_on(QueryEngine& eng, const std::string& sql) {
        auto result = eng.execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return std::move(*result);
    }

    /// Execute SQL and assert failure with the expected status code.
    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, expected);
    }

    /// Execute SQL on a specific engine and assert failure.
    static void exec_error_on(QueryEngine& eng, const std::string& sql, StatusCode expected) {
        auto result = eng.execute(sql);
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, expected);
    }

    /// Switch the default engine to a different database by name.
    void use_database(const std::string& name) {
        auto db = catalog_.get_database(name);
        ASSERT_TRUE(db.has_value()) << "database '" << name << "' not found";
        engine_->set_current_database(db->database_id);
    }

    /// Switch a specific engine to a database by name.
    void use_database_on(QueryEngine& eng, const std::string& name) {
        auto db = catalog_.get_database(name);
        ASSERT_TRUE(db.has_value()) << "database '" << name << "' not found";
        eng.set_current_database(db->database_id);
    }

    /// Get the filesystem path for a database directory.
    fs::path database_dir(database_id_t db_id) const {
        return data_dir_ / "databases" / std::to_string(db_id);
    }

    DiskManager dm_;
    Catalog catalog_;
    fs::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// Database Lifecycle
// =============================================================================

TEST_F(MultiDatabaseTest, CreateDatabaseAppearsInListDatabases) {
    exec_ok("CREATE DATABASE analytics");

    auto databases = catalog_.list_databases();
    ASSERT_GE(databases.size(), 2u); // default + analytics

    bool found = false;
    for (const auto& db : databases) {
        if (db.name == "analytics") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "newly created database should appear in list_databases()";
}

TEST_F(MultiDatabaseTest, DropDatabaseRemovesFromList) {
    exec_ok("CREATE DATABASE tempdb");
    exec_ok("DROP DATABASE tempdb");

    auto databases = catalog_.list_databases();
    for (const auto& db : databases) {
        EXPECT_NE(db.name, "tempdb") << "dropped database should not appear in list_databases()";
    }
}

TEST_F(MultiDatabaseTest, CreateDatabaseIfNotExistsSucceedsSilently) {
    exec_ok("CREATE DATABASE mydb");
    auto qr = exec_ok("CREATE DATABASE IF NOT EXISTS mydb");
    EXPECT_EQ(qr.message, "CREATE DATABASE");

    // Should still have exactly one "mydb".
    auto databases = catalog_.list_databases();
    int count = 0;
    for (const auto& db : databases) {
        if (db.name == "mydb")
            ++count;
    }
    EXPECT_EQ(count, 1);
}

TEST_F(MultiDatabaseTest, DropDatabaseIfExistsOnNonExistentSucceeds) {
    auto qr = exec_ok("DROP DATABASE IF EXISTS nonexistent");
    EXPECT_EQ(qr.message, "DROP DATABASE");
}

TEST_F(MultiDatabaseTest, CannotDropDefaultDatabase) {
    exec_error("DROP DATABASE demo", StatusCode::CONSTRAINT_VIOLATION);

    // Default database should still exist.
    auto db = catalog_.get_database("demo");
    EXPECT_TRUE(db.has_value());
}

// =============================================================================
// Table Isolation
// =============================================================================

TEST_F(MultiDatabaseTest, SameTableNameInDifferentDatabases) {
    // Create 'users' in default database.
    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'default_alice')");

    // Create 'users' in a second database.
    exec_ok("CREATE DATABASE app");
    use_database("app");

    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (2, 'app_bob')");

    // Verify app database sees only its own data.
    auto qr_app = exec_ok("SELECT * FROM users");
    ASSERT_EQ(qr_app.rows.size(), 1u);
    EXPECT_EQ(qr_app.rows[0][0].as_int32(), 2);
    EXPECT_EQ(qr_app.rows[0][1].as_string(), "app_bob");

    // Switch back and verify default database sees only its own data.
    use_database("demo");

    auto qr_default = exec_ok("SELECT * FROM users");
    ASSERT_EQ(qr_default.rows.size(), 1u);
    EXPECT_EQ(qr_default.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr_default.rows[0][1].as_string(), "default_alice");
}

TEST_F(MultiDatabaseTest, InsertIsolationAcrossDatabases) {
    exec_ok("CREATE TABLE counters (id INT, val INT)");
    exec_ok("INSERT INTO counters VALUES (1, 100)");

    exec_ok("CREATE DATABASE metrics");
    use_database("metrics");

    exec_ok("CREATE TABLE counters (id INT, val INT)");
    exec_ok("INSERT INTO counters VALUES (1, 999)");

    // Verify each database has its own data.
    auto qr_metrics = exec_ok("SELECT * FROM counters");
    ASSERT_EQ(qr_metrics.rows.size(), 1u);
    EXPECT_EQ(qr_metrics.rows[0][1].as_int32(), 999);

    use_database("demo");
    auto qr_default = exec_ok("SELECT * FROM counters");
    ASSERT_EQ(qr_default.rows.size(), 1u);
    EXPECT_EQ(qr_default.rows[0][1].as_int32(), 100);
}

TEST_F(MultiDatabaseTest, DropTableInOneDatabaseDoesNotAffectOther) {
    exec_ok("CREATE TABLE shared (id INT)");
    exec_ok("INSERT INTO shared VALUES (1)");

    exec_ok("CREATE DATABASE other");
    use_database("other");

    exec_ok("CREATE TABLE shared (id INT)");
    exec_ok("INSERT INTO shared VALUES (2)");

    // Drop the table only in the 'other' database.
    exec_ok("DROP TABLE shared");
    exec_error("SELECT * FROM shared", StatusCode::NOT_FOUND);

    // Default database table should be untouched.
    use_database("demo");
    auto qr = exec_ok("SELECT * FROM shared");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
}

// =============================================================================
// Cascade Drop
// =============================================================================

TEST_F(MultiDatabaseTest, CascadeDropRemovesAllTablesAndDatabase) {
    exec_ok("CREATE DATABASE warehouse");
    use_database("warehouse");

    exec_ok("CREATE TABLE products (id INT, name VARCHAR)");
    exec_ok("INSERT INTO products VALUES (1, 'widget')");
    exec_ok("CREATE TABLE orders (id INT, product_id INT)");
    exec_ok("INSERT INTO orders VALUES (1, 1)");
    exec_ok("CREATE TABLE inventory (id INT, qty INT)");

    // Switch back so we're not in the database being dropped.
    use_database("demo");

    auto qr = exec_ok("DROP DATABASE warehouse CASCADE");
    EXPECT_EQ(qr.message, "DROP DATABASE");

    // Database should be gone from catalog.
    auto db = catalog_.get_database("warehouse");
    EXPECT_FALSE(db.has_value());
}

TEST_F(MultiDatabaseTest, DropDatabaseWithoutCascadeFailsIfTablesExist) {
    exec_ok("CREATE DATABASE mydb");
    use_database("mydb");

    exec_ok("CREATE TABLE t1 (id INT)");

    use_database("demo");
    exec_error("DROP DATABASE mydb", StatusCode::CONSTRAINT_VIOLATION);

    // Database should still exist.
    auto db = catalog_.get_database("mydb");
    EXPECT_TRUE(db.has_value());
}

TEST_F(MultiDatabaseTest, CascadeDropCleansUpStorageDirectory) {
    exec_ok("CREATE DATABASE storagetest");
    auto db = catalog_.get_database("storagetest");
    ASSERT_TRUE(db.has_value());
    auto db_id = db->database_id;

    use_database("storagetest");
    exec_ok("CREATE TABLE data (id INT, payload VARCHAR)");
    exec_ok("INSERT INTO data VALUES (1, 'test')");

    // Verify the database directory exists on disk.
    EXPECT_TRUE(fs::exists(database_dir(db_id)))
        << "database directory should exist after creating tables";

    use_database("demo");
    exec_ok("DROP DATABASE storagetest CASCADE");

    // Verify the database directory is removed from disk.
    EXPECT_FALSE(fs::exists(database_dir(db_id)))
        << "database directory should be removed after CASCADE drop";
}

// =============================================================================
// Name Resolution
// =============================================================================

TEST_F(MultiDatabaseTest, QueryInDatabaseACannotSeeTablesFromDatabaseB) {
    exec_ok("CREATE DATABASE db_a");
    exec_ok("CREATE DATABASE db_b");

    use_database("db_a");
    exec_ok("CREATE TABLE only_in_a (id INT)");
    exec_ok("INSERT INTO only_in_a VALUES (42)");

    // Switch to db_b and try to access the table from db_a.
    use_database("db_b");
    exec_error("SELECT * FROM only_in_a", StatusCode::NOT_FOUND);
}

TEST_F(MultiDatabaseTest, ErrorMessagesIdentifyDatabaseContext) {
    exec_ok("CREATE DATABASE myapp");
    use_database("myapp");

    auto result = engine_->execute("SELECT * FROM does_not_exist");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
    EXPECT_NE(result.error().message.find("'myapp'"), std::string::npos)
        << "error should name the database context: " << result.error().message;
}

TEST_F(MultiDatabaseTest, DDLTargetsCurrentDatabase) {
    exec_ok("CREATE DATABASE target");
    use_database("target");

    exec_ok("CREATE TABLE local_table (id INT, val VARCHAR)");
    exec_ok("INSERT INTO local_table VALUES (1, 'hello')");

    // Verify the table is only in the 'target' database.
    auto qr = exec_ok("SELECT * FROM local_table");
    EXPECT_EQ(qr.rows.size(), 1u);

    // Default database should not see it.
    use_database("demo");
    exec_error("SELECT * FROM local_table", StatusCode::NOT_FOUND);
}

// =============================================================================
// Storage Verification
// =============================================================================

TEST_F(MultiDatabaseTest, FilesCreatedUnderCorrectDatabaseDirectory) {
    exec_ok("CREATE DATABASE filecheck");
    auto db = catalog_.get_database("filecheck");
    ASSERT_TRUE(db.has_value());
    auto db_id = db->database_id;

    use_database("filecheck");
    exec_ok("CREATE TABLE docs (id INT, body VARCHAR)");

    // The tables subdirectory should exist inside the database directory.
    auto tables_dir = database_dir(db_id) / "tables";
    EXPECT_TRUE(fs::exists(tables_dir)) << "tables directory should exist at: " << tables_dir;

    // At least one .db file should be present.
    bool found_db_file = false;
    for (const auto& entry : fs::directory_iterator(tables_dir)) {
        if (entry.path().extension() == ".db") {
            found_db_file = true;
            break;
        }
    }
    EXPECT_TRUE(found_db_file) << "should find a .db file under: " << tables_dir;
}

TEST_F(MultiDatabaseTest, DefaultDatabaseDirectoryExistsAfterInit) {
    // The default database directory should be created during storage
    // initialization when the first table is created in the default db.
    exec_ok("CREATE TABLE probe (id INT)");

    auto default_dir = database_dir(default_database_id);
    EXPECT_TRUE(fs::exists(default_dir))
        << "default database directory should exist at: " << default_dir;
}

// =============================================================================
// Multi-connection simulation (multiple QueryEngine instances)
// =============================================================================

TEST_F(MultiDatabaseTest, TwoConnectionsDifferentDatabases) {
    // Create a second database.
    exec_ok("CREATE DATABASE conn_b_db");

    // Simulate a second connection sharing the same catalog and storage.
    QueryEngine engine_b(catalog_, *storage_);

    // engine_ (connection A) stays in default db.
    // engine_b (connection B) switches to conn_b_db.
    use_database_on(engine_b, "conn_b_db");

    // Connection A creates a table.
    exec_ok("CREATE TABLE conn_a_table (id INT, msg VARCHAR)");
    exec_ok("INSERT INTO conn_a_table VALUES (1, 'from_a')");

    // Connection B creates a same-named table.
    exec_ok_on(engine_b, "CREATE TABLE conn_a_table (id INT, msg VARCHAR)");
    exec_ok_on(engine_b, "INSERT INTO conn_a_table VALUES (2, 'from_b')");

    // Each connection sees only its own data.
    auto qr_a = exec_ok("SELECT * FROM conn_a_table");
    ASSERT_EQ(qr_a.rows.size(), 1u);
    EXPECT_EQ(qr_a.rows[0][1].as_string(), "from_a");

    auto qr_b = exec_ok_on(engine_b, "SELECT * FROM conn_a_table");
    ASSERT_EQ(qr_b.rows.size(), 1u);
    EXPECT_EQ(qr_b.rows[0][1].as_string(), "from_b");
}

TEST_F(MultiDatabaseTest, ConnectionDoesNotSeeCrossDbTables) {
    exec_ok("CREATE DATABASE isolated");

    QueryEngine engine_b(catalog_, *storage_);
    use_database_on(engine_b, "isolated");

    exec_ok_on(engine_b, "CREATE TABLE secret (id INT)");
    exec_ok_on(engine_b, "INSERT INTO secret VALUES (42)");

    // Default connection cannot see the table in 'isolated'.
    exec_error("SELECT * FROM secret", StatusCode::NOT_FOUND);

    // But the second connection can.
    auto qr = exec_ok_on(engine_b, "SELECT * FROM secret");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 42);
}

// =============================================================================
// End-to-end: Full CRUD lifecycle across databases
// =============================================================================

TEST_F(MultiDatabaseTest, FullLifecycleAcrossTwoDatabases) {
    // Step 1: Create two databases.
    exec_ok("CREATE DATABASE sales");
    exec_ok("CREATE DATABASE inventory");

    // Step 2: Populate 'sales'.
    use_database("sales");
    exec_ok("CREATE TABLE orders (id INT, customer VARCHAR, total INT)");
    exec_ok("INSERT INTO orders VALUES (1, 'acme', 500)");
    exec_ok("INSERT INTO orders VALUES (2, 'globex', 300)");

    // Step 3: Populate 'inventory'.
    use_database("inventory");
    exec_ok("CREATE TABLE stock (id INT, item VARCHAR, qty INT)");
    exec_ok("INSERT INTO stock VALUES (1, 'widget', 100)");
    exec_ok("INSERT INTO stock VALUES (2, 'gizmo', 50)");

    // Step 4: Verify data in 'sales'.
    use_database("sales");
    auto sales_qr = exec_ok("SELECT * FROM orders ORDER BY id");
    ASSERT_EQ(sales_qr.rows.size(), 2u);
    EXPECT_EQ(sales_qr.rows[0][1].as_string(), "acme");
    EXPECT_EQ(sales_qr.rows[1][1].as_string(), "globex");

    // Step 5: Update in 'sales'.
    exec_ok("UPDATE orders SET total = 600 WHERE customer = 'acme'");
    auto updated = exec_ok("SELECT * FROM orders WHERE customer = 'acme'");
    ASSERT_EQ(updated.rows.size(), 1u);
    EXPECT_EQ(updated.rows[0][2].as_int32(), 600);

    // Step 6: Verify 'inventory' is unaffected.
    use_database("inventory");
    auto inv_qr = exec_ok("SELECT * FROM stock ORDER BY id");
    ASSERT_EQ(inv_qr.rows.size(), 2u);
    EXPECT_EQ(inv_qr.rows[0][2].as_int32(), 100);

    // Step 7: Delete from 'inventory'.
    exec_ok("DELETE FROM stock WHERE item = 'gizmo'");
    auto remaining = exec_ok("SELECT * FROM stock");
    ASSERT_EQ(remaining.rows.size(), 1u);
    EXPECT_EQ(remaining.rows[0][1].as_string(), "widget");

    // Step 8: Verify 'sales' is unaffected by inventory delete.
    use_database("sales");
    auto sales_check = exec_ok("SELECT * FROM orders");
    EXPECT_EQ(sales_check.rows.size(), 2u);

    // Step 9: Drop 'inventory' database with cascade.
    use_database("demo");
    exec_ok("DROP DATABASE inventory CASCADE");

    // Step 10: Verify 'sales' is still intact.
    use_database("sales");
    auto final_sales = exec_ok("SELECT * FROM orders");
    EXPECT_EQ(final_sales.rows.size(), 2u);

    // Step 11: Verify 'inventory' is gone.
    auto inv_db = catalog_.get_database("inventory");
    EXPECT_FALSE(inv_db.has_value());
}
