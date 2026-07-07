/// @file test_qa_gdb_695.cpp
/// QA adversarial tests for GDB-695: standalone TRAVERSE WHERE can reference
/// traversal columns (node / depth / source / __path).
///
/// The fix adds a ScopeTable carrying the traversal's bound output columns
/// (aliased to the edge type) to the WHERE scope in Binder::bind_traverse(),
/// keeping the parent-scope chain for correlation. Adversarial coverage
/// beyond the dev suite (test_traverse_where_binding.cpp):
///   * compound predicates (AND / OR / parentheses / arithmetic on depth)
///   * boundary depths (0, negative, INT64_MAX) and constant predicates
///   * IS NULL / IS NOT NULL on traversal columns
///   * case-insensitive and qualified column resolution, wrong qualifiers
///   * type mismatches (string vs depth, PATH vs integer)
///   * interactions with DIRECTION IN, MAX_DEPTH, FETCH, WITH TRACE combined
///   * self-loops, parallel edges, nonexistent start nodes
///   * TRAVERSE-with-WHERE as an IN subquery, including outer-column
///     shadowing of traversal column names
///   * deep-chain stress with a depth filter

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

/// Full SQL-pipeline fixture: users table (1..5) plus the canonical diamond
/// follows graph 1->2, 1->3, 2->4, 3->4, 4->5.
///
/// BFS from users(1), DIRECTION OUT:
///   depth 1: nodes 2, 3
///   depth 2: node 4
///   depth 3: node 5
class QA_GDB695_TraverseWhere : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb695";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");
        exec_ok("INSERT INTO users VALUES (3, 'Jane')");
        exec_ok("INSERT INTO users VALUES (4, 'Jake')");
        exec_ok("INSERT INTO users VALUES (5, 'Zara')");

        exec_ok("CREATE EDGE TYPE follows FROM users TO users");
        exec_ok("LINK users(1) TO users(2) VIA follows");
        exec_ok("LINK users(1) TO users(3) VIA follows");
        exec_ok("LINK users(2) TO users(4) VIA follows");
        exec_ok("LINK users(3) TO users(4) VIA follows");
        exec_ok("LINK users(4) TO users(5) VIA follows");
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
        if (!result.has_value()) {
            return QueryResult{};
        }
        return std::move(*result);
    }

    /// Execute SQL that must fail; returns the error for further inspection.
    Error exec_err(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << ": expected an error but query succeeded";
        if (result.has_value()) {
            return Error{StatusCode::OK, "query unexpectedly succeeded"};
        }
        EXPECT_FALSE(result.error().message.empty()) << sql << ": error message must not be empty";
        return result.error();
    }

    static std::optional<size_t> col_index(const QueryResult& qr, const std::string& name) {
        for (size_t i = 0; i < qr.column_names.size(); ++i) {
            if (qr.column_names[i] == name) {
                return i;
            }
        }
        return std::nullopt;
    }

    static int64_t val_to_int64(const Value& v) {
        if (v.type_id() == TypeId::INT32) {
            return v.as_int32();
        }
        return v.as_int64();
    }

    /// Sorted int64 values of the named column across all rows.
    std::vector<int64_t> sorted_column(const QueryResult& qr, const std::string& name) {
        auto idx = col_index(qr, name);
        EXPECT_TRUE(idx.has_value()) << "column missing from result: " << name;
        std::vector<int64_t> out;
        if (!idx.has_value()) {
            return out;
        }
        out.reserve(qr.rows.size());
        for (const auto& row : qr.rows) {
            out.push_back(val_to_int64(row[*idx]));
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

// ---------------------------------------------------------------------------
// Compound predicates
// ---------------------------------------------------------------------------

TEST_F(QA_GDB695_TraverseWhere, AndPredicateOnNodeAndDepthMatches) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE node = 4 AND depth = 2");
    EXPECT_EQ(sorted_column(qr, "node"), (std::vector<int64_t>{4}));
}

TEST_F(QA_GDB695_TraverseWhere, AndPredicateContradictionYieldsEmpty) {
    // Node 4 exists only at depth 2; demanding depth 1 must yield nothing.
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE node = 4 AND depth = 1");
    EXPECT_TRUE(qr.rows.empty());
}

TEST_F(QA_GDB695_TraverseWhere, OrPredicateAcrossDepths) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE depth = 1 OR node = 5");
    EXPECT_EQ(sorted_column(qr, "node"), (std::vector<int64_t>{2, 3, 5}));
}

TEST_F(QA_GDB695_TraverseWhere, ParenthesizedCompoundPredicate) {
    auto qr =
        exec_ok("TRAVERSE follows FROM users(1) WHERE (depth = 1 AND node = 2) OR (depth = 3)");
    EXPECT_EQ(sorted_column(qr, "node"), (std::vector<int64_t>{2, 5}));
}

TEST_F(QA_GDB695_TraverseWhere, ArithmeticOnDepthInPredicate) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE depth + 1 = 3");
    EXPECT_EQ(sorted_column(qr, "node"), (std::vector<int64_t>{4}));
}

TEST_F(QA_GDB695_TraverseWhere, ConstantTruePredicateKeepsAllRows) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE 1 = 1");
    EXPECT_EQ(sorted_column(qr, "node"), (std::vector<int64_t>{2, 3, 4, 5}));
}

TEST_F(QA_GDB695_TraverseWhere, ConstantFalsePredicateDropsAllRows) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE 1 = 0");
    EXPECT_TRUE(qr.rows.empty());
}

// ---------------------------------------------------------------------------
// Boundary values on depth
// ---------------------------------------------------------------------------

TEST_F(QA_GDB695_TraverseWhere, DepthZeroYieldsEmpty) {
    // The standalone traversal never emits the start node at depth 0.
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE depth = 0");
    EXPECT_TRUE(qr.rows.empty());
}

TEST_F(QA_GDB695_TraverseWhere, NegativeDepthYieldsEmpty) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE depth = -1");
    EXPECT_TRUE(qr.rows.empty());
}

TEST_F(QA_GDB695_TraverseWhere, Int64MaxDepthLiteralYieldsEmpty) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE depth = 9223372036854775807");
    EXPECT_TRUE(qr.rows.empty());
}

// ---------------------------------------------------------------------------
// NULL semantics on traversal columns
// ---------------------------------------------------------------------------

TEST_F(QA_GDB695_TraverseWhere, NodeIsNullYieldsEmpty) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE node IS NULL");
    EXPECT_TRUE(qr.rows.empty());
}

TEST_F(QA_GDB695_TraverseWhere, NodeIsNotNullKeepsAllRows) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE node IS NOT NULL");
    EXPECT_EQ(sorted_column(qr, "node"), (std::vector<int64_t>{2, 3, 4, 5}));
}

// ---------------------------------------------------------------------------
// Name resolution: case, qualification, undeclared columns
// ---------------------------------------------------------------------------

TEST_F(QA_GDB695_TraverseWhere, ColumnNamesResolveCaseInsensitively) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE DEPTH = 1");
    EXPECT_EQ(sorted_column(qr, "node"), (std::vector<int64_t>{2, 3}));

    auto qr2 = exec_ok("TRAVERSE follows FROM users(1) WHERE Node = 4");
    EXPECT_EQ(sorted_column(qr2, "node"), (std::vector<int64_t>{4}));
}

TEST_F(QA_GDB695_TraverseWhere, QualifierOfDifferentTableFailsToBind) {
    // Traversal columns are exposed only under the edge-type alias; the FROM
    // table name must not leak in as a qualifier.
    auto err = exec_err("TRAVERSE follows FROM users(1) WHERE users.depth = 1");
    EXPECT_EQ(err.code, StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB695_TraverseWhere, FromTableColumnNotVisibleInWhere) {
    // Only traversal output columns are in scope, not the FROM table's rows.
    auto err = exec_err("TRAVERSE follows FROM users(1) WHERE name = 'Bob'");
    EXPECT_EQ(err.code, StatusCode::NOT_FOUND);
}

// ---------------------------------------------------------------------------
// Type mismatches in predicates
// ---------------------------------------------------------------------------

TEST_F(QA_GDB695_TraverseWhere, StringComparedToDepthFailsGracefully) {
    auto result = engine_->execute("TRAVERSE follows FROM users(1) WHERE depth = 'two'");
    ASSERT_FALSE(result.has_value())
        << "comparing INT64 depth to a non-numeric string must not succeed";
    EXPECT_FALSE(result.error().message.empty());
}

TEST_F(QA_GDB695_TraverseWhere, PathComparedToIntegerFailsGracefully) {
    auto result = engine_->execute("TRAVERSE follows FROM users(1) WHERE __path = 1 WITH TRACE");
    ASSERT_FALSE(result.has_value()) << "PATH vs INTEGER comparison must not succeed";
    EXPECT_FALSE(result.error().message.empty());
}

// ---------------------------------------------------------------------------
// Clause interactions: DIRECTION, MAX_DEPTH, FETCH, WITH TRACE
// ---------------------------------------------------------------------------

TEST_F(QA_GDB695_TraverseWhere, DirectionInWithDepthFilter) {
    // From users(5), DIRECTION IN: depth 1 -> {4}, depth 2 -> {2, 3}, depth 3 -> {1}.
    auto qr = exec_ok("TRAVERSE follows FROM users(5) DIRECTION IN WHERE depth = 2");
    EXPECT_EQ(sorted_column(qr, "node"), (std::vector<int64_t>{2, 3}));
}

TEST_F(QA_GDB695_TraverseWhere, MaxDepthCapsBfsBeforeWhere) {
    // BFS stops at depth 1; the depth = 2 filter can never match.
    auto qr = exec_ok("TRAVERSE follows FROM users(1) MAX_DEPTH 1 WHERE depth = 2");
    EXPECT_TRUE(qr.rows.empty());
}

TEST_F(QA_GDB695_TraverseWhere, MaxDepthWithRangeFilter) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) MAX_DEPTH 2 WHERE depth >= 1");
    EXPECT_EQ(sorted_column(qr, "node"), (std::vector<int64_t>{2, 3, 4}));
}

TEST_F(QA_GDB695_TraverseWhere, SourceFilterWithFetch) {
    // Only node 5 is discovered from node 4.
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE source = 4 FETCH");
    EXPECT_EQ(sorted_column(qr, "node"), (std::vector<int64_t>{5}));
}

TEST_F(QA_GDB695_TraverseWhere, AllClausesCombinedWithUnsatisfiableFilter) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 1 "
                      "WHERE depth = 2 FETCH WITH TRACE");
    EXPECT_TRUE(qr.rows.empty());
}

TEST_F(QA_GDB695_TraverseWhere, AllClausesCombinedWithSatisfiableFilter) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 3 "
                      "WHERE depth = 3 AND node = 5 FETCH WITH TRACE");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(sorted_column(qr, "node"), (std::vector<int64_t>{5}));

    auto path_idx = col_index(qr, "__path");
    ASSERT_TRUE(path_idx.has_value()) << "WITH TRACE must surface __path";
    ASSERT_EQ(qr.rows[0][*path_idx].type_id(), TypeId::PATH);
    const Path& p = qr.rows[0][*path_idx].as_path();
    ASSERT_EQ(p.steps.size(), 4u) << "depth-3 hit must keep its full three-hop path";
    EXPECT_EQ(p.steps.front().node_pk_as_int64(), 1);
    EXPECT_EQ(p.steps.back().node_pk_as_int64(), 5);

    auto source_idx = col_index(qr, "source");
    ASSERT_TRUE(source_idx.has_value()) << "FETCH must surface source";
    EXPECT_EQ(val_to_int64(qr.rows[0][*source_idx]), 4);
}

// ---------------------------------------------------------------------------
// Graph shape edge cases
// ---------------------------------------------------------------------------

TEST_F(QA_GDB695_TraverseWhere, NonexistentStartNodeYieldsEmpty) {
    auto qr = exec_ok("TRAVERSE follows FROM users(99) WHERE depth = 1");
    EXPECT_TRUE(qr.rows.empty());
}

TEST_F(QA_GDB695_TraverseWhere, SelfLoopOnStartNodeDoesNotLeakThroughFilter) {
    exec_ok("LINK users(1) TO users(1) VIA follows");
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE depth = 1");
    // The already-visited start node must not reappear at depth 1.
    EXPECT_EQ(sorted_column(qr, "node"), (std::vector<int64_t>{2, 3}));
}

TEST_F(QA_GDB695_TraverseWhere, ParallelEdgesDoNotDuplicateFilteredRows) {
    exec_ok("LINK users(1) TO users(2) VIA follows");
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE depth = 1");
    EXPECT_EQ(sorted_column(qr, "node"), (std::vector<int64_t>{2, 3}));
}

// ---------------------------------------------------------------------------
// TRAVERSE ... WHERE as a subquery
// ---------------------------------------------------------------------------

TEST_F(QA_GDB695_TraverseWhere, InSubqueryWithDepthFilter) {
    auto qr = exec_ok("SELECT name FROM users "
                      "WHERE id IN (TRAVERSE follows FROM users(1) WHERE depth = 1) "
                      "ORDER BY name");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
    EXPECT_EQ(qr.rows[1][0].as_string(), "Jane");
}

TEST_F(QA_GDB695_TraverseWhere, TraversalColumnShadowsOuterColumnOfSameName) {
    // The outer table deliberately has a column named `depth` with a value
    // (99) that would empty the traversal if the binder resolved the inner
    // unqualified `depth` against the outer scope instead of the traversal.
    exec_ok("CREATE TABLE metrics (id INT PRIMARY KEY, depth INT)");
    exec_ok("INSERT INTO metrics VALUES (2, 99)");

    auto qr = exec_ok("SELECT id FROM metrics "
                      "WHERE id IN (TRAVERSE follows FROM users(1) WHERE depth = 1)");
    ASSERT_EQ(qr.rows.size(), 1u) << "inner depth must bind to the traversal column, "
                                     "not the outer metrics.depth";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 2);
}

TEST_F(QA_GDB695_TraverseWhere, CorrelatedOuterReferenceInWhereFailsAtExecution) {
    // KNOWN LIMITATION (found during GDB-695 QA, Medium): the WHERE scope
    // chains to the parent scope "for correlation", so an outer column
    // reference (u.id) binds successfully. But TraversalOperator evaluates
    // the predicate against only its own output schema (node/depth/...),
    // never the outer row, so the query fails at execution time with
    // "column not found: u.id" instead of either filtering per outer row or
    // being rejected at bind time. This test pins the current graceful-error
    // behavior; if correlation is implemented, rewrite to assert rows
    // {2, 3, 4, 5}.
    auto err = exec_err("SELECT u.id FROM users u "
                        "WHERE u.id IN (TRAVERSE follows FROM users(1) WHERE node = u.id) "
                        "ORDER BY u.id");
    EXPECT_EQ(err.code, StatusCode::NOT_FOUND);
    EXPECT_NE(err.message.find("column not found"), std::string::npos)
        << "actual message: " << err.message;
}

// ---------------------------------------------------------------------------
// Stress: deep chain with a depth filter
// ---------------------------------------------------------------------------

TEST_F(QA_GDB695_TraverseWhere, DeepChainDepthFilterFindsOnlyTail) {
    exec_ok("CREATE TABLE chain (id INT PRIMARY KEY)");
    for (int i = 0; i <= 100; ++i) {
        exec_ok("INSERT INTO chain VALUES (" + std::to_string(i) + ")");
    }
    exec_ok("CREATE EDGE TYPE next_link FROM chain TO chain");
    for (int i = 0; i < 100; ++i) {
        exec_ok("LINK chain(" + std::to_string(i) + ") TO chain(" + std::to_string(i + 1) +
                ") VIA next_link");
    }

    auto qr = exec_ok("TRAVERSE next_link FROM chain(0) MAX_DEPTH 150 WHERE depth = 100");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(sorted_column(qr, "node"), (std::vector<int64_t>{100}));

    auto qr2 = exec_ok("TRAVERSE next_link FROM chain(0) MAX_DEPTH 150 WHERE depth > 100");
    EXPECT_TRUE(qr2.rows.empty());
}

} // namespace
} // namespace sixseven
