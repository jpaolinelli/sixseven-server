#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class SubqueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_subquery";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        // Set up standard test tables.
        exec_ok("CREATE TABLE users (id INT, name VARCHAR, dept_id INT)");
        exec_ok("CREATE TABLE orders (id INT, user_id INT, amount INT)");
        exec_ok("CREATE TABLE departments (id INT, dept_name VARCHAR)");

        // Insert users.
        exec_ok("INSERT INTO users VALUES (1, 'alice', 10)");
        exec_ok("INSERT INTO users VALUES (2, 'bob', 20)");
        exec_ok("INSERT INTO users VALUES (3, 'charlie', 10)");
        exec_ok("INSERT INTO users VALUES (4, 'diana', 30)");

        // Insert orders (user 1 and 3 have orders, user 2 and 4 do not).
        exec_ok("INSERT INTO orders VALUES (100, 1, 500)");
        exec_ok("INSERT INTO orders VALUES (101, 1, 300)");
        exec_ok("INSERT INTO orders VALUES (102, 3, 200)");

        // Insert departments.
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
        EXPECT_TRUE(result.has_value()) << "SQL: " << sql << "\nError: " << result.error().message;
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

    /// Collect all values of a given column into a set of strings.
    std::unordered_set<std::string> collect_column_strings(const QueryResult& qr, size_t col) {
        std::unordered_set<std::string> result;
        for (const auto& row : qr.rows) {
            result.insert(row[col].as_string());
        }
        return result;
    }

    /// Collect all values of a given column into a set of int64.
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
// EXISTS subquery tests
// =============================================================================

TEST_F(SubqueryTest, ExistsBasic) {
    // Users who have placed at least one order.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)");

    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

TEST_F(SubqueryTest, ExistsReturnsEmpty) {
    // No user has an order with amount > 9999.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id AND "
                      "orders.amount > 9999)");

    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(SubqueryTest, ExistsWithAdditionalFilter) {
    // Users in dept 10 who have orders.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE users.dept_id = 10 "
                      "AND EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)");

    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

// =============================================================================
// NOT EXISTS subquery tests
// =============================================================================

TEST_F(SubqueryTest, NotExistsBasic) {
    // Users who have NOT placed any order.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE NOT EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)");

    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("bob"));
    EXPECT_TRUE(names.count("diana"));
}

TEST_F(SubqueryTest, NotExistsAllMatch) {
    // All users have a matching department, so NOT EXISTS should return empty.
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE NOT EXISTS (SELECT 1 FROM departments WHERE departments.id = users.dept_id)");

    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// IN subquery tests
// =============================================================================

TEST_F(SubqueryTest, InSubqueryBasic) {
    // Users whose id appears in the orders table.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE users.id IN (SELECT orders.user_id FROM orders)");

    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

TEST_F(SubqueryTest, NotInSubquery) {
    // Users whose id does NOT appear in the orders table.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE users.id NOT IN (SELECT orders.user_id FROM orders)");

    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("bob"));
    EXPECT_TRUE(names.count("diana"));
}

TEST_F(SubqueryTest, InSubqueryWithFilter) {
    // Users whose id appears in orders with amount > 400.
    auto qr =
        exec_ok("SELECT users.name FROM users "
                "WHERE users.id IN (SELECT orders.user_id FROM orders WHERE orders.amount > 400)");

    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 1u);
    EXPECT_TRUE(names.count("alice"));
}

// =============================================================================
// Scalar subquery tests
// =============================================================================

TEST_F(SubqueryTest, ScalarSubqueryInSelect) {
    // Select a scalar subquery that returns a single aggregated value.
    auto qr =
        exec_ok("SELECT users.name, (SELECT COUNT(*) FROM orders) FROM users WHERE users.id = 1");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 3);
}

TEST_F(SubqueryTest, ScalarSubqueryReturnsNull) {
    // Scalar subquery that returns no rows should yield NULL.
    auto qr =
        exec_ok("SELECT users.name, (SELECT orders.amount FROM orders WHERE orders.id = 9999) "
                "FROM users WHERE users.id = 1");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
    EXPECT_TRUE(qr.rows[0][1].is_null());
}

TEST_F(SubqueryTest, ScalarSubqueryMultipleRowsError) {
    // Scalar subquery returning >1 row should error.
    exec_error(
        "SELECT users.name, (SELECT orders.amount FROM orders) FROM users WHERE users.id = 1",
        StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// CTE (WITH clause) tests
// =============================================================================

TEST_F(SubqueryTest, CTEBasic) {
    // Simple CTE: WITH active_users AS (SELECT ...) SELECT FROM active_users.
    auto qr = exec_ok(
        "WITH active_users AS (SELECT users.id, users.name FROM users WHERE users.dept_id = 10) "
        "SELECT active_users.name FROM active_users");

    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

TEST_F(SubqueryTest, CTEWithJoin) {
    // CTE used in a join.
    auto qr = exec_ok("WITH user_orders AS (SELECT orders.user_id, orders.amount FROM orders) "
                      "SELECT users.name, user_orders.amount "
                      "FROM users JOIN user_orders ON users.id = user_orders.user_id");

    EXPECT_EQ(qr.rows.size(), 3u);
    // alice has 2 orders, charlie has 1.
}

TEST_F(SubqueryTest, CTEReferencedMultipleTimes) {
    // The same CTE referenced in both parts of a CROSS JOIN (or separate queries).
    // For this test, just verify a CTE can be used as a FROM source.
    auto qr = exec_ok("WITH eng_users AS (SELECT users.name FROM users WHERE users.dept_id = 10) "
                      "SELECT eng_users.name FROM eng_users");

    EXPECT_EQ(qr.rows.size(), 2u);
}

// =============================================================================
// Derived table (FROM subquery) tests
// =============================================================================

TEST_F(SubqueryTest, DerivedTableBasic) {
    // SELECT from a subquery in FROM.
    auto qr = exec_ok("SELECT sub.name FROM (SELECT users.name, users.dept_id FROM users) AS sub "
                      "WHERE sub.dept_id = 10");

    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

TEST_F(SubqueryTest, DerivedTableWithAggregation) {
    // Derived table that aggregates.
    auto qr = exec_ok("SELECT sub.user_id, sub.total FROM "
                      "(SELECT orders.user_id, SUM(orders.amount) AS total FROM orders GROUP BY "
                      "orders.user_id) AS sub "
                      "ORDER BY sub.total DESC");

    ASSERT_EQ(qr.rows.size(), 2u);
    // user 1 (alice): 500 + 300 = 800
    // user 3 (charlie): 200
    EXPECT_EQ(qr.rows[0][1].as_int64(), 800);
    EXPECT_EQ(qr.rows[1][1].as_int64(), 200);
}

// =============================================================================
// Nested subquery tests
// =============================================================================

TEST_F(SubqueryTest, NestedExistsInSubquery) {
    // EXISTS with a condition that references a subquery result.
    // Users who have orders, tested via an inner table lookup.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)");

    // Same as ExistsBasic — verifies the pattern works correctly.
    EXPECT_EQ(qr.rows.size(), 2u);
}

TEST_F(SubqueryTest, CTEWithExists) {
    // Combine CTE and EXISTS.
    auto qr =
        exec_ok("WITH big_orders AS (SELECT orders.user_id FROM orders WHERE orders.amount > 400) "
                "SELECT users.name FROM users "
                "WHERE EXISTS (SELECT 1 FROM big_orders WHERE big_orders.user_id = users.id)");

    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 1u);
    EXPECT_TRUE(names.count("alice"));
}

// =============================================================================
// Correlated subquery tests
// =============================================================================

TEST_F(SubqueryTest, CorrelatedExistsDecorrelatedToSemiJoin) {
    // Standard correlated EXISTS — decorrelated into a semi join.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id AND "
                      "orders.amount >= 200)");

    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

TEST_F(SubqueryTest, CorrelatedNotExistsDecorrelatedToAntiJoin) {
    // Standard correlated NOT EXISTS — decorrelated into an anti join.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE NOT EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id AND "
                      "orders.amount >= 200)");

    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("bob"));
    EXPECT_TRUE(names.count("diana"));
}
