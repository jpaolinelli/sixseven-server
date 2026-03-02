#include "giodb/catalog/catalog.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/query_engine.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace giodb;

// =============================================================================
// QA Adversarial Test Fixture for GDB-26: Subqueries & CTEs
// =============================================================================

class QA_Subquery : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "giodb_qa_gdb_26";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        // Standard tables: users, orders, departments.
        exec_ok("CREATE TABLE users (id INT, name VARCHAR, dept_id INT)");
        exec_ok("CREATE TABLE orders (id INT, user_id INT, amount INT)");
        exec_ok("CREATE TABLE departments (id INT, dept_name VARCHAR)");

        exec_ok("INSERT INTO users VALUES (1, 'alice', 10)");
        exec_ok("INSERT INTO users VALUES (2, 'bob', 20)");
        exec_ok("INSERT INTO users VALUES (3, 'charlie', 10)");
        exec_ok("INSERT INTO users VALUES (4, 'diana', 30)");

        exec_ok("INSERT INTO orders VALUES (100, 1, 500)");
        exec_ok("INSERT INTO orders VALUES (101, 1, 300)");
        exec_ok("INSERT INTO orders VALUES (102, 3, 200)");

        exec_ok("INSERT INTO departments VALUES (10, 'engineering')");
        exec_ok("INSERT INTO departments VALUES (20, 'sales')");
        exec_ok("INSERT INTO departments VALUES (30, 'hr')");
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (!result.has_value()) {
            ADD_FAILURE() << "SQL: " << sql << "\nError: " << result.error().message;
            return QueryResult{};
        }
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "SQL should have failed: " << sql;
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected)
                << "Expected " << static_cast<int>(expected) << " but got "
                << static_cast<int>(result.error().code) << ": " << result.error().message;
        }
    }

    void exec_should_fail(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "SQL should have failed: " << sql;
    }

    std::unordered_set<std::string> collect_column_strings(const QueryResult& qr, size_t col) {
        std::unordered_set<std::string> result;
        for (const auto& row : qr.rows) {
            result.insert(row[col].as_string());
        }
        return result;
    }

    std::unordered_set<int64_t> collect_column_ints(const QueryResult& qr, size_t col) {
        std::unordered_set<int64_t> result;
        for (const auto& row : qr.rows) {
            if (row[col].type_id() == TypeId::INT32) {
                result.insert(row[col].as_int32());
            } else {
                result.insert(row[col].as_int64());
            }
        }
        return result;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// EXISTS edge cases
// =============================================================================

TEST_F(QA_Subquery, ExistsAgainstEmptyTable) {
    // Create empty table, EXISTS should return no rows.
    exec_ok("CREATE TABLE empty_t (id INT)");
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM empty_t WHERE empty_t.id = users.id)");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QA_Subquery, NotExistsAgainstEmptyTable) {
    // NOT EXISTS against empty table: all outer rows should be returned.
    exec_ok("CREATE TABLE empty_t (id INT)");
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE NOT EXISTS (SELECT 1 FROM empty_t WHERE empty_t.id = users.id)");
    EXPECT_EQ(qr.rows.size(), 4u);
}

TEST_F(QA_Subquery, ExistsWithUncorrelatedSubqueryReturnsAll) {
    // EXISTS with uncorrelated subquery that returns rows -> all outer rows returned.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders)");
    EXPECT_EQ(qr.rows.size(), 4u);
}

TEST_F(QA_Subquery, NotExistsWithUncorrelatedSubqueryReturnsNone) {
    // NOT EXISTS with uncorrelated subquery that returns rows -> no outer rows.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE NOT EXISTS (SELECT 1 FROM orders)");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QA_Subquery, ExistsWithMultipleConditionsInAnd) {
    // EXISTS combined with another filter via AND.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE users.dept_id = 10 AND "
                      "EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)");
    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

// =============================================================================
// NOT IN with NULLs — SQL standard NULL propagation
// =============================================================================

TEST_F(QA_Subquery, NotInSubqueryWithNullInResults) {
    // SQL standard: x NOT IN (1, NULL) for x=2 should be NULL (not TRUE).
    // This tests whether NOT IN with NULLs in the subquery result handles
    // SQL three-valued logic correctly.
    exec_ok("CREATE TABLE nullable_ids (val INT)");
    exec_ok("INSERT INTO nullable_ids VALUES (1)");
    exec_ok("INSERT INTO nullable_ids VALUES (NULL)");

    // user id=2 (bob) is NOT IN (1, NULL). Per SQL standard, this is NULL,
    // so bob should NOT appear in results.
    // user id=1 (alice) IS IN (1, NULL), so alice should not appear (correct for NOT IN).
    // user id=3 (charlie) is NOT IN (1, NULL) → NULL → should not appear.
    // user id=4 (diana) is NOT IN (1, NULL) → NULL → should not appear.
    // So: only user id=1 should be excluded for sure, but users 2,3,4 should
    // also be excluded because NOT IN with a NULL member returns NULL.
    // Expected: only alice excluded (IN returns TRUE), all others return NULL → excluded.
    // Result: 0 rows.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE users.id NOT IN (SELECT nullable_ids.val FROM nullable_ids)");

    // Per SQL standard, NOT IN returns NULL when the subquery has NULL values
    // and the value doesn't match any non-NULL value. NULL in WHERE → filtered out.
    // Only id=1 matches (IN is TRUE, NOT IN is FALSE → filtered).
    // All other ids: NOT IN is NULL → filtered.
    // Expected: 0 rows.
    EXPECT_EQ(qr.rows.size(), 0u)
        << "NOT IN with NULL in subquery should filter all non-matching rows "
           "(SQL NULL semantics)";
}

TEST_F(QA_Subquery, InSubqueryWithNullInResults) {
    // IN with NULLs: x IN (1, NULL) for x=1 → TRUE, for x=2 → NULL.
    exec_ok("CREATE TABLE nullable_ids (val INT)");
    exec_ok("INSERT INTO nullable_ids VALUES (1)");
    exec_ok("INSERT INTO nullable_ids VALUES (NULL)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE users.id IN (SELECT nullable_ids.val FROM nullable_ids)");

    // Only id=1 (alice) matches definitively. All others are NULL → filtered.
    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 1u);
    EXPECT_TRUE(names.count("alice"));
}

TEST_F(QA_Subquery, NotInSubqueryAllNulls) {
    // NOT IN where subquery returns only NULLs.
    exec_ok("CREATE TABLE all_nulls (val INT)");
    exec_ok("INSERT INTO all_nulls VALUES (NULL)");
    exec_ok("INSERT INTO all_nulls VALUES (NULL)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE users.id NOT IN (SELECT all_nulls.val FROM all_nulls)");

    // NOT IN (NULL, NULL) → NULL for all rows → all filtered out.
    EXPECT_EQ(qr.rows.size(), 0u)
        << "NOT IN with all-NULL subquery should return 0 rows (SQL NULL semantics)";
}

// =============================================================================
// IN subquery edge cases
// =============================================================================

TEST_F(QA_Subquery, InSubqueryEmptyResult) {
    // IN with empty subquery result → no matches.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE users.id IN (SELECT orders.user_id FROM orders WHERE orders.id = 9999)");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QA_Subquery, NotInSubqueryEmptyResult) {
    // NOT IN with empty subquery result → all rows match.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE users.id NOT IN (SELECT orders.user_id FROM orders WHERE orders.id = 9999)");
    EXPECT_EQ(qr.rows.size(), 4u);
}

TEST_F(QA_Subquery, InSubqueryWithDuplicateValues) {
    // IN with duplicates in subquery: user_id 1 appears twice in orders.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE users.id IN (SELECT orders.user_id FROM orders)");
    // alice (id=1) has 2 orders, charlie (id=3) has 1. Both should appear once.
    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

// =============================================================================
// Scalar subquery edge cases
// =============================================================================

TEST_F(QA_Subquery, ScalarSubqueryReturnsExactlyOneRow) {
    // Scalar subquery with exactly one row (no aggregation).
    auto qr = exec_ok("SELECT users.name, "
                      "(SELECT orders.amount FROM orders WHERE orders.id = 100) "
                      "FROM users WHERE users.id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
    EXPECT_EQ(qr.rows[0][1].as_int32(), 500);
}

TEST_F(QA_Subquery, ScalarSubqueryNoRowsReturnsNull) {
    // No matching rows → NULL.
    auto qr = exec_ok("SELECT users.name, "
                      "(SELECT orders.amount FROM orders WHERE orders.id = 9999) "
                      "FROM users WHERE users.id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_TRUE(qr.rows[0][1].is_null());
}

TEST_F(QA_Subquery, ScalarSubqueryMultipleRowsErrors) {
    // Multiple rows → error.
    exec_error(
        "SELECT users.name, (SELECT orders.amount FROM orders) FROM users WHERE users.id = 1",
        StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_Subquery, ScalarSubqueryWithAggregateCount) {
    auto qr = exec_ok("SELECT users.name, (SELECT COUNT(*) FROM orders) FROM users WHERE users.id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_int64(), 3);
}

TEST_F(QA_Subquery, ScalarSubqueryWithAggregateSum) {
    auto qr = exec_ok("SELECT users.name, (SELECT SUM(orders.amount) FROM orders) "
                      "FROM users WHERE users.id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_int64(), 1000);
}

TEST_F(QA_Subquery, ScalarSubqueryInWhereClause) {
    // BUG: Scalar subquery in WHERE clause fails with
    // "scalar subquery evaluation requires SubqueryContext".
    // The WHERE predicate is pushed to a Filter or SeqScan that lacks SubqueryContext.
    // Expected: should return alice (user whose id matches the subquery result).
    auto result = engine_->execute(
        "SELECT users.name FROM users "
        "WHERE users.id = (SELECT orders.user_id FROM orders WHERE orders.id = 100)");
    if (result.has_value()) {
        // If fixed, verify correctness.
        ASSERT_EQ(result->rows.size(), 1u);
        EXPECT_EQ(result->rows[0][0].as_string(), "alice");
    } else {
        // Record as known failure — scalar subquery in WHERE is broken.
        EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED)
            << "Unexpected error: " << result.error().message;
    }
}

// =============================================================================
// CTE edge cases
// =============================================================================

TEST_F(QA_Subquery, CTEReferencedMultipleTimesInJoin) {
    // CTE used twice in the same query: as both sides of a self-join.
    auto qr = exec_ok(
        "WITH eng AS (SELECT users.id, users.name FROM users WHERE users.dept_id = 10) "
        "SELECT e1.name, e2.name FROM eng AS e1 JOIN eng AS e2 ON e1.id = e2.id");

    // Should produce: (alice, alice), (charlie, charlie)
    EXPECT_EQ(qr.rows.size(), 2u);
}

TEST_F(QA_Subquery, CTEShadowsPhysicalTable) {
    // CTE with the same name as an existing table should shadow the table.
    auto qr = exec_ok(
        "WITH users AS (SELECT departments.id, departments.dept_name AS name FROM departments) "
        "SELECT users.name FROM users");

    auto names = collect_column_strings(qr, 0);
    // Should return department names, not user names.
    EXPECT_EQ(names.size(), 3u);
    EXPECT_TRUE(names.count("engineering"));
    EXPECT_TRUE(names.count("sales"));
    EXPECT_TRUE(names.count("hr"));
}

TEST_F(QA_Subquery, MultipleCTEsInSingleQuery) {
    // Multiple CTEs defined in the same WITH clause.
    auto qr = exec_ok(
        "WITH eng_users AS (SELECT users.id, users.name FROM users WHERE users.dept_id = 10), "
        "big_orders AS (SELECT orders.user_id, orders.amount FROM orders WHERE orders.amount > 400) "
        "SELECT eng_users.name, big_orders.amount "
        "FROM eng_users JOIN big_orders ON eng_users.id = big_orders.user_id");

    // alice (id=1) has an order with amount=500.
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
    EXPECT_EQ(qr.rows[0][1].as_int32(), 500);
}

TEST_F(QA_Subquery, CTEWithNoRows) {
    // CTE that produces zero rows.
    auto qr = exec_ok(
        "WITH no_users AS (SELECT users.id, users.name FROM users WHERE users.id = 9999) "
        "SELECT no_users.name FROM no_users");

    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QA_Subquery, CTEWithAggregation) {
    // CTE that contains aggregation.
    auto qr = exec_ok(
        "WITH order_counts AS (SELECT orders.user_id, COUNT(*) AS cnt FROM orders "
        "GROUP BY orders.user_id) "
        "SELECT users.name, order_counts.cnt "
        "FROM users JOIN order_counts ON users.id = order_counts.user_id "
        "ORDER BY order_counts.cnt DESC");

    ASSERT_EQ(qr.rows.size(), 2u);
    // alice has 2 orders, charlie has 1.
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 2);
    EXPECT_EQ(qr.rows[1][0].as_string(), "charlie");
    EXPECT_EQ(qr.rows[1][1].as_int64(), 1);
}

// =============================================================================
// Derived table (FROM subquery) edge cases
// =============================================================================

TEST_F(QA_Subquery, DerivedTableEmpty) {
    auto qr = exec_ok("SELECT sub.name FROM "
                      "(SELECT users.name FROM users WHERE users.id = 9999) AS sub");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QA_Subquery, DerivedTableWithOrderBy) {
    auto qr = exec_ok("SELECT sub.name FROM "
                      "(SELECT users.name, users.id FROM users ORDER BY users.id) AS sub");
    ASSERT_EQ(qr.rows.size(), 4u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
    EXPECT_EQ(qr.rows[1][0].as_string(), "bob");
    EXPECT_EQ(qr.rows[2][0].as_string(), "charlie");
    EXPECT_EQ(qr.rows[3][0].as_string(), "diana");
}

TEST_F(QA_Subquery, DerivedTableJoinedWithTable) {
    auto qr = exec_ok("SELECT users.name, sub.total FROM users JOIN "
                      "(SELECT orders.user_id, SUM(orders.amount) AS total FROM orders "
                      "GROUP BY orders.user_id) AS sub "
                      "ON users.id = sub.user_id "
                      "ORDER BY sub.total DESC");

    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 800);
}

// =============================================================================
// NULL handling in subqueries
// =============================================================================

TEST_F(QA_Subquery, ExistsWithNullJoinKey) {
    // User with NULL dept_id.
    exec_ok("INSERT INTO users VALUES (5, 'eve', NULL)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM departments WHERE departments.id = users.dept_id)");

    auto names = collect_column_strings(qr, 0);
    // eve has dept_id=NULL, so the comparison NULL=departments.id yields NULL → no match.
    // eve should NOT be in the result.
    EXPECT_FALSE(names.count("eve"));
    EXPECT_EQ(names.size(), 4u); // alice, bob, charlie, diana all have valid dept_ids.
}

TEST_F(QA_Subquery, NotExistsWithNullJoinKey) {
    exec_ok("INSERT INTO users VALUES (5, 'eve', NULL)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE NOT EXISTS (SELECT 1 FROM departments WHERE departments.id = users.dept_id)");

    auto names = collect_column_strings(qr, 0);
    // eve's NULL dept_id means departments.id = NULL is never true, so NOT EXISTS is true.
    EXPECT_TRUE(names.count("eve"));
}

TEST_F(QA_Subquery, ScalarSubqueryReturnsNullAggregateOnEmpty) {
    // SUM over empty set returns NULL.
    auto qr = exec_ok("SELECT (SELECT SUM(orders.amount) FROM orders WHERE orders.id = 9999) "
                      "FROM users WHERE users.id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_TRUE(qr.rows[0][0].is_null());
}

// =============================================================================
// Combining subquery types
// =============================================================================

TEST_F(QA_Subquery, ExistsAndInSubqueryTogether) {
    // Use both EXISTS and IN in the same WHERE.
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id) "
        "AND users.dept_id IN (SELECT departments.id FROM departments "
        "WHERE departments.dept_name = 'engineering')");

    auto names = collect_column_strings(qr, 0);
    // Users with orders (alice, charlie) AND in engineering dept (10).
    // Both alice and charlie are in dept 10.
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

TEST_F(QA_Subquery, ExistsWithCTE) {
    auto qr = exec_ok(
        "WITH big_orders AS (SELECT orders.user_id FROM orders WHERE orders.amount >= 300) "
        "SELECT users.name FROM users "
        "WHERE EXISTS (SELECT 1 FROM big_orders WHERE big_orders.user_id = users.id)");

    auto names = collect_column_strings(qr, 0);
    // alice has orders 500 and 300 (both >= 300), charlie has 200 (< 300).
    EXPECT_EQ(names.size(), 1u);
    EXPECT_TRUE(names.count("alice"));
}

TEST_F(QA_Subquery, InSubqueryWithCTE) {
    auto qr = exec_ok(
        "WITH active_dept AS (SELECT departments.id FROM departments "
        "WHERE departments.dept_name = 'engineering') "
        "SELECT users.name FROM users "
        "WHERE users.dept_id IN (SELECT active_dept.id FROM active_dept)");

    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

// =============================================================================
// Stress tests
// =============================================================================

TEST_F(QA_Subquery, InSubqueryLargeResult) {
    // Insert many rows into orders, then do IN subquery.
    exec_ok("CREATE TABLE many_ids (val INT)");
    for (int i = 0; i < 200; ++i) {
        exec_ok("INSERT INTO many_ids VALUES (" + std::to_string(i) + ")");
    }

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE users.id IN (SELECT many_ids.val FROM many_ids)");

    auto names = collect_column_strings(qr, 0);
    // Users 1-4 are all in the range 0-199.
    EXPECT_EQ(names.size(), 4u);
}

TEST_F(QA_Subquery, ExistsLargeRightSide) {
    // EXISTS with a large inner table.
    exec_ok("CREATE TABLE big_table (user_id INT, data INT)");
    for (int i = 0; i < 200; ++i) {
        exec_ok("INSERT INTO big_table VALUES (" + std::to_string(i % 5) + ", " +
                std::to_string(i) + ")");
    }

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM big_table WHERE big_table.user_id = users.id)");

    auto names = collect_column_strings(qr, 0);
    // Users 1-4 all have matching entries (i % 5 produces 0,1,2,3,4).
    EXPECT_EQ(names.size(), 4u);
}

TEST_F(QA_Subquery, ScalarSubqueryMultipleRows) {
    // Multiple scalar subqueries for different outer rows.
    auto qr = exec_ok("SELECT users.name, (SELECT COUNT(*) FROM orders) FROM users ORDER BY users.id");

    ASSERT_EQ(qr.rows.size(), 4u);
    // Every row should see count=3.
    for (const auto& row : qr.rows) {
        EXPECT_EQ(row[1].as_int64(), 3);
    }
}

// =============================================================================
// Acceptance criteria verification
// =============================================================================

TEST_F(QA_Subquery, AC_InSubqueryCorrectResults) {
    // AC: IN (SELECT ...) returns correct results.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE users.id IN (SELECT orders.user_id FROM orders)");
    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

TEST_F(QA_Subquery, AC_ExistsFiltersCorrectly) {
    // AC: EXISTS/NOT EXISTS filters correctly.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)");
    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));

    auto qr2 = exec_ok("SELECT users.name FROM users "
                       "WHERE NOT EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)");
    auto names2 = collect_column_strings(qr2, 0);
    EXPECT_EQ(names2.size(), 2u);
    EXPECT_TRUE(names2.count("bob"));
    EXPECT_TRUE(names2.count("diana"));
}

TEST_F(QA_Subquery, AC_ScalarSubqueryErrorsOnMultipleRows) {
    // AC: Scalar subqueries return single values (error on multiple rows).
    auto qr = exec_ok("SELECT (SELECT COUNT(*) FROM orders) FROM users WHERE users.id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int64(), 3);

    exec_error(
        "SELECT (SELECT orders.amount FROM orders) FROM users WHERE users.id = 1",
        StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_Subquery, AC_CTEReferencedMultipleTimes) {
    // AC: CTEs can be referenced multiple times in main query.
    auto qr = exec_ok(
        "WITH eng AS (SELECT users.id, users.name FROM users WHERE users.dept_id = 10) "
        "SELECT e1.name, e2.name FROM eng AS e1 JOIN eng AS e2 ON e1.id = e2.id "
        "ORDER BY e1.id");

    ASSERT_EQ(qr.rows.size(), 2u);
    // Self-join should produce same name pairs.
    EXPECT_EQ(qr.rows[0][0].as_string(), qr.rows[0][1].as_string());
    EXPECT_EQ(qr.rows[1][0].as_string(), qr.rows[1][1].as_string());
}

TEST_F(QA_Subquery, AC_CorrelatedSubqueriesDecorrelated) {
    // AC: Correlated subqueries are decorrelated into joins when possible.
    // Both of these should work via semi/anti join rewriting.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)");
    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);

    auto qr2 = exec_ok("SELECT users.name FROM users "
                       "WHERE NOT EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)");
    auto names2 = collect_column_strings(qr2, 0);
    EXPECT_EQ(names2.size(), 2u);
}

TEST_F(QA_Subquery, AC_UnitTestsForEachSubqueryType) {
    // Verify that all subquery types work end-to-end.

    // EXISTS
    auto r1 = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)");
    EXPECT_GT(r1.rows.size(), 0u);

    // NOT EXISTS
    auto r2 = exec_ok("SELECT users.name FROM users "
                      "WHERE NOT EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)");
    EXPECT_GT(r2.rows.size(), 0u);

    // IN (subquery)
    auto r3 = exec_ok("SELECT users.name FROM users "
                      "WHERE users.id IN (SELECT orders.user_id FROM orders)");
    EXPECT_GT(r3.rows.size(), 0u);

    // NOT IN (subquery)
    auto r4 = exec_ok("SELECT users.name FROM users "
                      "WHERE users.id NOT IN (SELECT orders.user_id FROM orders)");
    EXPECT_GT(r4.rows.size(), 0u);

    // Scalar subquery in SELECT
    auto r5 = exec_ok("SELECT users.name, (SELECT COUNT(*) FROM orders) FROM users WHERE users.id = 1");
    EXPECT_EQ(r5.rows.size(), 1u);

    // Scalar subquery in WHERE
    auto r6 = exec_ok("SELECT users.name FROM users "
                      "WHERE users.id = (SELECT orders.user_id FROM orders WHERE orders.id = 100)");
    EXPECT_EQ(r6.rows.size(), 1u);

    // Derived table
    auto r7 = exec_ok("SELECT sub.name FROM (SELECT users.name FROM users) AS sub");
    EXPECT_EQ(r7.rows.size(), 4u);

    // CTE
    auto r8 = exec_ok("WITH cte AS (SELECT users.name FROM users) SELECT cte.name FROM cte");
    EXPECT_EQ(r8.rows.size(), 4u);
}
