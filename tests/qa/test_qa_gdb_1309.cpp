/// @file test_qa_gdb_1309.cpp
/// @brief QA adversarial regression tests for GDB-1309: correlated IN/scalar
///        subquery WHERE clauses now resolve outer-query column references
///        via an outer-row fallback in eval_column_ref, instead of failing
///        NOT_FOUND. This file specifically probes the implementer's own
///        flagged risk: only ONE LEVEL of outer context is tracked, so a
///        subquery nested two levels deep that references the OUTERMOST
///        query's column (skipping the middle level) is untested territory.
///        A silently WRONG outer row would be worse than the original clean
///        failure -- that is what this file adversarially hunts for.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Fixture: users -> departments -> regions, a 3-level hierarchy so a
// deeply-nested subquery can reference an ancestor two levels up.
// =============================================================================

class QAGdb1309Test : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_1309";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        exec_ok("CREATE TABLE users (id INT, name VARCHAR, dept_id INT)");
        exec_ok("CREATE TABLE orders (id INT, user_id INT, amount INT)");
        exec_ok("CREATE TABLE departments (id INT, dept_name VARCHAR)");
        // regions has no real FK to departments; dept_ref is only used by
        // adversarial tests to compare against an ancestor's dept_id.
        exec_ok("CREATE TABLE regions (id INT, region_name VARCHAR, dept_ref INT)");

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

        // Regions deliberately do NOT mirror department ids 1:1: region 900
        // has dept_ref=10 (matches alice/charlie's dept), region 901 has
        // dept_ref=999 (matches nobody). This lets tests distinguish "resolved
        // to the correct outer row" from "resolved to some other row".
        exec_ok("INSERT INTO regions VALUES (900, 'north', 10)");
        exec_ok("INSERT INTO regions VALUES (901, 'south', 999)");
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

    Result<QueryResult> exec(const std::string& sql) { return engine_->execute(sql); }

    std::unordered_set<std::string> collect_column_strings(const QueryResult& qr, size_t col) {
        std::unordered_set<std::string> result;
        for (const auto& row : qr.rows) {
            result.insert(row[col].as_string());
        }
        return result;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// -----------------------------------------------------------------------------
// (1) 2-level nested correlation: the INNERMOST subquery references the
// OUTERMOST query's column, skipping the middle level entirely.
//
//   SELECT u.name FROM users u WHERE u.dept_id IN (
//     SELECT d.id FROM departments d WHERE d.id IN (
//       SELECT r.dept_ref FROM regions r WHERE r.dept_ref = u.dept_id
//     )
//   )
//
// The middle subquery (over departments) is NOT itself correlated to `u`;
// only the innermost (over regions) is, and it skips the middle level. Only
// dept 10 (alice, charlie) has a matching region (900, dept_ref=10). Dept 20
// (bob) and dept 30 (diana) have no matching region, so they must NOT appear.
// If the implementation silently resolved `u.dept_id` to the wrong ancestor
// row (e.g. departments' `d` row, which has no `dept_id` column, or garbage),
// this test would either see the wrong row set or a spurious NOT_FOUND.
// -----------------------------------------------------------------------------
TEST_F(QAGdb1309Test, DeeplyNestedCorrelationSkippingMiddleLevelFailsCleanNotWrongData) {
    auto result =
        exec("SELECT u.name FROM users u WHERE u.dept_id IN "
             "(SELECT d.id FROM departments d WHERE d.id IN "
             "(SELECT r.dept_ref FROM regions r WHERE r.dept_ref = u.dept_id))");

    // GDB-1309 confirmed behavior: skip-level correlation (innermost
    // subquery referencing the OUTERMOST query's alias, bypassing the
    // uncorrelated middle level) is NOT supported. It fails at BIND time
    // with a clean NOT_FOUND ("table not found: u") because the binder for
    // the middle (departments) subquery has no notion of its own
    // grandparent's alias -- it only threads one level of outer context.
    //
    // This is the safe outcome: a real limitation (a valid, if unusual, SQL
    // pattern is rejected) but NOT the dangerous outcome the handoff asked
    // to rule out -- it never silently returns wrong/incomplete data. If a
    // future change makes this start returning a result (empty or
    // otherwise) instead of erroring, this test must be revisited to verify
    // the result is correct (alice, charlie only) rather than silently
    // accepting whatever came back.
    ASSERT_FALSE(result.has_value())
        << "Skip-level nested correlation now returns a result instead of "
           "failing clean -- re-verify this produces the CORRECT rows "
           "(alice, charlie only), not silently wrong/incomplete data.";
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
    EXPECT_NE(result.error().message.find("table not found"), std::string::npos)
        << "Unexpected error message: " << result.error().message;
}

// -----------------------------------------------------------------------------
// (2) Outer JOIN combined with a correlated subquery in the WHERE clause.
// The correlated subquery references a column from one side of the join
// (users), while the join itself pulls in orders. Verifies the outer-row
// fallback resolves against the *joined* row's schema, not just a
// single-table scan's schema.
// -----------------------------------------------------------------------------
TEST_F(QAGdb1309Test, JoinWithCorrelatedSubqueryInWhereClause) {
    auto qr = exec_ok(
        "SELECT u.name, o.amount FROM users u JOIN orders o ON o.user_id = u.id "
        "WHERE u.dept_id IN (SELECT d.id FROM departments d WHERE d.id = u.dept_id)");

    // alice (dept 10) has 2 orders, charlie (dept 10) has 1 order; bob/diana
    // have no orders so the JOIN alone excludes them regardless.
    ASSERT_EQ(qr.rows.size(), 3u);
    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
}

// Same JOIN shape, but the correlated subquery's condition can never match
// (department id > 9999), proving the correlated predicate is genuinely
// applied post-join and not just vacuously true.
TEST_F(QAGdb1309Test, JoinWithCorrelatedSubqueryInWhereClauseNoMatch) {
    auto qr = exec_ok(
        "SELECT u.name, o.amount FROM users u JOIN orders o ON o.user_id = u.id "
        "WHERE u.dept_id IN (SELECT d.id FROM departments d WHERE d.id = u.dept_id "
        "AND d.id > 9999)");

    EXPECT_EQ(qr.rows.size(), 0u);
}

// -----------------------------------------------------------------------------
// (3) EXISTS-based correlated subqueries must remain unaffected (EXISTS
// unconditionally decorrelates into a join at plan time, a separate code
// path from the IN/scalar runtime-fallback path GDB-1309 touches).
// -----------------------------------------------------------------------------
TEST_F(QAGdb1309Test, ExistsCorrelatedSubqueryStillWorks) {
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)");

    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
    EXPECT_FALSE(names.count("bob"));
    EXPECT_FALSE(names.count("diana"));
}

TEST_F(QAGdb1309Test, NotExistsCorrelatedSubqueryStillWorks) {
    auto qr = exec_ok("SELECT users.name FROM users "
                      "WHERE NOT EXISTS (SELECT 1 FROM orders WHERE orders.user_id = users.id)");

    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 2u);
    EXPECT_TRUE(names.count("bob"));
    EXPECT_TRUE(names.count("diana"));
}

// -----------------------------------------------------------------------------
// (4) Performance-sanity: a genuinely NON-correlated subquery (no outer
// column reference at all) must still produce correct, fully-filtered
// results -- the pushdown-disable gate in Planner::plan_select is keyed on
// `outer_tuple_ == nullptr`, which must remain true (pushdown active) for an
// uncorrelated subquery's own inner plan.
// -----------------------------------------------------------------------------
TEST_F(QAGdb1309Test, NonCorrelatedSubqueryStillFiltersCorrectly) {
    auto qr = exec_ok("SELECT users.name FROM users WHERE users.dept_id IN "
                      "(SELECT departments.id FROM departments WHERE departments.id < 30)");

    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 3u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("bob"));
    EXPECT_TRUE(names.count("charlie"));
    EXPECT_FALSE(names.count("diana"));
}

// -----------------------------------------------------------------------------
// (5) Adversarial: outer-qualified column with a name that COLLIDES with a
// column name inside the subquery's own local table but resolves to a
// different value. Confirms local resolution always wins over the outer
// fallback (fallback must only trigger when local resolution fails).
// -----------------------------------------------------------------------------
TEST_F(QAGdb1309Test, LocalColumnShadowsOuterColumnOfSameName) {
    // departments.id exists locally, so `d.id = 10` should resolve locally
    // and NOT fall back to any outer row, regardless of correlation.
    auto qr = exec_ok("SELECT u.name FROM users u WHERE u.id IN "
                      "(SELECT d.id FROM departments d WHERE d.id = 10)");

    // departments.id = 10 is a single fixed row; the subquery returns {10},
    // so only users.id = 10 would match -- none of our seeded users have
    // id = 10, so the outer query should return zero rows. This proves the
    // WHERE clause used the department's own `id`, not something outer-row
    // derived.
    EXPECT_EQ(qr.rows.size(), 0u);
}

// -----------------------------------------------------------------------------
// (6) Ambiguous ancestor-skipping case where the middle level's own column
// NAME (not just table) matches the outer level's column name and type,
// distinguishing "outer fallback found the truly nearest ancestor" from
// "outer fallback found the -- textually matching but structurally wrong --
// grandparent by accident" is covered by test (1) above via disjoint data.
// This test adds a straightforward 2-level (not skipping) case as a control:
// the innermost subquery correlates to its DIRECT parent, one level up.
// -----------------------------------------------------------------------------
TEST_F(QAGdb1309Test, TwoLevelDirectParentCorrelationControlCase) {
    auto qr =
        exec_ok("SELECT u.name FROM users u WHERE u.dept_id IN "
                "(SELECT d.id FROM departments d WHERE d.id = u.dept_id AND d.id <> 30)");

    // alice/charlie (dept 10) and bob (dept 20) all satisfy `d.id = u.dept_id
    // AND d.id <> 30`; only diana (dept 30) is excluded.
    auto names = collect_column_strings(qr, 0);
    EXPECT_EQ(names.size(), 3u);
    EXPECT_TRUE(names.count("alice"));
    EXPECT_TRUE(names.count("charlie"));
    EXPECT_TRUE(names.count("bob"));
    EXPECT_FALSE(names.count("diana"));
}

// =============================================================================
// (7) NEAREST combined with a correlated IN-subquery in the WHERE clause.
// NEAREST cannot itself reference an outer row as its query vector (that
// combination is rejected at parse time, per
// QASubqueryBlendTest.CorrelatedVectorPerUserRejected) -- but NEAREST as a
// WHERE predicate on the OUTER query, combined with a genuinely correlated
// IN-subquery filtering the same outer rows, is a realistic and previously
// untested combination per the handoff's risk note.
// =============================================================================

class QAGdb1309NearestTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_1309_nearest";
        std::error_code ec;
        std::filesystem::remove_all(data_dir_, ec);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
        provider_registry_ = std::make_unique<ProviderRegistry>(catalog_);

        exec_ok("CREATE TABLE articles (id INT PRIMARY KEY, title VARCHAR, dept_id INT, "
                "body_vec EMBEDDING)");
        exec_ok("CREATE TABLE departments (id INT, dept_name VARCHAR)");

        auto articles = catalog_.get_table(default_database_id, "articles");
        ASSERT_TRUE(articles.has_value());
        register_embedding(articles->table_id, 3, 4, "title", "builtin/4");
        engine_->set_provider_registry(provider_registry_.get());

        exec_ok("INSERT INTO articles VALUES (10, 'ml', 10, [1.0, 0.0, 0.0, 0.0])");
        exec_ok("INSERT INTO articles VALUES (11, 'db', 20, [0.0, 1.0, 0.0, 0.0])");
        exec_ok("INSERT INTO articles VALUES (12, 'vec', 10, [1.0, 0.0, 0.0, 0.0])");

        exec_ok("INSERT INTO departments VALUES (10, 'engineering')");
        exec_ok("INSERT INTO departments VALUES (20, 'sales')");
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        provider_registry_.reset();
        std::error_code ec;
        std::filesystem::remove_all(data_dir_, ec);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << "SQL: " << sql << "\nError: " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void register_embedding(table_id_t table_id,
                            int32_t col_id,
                            int32_t dim,
                            const std::string& source,
                            const std::string& provider) {
        EmbeddingColumnDef emb_def;
        emb_def.table_id = table_id;
        emb_def.column_id = col_id;
        emb_def.dimension = dim;
        emb_def.source_expr = source;
        emb_def.provider = provider;
        ASSERT_TRUE(catalog_.register_embedding_column(emb_def).has_value());
        if (catalog_.get_embedding_provider(provider).has_value()) {
            return;
        }
        EmbeddingProviderConfig prov;
        prov.name = provider;
        prov.type = "builtin";
        prov.dimension = dim;
        ASSERT_TRUE(catalog_.register_embedding_provider(prov).has_value());
    }

    std::unordered_set<std::string> titles(const QueryResult& qr) {
        std::unordered_set<std::string> out;
        for (const auto& row : qr.rows) {
            out.insert(row[0].as_string());
        }
        return out;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<ProviderRegistry> provider_registry_;
};

TEST_F(QAGdb1309NearestTest, NearestCombinedWithCorrelatedSubqueryInWhere) {
    // Articles near [1,0,0,0] (== ml, vec; db is orthogonal/far) AND whose
    // department genuinely exists (correlated: departments.id = articles.dept_id).
    // Exercises the GDB-1309 outer-row fallback path (correlated IN) alongside
    // a NEAREST predicate scanning the same outer table.
    auto qr = exec_ok("SELECT articles.title FROM articles "
                      "WHERE NEAREST(body_vec, 2) TO [1.0, 0.0, 0.0, 0.0] "
                      "AND articles.dept_id IN "
                      "(SELECT departments.id FROM departments WHERE departments.id = "
                      "articles.dept_id)");

    auto got = titles(qr);
    EXPECT_EQ(got.size(), 2u);
    EXPECT_TRUE(got.count("ml"));
    EXPECT_TRUE(got.count("vec"));
    EXPECT_FALSE(got.count("db"));
}

TEST_F(QAGdb1309NearestTest, NearestCombinedWithCorrelatedSubqueryNoMatchingDept) {
    // Same shape, but the correlated subquery can never match (dept > 9999),
    // proving the correlated predicate genuinely filters rather than being a
    // no-op that lets NEAREST's result through unfiltered.
    auto qr = exec_ok("SELECT articles.title FROM articles "
                      "WHERE NEAREST(body_vec, 2) TO [1.0, 0.0, 0.0, 0.0] "
                      "AND articles.dept_id IN "
                      "(SELECT departments.id FROM departments WHERE departments.id = "
                      "articles.dept_id AND departments.id > 9999)");

    EXPECT_EQ(qr.rows.size(), 0u);
}
