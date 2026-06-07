#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class QueryEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_qe";
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

    /// Helper: create the standard test table.
    void create_test_table() { exec_ok("CREATE TABLE users (id INT, name VARCHAR, age INT)"); }

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
    auto qr = exec_ok("CREATE TABLE IF NOT EXISTS users (id INT, name VARCHAR)");
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
// Database DDL tests
// =============================================================================

TEST_F(QueryEngineTest, CreateDatabase) {
    auto qr = exec_ok("CREATE DATABASE mydb");
    EXPECT_EQ(qr.message, "CREATE DATABASE");
}

TEST_F(QueryEngineTest, CreateDatabaseIfNotExists) {
    exec_ok("CREATE DATABASE mydb");
    // Second CREATE with IF NOT EXISTS should succeed silently.
    auto qr = exec_ok("CREATE DATABASE IF NOT EXISTS mydb");
    EXPECT_EQ(qr.message, "CREATE DATABASE");
}

TEST_F(QueryEngineTest, CreateDatabaseDuplicateError) {
    exec_ok("CREATE DATABASE mydb");
    exec_error("CREATE DATABASE mydb", StatusCode::ALREADY_EXISTS);
}

TEST_F(QueryEngineTest, DropDatabase) {
    exec_ok("CREATE DATABASE mydb");
    auto qr = exec_ok("DROP DATABASE mydb");
    EXPECT_EQ(qr.message, "DROP DATABASE");
    // Database should no longer exist — creating it again should work.
    exec_ok("CREATE DATABASE mydb");
}

TEST_F(QueryEngineTest, DropDatabaseIfExists) {
    // DROP on non-existent database with IF EXISTS should succeed.
    auto qr = exec_ok("DROP DATABASE IF EXISTS nonexistent");
    EXPECT_EQ(qr.message, "DROP DATABASE");
}

TEST_F(QueryEngineTest, DropDatabaseNotFound) {
    exec_error("DROP DATABASE nonexistent", StatusCode::NOT_FOUND);
}

TEST_F(QueryEngineTest, DropDefaultDatabaseSucceeds) {
    auto qr = exec_ok("DROP DATABASE demo");
    EXPECT_EQ(qr.message, "DROP DATABASE");
}

TEST_F(QueryEngineTest, DropDatabaseNotEmptyWithoutCascade) {
    exec_ok("CREATE DATABASE mydb");
    // Add a table to the database via catalog directly so it's non-empty.
    TableSchema ts;
    ts.name = "t1";
    CatalogColumnDef col;
    col.ordinal = 0;
    col.name = "id";
    col.type_id = TypeId::INT32;
    ts.columns.push_back(col);
    auto db = catalog_.get_database("mydb");
    ASSERT_TRUE(db.has_value());
    auto tid = catalog_.create_table(db->database_id, std::move(ts));
    ASSERT_TRUE(tid.has_value());

    exec_error("DROP DATABASE mydb", StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(QueryEngineTest, DropDatabaseCascade) {
    exec_ok("CREATE DATABASE mydb");
    // Add a table to the database via catalog + storage so CASCADE can clean up.
    TableSchema ts;
    ts.name = "t1";
    CatalogColumnDef col;
    col.ordinal = 0;
    col.name = "id";
    col.type_id = TypeId::INT32;
    ts.columns.push_back(col);
    auto db = catalog_.get_database("mydb");
    ASSERT_TRUE(db.has_value());
    auto tid = catalog_.create_table(db->database_id, ts);
    ASSERT_TRUE(tid.has_value());
    auto schema = catalog_.get_table(db->database_id, "t1");
    ASSERT_TRUE(schema.has_value());
    auto storage_result = storage_->create_table_storage(db->database_id, *tid, *schema);
    ASSERT_TRUE(storage_result.has_value());

    auto qr = exec_ok("DROP DATABASE mydb CASCADE");
    EXPECT_EQ(qr.message, "DROP DATABASE");
    // Database should be gone — creating it again should work.
    exec_ok("CREATE DATABASE mydb");
}

TEST(DropDatabaseCascadePersistence, RemovesOrphanedSysTableRows) {
    namespace fs = std::filesystem;
    auto data_dir = fs::temp_directory_path() / "sixseven_test_drop_db_cascade_persist";
    fs::remove_all(data_dir);
    fs::create_directories(data_dir);
    auto config = Config::load_defaults();
    config.data_dir = data_dir.string();

    {
        DiskManager dm;
        Catalog catalog;
        init_test_catalog(catalog);
        StorageManager storage(dm, data_dir);
        CatalogPersistence persistence(catalog, storage);
        QueryEngine engine(catalog, storage);
        engine.set_catalog_persistence(&persistence);

        auto boot =
            SystemBootstrap::bootstrap(engine, catalog, storage, persistence, config, data_dir);
        ASSERT_TRUE(boot.has_value()) << boot.error().message;

        engine.set_current_database(default_database_id);

        auto r1 = engine.execute("CREATE DATABASE testdb");
        ASSERT_TRUE(r1.has_value()) << r1.error().message;

        auto db = catalog.get_database("testdb");
        ASSERT_TRUE(db.has_value());
        engine.set_current_database(db->database_id);

        auto r2 = engine.execute("CREATE TABLE t1 (id INT, name VARCHAR)");
        ASSERT_TRUE(r2.has_value()) << r2.error().message;

        engine.set_current_database(default_database_id);

        auto r3 = engine.execute("DROP DATABASE testdb CASCADE");
        ASSERT_TRUE(r3.has_value()) << r3.error().message;
    }

    // Simulate restart: load_catalog should produce no warnings about orphaned tables.
    {
        DiskManager dm;
        Catalog catalog;
        init_test_catalog(catalog);
        StorageManager storage(dm, data_dir);
        CatalogPersistence persistence(catalog, storage);
        QueryEngine engine(catalog, storage);
        engine.set_catalog_persistence(&persistence);

        auto boot =
            SystemBootstrap::bootstrap(engine, catalog, storage, persistence, config, data_dir);
        ASSERT_TRUE(boot.has_value()) << boot.error().message;

        // The database should not exist after restart.
        auto db = catalog.get_database("testdb");
        EXPECT_FALSE(db.has_value());

        // No table named t1 should exist in any database.
        auto tables = catalog.list_tables(default_database_id);
        for (const auto& t : tables) {
            EXPECT_NE(t.name, "t1") << "orphaned table 't1' found after restart";
        }
    }

    fs::remove_all(data_dir);
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

TEST_F(QueryEngineTest, SelectWithLargeOffsetNoOrderBy) {
    // Tests the OFFSET pushdown into SeqScan (skip rows without deserializing).
    create_test_table();
    insert_test_data(); // inserts 3 rows: id 1, 2, 3

    // OFFSET without ORDER BY — uses the SeqScan skip optimization.
    auto qr = exec_ok("SELECT * FROM users LIMIT 1 OFFSET 2");
    ASSERT_EQ(qr.rows.size(), 1u);
    // Exactly 1 row returned after skipping 2.

    // OFFSET past all rows → empty result.
    auto qr2 = exec_ok("SELECT * FROM users LIMIT 5 OFFSET 10");
    EXPECT_EQ(qr2.rows.size(), 0u);
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

// =============================================================================
// JOIN integration tests (end-to-end SQL through parse → bind → plan → execute)
// =============================================================================

TEST_F(QueryEngineTest, InnerJoin) {
    exec_ok("CREATE TABLE employees (id INT, name VARCHAR, dept_id INT)");
    exec_ok("CREATE TABLE departments (id INT, dept_name VARCHAR)");

    exec_ok("INSERT INTO employees VALUES (1, 'alice', 10)");
    exec_ok("INSERT INTO employees VALUES (2, 'bob', 20)");
    exec_ok("INSERT INTO employees VALUES (3, 'charlie', 30)");

    exec_ok("INSERT INTO departments VALUES (10, 'engineering')");
    exec_ok("INSERT INTO departments VALUES (20, 'sales')");
    exec_ok("INSERT INTO departments VALUES (40, 'hr')");

    auto qr = exec_ok("SELECT employees.name, departments.dept_name "
                      "FROM employees JOIN departments ON employees.dept_id = departments.id");

    // alice(dept_id=10) matches engineering, bob(dept_id=20) matches sales.
    // charlie(dept_id=30) has no match, hr(id=40) has no match.
    ASSERT_EQ(qr.rows.size(), 2u);
    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "name");
    EXPECT_EQ(qr.column_names[1], "dept_name");

    // Verify content (order depends on scan order).
    bool found_alice = false;
    bool found_bob = false;
    for (auto& row : qr.rows) {
        if (row[0].as_string() == "alice") {
            EXPECT_EQ(row[1].as_string(), "engineering");
            found_alice = true;
        }
        if (row[0].as_string() == "bob") {
            EXPECT_EQ(row[1].as_string(), "sales");
            found_bob = true;
        }
    }
    EXPECT_TRUE(found_alice);
    EXPECT_TRUE(found_bob);
}

TEST_F(QueryEngineTest, LeftJoin) {
    exec_ok("CREATE TABLE employees (id INT, name VARCHAR, dept_id INT)");
    exec_ok("CREATE TABLE departments (id INT, dept_name VARCHAR)");

    exec_ok("INSERT INTO employees VALUES (1, 'alice', 10)");
    exec_ok("INSERT INTO employees VALUES (2, 'bob', 20)");
    exec_ok("INSERT INTO employees VALUES (3, 'charlie', 99)");

    exec_ok("INSERT INTO departments VALUES (10, 'engineering')");
    exec_ok("INSERT INTO departments VALUES (20, 'sales')");

    auto qr = exec_ok("SELECT employees.name, departments.dept_name "
                      "FROM employees LEFT JOIN departments ON employees.dept_id = departments.id");

    // All 3 employees should appear. charlie has no matching dept → NULL.
    ASSERT_EQ(qr.rows.size(), 3u);

    bool found_charlie_null = false;
    for (auto& row : qr.rows) {
        if (row[0].as_string() == "charlie") {
            EXPECT_TRUE(row[1].is_null());
            found_charlie_null = true;
        }
    }
    EXPECT_TRUE(found_charlie_null);
}

TEST_F(QueryEngineTest, CrossJoin) {
    exec_ok("CREATE TABLE colors (name VARCHAR)");
    exec_ok("CREATE TABLE sizes (name VARCHAR)");

    exec_ok("INSERT INTO colors VALUES ('red')");
    exec_ok("INSERT INTO colors VALUES ('blue')");

    exec_ok("INSERT INTO sizes VALUES ('S')");
    exec_ok("INSERT INTO sizes VALUES ('M')");
    exec_ok("INSERT INTO sizes VALUES ('L')");

    auto qr = exec_ok("SELECT colors.name, sizes.name "
                      "FROM colors CROSS JOIN sizes");

    // 2 colors x 3 sizes = 6 combinations.
    ASSERT_EQ(qr.rows.size(), 6u);
    ASSERT_EQ(qr.column_names.size(), 2u);
}

TEST_F(QueryEngineTest, JoinWithWhereFilter) {
    exec_ok("CREATE TABLE employees (id INT, name VARCHAR, dept_id INT)");
    exec_ok("CREATE TABLE departments (id INT, dept_name VARCHAR)");

    exec_ok("INSERT INTO employees VALUES (1, 'alice', 10)");
    exec_ok("INSERT INTO employees VALUES (2, 'bob', 20)");

    exec_ok("INSERT INTO departments VALUES (10, 'engineering')");
    exec_ok("INSERT INTO departments VALUES (20, 'sales')");

    auto qr = exec_ok("SELECT employees.name, departments.dept_name "
                      "FROM employees JOIN departments ON employees.dept_id = departments.id "
                      "WHERE departments.dept_name = 'engineering'");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
    EXPECT_EQ(qr.rows[0][1].as_string(), "engineering");
}

TEST_F(QueryEngineTest, JoinWithSelectStar) {
    exec_ok("CREATE TABLE t1 (a INT, b VARCHAR)");
    exec_ok("CREATE TABLE t2 (c INT, d VARCHAR)");

    exec_ok("INSERT INTO t1 VALUES (1, 'x')");
    exec_ok("INSERT INTO t2 VALUES (1, 'y')");

    auto qr = exec_ok("SELECT * FROM t1 JOIN t2 ON t1.a = t2.c");

    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.column_names.size(), 4u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "x");
    EXPECT_EQ(qr.rows[0][2].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][3].as_string(), "y");
}

// =============================================================================
// GROUP BY / Aggregation integration tests (end-to-end SQL pipeline)
// =============================================================================

TEST_F(QueryEngineTest, GroupByCountStar) {
    exec_ok("CREATE TABLE employees (id INT, dept VARCHAR, salary INT)");
    exec_ok("INSERT INTO employees VALUES (1, 'eng', 100)");
    exec_ok("INSERT INTO employees VALUES (2, 'eng', 120)");
    exec_ok("INSERT INTO employees VALUES (3, 'sales', 80)");
    exec_ok("INSERT INTO employees VALUES (4, 'eng', 110)");
    exec_ok("INSERT INTO employees VALUES (5, 'sales', 90)");

    auto qr = exec_ok("SELECT dept, COUNT(*) FROM employees GROUP BY dept");

    ASSERT_EQ(qr.rows.size(), 2u);
    ASSERT_EQ(qr.column_names.size(), 2u);

    // Results are unordered — find each group.
    std::unordered_map<std::string, int64_t> counts;
    for (auto& row : qr.rows) {
        counts[row[0].as_string()] = row[1].as_int64();
    }
    EXPECT_EQ(counts["eng"], 3);
    EXPECT_EQ(counts["sales"], 2);
}

TEST_F(QueryEngineTest, GroupByWithHaving) {
    // This is the exact AC integration test:
    // SELECT dept, COUNT(*) FROM employees GROUP BY dept HAVING COUNT(*) > 1
    exec_ok("CREATE TABLE employees (id INT, dept VARCHAR, salary INT)");
    exec_ok("INSERT INTO employees VALUES (1, 'eng', 100)");
    exec_ok("INSERT INTO employees VALUES (2, 'eng', 120)");
    exec_ok("INSERT INTO employees VALUES (3, 'sales', 80)");
    exec_ok("INSERT INTO employees VALUES (4, 'hr', 70)");

    auto qr = exec_ok("SELECT dept, COUNT(*) FROM employees GROUP BY dept HAVING COUNT(*) > 1");

    // eng has 2, sales and hr have 1 each — only eng passes HAVING.
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "eng");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 2);
}

TEST_F(QueryEngineTest, GroupBySumAvg) {
    exec_ok("CREATE TABLE employees (id INT, dept VARCHAR, salary INT)");
    exec_ok("INSERT INTO employees VALUES (1, 'eng', 100)");
    exec_ok("INSERT INTO employees VALUES (2, 'eng', 200)");
    exec_ok("INSERT INTO employees VALUES (3, 'sales', 150)");

    auto qr = exec_ok("SELECT dept, SUM(salary), AVG(salary) FROM employees GROUP BY dept");

    ASSERT_EQ(qr.rows.size(), 2u);

    for (auto& row : qr.rows) {
        if (row[0].as_string() == "eng") {
            EXPECT_EQ(row[1].as_int64(), 300);            // SUM
            EXPECT_DOUBLE_EQ(row[2].as_float64(), 150.0); // AVG
        } else {
            EXPECT_EQ(row[0].as_string(), "sales");
            EXPECT_EQ(row[1].as_int64(), 150);            // SUM
            EXPECT_DOUBLE_EQ(row[2].as_float64(), 150.0); // AVG
        }
    }
}

TEST_F(QueryEngineTest, GroupByMinMax) {
    exec_ok("CREATE TABLE employees (id INT, dept VARCHAR, salary INT)");
    exec_ok("INSERT INTO employees VALUES (1, 'eng', 100)");
    exec_ok("INSERT INTO employees VALUES (2, 'eng', 200)");
    exec_ok("INSERT INTO employees VALUES (3, 'eng', 150)");

    auto qr = exec_ok("SELECT dept, MIN(salary), MAX(salary) FROM employees GROUP BY dept");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "eng");
    EXPECT_EQ(qr.rows[0][1].as_int32(), 100); // MIN
    EXPECT_EQ(qr.rows[0][2].as_int32(), 200); // MAX
}

TEST_F(QueryEngineTest, GlobalAggregationNoGroupBy) {
    exec_ok("CREATE TABLE nums (val INT)");
    exec_ok("INSERT INTO nums VALUES (10)");
    exec_ok("INSERT INTO nums VALUES (20)");
    exec_ok("INSERT INTO nums VALUES (30)");

    auto qr = exec_ok("SELECT COUNT(*), SUM(val), AVG(val) FROM nums");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int64(), 3);             // COUNT(*)
    EXPECT_EQ(qr.rows[0][1].as_int64(), 60);            // SUM
    EXPECT_DOUBLE_EQ(qr.rows[0][2].as_float64(), 20.0); // AVG
}

// =============================================================================
// ORDER BY alias with HAVING (GDB-603)
// =============================================================================

TEST_F(QueryEngineTest, OrderByAliasWithHaving) {
    exec_ok("CREATE TABLE employees (id INT, department VARCHAR, salary INT)");
    exec_ok("INSERT INTO employees VALUES (1, 'eng', 100)");
    exec_ok("INSERT INTO employees VALUES (2, 'eng', 120)");
    exec_ok("INSERT INTO employees VALUES (3, 'eng', 130)");
    exec_ok("INSERT INTO employees VALUES (4, 'sales', 80)");
    exec_ok("INSERT INTO employees VALUES (5, 'sales', 90)");
    exec_ok("INSERT INTO employees VALUES (6, 'sales', 100)");
    exec_ok("INSERT INTO employees VALUES (7, 'hr', 70)");

    // Without HAVING — alias in ORDER BY should work.
    auto qr1 = exec_ok("SELECT department, COUNT(*) AS cnt, AVG(salary) AS avg_sal "
                       "FROM employees GROUP BY department ORDER BY avg_sal DESC");
    ASSERT_EQ(qr1.rows.size(), 3u);
    // eng avg=116.67, sales avg=90, hr avg=70
    EXPECT_EQ(qr1.rows[0][0].as_string(), "eng");
    EXPECT_EQ(qr1.rows[2][0].as_string(), "hr");

    // With HAVING — alias in ORDER BY should still work (GDB-603 bug).
    auto qr2 =
        exec_ok("SELECT department, COUNT(*) AS cnt, AVG(salary) AS avg_sal "
                "FROM employees GROUP BY department HAVING COUNT(*) > 2 ORDER BY avg_sal DESC");
    ASSERT_EQ(qr2.rows.size(), 2u); // eng (3) and sales (3) pass; hr (1) doesn't.
    EXPECT_EQ(qr2.rows[0][0].as_string(), "eng");
    EXPECT_EQ(qr2.rows[1][0].as_string(), "sales");
}

TEST_F(QueryEngineTest, OrderByCountAliasWithHaving) {
    exec_ok("CREATE TABLE employees2 (id INT, department VARCHAR, salary INT)");
    exec_ok("INSERT INTO employees2 VALUES (1, 'eng', 100)");
    exec_ok("INSERT INTO employees2 VALUES (2, 'eng', 120)");
    exec_ok("INSERT INTO employees2 VALUES (3, 'eng', 130)");
    exec_ok("INSERT INTO employees2 VALUES (4, 'sales', 80)");
    exec_ok("INSERT INTO employees2 VALUES (5, 'sales', 90)");
    exec_ok("INSERT INTO employees2 VALUES (6, 'hr', 70)");

    // ORDER BY COUNT alias with HAVING.
    auto qr = exec_ok("SELECT department, COUNT(*) AS cnt "
                      "FROM employees2 GROUP BY department HAVING COUNT(*) > 1 ORDER BY cnt DESC");
    ASSERT_EQ(qr.rows.size(), 2u); // eng(3) and sales(2) pass; hr(1) doesn't.
    EXPECT_EQ(qr.rows[0][0].as_string(), "eng");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 3);
    EXPECT_EQ(qr.rows[1][0].as_string(), "sales");
    EXPECT_EQ(qr.rows[1][1].as_int64(), 2);
}

TEST_F(QueryEngineTest, OrderByAliasWithoutAggregate) {
    exec_ok("CREATE TABLE items (id INT, name VARCHAR, price INT)");
    exec_ok("INSERT INTO items VALUES (1, 'apple', 3)");
    exec_ok("INSERT INTO items VALUES (2, 'banana', 1)");
    exec_ok("INSERT INTO items VALUES (3, 'cherry', 5)");

    // Non-aggregate alias in ORDER BY.
    auto qr = exec_ok("SELECT name AS item_name, price AS cost FROM items ORDER BY cost ASC");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "banana");
    EXPECT_EQ(qr.rows[1][0].as_string(), "apple");
    EXPECT_EQ(qr.rows[2][0].as_string(), "cherry");
}

TEST_F(QueryEngineTest, OrderByAliasCaseInsensitive) {
    exec_ok("CREATE TABLE vals (id INT, v INT)");
    exec_ok("INSERT INTO vals VALUES (1, 30)");
    exec_ok("INSERT INTO vals VALUES (2, 10)");
    exec_ok("INSERT INTO vals VALUES (3, 20)");

    // Alias case should be insensitive.
    auto qr = exec_ok("SELECT v AS MyVal FROM vals ORDER BY myval ASC");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 10);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 20);
    EXPECT_EQ(qr.rows[2][0].as_int32(), 30);
}

// =============================================================================
// INSERT with DEFAULT expressions (GDB-250)
// =============================================================================

TEST_F(QueryEngineTest, InsertWithDefaultGenUuidAndTimestamp) {
    // AC: INSERT with DEFAULT columns works end-to-end.
    exec_ok("CREATE TABLE users ("
            "  id UUID PRIMARY KEY DEFAULT gen_uuid(),"
            "  name TEXT NOT NULL,"
            "  age INT,"
            "  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
            ")");

    auto before = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();

    auto ins = exec_ok("INSERT INTO users (name, age) VALUES ('Alice', 30)");
    EXPECT_EQ(ins.affected_rows, 1);

    auto after = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();

    auto qr = exec_ok("SELECT id, name, age, created_at FROM users");
    ASSERT_EQ(qr.rows.size(), 1u);

    // id should be a valid UUID (not null).
    EXPECT_FALSE(qr.rows[0][0].is_null());
    EXPECT_EQ(qr.rows[0][0].type_id(), TypeId::UUID);
    const auto& uuid = qr.rows[0][0].as_uuid();
    EXPECT_EQ((uuid[6] >> 4) & 0x0F, 4); // Version 4.

    // name and age should be the provided values.
    EXPECT_EQ(qr.rows[0][1].as_string(), "Alice");
    EXPECT_EQ(qr.rows[0][2].as_int32(), 30);

    // created_at should be a recent timestamp.
    EXPECT_FALSE(qr.rows[0][3].is_null());
    EXPECT_EQ(qr.rows[0][3].type_id(), TypeId::TIMESTAMP);
    auto ts = qr.rows[0][3].as_timestamp().microseconds;
    EXPECT_GE(ts, before);
    EXPECT_LE(ts, after);
}

TEST_F(QueryEngineTest, InsertAllColumnsNoDefaultsApplied) {
    // AC: INSERT without an explicit column list still works (all values provided).
    exec_ok("CREATE TABLE items ("
            "  id INT PRIMARY KEY,"
            "  name TEXT NOT NULL,"
            "  score INT DEFAULT 0"
            ")");

    exec_ok("INSERT INTO items VALUES (1, 'widget', 99)");

    auto qr = exec_ok("SELECT id, name, score FROM items");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "widget");
    // The provided value (99) should be used, not the default (0).
    EXPECT_EQ(qr.rows[0][2].as_int32(), 99);
}

TEST_F(QueryEngineTest, InsertMixOfProvidedAndDefaults) {
    // AC: INSERT with only some defaults works (mix of provided values and defaults).
    exec_ok("CREATE TABLE logs ("
            "  id UUID DEFAULT gen_uuid(),"
            "  msg TEXT NOT NULL,"
            "  level INT DEFAULT 0,"
            "  ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
            ")");

    // Provide msg and level, let id and ts use defaults.
    exec_ok("INSERT INTO logs (msg, level) VALUES ('hello', 1)");

    auto qr = exec_ok("SELECT id, msg, level, ts FROM logs");
    ASSERT_EQ(qr.rows.size(), 1u);

    EXPECT_FALSE(qr.rows[0][0].is_null()); // id defaulted to UUID
    EXPECT_EQ(qr.rows[0][0].type_id(), TypeId::UUID);
    EXPECT_EQ(qr.rows[0][1].as_string(), "hello");
    EXPECT_EQ(qr.rows[0][2].as_int32(), 1); // Provided, not the default 0
    EXPECT_FALSE(qr.rows[0][3].is_null());  // ts defaulted to CURRENT_TIMESTAMP
    EXPECT_EQ(qr.rows[0][3].type_id(), TypeId::TIMESTAMP);
}

TEST_F(QueryEngineTest, InsertNullForNullableColumnWithNoDefault) {
    // AC: NULL is used for nullable columns with no DEFAULT when omitted from INSERT.
    exec_ok("CREATE TABLE profiles ("
            "  id INT PRIMARY KEY,"
            "  name TEXT NOT NULL,"
            "  bio TEXT"
            ")");

    // Omit bio (nullable, no default) — should get NULL.
    exec_ok("INSERT INTO profiles (id, name) VALUES (1, 'Alice')");

    auto qr = exec_ok("SELECT id, name, bio FROM profiles");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "Alice");
    EXPECT_TRUE(qr.rows[0][2].is_null()); // bio should be NULL.
}

TEST_F(QueryEngineTest, InsertErrorForNonNullableColumnWithNoDefault) {
    // AC: Non-nullable columns without a DEFAULT that are omitted produce a clear error.
    exec_ok("CREATE TABLE strict ("
            "  id INT PRIMARY KEY,"
            "  name TEXT NOT NULL,"
            "  email TEXT NOT NULL"
            ")");

    // Omit email (non-nullable, no default) — should error.
    exec_error("INSERT INTO strict (id, name) VALUES (1, 'Alice')", StatusCode::INVALID_ARGUMENT);
}

TEST_F(QueryEngineTest, InsertMultipleRowsEachGetUniqueDefaults) {
    // Each row should get a unique gen_uuid() value.
    exec_ok("CREATE TABLE multi ("
            "  id UUID DEFAULT gen_uuid(),"
            "  val INT"
            ")");

    exec_ok("INSERT INTO multi (val) VALUES (1)");
    exec_ok("INSERT INTO multi (val) VALUES (2)");

    auto qr = exec_ok("SELECT id, val FROM multi ORDER BY val");
    ASSERT_EQ(qr.rows.size(), 2u);

    EXPECT_FALSE(qr.rows[0][0].is_null());
    EXPECT_FALSE(qr.rows[1][0].is_null());
    EXPECT_EQ(qr.rows[0][0].type_id(), TypeId::UUID);
    EXPECT_EQ(qr.rows[1][0].type_id(), TypeId::UUID);
    // The two UUIDs should be different.
    EXPECT_NE(qr.rows[0][0].as_uuid(), qr.rows[1][0].as_uuid());
}

// =============================================================================
// Graph operations without GraphEngine (GDB-256 regression: nullptr guard)
// =============================================================================

TEST_F(QueryEngineTest, CreateEdgeTypeFailsWithoutGraphEngine) {
    exec_ok("CREATE TABLE users2 (id INT PRIMARY KEY, name VARCHAR)");
    exec_error("CREATE EDGE TYPE knows FROM users2 TO users2", StatusCode::INTERNAL_ERROR);
}

TEST_F(QueryEngineTest, DropEdgeTypeFailsWithoutGraphEngine) {
    exec_error("DROP EDGE TYPE knows", StatusCode::INTERNAL_ERROR);
}

TEST_F(QueryEngineTest, LinkFailsWithoutGraphEngine) {
    exec_ok("CREATE TABLE users2 (id INT PRIMARY KEY, name VARCHAR)");
    // The binder resolves the edge type first — NOT_FOUND because it doesn't exist.
    exec_error("LINK users2(1) TO users2(2) VIA knows", StatusCode::NOT_FOUND);
}

TEST_F(QueryEngineTest, UnlinkFailsWithoutGraphEngine) {
    exec_ok("CREATE TABLE users2 (id INT PRIMARY KEY, name VARCHAR)");
    // The binder resolves the edge type first — NOT_FOUND because it doesn't exist.
    exec_error("UNLINK users2(1) FROM users2(2) VIA knows", StatusCode::NOT_FOUND);
}

// =============================================================================
// Graph operations WITH GraphEngine (GDB-256: verifies the fix)
// =============================================================================

class QueryEngineGraphTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_qe_graph";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
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

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

TEST_F(QueryEngineGraphTest, CreateEdgeTypeSucceeds) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    auto qr = exec_ok("CREATE EDGE TYPE knows FROM users TO users");
    EXPECT_EQ(qr.message, "CREATE EDGE TYPE");
}

TEST_F(QueryEngineGraphTest, DropEdgeTypeSucceeds) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE EDGE TYPE knows FROM users TO users");
    auto qr = exec_ok("DROP EDGE TYPE knows");
    EXPECT_EQ(qr.message, "DROP EDGE TYPE");
}

TEST_F(QueryEngineGraphTest, LinkAndUnlinkSucceed) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO users VALUES (2, 'bob')");
    exec_ok("CREATE EDGE TYPE knows FROM users TO users");

    auto link = exec_ok("LINK users(1) TO users(2) VIA knows");
    EXPECT_EQ(link.affected_rows, 1);
    EXPECT_EQ(link.message, "LINK");

    auto unlink = exec_ok("UNLINK users(1) FROM users(2) VIA knows");
    EXPECT_EQ(unlink.affected_rows, 1);
    EXPECT_EQ(unlink.message, "UNLINK");
}

TEST_F(QueryEngineGraphTest, CreateEdgeTypeDuplicateFails) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE EDGE TYPE knows FROM users TO users");
    exec_error("CREATE EDGE TYPE knows FROM users TO users", StatusCode::ALREADY_EXISTS);
}

TEST_F(QueryEngineGraphTest, DropEdgeTypeNotFoundFails) {
    exec_error("DROP EDGE TYPE nonexistent", StatusCode::NOT_FOUND);
}

TEST_F(QueryEngineGraphTest, DropEdgeTypeIfExists) {
    auto qr = exec_ok("DROP EDGE TYPE IF EXISTS nonexistent");
    EXPECT_EQ(qr.message, "DROP EDGE TYPE");
}

TEST_F(QueryEngineGraphTest, FullGraphCRUDPipeline) {
    // End-to-end: create tables, create edge type, link, unlink, drop edge type.
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO posts VALUES (10, 'hello world')");

    exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
    exec_ok("LINK users(1) TO posts(10) VIA authored");
    exec_ok("UNLINK users(1) FROM posts(10) VIA authored");
    exec_ok("DROP EDGE TYPE authored");

    // After drop, edge type no longer exists in graph engine.
    exec_error("LINK users(1) TO posts(10) VIA authored", StatusCode::NOT_FOUND);
}

// =============================================================================
// GDB-307: LINK/UNLINK with UUID primary keys (key coercion)
// =============================================================================

TEST_F(QueryEngineGraphTest, LinkWithUuidPrimaryKey) {
    exec_ok("CREATE TABLE people (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES "
            "('6f2fff6c-9762-4191-86e1-d34597e3c75a', 'Alice')");
    exec_ok("INSERT INTO people VALUES "
            "('d1458b55-f0bf-44d4-b191-e52f1ef1f60a', 'Bob')");
    exec_ok("CREATE EDGE TYPE follows FROM people TO people");

    auto link = exec_ok("LINK people('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
                        "TO people('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA follows");
    EXPECT_EQ(link.affected_rows, 1);
    EXPECT_EQ(link.message, "LINK");
}

TEST_F(QueryEngineGraphTest, UnlinkWithUuidPrimaryKey) {
    exec_ok("CREATE TABLE people (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES "
            "('6f2fff6c-9762-4191-86e1-d34597e3c75a', 'Alice')");
    exec_ok("INSERT INTO people VALUES "
            "('d1458b55-f0bf-44d4-b191-e52f1ef1f60a', 'Bob')");
    exec_ok("CREATE EDGE TYPE follows FROM people TO people");
    exec_ok("LINK people('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
            "TO people('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA follows");

    auto unlink = exec_ok("UNLINK people('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
                          "FROM people('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA follows");
    EXPECT_EQ(unlink.affected_rows, 1);
    EXPECT_EQ(unlink.message, "UNLINK");
}

TEST_F(QueryEngineGraphTest, LinkWithStringPrimaryKey) {
    exec_ok("CREATE TABLE tags (slug VARCHAR PRIMARY KEY, label VARCHAR)");
    exec_ok("INSERT INTO tags VALUES ('cpp', 'C++')");
    exec_ok("INSERT INTO tags VALUES ('db', 'Database')");
    exec_ok("CREATE EDGE TYPE related FROM tags TO tags");

    auto link = exec_ok("LINK tags('cpp') TO tags('db') VIA related");
    EXPECT_EQ(link.affected_rows, 1);
}

TEST_F(QueryEngineGraphTest, LinkWithHeterogeneousPkTypes) {
    exec_ok("CREATE TABLE authors (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE TABLE articles (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO authors VALUES "
            "('aabbccdd-1122-3344-5566-778899aabbcc', 'Eve')");
    exec_ok("INSERT INTO articles VALUES (42, 'SixSevenDB Internals')");
    exec_ok("CREATE EDGE TYPE wrote FROM authors TO articles");

    auto link = exec_ok("LINK authors('aabbccdd-1122-3344-5566-778899aabbcc') "
                        "TO articles(42) VIA wrote");
    EXPECT_EQ(link.affected_rows, 1);

    auto unlink = exec_ok("UNLINK authors('aabbccdd-1122-3344-5566-778899aabbcc') "
                          "FROM articles(42) VIA wrote");
    EXPECT_EQ(unlink.affected_rows, 1);
}

TEST_F(QueryEngineGraphTest, LinkWithInvalidUuidFails) {
    exec_ok("CREATE TABLE nodes (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE EDGE TYPE links FROM nodes TO nodes");

    exec_error("LINK nodes('not-a-uuid') TO nodes('also-bad') VIA links", StatusCode::TYPE_ERROR);
}

// =============================================================================
// GDB-313: TRAVERSE / SHORTEST PATH with UUID primary keys (key coercion)
// =============================================================================

TEST_F(QueryEngineGraphTest, TraverseWithUuidPrimaryKey) {
    exec_ok("CREATE TABLE people (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES "
            "('6f2fff6c-9762-4191-86e1-d34597e3c75a', 'Alice')");
    exec_ok("INSERT INTO people VALUES "
            "('d1458b55-f0bf-44d4-b191-e52f1ef1f60a', 'Bob')");
    exec_ok("CREATE EDGE TYPE follows FROM people TO people");
    exec_ok("LINK people('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
            "TO people('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA follows");

    auto qr = exec_ok("SELECT name, __depth "
                      "FROM TRAVERSE follows "
                      "FROM people('6f2fff6c-9762-4191-86e1-d34597e3c75a') DIRECTION OUT FETCH");
    ASSERT_GE(qr.rows.size(), 1u);
}

TEST_F(QueryEngineGraphTest, ShortestPathWithUuidPrimaryKey) {
    exec_ok("CREATE TABLE people (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES "
            "('6f2fff6c-9762-4191-86e1-d34597e3c75a', 'Alice')");
    exec_ok("INSERT INTO people VALUES "
            "('d1458b55-f0bf-44d4-b191-e52f1ef1f60a', 'Bob')");
    exec_ok("CREATE EDGE TYPE follows FROM people TO people");
    exec_ok("LINK people('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
            "TO people('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA follows");

    auto qr = exec_ok("SHORTEST PATH "
                      "FROM people('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
                      "TO people('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA follows");
    ASSERT_GE(qr.rows.size(), 1u);
}

// GDB-315: UNLINK should validate PK existence (symmetric with LINK).
TEST_F(QueryEngineGraphTest, UnlinkNonExistentSourceFails) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("CREATE EDGE TYPE knows FROM users TO users");

    exec_error("UNLINK users(999) FROM users(1) VIA knows", StatusCode::NOT_FOUND);
}

TEST_F(QueryEngineGraphTest, UnlinkNonExistentTargetFails) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("CREATE EDGE TYPE knows FROM users TO users");

    exec_error("UNLINK users(1) FROM users(999) VIA knows", StatusCode::NOT_FOUND);
}

TEST_F(QueryEngineGraphTest, UnlinkBothNonExistentFails) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE EDGE TYPE knows FROM users TO users");

    exec_error("UNLINK users(888) FROM users(999) VIA knows", StatusCode::NOT_FOUND);
}

// =============================================================================
// Describe tests (column metadata without execution)
// =============================================================================

TEST_F(QueryEngineTest, DescribeSelectReturnsColumns) {
    create_test_table();

    auto result = engine_->describe("SELECT id, name FROM users");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);
    EXPECT_EQ((*result)[0].name, "id");
    EXPECT_EQ((*result)[0].type_id, TypeId::INT32);
    EXPECT_EQ((*result)[1].name, "name");
    EXPECT_EQ((*result)[1].type_id, TypeId::STRING);
}

TEST_F(QueryEngineTest, DescribeSelectStarReturnsAllColumns) {
    create_test_table();

    auto result = engine_->describe("SELECT * FROM users");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 3u);
    EXPECT_EQ((*result)[0].name, "id");
    EXPECT_EQ((*result)[1].name, "name");
    EXPECT_EQ((*result)[2].name, "age");
}

TEST_F(QueryEngineTest, DescribeInsertReturnsEmpty) {
    create_test_table();

    auto result = engine_->describe("INSERT INTO users VALUES (1, 'alice', 30)");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(QueryEngineTest, DescribeUpdateReturnsEmpty) {
    create_test_table();

    auto result = engine_->describe("UPDATE users SET name = 'bob' WHERE id = 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(QueryEngineTest, DescribeDeleteReturnsEmpty) {
    create_test_table();

    auto result = engine_->describe("DELETE FROM users WHERE id = 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(QueryEngineTest, DescribeCreateTableReturnsEmpty) {
    auto result = engine_->describe("CREATE TABLE foo (id INT)");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(QueryEngineTest, DescribeInvalidSqlReturnsError) {
    auto result = engine_->describe("NOT VALID SQL AT ALL");
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// GDB-601: MATCH WHERE clause cannot resolve columns not in SELECT list
// =============================================================================

TEST_F(QueryEngineGraphTest, MatchWhereColumnNotInSelect) {
    // Setup: users table with id, name, age.
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR, age INT)");
    exec_ok("INSERT INTO users VALUES (1, 'Alice', 50)");
    exec_ok("INSERT INTO users VALUES (2, 'Bob', 30)");
    exec_ok("INSERT INTO users VALUES (3, 'Charlie', 60)");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(2) TO users(3) VIA follows");
    exec_ok("LINK users(3) TO users(1) VIA follows");

    // Bug: WHERE references a.age but SELECT only has a.name.
    // This used to fail with "column not found: a.age".
    auto qr = exec_ok("SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) WHERE a.age > 40");
    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "name");

    // Alice (50) and Charlie (60) match; Bob (30) does not.
    std::vector<std::string> names;
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row.size(), 1u);
        names.push_back(row[0].as_string());
    }
    std::sort(names.begin(), names.end());
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "Alice");
    EXPECT_EQ(names[1], "Charlie");
}

TEST_F(QueryEngineGraphTest, MatchWhereOnTargetColumnNotInSelect) {
    // WHERE references b.name but SELECT only has a.name.
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR, age INT)");
    exec_ok("INSERT INTO users VALUES (1, 'Alice', 50)");
    exec_ok("INSERT INTO users VALUES (2, 'Bob', 30)");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(2) TO users(1) VIA follows");

    auto qr =
        exec_ok("SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) WHERE b.name = 'Bob'");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Alice");
}

TEST_F(QueryEngineGraphTest, MatchWhereColumnInSelectStillWorks) {
    // Regression: ensure the fix doesn't break the case where WHERE column IS in SELECT.
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR, age INT)");
    exec_ok("INSERT INTO users VALUES (1, 'Alice', 50)");
    exec_ok("INSERT INTO users VALUES (2, 'Bob', 30)");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(2) TO users(1) VIA follows");

    auto qr = exec_ok(
        "SELECT a.name, a.age FROM MATCH (a:users)-[e:follows]->(b:users) WHERE a.age > 40");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Alice");
    EXPECT_EQ(qr.rows[0][1].as_int32(), 50);
}

TEST_F(QueryEngineGraphTest, StandaloneMatchWhereColumnNotInReturn) {
    // Standalone MATCH ... WHERE ... RETURN (not SELECT ... FROM MATCH).
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR, age INT)");
    exec_ok("INSERT INTO users VALUES (1, 'Alice', 50)");
    exec_ok("INSERT INTO users VALUES (2, 'Bob', 30)");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(2) TO users(1) VIA follows");

    auto qr = exec_ok("MATCH (a:users)-[e:follows]->(b:users) WHERE a.age > 40 RETURN a.name");
    ASSERT_EQ(qr.column_names.size(), 1u);
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Alice");
}

TEST_F(QueryEngineGraphTest, MatchWhereNameInWhereAgeInSelect) {
    // Exact repro case #4 from the ticket:
    // SELECT a.age FROM MATCH ... WHERE a.name = 'Alice'
    // (name only in WHERE, age only in SELECT)
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR, age INT)");
    exec_ok("INSERT INTO users VALUES (1, 'Alice', 50)");
    exec_ok("INSERT INTO users VALUES (2, 'Bob', 30)");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(2) TO users(1) VIA follows");

    auto qr =
        exec_ok("SELECT a.age FROM MATCH (a:users)-[e:follows]->(b:users) WHERE a.name = 'Alice'");
    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "age");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 50);
}

// -- SELECT without FROM (literal expressions) ---------------------------------

TEST_F(QueryEngineTest, SelectIntegerLiteral) {
    auto qr = exec_ok("SELECT 1 AS val");
    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "val");
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
}

TEST_F(QueryEngineTest, SelectStringLiteral) {
    auto qr = exec_ok("SELECT 'hello' AS greeting");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "hello");
}

TEST_F(QueryEngineTest, SelectMultipleLiterals) {
    auto qr = exec_ok("SELECT 42 AS num, 'world' AS text");
    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 42);
    EXPECT_EQ(qr.rows[0][1].as_string(), "world");
}

TEST_F(QueryEngineTest, SelectBoolLiteral) {
    auto qr = exec_ok("SELECT TRUE AS flag");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_TRUE(qr.rows[0][0].as_bool());
}

TEST_F(QueryEngineTest, SelectNullLiteral) {
    auto qr = exec_ok("SELECT NULL AS nothing");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_TRUE(qr.rows[0][0].is_null());
}

// GDB-661: SELECT-without-FROM must fold unary expressions wrapping literals.

TEST_F(QueryEngineTest, SelectNegativeIntegerLiteral) {
    auto qr = exec_ok("SELECT -5 AS neg");
    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "neg");
    ASSERT_EQ(qr.column_types.size(), 1u);
    EXPECT_EQ(qr.column_types[0], TypeId::INT32);
    EXPECT_EQ(qr.rows[0][0].as_int32(), -5);
}

TEST_F(QueryEngineTest, SelectNegativeIntegerLiteralNoAlias) {
    // No alias: fall back to a rendered default name "-<value>".
    auto qr = exec_ok("SELECT -42");
    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "-42");
    EXPECT_EQ(qr.rows[0][0].as_int32(), -42);
}

TEST_F(QueryEngineTest, SelectNegativeFloatLiteral) {
    auto qr = exec_ok("SELECT -3.14 AS neg_pi");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.column_types[0], TypeId::FLOAT64);
    EXPECT_DOUBLE_EQ(qr.rows[0][0].as_float64(), -3.14);
}

TEST_F(QueryEngineTest, SelectDoubleNegateInteger) {
    // `- -5` parses as UnaryExpr(NEGATE, UnaryExpr(NEGATE, LiteralExpr(5))).
    // (`--5` would be a SQL line comment, so the space matters.)
    auto qr = exec_ok("SELECT - -5 AS pos");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 5);
}

TEST_F(QueryEngineTest, SelectNotBooleanLiteral) {
    auto qr = exec_ok("SELECT NOT TRUE AS flipped");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.column_types[0], TypeId::BOOL);
    EXPECT_FALSE(qr.rows[0][0].as_bool());
}

TEST_F(QueryEngineTest, SelectMixedNegativeAndPositiveLiterals) {
    auto qr = exec_ok("SELECT -1 AS a, 2 AS b, -3.5 AS c");
    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.column_names.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), -1);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 2);
    EXPECT_DOUBLE_EQ(qr.rows[0][2].as_float64(), -3.5);
}
