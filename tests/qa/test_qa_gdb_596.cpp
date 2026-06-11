/// @file test_qa_gdb_596.cpp
/// @brief Adversarial QA tests for GDB-596: Documentation accuracy verification
///
/// Verifies that documented behavior matches actual server behavior:
///   1. All "Implemented" admin commands succeed
///   2. All "Not Yet Implemented" admin commands fail gracefully
///   3. Transaction statements are not implemented
///   4. BOOL is accepted as a type alias for BOOLEAN (GDB-600 fix)
///   5. Window functions are functional (GDB-599/607 fixes)
///   6. Edge property access via edge_type.property syntax works (GDB-606 fix)
///
/// Adversarial categories: boundary values, error paths, case sensitivity,
/// combined features, stress.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_qa_helpers.h"

namespace sixseven {
namespace {

// ============================================================================
// Fixture: sets up a basic database with tables for testing admin commands,
// window functions, BOOL type, and edge property access.
// ============================================================================

class QA_GDB596 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb596";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        // Create tables for admin command and window function testing
        exec_ok("CREATE TABLE employees (id INT PRIMARY KEY, name VARCHAR, "
                "department VARCHAR, salary DOUBLE)");
        exec_ok("INSERT INTO employees VALUES (1, 'Alice', 'eng', 80000)");
        exec_ok("INSERT INTO employees VALUES (2, 'Bob', 'eng', 90000)");
        exec_ok("INSERT INTO employees VALUES (3, 'Carol', 'sales', 70000)");
        exec_ok("INSERT INTO employees VALUES (4, 'Dave', 'sales', 75000)");
        exec_ok("INSERT INTO employees VALUES (5, 'Eve', 'eng', 85000)");

        // Create tables and edges for graph property access testing
        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");

        exec_ok("CREATE TABLE products (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO products VALUES (10, 'Widget')");
        exec_ok("INSERT INTO products VALUES (20, 'Gadget')");

        exec_ok("CREATE EDGE TYPE rated (score DOUBLE, review VARCHAR) "
                "FROM users TO products");
        exec_ok("LINK users(1) TO products(10) VIA rated (score = 4.5, review = 'great')");
        exec_ok("LINK users(1) TO products(20) VIA rated (score = 2.0, review = 'poor')");
        exec_ok("LINK users(2) TO products(10) VIA rated (score = 5.0, review = 'perfect')");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void exec_err(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << ": expected error but got success";
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// Section 1: Implemented admin commands (docs say these work)
// ============================================================================

TEST_F(QA_GDB596, AdminShowTables) {
    auto qr = exec_ok("SHOW TABLES");
    // Should list employees, users, products
    EXPECT_GE(qr.rows.size(), 3u);
}

TEST_F(QA_GDB596, AdminShowColumnsFromEmployees) {
    auto qr = exec_ok("SHOW COLUMNS FROM employees");
    // Should list id, name, department, salary
    EXPECT_GE(qr.rows.size(), 4u);
}

TEST_F(QA_GDB596, AdminShowColumnsFromUsers) {
    auto qr = exec_ok("SHOW COLUMNS FROM users");
    EXPECT_GE(qr.rows.size(), 2u);
}

TEST_F(QA_GDB596, AdminShowEdgeTypes) {
    auto qr = exec_ok("SHOW EDGE TYPES");
    EXPECT_GE(qr.rows.size(), 1u);
}

TEST_F(QA_GDB596, AdminShowIndexes) {
    // Should succeed even if no explicit indexes beyond PK
    exec_ok("SHOW INDEXES");
}

TEST_F(QA_GDB596, AdminShowEmbeddings) {
    // Should succeed (possibly empty result)
    exec_ok("SHOW EMBEDDINGS");
}

TEST_F(QA_GDB596, AdminShowDatabases) {
    exec_ok("SHOW DATABASES");
}

TEST_F(QA_GDB596, AdminExplainSelect) {
    auto qr = exec_ok("EXPLAIN SELECT * FROM employees WHERE salary > 80000");
    EXPECT_GE(qr.rows.size(), 1u);
}

TEST_F(QA_GDB596, AdminExplainAnalyzeSelect) {
    auto qr = exec_ok("EXPLAIN ANALYZE SELECT * FROM employees WHERE salary > 80000");
    EXPECT_GE(qr.rows.size(), 1u);
}

// ============================================================================
// Section 2: Not Yet Implemented admin commands (docs say these fail)
// ============================================================================

TEST_F(QA_GDB596, AdminDescribeNotImplemented) {
    exec_err("DESCRIBE employees");
}

// Flipped by GDB-720: these previously asserted the pre-GDB-711 behavior
// (VACUUM/ANALYZE rejected as not-implemented) but were masked by the fixture
// abort this ticket fixes. GDB-711 dispatched both statements to the executor,
// so they now succeed.
TEST_F(QA_GDB596, AdminVacuumSucceeds) {
    exec_ok("VACUUM");
}

TEST_F(QA_GDB596, AdminAnalyzeSucceeds) {
    exec_ok("ANALYZE");
}

TEST_F(QA_GDB596, AdminSetLogLevelNotImplemented) {
    exec_err("SET log_level = 'debug'");
}

TEST_F(QA_GDB596, AdminSetMaxConnectionsNotImplemented) {
    exec_err("SET max_connections = 200");
}

TEST_F(QA_GDB596, AdminShowParameterNotImplemented) {
    exec_err("SHOW PARAMETER max_connections");
}

TEST_F(QA_GDB596, AdminShowProvidersNotImplemented) {
    // SHOW PROVIDERS should fail because provider cache is not initialized
    exec_err("SHOW PROVIDERS");
}

// ============================================================================
// Section 3: Transactions not implemented (docs say so)
// ============================================================================

TEST_F(QA_GDB596, TransactionBeginNotImplemented) {
    exec_err("BEGIN");
}

TEST_F(QA_GDB596, TransactionCommitNotImplemented) {
    exec_err("COMMIT");
}

TEST_F(QA_GDB596, TransactionRollbackNotImplemented) {
    exec_err("ROLLBACK");
}

TEST_F(QA_GDB596, TransactionSavepointNotImplemented) {
    exec_err("SAVEPOINT sp1");
}

TEST_F(QA_GDB596, TransactionRollbackToNotImplemented) {
    exec_err("ROLLBACK TO sp1");
}

// ============================================================================
// Section 4: BOOL as type alias (docs still use BOOL; GDB-600 added support)
// ============================================================================

TEST_F(QA_GDB596, BoolTypeAccepted) {
    exec_ok("CREATE TABLE bool_test (id INT PRIMARY KEY, active BOOL)");
    exec_ok("INSERT INTO bool_test VALUES (1, true)");
    exec_ok("INSERT INTO bool_test VALUES (2, false)");
    auto qr = exec_ok("SELECT * FROM bool_test WHERE active = true");
    EXPECT_EQ(qr.rows.size(), 1u);
}

TEST_F(QA_GDB596, BooleanTypeStillWorks) {
    exec_ok("CREATE TABLE boolean_test (id INT PRIMARY KEY, active BOOLEAN)");
    exec_ok("INSERT INTO boolean_test VALUES (1, true)");
    auto qr = exec_ok("SELECT * FROM boolean_test WHERE active = true");
    EXPECT_EQ(qr.rows.size(), 1u);
}

TEST_F(QA_GDB596, BoolDefaultValue) {
    exec_ok("CREATE TABLE bool_default_test (id INT PRIMARY KEY, "
            "published BOOL DEFAULT false)");
    exec_ok("INSERT INTO bool_default_test (id) VALUES (1)");
    auto qr = exec_ok("SELECT published FROM bool_default_test WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_bool(), false);
}

// ============================================================================
// Section 5: Window functions work (docs show them; GDB-599/607 fixed them)
// ============================================================================

TEST_F(QA_GDB596, WindowRowNumber) {
    auto qr = exec_ok("SELECT name, ROW_NUMBER() OVER (ORDER BY salary DESC) AS rank "
                      "FROM employees");
    EXPECT_EQ(qr.rows.size(), 5u);
    EXPECT_EQ(qr.column_names.size(), 2u);
}

TEST_F(QA_GDB596, WindowRowNumberWithPartition) {
    auto qr =
        exec_ok("SELECT name, department, "
                "ROW_NUMBER() OVER (PARTITION BY department ORDER BY salary DESC) AS dept_rank "
                "FROM employees");
    EXPECT_EQ(qr.rows.size(), 5u);
}

TEST_F(QA_GDB596, WindowSumOverPartition) {
    auto qr = exec_ok("SELECT name, department, salary, "
                      "SUM(salary) OVER (PARTITION BY department) AS dept_total "
                      "FROM employees");
    EXPECT_EQ(qr.rows.size(), 5u);
    // Verify eng total = 80000 + 90000 + 85000 = 255000
    // Find an eng row
    size_t dept_col = 1;
    size_t total_col = 3;
    for (const auto& row : qr.rows) {
        if (row[dept_col].as_string() == "eng") {
            EXPECT_DOUBLE_EQ(row[total_col].as_float64(), 255000.0);
            break;
        }
    }
}

TEST_F(QA_GDB596, WindowLag) {
    auto qr = exec_ok("SELECT name, salary, LAG(salary) OVER (ORDER BY salary) AS prev_salary "
                      "FROM employees");
    EXPECT_EQ(qr.rows.size(), 5u);
}

TEST_F(QA_GDB596, WindowCombinedExample) {
    // The exact example from the documentation
    auto qr = exec_ok("SELECT name, salary, "
                      "ROW_NUMBER() OVER (PARTITION BY department ORDER BY salary DESC) AS rank, "
                      "LAG(salary) OVER (ORDER BY salary) AS prev_salary, "
                      "SUM(salary) OVER (PARTITION BY department) AS dept_total "
                      "FROM employees");
    EXPECT_EQ(qr.rows.size(), 5u);
    EXPECT_EQ(qr.column_names.size(), 5u);
}

TEST_F(QA_GDB596, WindowNoOrderBy) {
    // Window function without ORDER BY — GDB-607 fixed the default frame
    auto qr = exec_ok("SELECT name, SUM(salary) OVER () AS grand_total FROM employees");
    EXPECT_EQ(qr.rows.size(), 5u);
    // Every row should have the same grand total = 400000
    for (const auto& row : qr.rows) {
        EXPECT_DOUBLE_EQ(row[1].as_float64(), 400000.0);
    }
}

// ============================================================================
// Section 6: Edge property access (docs show rated.score; GDB-606 fixed it)
// ============================================================================

TEST_F(QA_GDB596, EdgePropertyQualifiedAccess) {
    // The exact pattern from docs/graph-queries.md:
    // SELECT name, rated.score, rated.review FROM TRAVERSE rated FROM users(1) ...
    auto qr = exec_ok("SELECT name, rated.score, rated.review "
                      "FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");
    EXPECT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.column_names.size(), 3u);
}

TEST_F(QA_GDB596, EdgePropertyStarExpansion) {
    // Docs say: "Star expansion includes edge properties"
    auto qr = exec_ok("SELECT * FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH");
    // Should include edge properties score, review plus product cols and meta cols
    bool has_score =
        std::find(qr.column_names.begin(), qr.column_names.end(), "score") != qr.column_names.end();
    bool has_review = std::find(qr.column_names.begin(), qr.column_names.end(), "review") !=
                      qr.column_names.end();
    EXPECT_TRUE(has_score) << "Star expansion should include edge property 'score'";
    EXPECT_TRUE(has_review) << "Star expansion should include edge property 'review'";
}

TEST_F(QA_GDB596, EdgePropertyInDirection) {
    // Verify edge properties work with IN direction too
    auto qr = exec_ok("SELECT name, rated.score "
                      "FROM TRAVERSE rated FROM products(10) DIRECTION IN FETCH AS t");
    // products(10) has edges from users(1) and users(2)
    EXPECT_EQ(qr.rows.size(), 2u);
}

// ============================================================================
// Adversarial: SHOW COLUMNS on nonexistent table
// ============================================================================

TEST_F(QA_GDB596, AdminShowColumnsNonexistentTable) {
    exec_err("SHOW COLUMNS FROM nonexistent_table");
}

// ============================================================================
// Adversarial: EXPLAIN on invalid query
// ============================================================================

TEST_F(QA_GDB596, AdminExplainInvalidQuery) {
    exec_err("EXPLAIN SELECT * FROM nonexistent");
}

// ============================================================================
// Adversarial: Window function on empty result set
// ============================================================================

TEST_F(QA_GDB596, WindowFunctionEmptyResult) {
    auto qr = exec_ok("SELECT name, ROW_NUMBER() OVER (ORDER BY salary) AS rn "
                      "FROM employees WHERE salary > 999999");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// ============================================================================
// Adversarial: Multiple window functions with different partitions
// ============================================================================

TEST_F(QA_GDB596, WindowMultipleDifferentPartitions) {
    auto qr = exec_ok("SELECT name, "
                      "ROW_NUMBER() OVER (PARTITION BY department ORDER BY salary) AS dept_rn, "
                      "ROW_NUMBER() OVER (ORDER BY name) AS global_rn "
                      "FROM employees");
    EXPECT_EQ(qr.rows.size(), 5u);
    EXPECT_EQ(qr.column_names.size(), 3u);
}

// ============================================================================
// Adversarial: Edge property with WHERE filter on edge property
// ============================================================================

TEST_F(QA_GDB596, EdgePropertyWhereFilter) {
    auto qr = exec_ok("SELECT name, rated.score "
                      "FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t "
                      "WHERE score > 3.0");
    // Only products(10) with score=4.5 should pass
    EXPECT_EQ(qr.rows.size(), 1u);
}

} // namespace
} // namespace sixseven
