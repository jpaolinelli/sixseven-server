#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// GDB-1187: Adversarial nested subquery QA tests
//
// Fixture mirrors SubqueryTest:
//   users: alice(1,dept10) bob(2,dept20) charlie(3,dept10) diana(4,dept30)
//   orders: (100,u1,500) (101,u1,300) (102,u3,200)
//   departments: 10=engineering 20=sales 30=hr
// =============================================================================

class QA_GDB1187 : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1187";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

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
        EXPECT_TRUE(result.has_value()) << "SQL: " << sql << "\nError: " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void exec_error(const std::string& sql, StatusCode expected_code) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "Expected failure for: " << sql;
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected_code)
                << "Wrong error code for: " << sql
                << "\n  got: " << result.error().message;
        }
    }

    std::unordered_set<std::string> names_col(const QueryResult& qr, size_t col = 0) {
        std::unordered_set<std::string> s;
        for (const auto& row : qr.rows) {
            s.insert(row[col].as_string());
        }
        return s;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// ---------------------------------------------------------------------------
// AC1: scalar-inside-EXISTS — verified with a discriminating exact result
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, ScalarInsideExistsExactResult) {
    // The scalar (SELECT orders.amount FROM orders WHERE orders.id = 101) = 300.
    // EXISTS passes only for users with an order whose amount > 300, i.e. alice (500).
    // charlie's order is exactly 200, which is NOT > 300 -> charlie excluded.
    // Discriminates from plain ExistsBasic which returns {alice, charlie}.
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE EXISTS (SELECT 1 FROM orders "
        "WHERE orders.user_id = users.id "
        "AND orders.amount > "
        "(SELECT orders.amount FROM orders WHERE orders.id = 101))");

    auto n = names_col(qr);
    EXPECT_EQ(n.size(), 1u);
    EXPECT_TRUE(n.count("alice"));
    EXPECT_FALSE(n.count("charlie"));
    EXPECT_FALSE(n.count("bob"));
    EXPECT_FALSE(n.count("diana"));
}

// ---------------------------------------------------------------------------
// AC2: EXISTS-inside-EXISTS — verified with discriminating exact result
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, ExistsInsideExistsExactResult) {
    // Outer EXISTS: departments with dept_name='engineering' matching users.dept_id.
    // Inner EXISTS (inside departments row scan): users has at least one order.
    // Both must hold -> alice and charlie.
    // bob (sales) and diana (hr) fail the outer EXISTS dept name check.
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE EXISTS (SELECT 1 FROM departments "
        "WHERE departments.id = users.dept_id "
        "AND departments.dept_name = 'engineering' "
        "AND EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id))");

    auto n = names_col(qr);
    EXPECT_EQ(n.size(), 2u);
    EXPECT_TRUE(n.count("alice"));
    EXPECT_TRUE(n.count("charlie"));
    EXPECT_FALSE(n.count("bob"));
    EXPECT_FALSE(n.count("diana"));
}

// ---------------------------------------------------------------------------
// AC3: scalar-inside-scalar — verified with discriminating exact result
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, ScalarInsideScalarExactResult) {
    // Innermost scalar: dept_name for dept id=10 -> 'engineering'.
    // Outer scalar: dept id where dept_name='engineering' -> 10.
    // Filter: users.dept_id = 10 -> alice and charlie.
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE users.dept_id = "
        "(SELECT departments.id FROM departments "
        "WHERE departments.dept_name = "
        "(SELECT departments.dept_name FROM departments WHERE departments.id = 10))");

    auto n = names_col(qr);
    EXPECT_EQ(n.size(), 2u);
    EXPECT_TRUE(n.count("alice"));
    EXPECT_TRUE(n.count("charlie"));
    EXPECT_FALSE(n.count("bob"));
    EXPECT_FALSE(n.count("diana"));
}

// ---------------------------------------------------------------------------
// Deeper nesting: 3 levels (scalar inside scalar inside EXISTS)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, ThreeLevelNesting_ScalarScalarExists) {
    // Level 3 (innermost scalar): 'engineering'
    // Level 2 (scalar): dept id = 10
    // Level 1 (EXISTS): users.dept_id = 10 -> alice and charlie
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE EXISTS ("
        "  SELECT 1 FROM departments "
        "  WHERE departments.id = users.dept_id "
        "  AND departments.id = ("
        "    SELECT departments.id FROM departments "
        "    WHERE departments.dept_name = ("
        "      SELECT departments.dept_name FROM departments "
        "      WHERE departments.id = 10"
        "    )"
        "  )"
        ")");

    auto n = names_col(qr);
    EXPECT_EQ(n.size(), 2u);
    EXPECT_TRUE(n.count("alice"));
    EXPECT_TRUE(n.count("charlie"));
    EXPECT_FALSE(n.count("bob"));
    EXPECT_FALSE(n.count("diana"));
}

// ---------------------------------------------------------------------------
// Deeper nesting: EXISTS inside EXISTS inside EXISTS (3 levels)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, ThreeLevelExistsInsideExistsInsideExists) {
    // L1: exists order for user
    // L2: exists dept matching user's dept_id
    // L3: exists any user in dept 10 (always true if dept 10 has users)
    // Effectively: users with orders AND in a valid dept AND dept 10 has someone.
    // alice and charlie satisfy all three.
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id "
        "AND EXISTS (SELECT 1 FROM departments WHERE departments.id = users.dept_id "
        "AND EXISTS (SELECT 1 FROM users WHERE users.dept_id = 10)))");

    auto n = names_col(qr);
    EXPECT_EQ(n.size(), 2u);
    EXPECT_TRUE(n.count("alice"));
    EXPECT_TRUE(n.count("charlie"));
    EXPECT_FALSE(n.count("bob"));
    EXPECT_FALSE(n.count("diana"));
}

// ---------------------------------------------------------------------------
// Nested + correlation to outer-outer query (correlated ref at right scope)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, CorrelatedNestedSubqueryOuterOuterRef) {
    // The innermost scalar references the outermost row (users.id), not the middle row.
    // EXISTS inner: orders where user_id = users.id AND amount > 250
    // The scalar subquery at the innermost level just returns a constant (200),
    // but crucially it is inside a correlated EXISTS that uses the outer users row.
    // alice: order 500 > 250 (YES) and order 300 > 250 (YES) -> EXISTS true
    // charlie: order 200 > 250 (NO) -> EXISTS false
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE EXISTS ("
        "  SELECT 1 FROM orders "
        "  WHERE orders.user_id = users.id "
        "  AND orders.amount > ("
        "    SELECT orders.amount FROM orders WHERE orders.id = 102"
        "  )"
        ")");
    // Order id=102 has amount=200. alice has 500>200 and 300>200 -> EXISTS true.
    // charlie has 200>200 = false -> no row matches -> EXISTS false.
    auto n = names_col(qr);
    EXPECT_EQ(n.size(), 1u);
    EXPECT_TRUE(n.count("alice"));
    EXPECT_FALSE(n.count("charlie"));
}

// ---------------------------------------------------------------------------
// Nested subquery returning EMPTY (scalar subquery -> NULL -> comparison)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, NestedScalarReturnsNullInExists) {
    // The inner scalar subquery selects from orders WHERE id=9999 (no rows) -> NULL.
    // orders.amount > NULL evaluates to NULL (unknown), so the AND in EXISTS
    // predicate is NULL -> EXISTS sees no matching row -> false for all users.
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE EXISTS ("
        "  SELECT 1 FROM orders "
        "  WHERE orders.user_id = users.id "
        "  AND orders.amount > ("
        "    SELECT orders.amount FROM orders WHERE orders.id = 9999"
        "  )"
        ")");

    EXPECT_EQ(qr.rows.size(), 0u);
}

// ---------------------------------------------------------------------------
// NOT EXISTS with nested scalar inside
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, NotExistsWithNestedScalarInside) {
    // The scalar returns amount=300 (order 101). NOT EXISTS where order amount > 300.
    // alice has order 500 > 300 -> EXISTS is true -> NOT EXISTS false -> alice excluded.
    // charlie has order 200 <= 300 -> EXISTS false -> NOT EXISTS true -> charlie included.
    // bob/diana have no orders at all -> EXISTS false -> NOT EXISTS true -> included.
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE NOT EXISTS ("
        "  SELECT 1 FROM orders "
        "  WHERE orders.user_id = users.id "
        "  AND orders.amount > ("
        "    SELECT orders.amount FROM orders WHERE orders.id = 101"
        "  )"
        ")");

    auto n = names_col(qr);
    EXPECT_EQ(n.size(), 3u);
    EXPECT_TRUE(n.count("charlie"));
    EXPECT_TRUE(n.count("bob"));
    EXPECT_TRUE(n.count("diana"));
    EXPECT_FALSE(n.count("alice"));
}

// ---------------------------------------------------------------------------
// NOT EXISTS with nested EXISTS inside
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, NotExistsWithNestedExistsInside) {
    // Outer NOT EXISTS: no dept row where dept matches user AND that dept has a user
    // with an order. Effectively: user is not in a dept that has any order-placing user.
    // dept 10: alice+charlie both have orders -> any user in dept 10 fails NOT EXISTS.
    // dept 20 (bob): bob has no orders, but EXISTS(orders for dept20 users) = false ->
    //   the whole inner EXISTS = false -> NOT EXISTS = true -> bob included.
    // dept 30 (diana): same reasoning -> diana included.
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE NOT EXISTS ("
        "  SELECT 1 FROM departments "
        "  WHERE departments.id = users.dept_id "
        "  AND EXISTS ("
        "    SELECT 1 FROM orders "
        "    WHERE orders.user_id = users.id"
        "  )"
        ")");

    auto n = names_col(qr);
    EXPECT_EQ(n.size(), 2u);
    EXPECT_TRUE(n.count("bob"));
    EXPECT_TRUE(n.count("diana"));
    EXPECT_FALSE(n.count("alice"));
    EXPECT_FALSE(n.count("charlie"));
}

// ---------------------------------------------------------------------------
// IN-inside-EXISTS: EXISTS contains an IN subquery
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, InInsideExists) {
    // EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id
    //   AND orders.amount IN (SELECT orders.amount FROM orders WHERE orders.id = 100))
    // The IN subquery returns {500}. So EXISTS passes only if user has an order with amount=500.
    // alice has order 500 -> YES. charlie has order 200 -> NO.
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE EXISTS ("
        "  SELECT 1 FROM orders "
        "  WHERE orders.user_id = users.id "
        "  AND orders.amount IN ("
        "    SELECT orders.amount FROM orders WHERE orders.id = 100"
        "  )"
        ")");

    auto n = names_col(qr);
    EXPECT_EQ(n.size(), 1u);
    EXPECT_TRUE(n.count("alice"));
    EXPECT_FALSE(n.count("charlie"));
}

// ---------------------------------------------------------------------------
// EXISTS-inside-IN: IN subquery contains an EXISTS (if supported)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, ExistsInsideInNotSupported_OrCorrect) {
    // users.id IN (SELECT orders.user_id FROM orders WHERE EXISTS(...))
    // EXISTS returns true/false for each order row; we get the user_ids of orders
    // that pass the EXISTS filter.
    // The EXISTS (SELECT 1 FROM departments WHERE departments.id = 10) is non-correlated
    // and always true -> IN returns all user_ids from orders -> {1, 3} -> alice, charlie.
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE users.id IN ("
        "  SELECT orders.user_id FROM orders "
        "  WHERE EXISTS (SELECT 1 FROM departments WHERE departments.id = 10)"
        ")");

    auto n = names_col(qr);
    EXPECT_EQ(n.size(), 2u);
    EXPECT_TRUE(n.count("alice"));
    EXPECT_TRUE(n.count("charlie"));
    EXPECT_FALSE(n.count("bob"));
    EXPECT_FALSE(n.count("diana"));
}

// ---------------------------------------------------------------------------
// Error path: nested scalar subquery returning >1 row must error, not crash
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, NestedScalarMultipleRowsErrors) {
    // The inner scalar returns ALL orders.amount (3 rows) -> must error.
    // The outer comparison should never succeed; the error must propagate cleanly.
    exec_error(
        "SELECT users.name FROM users "
        "WHERE EXISTS ("
        "  SELECT 1 FROM orders "
        "  WHERE orders.user_id = users.id "
        "  AND orders.amount > (SELECT orders.amount FROM orders)"
        ")",
        StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Error path: outermost scalar returning >1 row still errors
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, OuterScalarMultipleRowsStillErrors) {
    // The outer scalar subquery returns multiple amounts -> must error.
    exec_error(
        "SELECT users.name FROM users "
        "WHERE users.dept_id = (SELECT departments.id FROM departments)",
        StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Plain JOIN + nested subquery combination (regression guard)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, JoinCombinedWithNestedSubquery) {
    // JOIN users with orders (inner join on user_id), then filter with a nested
    // scalar in the WHERE. Confirms that plain join paths are not broken.
    // Scalar: amount of order id=101 = 300.
    // Filter: orders.amount > 300 -> only order 100 (500 by alice).
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "JOIN orders ON users.id = orders.user_id "
        "WHERE orders.amount > "
        "(SELECT orders.amount FROM orders WHERE orders.id = 101)");

    auto n = names_col(qr);
    EXPECT_EQ(n.size(), 1u);
    EXPECT_TRUE(n.count("alice"));
    EXPECT_FALSE(n.count("charlie"));
}

// ---------------------------------------------------------------------------
// Plain JOIN result set correctness (no nested subquery -- baseline regression)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, PlainJoinUnaffectedByFix) {
    // A plain INNER join should still produce the correct result after the fix.
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "JOIN orders ON users.id = orders.user_id");

    // alice has 2 orders, charlie has 1 -> 3 rows total.
    EXPECT_EQ(qr.rows.size(), 3u);
    auto n = names_col(qr);
    EXPECT_TRUE(n.count("alice"));
    EXPECT_TRUE(n.count("charlie"));
    EXPECT_FALSE(n.count("bob"));
    EXPECT_FALSE(n.count("diana"));
}

// ---------------------------------------------------------------------------
// Non-correlated scalar in SELECT list -- regression guard for projection path
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, NonCorrelatedScalarInSelectProjection) {
    // Non-correlated scalar subquery in SELECT list: (SELECT COUNT(*) FROM orders).
    // Must return 3 (all orders) for each outer row. Nested subquery fix must not
    // break this existing capability.
    auto qr = exec_ok(
        "SELECT users.name, (SELECT COUNT(*) FROM orders) "
        "FROM users WHERE users.id = 1");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 3);
}

// ---------------------------------------------------------------------------
// NOT IN with nested scalar (boundary: NOT IN with non-correlated subquery)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, NotInWithNestedScalar) {
    // users.id NOT IN (SELECT orders.user_id FROM orders WHERE orders.amount > 250)
    // The filter orders.amount > 250 yields user_ids {1} (alice's 500 and 300 both pass,
    // but user_id dedup is done by IN semantics -> set {1}).
    // NOT IN {1} -> bob(2), charlie(3), diana(4) pass. alice(1) does not.
    // Note: charlie's order 200 <= 250 so charlie's user_id not in sub result.
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE users.id NOT IN ("
        "  SELECT orders.user_id FROM orders WHERE orders.amount > 250"
        ")");

    auto n = names_col(qr);
    EXPECT_FALSE(n.count("alice"));
    // charlie(3) has only amount=200 which is <=250, so user_id=3 is NOT in the subresult.
    EXPECT_TRUE(n.count("charlie"));
    EXPECT_TRUE(n.count("bob"));
    EXPECT_TRUE(n.count("diana"));
}

// ---------------------------------------------------------------------------
// Nested subquery returns empty -> outer scalar yields NULL -> comparison is NULL
// -> WHERE filters out that row
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1187, NestedScalarNullPropagatesInWhere) {
    // users.dept_id = (SELECT departments.id FROM departments WHERE departments.id = 9999)
    // The scalar subquery has no matching row -> NULL.
    // users.dept_id = NULL -> NULL (unknown) -> WHERE excludes all rows.
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE users.dept_id = ("
        "  SELECT departments.id FROM departments WHERE departments.id = 9999"
        ")");

    EXPECT_EQ(qr.rows.size(), 0u);
}
