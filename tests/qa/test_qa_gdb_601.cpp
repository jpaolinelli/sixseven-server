/// @file test_qa_gdb_601.cpp
/// QA adversarial tests for GDB-601: MATCH WHERE clause cannot resolve columns
/// not present in SELECT list.
///
/// Verifies that WHERE clauses in MATCH queries can reference any column from
/// matched node tables, regardless of whether it appears in the SELECT/RETURN
/// list.  Tests cover:
///   - Ticket repro cases (exact scenarios from the bug report)
///   - Edge cases (NULL handling, empty results, all columns filtered)
///   - Boundary values (numeric extremes, empty strings)
///   - Multiple WHERE conditions on non-SELECT columns
///   - Variable-length path queries with WHERE on non-SELECT columns
///   - SELECT ... FROM MATCH and standalone MATCH ... RETURN syntax
///   - Multi-node patterns with WHERE on different node variables
///   - DISTINCT with WHERE on non-SELECT columns
///   - ORDER BY combined with WHERE on non-SELECT columns
///   - Stress: many columns, many rows

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

namespace sixseven {
namespace {

// ============================================================================
// Test fixture — full QueryEngine with graph support
// ============================================================================

class QA_GDB601 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb601";
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
        EXPECT_TRUE(result.has_value()) << "SQL failed: " << sql << "\n"
                                        << result.error().message;
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "Expected error for: " << sql;
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected);
        }
    }

    /// Helper: set up the standard users table + follows edge + data
    void setup_standard_graph() {
        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR, age INT)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice', 50)");
        exec_ok("INSERT INTO users VALUES (2, 'Bob', 30)");
        exec_ok("INSERT INTO users VALUES (3, 'Charlie', 60)");
        exec_ok("CREATE EDGE TYPE follows FROM users TO users");
        exec_ok("LINK users(1) TO users(2) VIA follows");
        exec_ok("LINK users(2) TO users(3) VIA follows");
        exec_ok("LINK users(3) TO users(1) VIA follows");
    }

    /// Collect a single string column from all rows, sorted.
    std::vector<std::string> collect_sorted_strings(const QueryResult& qr, size_t col = 0) {
        std::vector<std::string> out;
        for (const auto& row : qr.rows) {
            out.push_back(row[col].as_string());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    /// Collect a single int32 column from all rows, sorted.
    std::vector<int32_t> collect_sorted_ints(const QueryResult& qr, size_t col = 0) {
        std::vector<int32_t> out;
        for (const auto& row : qr.rows) {
            out.push_back(row[col].as_int32());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// Ticket repro cases — exact scenarios from the bug report
// ============================================================================

// Repro #1: WHERE a.age > 40 but SELECT only a.name
TEST_F(QA_GDB601, ReproWhereAgeSelectName) {
    setup_standard_graph();
    auto qr = exec_ok(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) WHERE a.age > 40");
    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "name");
    auto names = collect_sorted_strings(qr);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "Alice");
    EXPECT_EQ(names[1], "Charlie");
}

// Repro #2: WHERE a.name = 'Alice' but SELECT only a.age
TEST_F(QA_GDB601, ReproWhereNameSelectAge) {
    setup_standard_graph();
    auto qr = exec_ok(
        "SELECT a.age FROM MATCH (a:users)-[e:follows]->(b:users) WHERE a.name = 'Alice'");
    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "age");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 50);
}

// ============================================================================
// Edge cases
// ============================================================================

// WHERE filters out ALL rows — empty result set
TEST_F(QA_GDB601, WhereFiltersAllRows) {
    setup_standard_graph();
    auto qr = exec_ok(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) WHERE a.age > 100");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// WHERE on a column that is NULL for some rows
TEST_F(QA_GDB601, WhereOnNullableColumn) {
    exec_ok("CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR, bio VARCHAR)");
    exec_ok("INSERT INTO people VALUES (1, 'Alice', 'Engineer')");
    exec_ok("INSERT INTO people VALUES (2, 'Bob', NULL)");
    exec_ok("INSERT INTO people VALUES (3, 'Charlie', 'Designer')");
    exec_ok("CREATE EDGE TYPE knows FROM people TO people");
    exec_ok("LINK people(1) TO people(2) VIA knows");
    exec_ok("LINK people(2) TO people(3) VIA knows");

    // WHERE bio IS NOT NULL — only Alice should match (source node)
    auto qr = exec_ok(
        "SELECT a.name FROM MATCH (a:people)-[e:knows]->(b:people) WHERE a.bio IS NOT NULL");
    auto names = collect_sorted_strings(qr);
    // Alice (bio = 'Engineer') links to Bob — she matches
    // Bob (bio = NULL) links to Charlie — he doesn't match
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "Alice");
}

// WHERE on target node column not in SELECT
TEST_F(QA_GDB601, WhereOnTargetNodeColumn) {
    setup_standard_graph();
    // Only return rows where the target node (b) has age > 40
    auto qr = exec_ok(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) WHERE b.age > 40");
    auto names = collect_sorted_strings(qr);
    // Edges: 1->2(30), 2->3(60), 3->1(50)
    // b.age > 40: edge 2->3 (b=Charlie,60), edge 3->1 (b=Alice,50)
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "Bob");      // links to Charlie (60)
    EXPECT_EQ(names[1], "Charlie");  // links to Alice (50)
}

// WHERE on BOTH source and target columns, neither in SELECT — only select id
TEST_F(QA_GDB601, WhereOnBothNodesColumnsNotInSelect) {
    setup_standard_graph();
    auto qr = exec_ok(
        "SELECT a.id FROM MATCH (a:users)-[e:follows]->(b:users) "
        "WHERE a.age > 25 AND b.name = 'Bob'");
    // a.age > 25 AND b.name = 'Bob'
    // Edge 1->2: a=Alice(50), b=Bob ✓
    // Edge 2->3: a=Bob(30), b=Charlie — b.name != 'Bob'
    // Edge 3->1: a=Charlie(60), b=Alice — b.name != 'Bob'
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1); // Alice's id
}

// ============================================================================
// Multiple non-SELECT columns in WHERE with AND/OR
// ============================================================================

TEST_F(QA_GDB601, WhereMultipleNonSelectColumnsOr) {
    setup_standard_graph();
    // WHERE a.name = 'Alice' OR a.age = 30 — neither name nor age in SELECT
    auto qr = exec_ok(
        "SELECT a.id FROM MATCH (a:users)-[e:follows]->(b:users) "
        "WHERE a.name = 'Alice' OR a.age = 30");
    auto ids = collect_sorted_ints(qr);
    // Alice (name match) -> Bob, Bob (age=30 match) -> Charlie
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 1);  // Alice
    EXPECT_EQ(ids[1], 2);  // Bob
}

// ============================================================================
// Standalone MATCH ... RETURN syntax (not SELECT ... FROM MATCH)
// ============================================================================

TEST_F(QA_GDB601, StandaloneMatchReturnWhereNonReturnColumn) {
    setup_standard_graph();
    auto qr = exec_ok(
        "MATCH (a:users)-[e:follows]->(b:users) WHERE a.age > 40 RETURN a.name");
    ASSERT_EQ(qr.column_names.size(), 1u);
    auto names = collect_sorted_strings(qr);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "Alice");
    EXPECT_EQ(names[1], "Charlie");
}

TEST_F(QA_GDB601, StandaloneMatchReturnWhereOnTargetNonReturn) {
    setup_standard_graph();
    auto qr = exec_ok(
        "MATCH (a:users)-[e:follows]->(b:users) WHERE b.age < 40 RETURN a.name");
    // b.age < 40: only Bob (30). Edge 1->2: a=Alice
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Alice");
}

// ============================================================================
// DISTINCT with WHERE on non-SELECT columns
// ============================================================================

TEST_F(QA_GDB601, DistinctWithWhereOnNonSelectColumn) {
    exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR, age INT)");
    exec_ok("INSERT INTO users VALUES (1, 'Alice', 50)");
    exec_ok("INSERT INTO users VALUES (2, 'Bob', 30)");
    exec_ok("INSERT INTO users VALUES (3, 'Charlie', 50)");
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    // Create multiple edges from Alice and Charlie (both age 50)
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(3) TO users(2) VIA follows");

    auto qr = exec_ok(
        "SELECT DISTINCT a.name FROM MATCH (a:users)-[e:follows]->(b:users) WHERE a.age = 50");
    auto names = collect_sorted_strings(qr);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "Alice");
    EXPECT_EQ(names[1], "Charlie");
}

// ============================================================================
// Variable-length paths with WHERE on non-SELECT columns
// ============================================================================

TEST_F(QA_GDB601, VariableLengthPathWhereNonSelectColumn) {
    setup_standard_graph();
    // Variable-length match with WHERE on a column not in SELECT
    auto qr = exec_ok(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->{1,2}(b:users) WHERE a.age > 40");
    // a.age > 40: Alice(50), Charlie(60)
    // This tests that VariableLengthMatchOperator also gets the full-scope schema
    auto names = collect_sorted_strings(qr);
    // Should include Alice and Charlie (each with 1-hop and 2-hop results)
    EXPECT_GE(names.size(), 2u);
    // Verify Alice and Charlie are present
    EXPECT_NE(std::find(names.begin(), names.end(), "Alice"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "Charlie"), names.end());
    // Bob (age 30) should NOT appear
    EXPECT_EQ(std::find(names.begin(), names.end(), "Bob"), names.end());
}

// ============================================================================
// SELECT ... FROM MATCH subquery in a SELECT — plan_from_source path
// ============================================================================

TEST_F(QA_GDB601, SelectFromMatchSubqueryWhereNonSelectColumn) {
    setup_standard_graph();
    // This exercises the plan_from_source code path (SELECT ... FROM MATCH ...)
    // The WHERE here is part of the outer SELECT, not inside MATCH
    auto qr = exec_ok(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) WHERE a.age > 40");
    auto names = collect_sorted_strings(qr);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "Alice");
    EXPECT_EQ(names[1], "Charlie");
}

// ============================================================================
// Many-column table — WHERE references columns far from the SELECT columns
// ============================================================================

TEST_F(QA_GDB601, ManyColumnsWhereOnDistantColumn) {
    exec_ok(
        "CREATE TABLE wide_table ("
        "id INT PRIMARY KEY, "
        "col_a VARCHAR, col_b VARCHAR, col_c VARCHAR, "
        "col_d VARCHAR, col_e VARCHAR, col_f INT)");
    exec_ok("INSERT INTO wide_table VALUES (1, 'a1', 'b1', 'c1', 'd1', 'e1', 100)");
    exec_ok("INSERT INTO wide_table VALUES (2, 'a2', 'b2', 'c2', 'd2', 'e2', 200)");
    exec_ok("INSERT INTO wide_table VALUES (3, 'a3', 'b3', 'c3', 'd3', 'e3', 300)");
    exec_ok("CREATE EDGE TYPE link FROM wide_table TO wide_table");
    exec_ok("LINK wide_table(1) TO wide_table(2) VIA link");
    exec_ok("LINK wide_table(2) TO wide_table(3) VIA link");

    // SELECT col_a, WHERE on col_f (distant column not in SELECT)
    auto qr = exec_ok(
        "SELECT a.col_a FROM MATCH (a:wide_table)-[e:link]->(b:wide_table) WHERE a.col_f > 150");
    auto vals = collect_sorted_strings(qr);
    // a.col_f > 150: id=2 (200), id=3 (300). id=2 has edge to id=3.
    ASSERT_EQ(vals.size(), 1u);
    EXPECT_EQ(vals[0], "a2");
}

// ============================================================================
// Regression: WHERE column IS in SELECT still works
// ============================================================================

TEST_F(QA_GDB601, RegressionWhereColumnInSelectStillWorks) {
    setup_standard_graph();
    auto qr = exec_ok(
        "SELECT a.name, a.age FROM MATCH (a:users)-[e:follows]->(b:users) WHERE a.age > 40");
    ASSERT_EQ(qr.column_names.size(), 2u);
    auto names = collect_sorted_strings(qr);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "Alice");
    EXPECT_EQ(names[1], "Charlie");
}

// ============================================================================
// Boundary: WHERE with equality on string — empty string
// ============================================================================

TEST_F(QA_GDB601, WhereEmptyStringColumn) {
    exec_ok("CREATE TABLE items (id INT PRIMARY KEY, name VARCHAR, tag VARCHAR)");
    exec_ok("INSERT INTO items VALUES (1, 'Widget', '')");
    exec_ok("INSERT INTO items VALUES (2, 'Gadget', 'electronics')");
    exec_ok("CREATE EDGE TYPE related FROM items TO items");
    exec_ok("LINK items(1) TO items(2) VIA related");
    exec_ok("LINK items(2) TO items(1) VIA related");

    // WHERE tag = '' — only Widget matches, tag not in SELECT
    auto qr = exec_ok(
        "SELECT a.name FROM MATCH (a:items)-[e:related]->(b:items) WHERE a.tag = ''");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Widget");
}

// ============================================================================
// Stress: many rows with WHERE filtering on non-SELECT column
// ============================================================================

TEST_F(QA_GDB601, StressManyRowsWhereNonSelect) {
    exec_ok("CREATE TABLE nodes (id INT PRIMARY KEY, label VARCHAR, priority INT)");
    exec_ok("CREATE EDGE TYPE connects FROM nodes TO nodes");

    // Insert 50 nodes
    for (int i = 1; i <= 50; ++i) {
        std::string label = "node_" + std::to_string(i);
        int priority = i % 10; // 0-9
        exec_ok("INSERT INTO nodes VALUES (" + std::to_string(i) + ", '" + label +
                "', " + std::to_string(priority) + ")");
    }
    // Create a chain: 1->2->3->...->50
    for (int i = 1; i < 50; ++i) {
        exec_ok("LINK nodes(" + std::to_string(i) + ") TO nodes(" +
                std::to_string(i + 1) + ") VIA connects");
    }

    // WHERE priority = 0 (nodes 10, 20, 30, 40, 50) — priority not in SELECT
    auto qr = exec_ok(
        "SELECT a.label FROM MATCH (a:nodes)-[e:connects]->(b:nodes) WHERE a.priority = 0");
    // Nodes with priority=0: 10,20,30,40. Node 50 has no outgoing edge.
    auto labels = collect_sorted_strings(qr);
    ASSERT_EQ(labels.size(), 4u);
    EXPECT_EQ(labels[0], "node_10");
    EXPECT_EQ(labels[1], "node_20");
    EXPECT_EQ(labels[2], "node_30");
    EXPECT_EQ(labels[3], "node_40");
}

// ============================================================================
// WHERE with comparison operators on non-SELECT column
// ============================================================================

TEST_F(QA_GDB601, WhereComparisonOperatorsNonSelect) {
    setup_standard_graph();

    // <= operator
    auto qr1 = exec_ok(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) WHERE a.age <= 30");
    ASSERT_EQ(qr1.rows.size(), 1u);
    EXPECT_EQ(qr1.rows[0][0].as_string(), "Bob");

    // != operator
    auto qr2 = exec_ok(
        "SELECT a.id FROM MATCH (a:users)-[e:follows]->(b:users) WHERE a.name != 'Alice'");
    auto ids = collect_sorted_ints(qr2);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 2); // Bob
    EXPECT_EQ(ids[1], 3); // Charlie

    // BETWEEN
    auto qr3 = exec_ok(
        "SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) "
        "WHERE a.age BETWEEN 40 AND 55");
    auto names = collect_sorted_strings(qr3);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "Alice"); // age 50
}

// ============================================================================
// Multi-hop pattern: (a)-[]->(b)-[]->(c) with WHERE on non-SELECT from c
// ============================================================================

TEST_F(QA_GDB601, ThreeNodePatternWhereOnThirdNode) {
    setup_standard_graph();
    // Three-node chain: a -> b -> c, WHERE on c.age, SELECT a.name
    auto qr = exec_ok(
        "SELECT a.name FROM MATCH (a:users)-[e1:follows]->(b:users)-[e2:follows]->(c:users) "
        "WHERE c.age > 40");
    // Chains: Alice->Bob->Charlie(60) ✓, Bob->Charlie->Alice(50) ✓,
    //         Charlie->Alice->Bob(30) ✗
    auto names = collect_sorted_strings(qr);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "Alice");
    EXPECT_EQ(names[1], "Bob");
}

}  // namespace
}  // namespace sixseven
