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

#include "test_qa_helpers.h"

using namespace sixseven;

// =============================================================================
// QA Adversarial Tests for GDB-239:
//   Fix nested CTE resolution in IN subquery planning.
//   The fixpoint loop in inject_cte_bindings must handle:
//   - Multi-level CTE chains (A→B→C)
//   - Diamond dependencies (D→B,C ; B,C→A)
//   - Empty/NULL intermediate results
//   - NOT IN / EXISTS with nested CTEs
//   - Error paths (non-existent tables, self-references)
// =============================================================================

class QA_GDB239 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_239";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        bootstrap_qa_catalog(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        // Standard tables: users, orders, departments, categories.
        exec_ok("CREATE TABLE users (id INT, name VARCHAR, dept_id INT)");
        exec_ok("CREATE TABLE orders (id INT, user_id INT, amount INT)");
        exec_ok("CREATE TABLE departments (id INT, dept_name VARCHAR)");
        exec_ok("CREATE TABLE categories (id INT, cat_name VARCHAR, parent_id INT)");

        exec_ok("INSERT INTO users VALUES (1, 'alice', 10)");
        exec_ok("INSERT INTO users VALUES (2, 'bob', 20)");
        exec_ok("INSERT INTO users VALUES (3, 'charlie', 10)");
        exec_ok("INSERT INTO users VALUES (4, 'diana', 30)");
        exec_ok("INSERT INTO users VALUES (5, 'eve', 10)");

        exec_ok("INSERT INTO orders VALUES (100, 1, 500)");
        exec_ok("INSERT INTO orders VALUES (101, 1, 300)");
        exec_ok("INSERT INTO orders VALUES (102, 3, 200)");
        exec_ok("INSERT INTO orders VALUES (103, 5, 150)");

        exec_ok("INSERT INTO departments VALUES (10, 'engineering')");
        exec_ok("INSERT INTO departments VALUES (20, 'sales')");
        exec_ok("INSERT INTO departments VALUES (30, 'hr')");

        // Categories with parent-child hierarchy for deeper nesting tests.
        exec_ok("INSERT INTO categories VALUES (1, 'root', 0)");
        exec_ok("INSERT INTO categories VALUES (2, 'sub_a', 1)");
        exec_ok("INSERT INTO categories VALUES (3, 'sub_b', 1)");
        exec_ok("INSERT INTO categories VALUES (4, 'leaf_a1', 2)");
        exec_ok("INSERT INTO categories VALUES (5, 'leaf_a2', 2)");

        // Table with NULLs.
        exec_ok("CREATE TABLE nullable_vals (val INT)");
        exec_ok("INSERT INTO nullable_vals VALUES (1)");
        exec_ok("INSERT INTO nullable_vals VALUES (NULL)");
        exec_ok("INSERT INTO nullable_vals VALUES (3)");
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
// Core fix verification: the exact bug scenario from the ticket
// =============================================================================

TEST_F(QA_GDB239, ExactTicketReproduction) {
    // The exact query from the bug ticket.
    auto qr = exec_ok("WITH base_depts AS (SELECT departments.id FROM departments "
                      "WHERE departments.dept_name = 'engineering'), "
                      "eng_users AS (SELECT users.id AS uid FROM users "
                      "WHERE users.dept_id IN (SELECT base_depts.id FROM base_depts)) "
                      "SELECT orders.id FROM orders "
                      "WHERE orders.user_id IN (SELECT eng_users.uid FROM eng_users)");

    auto ids = collect_column_ints(qr, 0);
    // Engineering users: alice (1), charlie (3), eve (5).
    // Orders by eng users: 100 (alice), 101 (alice), 102 (charlie), 103 (eve).
    EXPECT_EQ(ids.size(), 4u);
    EXPECT_TRUE(ids.count(100));
    EXPECT_TRUE(ids.count(101));
    EXPECT_TRUE(ids.count(102));
    EXPECT_TRUE(ids.count(103));
}

// =============================================================================
// Multi-level CTE chains (depth > 2)
// =============================================================================

TEST_F(QA_GDB239, ThreeLevelCTEChain) {
    // CTE chain: level1 → level2 → level3, all used in IN subqueries.
    // level1: engineering dept IDs
    // level2: user IDs in those depts
    // level3: order IDs for those users
    auto qr = exec_ok("WITH level1 AS (SELECT departments.id FROM departments "
                      "WHERE departments.dept_name = 'engineering'), "
                      "level2 AS (SELECT users.id AS uid FROM users "
                      "WHERE users.dept_id IN (SELECT level1.id FROM level1)), "
                      "level3 AS (SELECT orders.id AS oid FROM orders "
                      "WHERE orders.user_id IN (SELECT level2.uid FROM level2)) "
                      "SELECT level3.oid FROM level3");

    auto ids = collect_column_ints(qr, 0);
    EXPECT_EQ(ids.size(), 4u);
    EXPECT_TRUE(ids.count(100));
    EXPECT_TRUE(ids.count(101));
    EXPECT_TRUE(ids.count(102));
    EXPECT_TRUE(ids.count(103));
}

TEST_F(QA_GDB239, ThreeLevelCTEChainWithFilter) {
    // 3-level chain where the final CTE adds a filter.
    auto qr = exec_ok("WITH level1 AS (SELECT departments.id FROM departments "
                      "WHERE departments.dept_name = 'engineering'), "
                      "level2 AS (SELECT users.id AS uid FROM users "
                      "WHERE users.dept_id IN (SELECT level1.id FROM level1)), "
                      "level3 AS (SELECT orders.id AS oid FROM orders "
                      "WHERE orders.user_id IN (SELECT level2.uid FROM level2) "
                      "AND orders.amount > 200) "
                      "SELECT level3.oid FROM level3");

    auto ids = collect_column_ints(qr, 0);
    // Orders > 200 by eng users: 100 (500), 101 (300).
    EXPECT_EQ(ids.size(), 2u);
    EXPECT_TRUE(ids.count(100));
    EXPECT_TRUE(ids.count(101));
}

// =============================================================================
// Diamond CTE dependencies
// =============================================================================

TEST_F(QA_GDB239, DiamondDependency) {
    // Diamond: base_depts → eng_users AND base_depts → eng_orders
    // Then final query uses both eng_users and eng_orders.
    auto qr = exec_ok("WITH base_depts AS (SELECT departments.id FROM departments "
                      "WHERE departments.dept_name = 'engineering'), "
                      "eng_users AS (SELECT users.id AS uid, users.name FROM users "
                      "WHERE users.dept_id IN (SELECT base_depts.id FROM base_depts)), "
                      "eng_orders AS (SELECT orders.id AS oid, orders.user_id FROM orders "
                      "WHERE orders.user_id IN (SELECT base_depts.id FROM base_depts)) "
                      "SELECT eng_users.name FROM eng_users "
                      "WHERE eng_users.uid IN (SELECT eng_orders.user_id FROM eng_orders)");

    // eng_orders references base_depts for user_id (but dept IDs are 10,
    // not user IDs — so no orders have user_id=10). Expected: 0 rows.
    // Actually: base_depts returns {10}. eng_orders looks for orders
    // where user_id IN {10}. No user has id=10. So 0 rows.
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QA_GDB239, DiamondDependencyCorrectJoin) {
    // Diamond with correct semantics: both eng_users and eng_orders
    // derive from base_depts properly.
    auto qr = exec_ok("WITH base_depts AS (SELECT departments.id FROM departments "
                      "WHERE departments.dept_name = 'engineering'), "
                      "eng_users AS (SELECT users.id AS uid FROM users "
                      "WHERE users.dept_id IN (SELECT base_depts.id FROM base_depts)), "
                      "eng_orders AS (SELECT orders.id AS oid FROM orders "
                      "WHERE orders.user_id IN (SELECT eng_users.uid FROM eng_users)) "
                      "SELECT eng_orders.oid FROM eng_orders");

    // eng_users = {1, 3, 5}. eng_orders = orders by those users.
    auto ids = collect_column_ints(qr, 0);
    EXPECT_EQ(ids.size(), 4u);
    EXPECT_TRUE(ids.count(100));
    EXPECT_TRUE(ids.count(101));
    EXPECT_TRUE(ids.count(102));
    EXPECT_TRUE(ids.count(103));
}

// =============================================================================
// Empty intermediate CTE results
// =============================================================================

TEST_F(QA_GDB239, NestedCTEWithEmptyIntermediate) {
    // First CTE returns no rows (no dept named 'nonexistent').
    // Second CTE references first via IN — should also return no rows.
    auto qr = exec_ok("WITH empty_depts AS (SELECT departments.id FROM departments "
                      "WHERE departments.dept_name = 'nonexistent'), "
                      "no_users AS (SELECT users.id AS uid FROM users "
                      "WHERE users.dept_id IN (SELECT empty_depts.id FROM empty_depts)) "
                      "SELECT orders.id FROM orders "
                      "WHERE orders.user_id IN (SELECT no_users.uid FROM no_users)");

    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QA_GDB239, ThreeLevelChainWithEmptyMiddle) {
    // level1 returns data, level2 filters to nothing, level3 should be empty.
    auto qr = exec_ok("WITH level1 AS (SELECT departments.id FROM departments "
                      "WHERE departments.dept_name = 'engineering'), "
                      "level2 AS (SELECT users.id AS uid FROM users "
                      "WHERE users.dept_id IN (SELECT level1.id FROM level1) "
                      "AND users.name = 'nonexistent'), "
                      "level3 AS (SELECT orders.id AS oid FROM orders "
                      "WHERE orders.user_id IN (SELECT level2.uid FROM level2)) "
                      "SELECT level3.oid FROM level3");

    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// NOT IN with nested CTE chain
// =============================================================================

TEST_F(QA_GDB239, NotInWithNestedCTEs) {
    // NOT IN subquery that uses a CTE chain.
    auto qr = exec_ok("WITH eng_dept AS (SELECT departments.id FROM departments "
                      "WHERE departments.dept_name = 'engineering'), "
                      "eng_users AS (SELECT users.id AS uid FROM users "
                      "WHERE users.dept_id IN (SELECT eng_dept.id FROM eng_dept)) "
                      "SELECT users.name FROM users "
                      "WHERE users.id NOT IN (SELECT eng_users.uid FROM eng_users)");

    auto names = collect_column_strings(qr, 0);
    // Non-engineering users: bob (dept 20), diana (dept 30).
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("bob"));
    EXPECT_TRUE(names.count("diana"));
}

// =============================================================================
// EXISTS with nested CTE chain (tests plan_from_source path)
// =============================================================================

TEST_F(QA_GDB239, ExistsWithNestedCTEInFrom) {
    // EXISTS subquery where the inner table is a CTE that references another CTE.
    auto qr =
        exec_ok("WITH eng_dept AS (SELECT departments.id FROM departments "
                "WHERE departments.dept_name = 'engineering'), "
                "eng_users AS (SELECT users.id AS uid FROM users "
                "WHERE users.dept_id IN (SELECT eng_dept.id FROM eng_dept)) "
                "SELECT orders.id FROM orders "
                "WHERE EXISTS (SELECT 1 FROM eng_users WHERE eng_users.uid = orders.user_id)");

    auto ids = collect_column_ints(qr, 0);
    EXPECT_EQ(ids.size(), 4u);
    EXPECT_TRUE(ids.count(100));
    EXPECT_TRUE(ids.count(101));
    EXPECT_TRUE(ids.count(102));
    EXPECT_TRUE(ids.count(103));
}

TEST_F(QA_GDB239, NotExistsWithNestedCTEInFrom) {
    // NOT EXISTS with a nested CTE chain.
    auto qr =
        exec_ok("WITH eng_dept AS (SELECT departments.id FROM departments "
                "WHERE departments.dept_name = 'engineering'), "
                "eng_users AS (SELECT users.id AS uid FROM users "
                "WHERE users.dept_id IN (SELECT eng_dept.id FROM eng_dept)) "
                "SELECT orders.id FROM orders "
                "WHERE NOT EXISTS (SELECT 1 FROM eng_users WHERE eng_users.uid = orders.user_id)");

    // No orders by non-engineering users exist in the data.
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// Combined EXISTS + IN with nested CTE chain
// =============================================================================

TEST_F(QA_GDB239, ExistsAndInBothUsingNestedCTEs) {
    // Both EXISTS and IN reference CTEs from a chain.
    auto qr = exec_ok("WITH eng_dept AS (SELECT departments.id FROM departments "
                      "WHERE departments.dept_name = 'engineering'), "
                      "eng_users AS (SELECT users.id AS uid, users.name FROM users "
                      "WHERE users.dept_id IN (SELECT eng_dept.id FROM eng_dept)) "
                      "SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id) "
                      "AND users.id IN (SELECT eng_users.uid FROM eng_users)");

    auto names = collect_column_strings(qr, 0);
    // Eng users with orders: alice, charlie, eve.
    EXPECT_EQ(names.size(), 3u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
    EXPECT_TRUE(names.count("eve"));
}

// =============================================================================
// CTE with aggregation in the chain
// =============================================================================

TEST_F(QA_GDB239, NestedCTEWithAggregation) {
    // First CTE uses aggregation, second CTE references it.
    auto qr = exec_ok("WITH user_totals AS (SELECT orders.user_id, SUM(orders.amount) AS total "
                      "FROM orders GROUP BY orders.user_id), "
                      "big_spenders AS (SELECT user_totals.user_id FROM user_totals "
                      "WHERE user_totals.total > 400) "
                      "SELECT users.name FROM users "
                      "WHERE users.id IN (SELECT big_spenders.user_id FROM big_spenders)");

    auto names = collect_column_strings(qr, 0);
    // alice: 500+300=800 > 400. charlie: 200 ≤ 400. eve: 150 ≤ 400.
    EXPECT_EQ(names.size(), 1u);
    EXPECT_TRUE(names.count("alice"));
}

// =============================================================================
// Single CTE (no nesting) still works - regression check
// =============================================================================

TEST_F(QA_GDB239, SingleCTERegression) {
    // Single CTE with no nesting — must still work after the fixpoint change.
    auto qr = exec_ok("WITH eng AS (SELECT departments.id FROM departments "
                      "WHERE departments.dept_name = 'engineering') "
                      "SELECT users.name FROM users "
                      "WHERE users.dept_id IN (SELECT eng.id FROM eng)");

    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 3u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
    EXPECT_TRUE(names.count("eve"));
}

TEST_F(QA_GDB239, TwoIndependentCTEs) {
    // Two CTEs that don't reference each other — both used in the query.
    auto qr = exec_ok("WITH eng AS (SELECT departments.id FROM departments "
                      "WHERE departments.dept_name = 'engineering'), "
                      "orderers AS (SELECT orders.user_id FROM orders) "
                      "SELECT users.name FROM users "
                      "WHERE users.dept_id IN (SELECT eng.id FROM eng) "
                      "AND users.id IN (SELECT orderers.user_id FROM orderers)");

    auto names = collect_column_strings(qr, 0);
    // Eng users with orders: alice, charlie, eve.
    EXPECT_EQ(names.size(), 3u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
    EXPECT_TRUE(names.count("eve"));
}

// =============================================================================
// CTE used in FROM clause (tests plan_from_source inject_cte_bindings)
// =============================================================================

TEST_F(QA_GDB239, NestedCTEUsedDirectlyInFrom) {
    // Nested CTE chain, final CTE used directly in FROM (not in IN subquery).
    auto qr = exec_ok("WITH eng_dept AS (SELECT departments.id FROM departments "
                      "WHERE departments.dept_name = 'engineering'), "
                      "eng_users AS (SELECT users.id AS uid, users.name FROM users "
                      "WHERE users.dept_id IN (SELECT eng_dept.id FROM eng_dept)) "
                      "SELECT eng_users.name FROM eng_users");

    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 3u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
    EXPECT_TRUE(names.count("eve"));
}

// =============================================================================
// Category hierarchy: multi-level parent-child chains
// =============================================================================

TEST_F(QA_GDB239, CategoryHierarchyTwoLevels) {
    // Find leaf categories under 'root' through 'sub_a'.
    auto qr = exec_ok("WITH root_cats AS (SELECT categories.id FROM categories "
                      "WHERE categories.cat_name = 'root'), "
                      "sub_cats AS (SELECT categories.id FROM categories "
                      "WHERE categories.parent_id IN (SELECT root_cats.id FROM root_cats)), "
                      "leaf_cats AS (SELECT categories.cat_name FROM categories "
                      "WHERE categories.parent_id IN (SELECT sub_cats.id FROM sub_cats)) "
                      "SELECT leaf_cats.cat_name FROM leaf_cats");

    auto names = collect_column_strings(qr, 0);
    // root → sub_a(2), sub_b(3). sub_a → leaf_a1(4), leaf_a2(5). sub_b → nothing.
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("leaf_a1"));
    EXPECT_TRUE(names.count("leaf_a2"));
}

// =============================================================================
// CTE with same name as real table (CTE should shadow the table)
// =============================================================================

TEST_F(QA_GDB239, CTEShadowsRealTable) {
    // CTE named 'departments' shadows the real table.
    // The second CTE references 'departments' which should be the CTE, not the table.
    auto qr = exec_ok("WITH departments AS (SELECT departments.id FROM departments "
                      "WHERE departments.dept_name = 'sales'), "
                      "sales_users AS (SELECT users.name FROM users "
                      "WHERE users.dept_id IN (SELECT departments.id FROM departments)) "
                      "SELECT sales_users.name FROM sales_users");

    auto names = collect_column_strings(qr, 0);
    // CTE 'departments' = dept_id 20 (sales). Users with dept_id=20: bob.
    EXPECT_EQ(names.size(), 1u);
    EXPECT_TRUE(names.count("bob"));
}

// =============================================================================
// Stress: many CTEs in a chain
// =============================================================================

TEST_F(QA_GDB239, FiveCTEChain) {
    // 5 CTEs where each references the previous one.
    auto qr = exec_ok("WITH cte1 AS (SELECT departments.id AS did FROM departments "
                      "WHERE departments.dept_name = 'engineering'), "
                      "cte2 AS (SELECT users.id AS uid FROM users "
                      "WHERE users.dept_id IN (SELECT cte1.did FROM cte1)), "
                      "cte3 AS (SELECT orders.id AS oid, orders.user_id FROM orders "
                      "WHERE orders.user_id IN (SELECT cte2.uid FROM cte2)), "
                      "cte4 AS (SELECT cte3.oid FROM cte3 WHERE cte3.oid > 100), "
                      "cte5 AS (SELECT cte4.oid FROM cte4 WHERE cte4.oid < 103) "
                      "SELECT cte5.oid FROM cte5");

    auto ids = collect_column_ints(qr, 0);
    // cte1={10}, cte2={1,3,5}, cte3={100,101,102,103}
    // cte4 (>100): {101,102,103}, cte5 (<103): {101,102}
    EXPECT_EQ(ids.size(), 2u);
    EXPECT_TRUE(ids.count(101));
    EXPECT_TRUE(ids.count(102));
}

// =============================================================================
// Error paths
// =============================================================================

TEST_F(QA_GDB239, CTEReferenceNonExistentTable) {
    // CTE references a table that doesn't exist.
    exec_should_fail("WITH bad AS (SELECT ghost.id FROM ghost), "
                     "level2 AS (SELECT users.id FROM users "
                     "WHERE users.id IN (SELECT bad.id FROM bad)) "
                     "SELECT level2.id FROM level2");
}

TEST_F(QA_GDB239, CTEReferenceNonExistentCTE) {
    // CTE references another CTE that doesn't exist.
    exec_should_fail("WITH real_cte AS (SELECT users.id FROM users "
                     "WHERE users.id IN (SELECT missing_cte.id FROM missing_cte)) "
                     "SELECT real_cte.id FROM real_cte");
}
