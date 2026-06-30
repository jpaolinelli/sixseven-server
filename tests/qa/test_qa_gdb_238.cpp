#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "test_qa_helpers.h"

using namespace sixseven;

// =============================================================================
// QA Adversarial Tests for GDB-238:
//   Fix correlated EXISTS inside OR combined with IN subquery.
//
// The fix modifies eval_exists() to handle correlated subqueries by manually
// scanning the inner table and evaluating the WHERE against combined
// outer+inner tuples when binding fails due to outer column references.
// =============================================================================

class QA_GDB238 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_238";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        // Register the default database (id=1); a bare Catalog only has the
        // system database (id=2), so CREATE TABLE below would otherwise fail
        // "database with id 1 not found" and every test would die at SetUp
        // (GDB-713 / GDB-1277 bootstrap).
        bootstrap_qa_catalog(catalog_);

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

    void exec_error(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "SQL should have failed: " << sql;
    }

    std::unordered_set<std::string> collect_names(const QueryResult& qr, size_t col = 0) {
        std::unordered_set<std::string> result;
        for (const auto& row : qr.rows) {
            result.insert(row[col].as_string());
        }
        return result;
    }

    std::unordered_set<int64_t> collect_ints(const QueryResult& qr, size_t col = 0) {
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
// Acceptance Criteria: Original bug case
// =============================================================================

TEST_F(QA_GDB238, OriginalBugCase_ExistsInsideOrWithIn) {
    // Exact reproduction of the bug from the ticket.
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE users.id > 0 "
        "AND (users.dept_id = 10 OR EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)) "
        "AND users.dept_id IN (SELECT departments.id FROM departments)");

    auto names = collect_names(qr);
    // alice (dept 10 → OR true), charlie (dept 10 → OR true).
    // bob: dept 20, no orders → OR false. diana: dept 30, no orders → OR false.
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

// =============================================================================
// Correlated EXISTS inside OR: Variations
// =============================================================================

TEST_F(QA_GDB238, ExistsInsideOr_RightSide) {
    // EXISTS on the right side of OR, plain condition on left.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE (users.dept_id = 20 OR EXISTS (SELECT 1 FROM orders WHERE "
                      "orders.user_id = users.id))");

    auto names = collect_names(qr);
    // alice: has orders → EXISTS true → OR true
    // bob: dept 20 → OR true (left side)
    // charlie: has orders → EXISTS true → OR true
    // diana: dept 30, no orders → OR false
    EXPECT_EQ(names.size(), 3u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("bob"));
    EXPECT_TRUE(names.count("charlie"));
}

TEST_F(QA_GDB238, ExistsInsideOr_LeftSide) {
    // EXISTS on the left side of OR.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE (EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id) OR "
                      "users.dept_id = 30)");

    auto names = collect_names(qr);
    // alice: has orders → true. bob: no orders, dept 20 → false.
    // charlie: has orders → true. diana: no orders, dept 30 → true.
    EXPECT_EQ(names.size(), 3u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
    EXPECT_TRUE(names.count("diana"));
}

TEST_F(QA_GDB238, ExistsInsideOr_BothSidesAreExists) {
    // Both sides of OR are correlated EXISTS.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE (EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id) "
                      "    OR EXISTS (SELECT 1 FROM departments WHERE departments.id = "
                      "users.dept_id AND departments.dept_name = 'hr'))");

    auto names = collect_names(qr);
    // alice: has orders → true. bob: no orders, dept 20 (sales) → false.
    // charlie: has orders → true. diana: no orders, dept 30 (hr) → true.
    EXPECT_EQ(names.size(), 3u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
    EXPECT_TRUE(names.count("diana"));
}

TEST_F(QA_GDB238, ExistsInsideOr_AllMatch) {
    // OR condition where everyone matches either side.
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE (users.id > 0 OR EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id))");

    auto names = collect_names(qr);
    // users.id > 0 is true for all 4 users → short circuit, all match.
    EXPECT_EQ(names.size(), 4u);
}

TEST_F(QA_GDB238, ExistsInsideOr_NoneMatch) {
    // OR condition where nobody matches either side.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE (users.id > 999 OR EXISTS (SELECT 1 FROM orders WHERE orders.user_id "
                      "= users.id AND orders.amount > 99999))");

    auto names = collect_names(qr);
    // No user has id > 999, and no order has amount > 99999.
    EXPECT_EQ(names.size(), 0u);
}

// =============================================================================
// NOT EXISTS inside OR
// =============================================================================

TEST_F(QA_GDB238, NotExistsInsideOr) {
    // NOT EXISTS inside OR: users who either have dept 10 OR have no orders.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE (users.dept_id = 10 OR NOT EXISTS (SELECT 1 FROM orders WHERE "
                      "orders.user_id = users.id))");

    auto names = collect_names(qr);
    // alice: dept 10 → true. bob: no orders → NOT EXISTS true.
    // charlie: dept 10 → true. diana: no orders → NOT EXISTS true.
    EXPECT_EQ(names.size(), 4u);
}

TEST_F(QA_GDB238, NotExistsInsideOr_CombinedWithIn) {
    // NOT EXISTS inside OR combined with IN subquery via AND.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE (users.dept_id = 10 OR NOT EXISTS (SELECT 1 FROM orders WHERE "
                      "orders.user_id = users.id)) "
                      "AND users.dept_id IN (SELECT departments.id FROM departments WHERE "
                      "departments.dept_name = 'sales')");

    auto names = collect_names(qr);
    // OR: alice (dept 10), bob (no orders), charlie (dept 10), diana (no orders) → all pass OR.
    // IN sales (dept 20): only bob.
    EXPECT_EQ(names.size(), 1u);
    EXPECT_TRUE(names.count("bob"));
}

// =============================================================================
// Correlated EXISTS with empty inner table
// =============================================================================

TEST_F(QA_GDB238, ExistsInsideOr_EmptyInnerTable) {
    exec_ok("CREATE TABLE empty_tbl (id INT, ref_id INT)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE (users.dept_id = 10 OR EXISTS (SELECT 1 FROM empty_tbl WHERE "
                      "empty_tbl.ref_id = users.id))");

    auto names = collect_names(qr);
    // Empty table → EXISTS always false. Only dept 10 matches: alice, charlie.
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

// =============================================================================
// Correlated EXISTS with no WHERE clause in subquery
// =============================================================================

TEST_F(QA_GDB238, ExistsInsideOr_NoWhereInSubquery) {
    // EXISTS without WHERE: any row in orders makes it true (non-correlated).
    // Since orders has rows, EXISTS is always true for all users.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE (users.dept_id = 999 OR EXISTS (SELECT 1 FROM orders))");

    auto names = collect_names(qr);
    // Non-correlated EXISTS with rows → always true → all users match.
    EXPECT_EQ(names.size(), 4u);
}

TEST_F(QA_GDB238, ExistsInsideOr_NoWhereInSubquery_EmptyTable) {
    exec_ok("CREATE TABLE empty_tbl2 (id INT)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE (users.dept_id = 10 OR EXISTS (SELECT 1 FROM empty_tbl2))");

    auto names = collect_names(qr);
    // Empty table with no WHERE → EXISTS false. Only dept 10: alice, charlie.
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

// =============================================================================
// Column name collision between outer and inner tables
// =============================================================================

TEST_F(QA_GDB238, ExistsInsideOr_ColumnNameCollision) {
    // Both tables have 'id' column. The combined schema must disambiguate.
    exec_ok("CREATE TABLE items (id INT, owner_id INT)");
    exec_ok("INSERT INTO items VALUES (1, 1)");
    exec_ok("INSERT INTO items VALUES (2, 3)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE (users.dept_id = 30 OR EXISTS (SELECT 1 FROM items WHERE "
                      "items.owner_id = users.id))");

    auto names = collect_names(qr);
    // alice: items.owner_id=1 matches users.id=1 → EXISTS true.
    // bob: no item owner, dept 20 → false.
    // charlie: items.owner_id=3 matches users.id=3 → EXISTS true.
    // diana: no item owner, dept 30 → true.
    EXPECT_EQ(names.size(), 3u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
    EXPECT_TRUE(names.count("diana"));
}

// =============================================================================
// Aliased inner table in EXISTS subquery
// =============================================================================

TEST_F(QA_GDB238, ExistsInsideOr_AliasedInnerTable) {
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE (users.dept_id = 30 OR EXISTS (SELECT 1 FROM orders o WHERE o.user_id = users.id))");

    auto names = collect_names(qr);
    // alice: has orders → true. bob: no orders, dept 20 → false.
    // charlie: has orders → true. diana: dept 30 → true.
    EXPECT_EQ(names.size(), 3u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
    EXPECT_TRUE(names.count("diana"));
}

// =============================================================================
// Nested boolean expressions: OR inside AND inside OR
// =============================================================================

TEST_F(QA_GDB238, ExistsInsideOr_NestedAndInsideOr) {
    // (condition AND EXISTS(...)) OR another_condition
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE (users.dept_id = 10 AND EXISTS (SELECT 1 FROM orders WHERE "
                      "orders.user_id = users.id)) "
                      "OR users.dept_id = 30");

    auto names = collect_names(qr);
    // Left of OR: dept 10 AND has orders → alice (dept 10, orders), charlie (dept 10, orders).
    // Right of OR: dept 30 → diana.
    EXPECT_EQ(names.size(), 3u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
    EXPECT_TRUE(names.count("diana"));
}

// =============================================================================
// EXISTS with multiple correlation conditions
// =============================================================================

TEST_F(QA_GDB238, ExistsInsideOr_MultipleCorrelationConditions) {
    // Correlated EXISTS with AND in the subquery WHERE.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE (users.dept_id = 30 OR EXISTS (SELECT 1 FROM orders WHERE "
                      "orders.user_id = users.id AND orders.amount > 250))");

    auto names = collect_names(qr);
    // alice: orders with amount > 250 → 500, 300 → EXISTS true.
    // bob: no orders → false, dept 20 → false.
    // charlie: orders amount 200, not > 250 → EXISTS false, dept 10 → false.
    // diana: no orders → false, dept 30 → true.
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("diana"));
}

// =============================================================================
// Stress: EXISTS inside OR with many outer rows
// =============================================================================

TEST_F(QA_GDB238, ExistsInsideOr_ManyOuterRows) {
    exec_ok("CREATE TABLE big_users (id INT, name VARCHAR, dept_id INT)");
    exec_ok("CREATE TABLE big_orders (id INT, user_id INT)");

    // Insert 100 users.
    for (int i = 1; i <= 100; ++i) {
        std::string name = "user" + std::to_string(i);
        int dept = (i % 3 == 0) ? 10 : 20;
        exec_ok("INSERT INTO big_users VALUES (" + std::to_string(i) + ", '" + name + "', " +
                std::to_string(dept) + ")");
    }

    // Insert orders for even-numbered users only.
    for (int i = 2; i <= 100; i += 2) {
        exec_ok("INSERT INTO big_orders VALUES (" + std::to_string(1000 + i) + ", " +
                std::to_string(i) + ")");
    }

    auto qr = exec_ok("SELECT big_users.name FROM big_users "
                      "WHERE (big_users.dept_id = 10 OR EXISTS (SELECT 1 FROM big_orders WHERE "
                      "big_orders.user_id = big_users.id))");

    // dept 10: i%3==0 → 33 users (3,6,9,...,99).
    // Even users: 50 users (2,4,6,...,100).
    // Union (dept 10 OR has orders): dept-10 users + even-non-dept10 users.
    // Dept 10 users: 3,6,9,12,...,99 (33 users).
    // Even users NOT in dept 10: even i where i%3!=0. Count those.
    // Total = dept10 + even_non_dept10.
    // Even and dept10: i divisible by both 2 and 3 = i%6==0 → 16 users (6,12,...,96).
    // Even NOT dept10: 50 - 16 = 34.
    // Total: 33 + 34 = 67.
    EXPECT_EQ(qr.rows.size(), 67u);
}

// =============================================================================
// EXISTS inside OR with NULL values in outer columns
// =============================================================================

TEST_F(QA_GDB238, ExistsInsideOr_NullOuterValue) {
    exec_ok("INSERT INTO users VALUES (5, 'eve', NULL)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE (users.dept_id = 10 OR EXISTS (SELECT 1 FROM orders WHERE "
                      "orders.user_id = users.id))");

    auto names = collect_names(qr);
    // alice: dept 10 → true. bob: dept 20, no orders → false.
    // charlie: dept 10 → true. diana: dept 30, no orders → false.
    // eve: dept NULL (NULL = 10 → NULL → false), no orders → false.
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

TEST_F(QA_GDB238, ExistsInsideOr_NullInInnerTable) {
    // Inner table has NULL values in the correlation column.
    exec_ok("CREATE TABLE sparse_orders (id INT, user_id INT)");
    exec_ok("INSERT INTO sparse_orders VALUES (1, NULL)");
    exec_ok("INSERT INTO sparse_orders VALUES (2, 1)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE (users.dept_id = 30 OR EXISTS (SELECT 1 FROM sparse_orders WHERE "
                      "sparse_orders.user_id = users.id))");

    auto names = collect_names(qr);
    // alice: sparse_orders has user_id=1 match → true.
    // bob: no match (NULL != 2) → false, dept 20 → false.
    // charlie: no match (NULL != 3, 1 != 3) → false, dept 10 → false.
    // diana: dept 30 → true.
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("diana"));
}

// =============================================================================
// EXISTS inside OR where OR is nested inside another AND
// =============================================================================

TEST_F(QA_GDB238, ExistsInsideOr_AndOutsideBothSides) {
    // AND on both sides: cond1 AND (cond2 OR EXISTS) AND cond3
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE users.id >= 2 "
        "AND (users.dept_id = 10 OR EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)) "
        "AND users.id <= 3");

    auto names = collect_names(qr);
    // id >= 2 AND id <= 3: bob (2), charlie (3).
    // OR: bob (dept 20, no orders → false). charlie (dept 10 → true).
    EXPECT_EQ(names.size(), 1u);
    EXPECT_TRUE(names.count("charlie"));
}

// =============================================================================
// Top-level correlated EXISTS (not inside OR) - regression check
// =============================================================================

TEST_F(QA_GDB238, TopLevelCorrelatedExists_Regression) {
    // Top-level EXISTS with AND should still be handled by join rewrite, not eval_exists.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)");

    auto names = collect_names(qr);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

TEST_F(QA_GDB238, TopLevelNotExists_Regression) {
    // Top-level NOT EXISTS should still be handled by ANTI join rewrite.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE NOT EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)");

    auto names = collect_names(qr);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("bob"));
    EXPECT_TRUE(names.count("diana"));
}

TEST_F(QA_GDB238, ExistsAndIn_Regression) {
    // Regression: EXISTS AND IN together at top level.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id) "
                      "AND users.dept_id IN (SELECT departments.id FROM departments "
                      "WHERE departments.dept_name = 'engineering')");

    auto names = collect_names(qr);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

// =============================================================================
// Non-correlated EXISTS inside OR (should use the non-correlated path)
// =============================================================================

TEST_F(QA_GDB238, NonCorrelatedExistsInsideOr) {
    // Non-correlated EXISTS (no outer references) inside OR.
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE (users.dept_id = 999 OR EXISTS (SELECT 1 FROM orders WHERE orders.amount > 400))");

    auto names = collect_names(qr);
    // Non-correlated EXISTS: orders with amount > 400 → yes (500) → true for all.
    EXPECT_EQ(names.size(), 4u);
}

TEST_F(QA_GDB238, NonCorrelatedExistsInsideOr_NoMatch) {
    // Non-correlated EXISTS returns false (no orders with huge amount).
    auto qr = exec_ok(
        "SELECT users.name FROM users "
        "WHERE (users.dept_id = 10 OR EXISTS (SELECT 1 FROM orders WHERE orders.amount > 99999))");

    auto names = collect_names(qr);
    // EXISTS false, only dept 10: alice, charlie.
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

// =============================================================================
// Multiple OR-nested EXISTS combined with AND-level IN
// =============================================================================

TEST_F(QA_GDB238, MultipleOrWithExistsCombinedAndIn) {
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE (users.dept_id = 10 OR EXISTS (SELECT 1 FROM orders WHERE "
                      "orders.user_id = users.id)) "
                      "AND users.dept_id IN (SELECT departments.id FROM departments WHERE "
                      "departments.dept_name = 'engineering')");

    auto names = collect_names(qr);
    // OR: alice (dept 10), charlie (dept 10). bob (dept 20, no orders → false).
    //     diana (dept 30, no orders → false).
    // IN engineering (dept 10): filters to alice, charlie.
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

// =============================================================================
// Correlated EXISTS with inequality correlation
// =============================================================================

TEST_F(QA_GDB238, ExistsInsideOr_InequalityCorrelation) {
    // Correlated EXISTS with > instead of =.
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE (users.dept_id = 30 OR EXISTS (SELECT 1 FROM orders WHERE "
                      "orders.user_id > users.id))");

    auto names = collect_names(qr);
    // alice (id=1): orders with user_id > 1 → user_id=3 → EXISTS true.
    // bob (id=2): orders with user_id > 2 → user_id=3 → EXISTS true.
    // charlie (id=3): orders with user_id > 3 → none → EXISTS false, dept 10 → false.
    // diana (id=4): no user_id > 4 → EXISTS false, dept 30 → true.
    EXPECT_EQ(names.size(), 3u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("bob"));
    EXPECT_TRUE(names.count("diana"));
}

// =============================================================================
// NOT EXISTS combined with NOT IN inside OR
// =============================================================================

TEST_F(QA_GDB238, NotExistsInsideOr_WithNotIn) {
    // NOT EXISTS in OR, then AND with NOT IN at top level.
    exec_ok("CREATE TABLE exclude_ids (val INT)");
    exec_ok("INSERT INTO exclude_ids VALUES (1)");
    exec_ok("INSERT INTO exclude_ids VALUES (2)");

    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE (users.dept_id = 10 OR NOT EXISTS (SELECT 1 FROM orders WHERE "
                      "orders.user_id = users.id)) "
                      "AND users.id NOT IN (SELECT exclude_ids.val FROM exclude_ids)");

    auto names = collect_names(qr);
    // OR: alice (dept 10), bob (no orders), charlie (dept 10), diana (no orders) → all.
    // NOT IN (1, 2): exclude alice (1), bob (2). Remaining: charlie (3), diana (4).
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("charlie"));
    EXPECT_TRUE(names.count("diana"));
}

// =============================================================================
// SELECT * with EXISTS inside OR (schema width stress)
// =============================================================================

TEST_F(QA_GDB238, ExistsInsideOr_SelectStar) {
    auto qr = exec_ok("SELECT * FROM users "
                      "WHERE (users.dept_id = 10 OR EXISTS (SELECT 1 FROM orders WHERE "
                      "orders.user_id = users.id))");

    // alice and charlie have dept 10, alice and charlie have orders.
    // Union: alice, charlie. (bob and diana excluded.)
    EXPECT_EQ(qr.rows.size(), 2u);
    // Verify the schema has 3 columns (id, name, dept_id).
    EXPECT_EQ(qr.column_names.size(), 3u);
}

// =============================================================================
// EXISTS inside OR with aggregate in outer query
// =============================================================================

TEST_F(QA_GDB238, ExistsInsideOr_WithCountAggregate) {
    auto qr = exec_ok("SELECT COUNT(*) FROM users "
                      "WHERE (users.dept_id = 10 OR EXISTS (SELECT 1 FROM orders WHERE "
                      "orders.user_id = users.id))");

    ASSERT_EQ(qr.rows.size(), 1u);
    // alice (dept 10), charlie (dept 10). Others: bob no orders + dept 20, diana no orders +
    // dept 30.
    auto count = qr.rows[0][0].as_int64();
    EXPECT_EQ(count, 2);
}

// =============================================================================
// Negative paths: malformed EXISTS-inside-OR queries must error cleanly, not
// crash or silently return wrong rows. These exercise the exec_error helper.
// =============================================================================

TEST_F(QA_GDB238, ExistsInsideOr_NonexistentInnerTable) {
    // EXISTS over a table that does not exist must fail to bind.
    exec_error("SELECT users.name FROM users "
               "WHERE (users.dept_id = 10 OR EXISTS (SELECT 1 FROM no_such_table))");
}

TEST_F(QA_GDB238, ExistsInsideOr_NonexistentCorrelationColumn) {
    // Correlated EXISTS referencing a column that does not exist must fail.
    exec_error("SELECT users.name FROM users "
               "WHERE (users.dept_id = 10 OR EXISTS (SELECT 1 FROM orders WHERE "
               "orders.no_such_col = users.id))");
}

TEST_F(QA_GDB238, InSubqueryMultipleColumnsRejected) {
    // The IN subquery combined with the OR-nested EXISTS must project exactly
    // one column; a two-column projection is rejected at bind time.
    exec_error("SELECT users.name FROM users "
               "WHERE users.dept_id IN "
               "(SELECT departments.id, departments.dept_name FROM departments)");
}

// =============================================================================
// Positive path exercising collect_ints: project an INT column rather than a
// name, and assert the integer id set.
// =============================================================================

TEST_F(QA_GDB238, ExistsInsideOr_ProjectIntColumn) {
    auto qr = exec_ok("SELECT users.id FROM users "
                      "WHERE (users.dept_id = 10 OR EXISTS (SELECT 1 FROM orders WHERE "
                      "orders.user_id = users.id))");

    auto ids = collect_ints(qr);
    // alice (id 1: dept 10 and has orders), charlie (id 3: dept 10 and has orders).
    EXPECT_EQ(ids.size(), 2u);
    EXPECT_TRUE(ids.count(1));
    EXPECT_TRUE(ids.count(3));
}
