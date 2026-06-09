/// @file test_qa_gdb_680.cpp
/// QA adversarial tests for GDB-680: TRACE integration tests (WITH TRACE
/// clause, __path meta-column, BFS path reconstruction — GDB-677/678/679).
///
/// Adversarial coverage beyond the dev suites (test_trace_traversal.cpp,
/// test_trace_enriched.cpp):
///   * self-loops on the start node and on intermediate nodes
///   * MAX_DEPTH 0, nonexistent start nodes, isolated start nodes
///   * parallel (duplicate) edges
///   * WHERE that filters out every row
///   * path functions (PATH_LENGTH, NODES) applied to __path
///   * ORDER BY / LIMIT / DISTINCT composed with __path
///   * __path referenced without WITH TRACE (binder error path)
///   * WHERE comparisons on PATH values (type error path)
///   * non-int64/int32 primary keys (string, UINT32) under TRACE
///   * heterogeneous edge types under TRACE (distinct and colliding PKs)
///   * deep-chain stress (200-node chain, MAX_DEPTH 250)

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/traversal.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

/// Full SQL-pipeline fixture: users table (1..5) plus the canonical diamond
/// follows graph 1->2, 1->3, 2->4, 3->4, 4->5.
class QA_GDB680_Trace : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb680";
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

    static void expect_cycle_free(const Path& path) {
        std::unordered_set<int64_t> seen;
        for (const auto& step : path.steps) {
            EXPECT_TRUE(seen.insert(step.node_pk).second)
                << "path contains a repeated node: " << step.node_pk;
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ---------------------------------------------------------------------------
// Self-loops
// ---------------------------------------------------------------------------

TEST_F(QA_GDB680_Trace, SelfLoopOnStartNodeKeepsPathsAcyclic) {
    exec_ok("LINK users(1) TO users(1) VIA follows");

    auto qr = exec_ok("SELECT __node, __depth, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");

    // The self-loop on the (already visited) start node must add no rows.
    ASSERT_EQ(qr.rows.size(), 4u);
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row[2].type_id(), TypeId::PATH);
        const Path& p = row[2].as_path();
        expect_cycle_free(p);
        EXPECT_EQ(p.steps.front().node_pk, 1);
        EXPECT_EQ(p.length(), val_to_int64(row[1]));
    }
}

TEST_F(QA_GDB680_Trace, SelfLoopOnIntermediateNodeKeepsPathsAcyclic) {
    exec_ok("LINK users(2) TO users(2) VIA follows");

    auto qr = exec_ok("SELECT __node, __depth, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 4u);
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row[2].type_id(), TypeId::PATH);
        const Path& p = row[2].as_path();
        expect_cycle_free(p);
        EXPECT_EQ(p.length(), val_to_int64(row[1]));
    }
}

// ---------------------------------------------------------------------------
// Boundary values: MAX_DEPTH 0, missing/isolated start nodes
// ---------------------------------------------------------------------------

TEST_F(QA_GDB680_Trace, MaxDepthZeroYieldsNoRowsEnriched) {
    auto qr = exec_ok("SELECT __node, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 0 WITH TRACE");
    EXPECT_EQ(qr.rows.size(), 0u) << "MAX_DEPTH 0 must emit no rows (start node is not a hit)";
}

TEST_F(QA_GDB680_Trace, MaxDepthZeroYieldsNoRowsStandalone) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) MAX_DEPTH 0 WITH TRACE");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QA_GDB680_Trace, NonexistentStartNodeYieldsNoRows) {
    auto qr = exec_ok("SELECT __node, __path "
                      "FROM TRAVERSE follows FROM users(99) DIRECTION OUT WITH TRACE");
    EXPECT_EQ(qr.rows.size(), 0u) << "a start key with no edges must produce an empty result";
}

TEST_F(QA_GDB680_Trace, IsolatedStartNodeYieldsNoRows) {
    exec_ok("INSERT INTO users VALUES (6, 'Loner')");
    auto qr = exec_ok("SELECT __node, __path "
                      "FROM TRAVERSE follows FROM users(6) DIRECTION OUT WITH TRACE");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// ---------------------------------------------------------------------------
// Parallel (duplicate) edges
// ---------------------------------------------------------------------------

TEST_F(QA_GDB680_Trace, ParallelEdgesProduceOneRowPerNodeWithValidPath) {
    exec_ok("LINK users(1) TO users(2) VIA follows"); // duplicate of an existing edge

    auto qr = exec_ok("SELECT __node, __depth, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");

    // BFS visits each node once regardless of edge multiplicity.
    ASSERT_EQ(qr.rows.size(), 4u);
    std::unordered_set<int64_t> nodes;
    for (const auto& row : qr.rows) {
        EXPECT_TRUE(nodes.insert(val_to_int64(row[0])).second)
            << "node " << val_to_int64(row[0]) << " emitted more than once";
        ASSERT_EQ(row[2].type_id(), TypeId::PATH);
        const Path& p = row[2].as_path();
        EXPECT_EQ(p.length(), val_to_int64(row[1]));
        expect_cycle_free(p);
    }
}

// ---------------------------------------------------------------------------
// WHERE interactions
// ---------------------------------------------------------------------------

TEST_F(QA_GDB680_Trace, WhereFilteringAllRowsYieldsEmptyResult) {
    auto qr = exec_ok("SELECT name, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "WHERE name = 'Nobody' WITH TRACE");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// BUG (High, filed during GDB-680 QA): standalone `TRAVERSE ... WHERE depth = 2`
// fails at bind time with "column not found: depth" because bind_traverse()
// binds the WHERE expression in a scope that never receives the traversal's
// own output columns (node/depth/source/__path). The parser accepts the
// syntax (pinned by Parser.TraverseWhereThenTrace) but it can never execute.
// This test pins the current graceful-error behavior; once fixed it should be
// rewritten to assert 1 row (node 4) with a complete two-hop path.
TEST_F(QA_GDB680_Trace, StandaloneWhereOnDepthCurrentlyFailsToBind) {
    auto err = exec_err("TRAVERSE follows FROM users(1) WHERE depth = 2 WITH TRACE");
    EXPECT_NE(err.message.find("column not found"), std::string::npos)
        << "actual message: " << err.message;

    // The same filter works through the enriched (FROM TRAVERSE) form.
    auto qr = exec_ok("SELECT __node, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "WHERE __depth = 2 WITH TRACE");
    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.rows[0][1].type_id(), TypeId::PATH);
    const Path& p = qr.rows[0][1].as_path();
    ASSERT_EQ(p.steps.size(), 3u) << "depth-2 hit must keep its full two-hop path";
    EXPECT_EQ(p.steps.front().node_pk, 1);
    EXPECT_EQ(p.steps.back().node_pk, 4);
}

// ---------------------------------------------------------------------------
// Path functions applied to __path
// ---------------------------------------------------------------------------

// BUG (Medium, found during GDB-680 QA): the graph path functions
// (PATH_LENGTH, PATH_COST, NODES, EDGES, RELATIONSHIPS) are implemented in
// the expression evaluator (since GDB-422) but were never added to the
// binder's type resolver, so every SQL use fails at bind time with
// "unknown function". With __path now exposed by WITH TRACE, the natural
// consumers of PATH values are unreachable from SQL. These tests pin the
// current graceful-error behavior; once fixed they should assert
// PATH_LENGTH(__path) == __depth and NODES(__path) == the full chain.
TEST_F(QA_GDB680_Trace, PathLengthFunctionCurrentlyUnknownToBinder) {
    auto err = exec_err("SELECT __depth, PATH_LENGTH(__path) "
                        "FROM TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");
    EXPECT_NE(err.message.find("unknown function"), std::string::npos)
        << "actual message: " << err.message;
}

TEST_F(QA_GDB680_Trace, NodesFunctionCurrentlyUnknownToBinder) {
    auto err = exec_err("SELECT NODES(__path) "
                        "FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                        "WHERE name = 'Zara' WITH TRACE");
    EXPECT_NE(err.message.find("unknown function"), std::string::npos)
        << "actual message: " << err.message;
}

// ---------------------------------------------------------------------------
// __path composed with ORDER BY / LIMIT / DISTINCT
// ---------------------------------------------------------------------------

TEST_F(QA_GDB680_Trace, OrderByDepthDescLimitOneKeepsDeepestPath) {
    auto qr = exec_ok("SELECT __node, __depth, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE "
                      "ORDER BY __depth DESC LIMIT 1");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 5);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 3);
    ASSERT_EQ(qr.rows[0][2].type_id(), TypeId::PATH);
    EXPECT_EQ(qr.rows[0][2].as_path().length(), 3);
}

TEST_F(QA_GDB680_Trace, OrderByPathDoesNotCrashAndKeepsAllRows) {
    // PATH values are not comparable; ORDER BY __path must not crash or drop
    // rows. (compare() returns TYPE_ERROR which the sort treats as equality.)
    auto result = engine_->execute("SELECT __node, __path "
                                   "FROM TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE "
                                   "ORDER BY __path");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 4u);
    for (const auto& row : result->rows) {
        EXPECT_EQ(row[1].type_id(), TypeId::PATH);
    }
}

// ---------------------------------------------------------------------------
// Error paths: __path without TRACE, PATH comparisons in WHERE
// ---------------------------------------------------------------------------

TEST_F(QA_GDB680_Trace, SelectPathWithoutTraceIsBindError) {
    exec_err("SELECT __path FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
}

TEST_F(QA_GDB680_Trace, WhereEqualityOnPathIsGracefulError) {
    // Comparing PATH values is unsupported; the query must fail cleanly with a
    // type error rather than crash or silently misfilter.
    auto result = engine_->execute("SELECT __node "
                                   "FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                                   "WHERE __path = __path WITH TRACE");
    ASSERT_FALSE(result.has_value()) << "PATH equality must not silently succeed";
    EXPECT_FALSE(result.error().message.empty());
}

// ---------------------------------------------------------------------------
// Non-integer and non-(int32/int64) primary keys under TRACE
// ---------------------------------------------------------------------------

TEST_F(QA_GDB680_Trace, StringPkTraceFailsCleanlyAndWithoutTraceWorks) {
    exec_ok("CREATE TABLE people (handle VARCHAR PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES ('a', 'Ann')");
    exec_ok("INSERT INTO people VALUES ('b', 'Ben')");
    exec_ok("CREATE EDGE TYPE knows FROM people TO people");
    exec_ok("LINK people('a') TO people('b') VIA knows");

    // Without TRACE the traversal works.
    auto qr = exec_ok("TRAVERSE knows FROM people('a')");
    EXPECT_EQ(qr.rows.size(), 1u);

    // With TRACE the string PK cannot be encoded into PathStep: clean error.
    auto err = exec_err("TRAVERSE knows FROM people('a') WITH TRACE");
    EXPECT_EQ(err.code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(err.message.find("integer primary keys"), std::string::npos)
        << "actual message: " << err.message;
}

// Operator-level: UINT32 PKs are integers but are rejected by TRACE's
// pk_to_int64 (only int32/int64 variants are handled). Pins the current
// behavior: a clean INVALID_ARGUMENT error, not a crash. See QA report
// (Medium finding) — arguably these should be supported.
TEST(QA_GDB680_TraceOperator, Uint32PkTraceFailsCleanly) {
    Catalog catalog;
    init_test_catalog(catalog);
    GraphEngine graph(catalog);

    TableSchema ts;
    ts.name = "unodes";
    CatalogColumnDef pk_col;
    pk_col.ordinal = 0;
    pk_col.name = "id";
    pk_col.type_id = TypeId::UINT32;
    pk_col.nullable = false;
    ts.columns.push_back(pk_col);
    ts.pk_columns = "id";
    auto tid = catalog.create_table(default_database_id, std::move(ts));
    ASSERT_TRUE(tid.has_value()) << tid.error().message;

    auto eid = graph.create_edge_type(
        default_database_id, "ulinks", *tid, *tid, TypeId::UINT32, TypeId::UINT32, {});
    ASSERT_TRUE(eid.has_value()) << eid.error().message;

    auto linked = graph.link(
        default_database_id, "ulinks", Value(static_cast<uint32_t>(1)), Value(static_cast<uint32_t>(2)));
    ASSERT_TRUE(linked.has_value()) << linked.error().message;

    TraversalConfig config;
    config.edge_type = "ulinks";
    config.start_key = Value(static_cast<uint32_t>(1));
    config.trace = true;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "node", TypeId::UINT32, false, 0});
    cols.push_back({"", "depth", TypeId::INT64, false, 0});
    cols.push_back({"", "__path", TypeId::PATH, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    TraversalOperator op(graph, std::move(config), std::move(schema), nullptr, bound);

    auto open_result = op.open();
    ASSERT_FALSE(open_result.has_value())
        << "UINT32 PKs are currently unsupported by TRACE and must fail at open()";
    EXPECT_EQ(open_result.error().code, StatusCode::INVALID_ARGUMENT);
    op.close();
}

// ---------------------------------------------------------------------------
// Heterogeneous edge types under TRACE
// ---------------------------------------------------------------------------

TEST_F(QA_GDB680_Trace, HeterogeneousEnrichedTraceDistinctPks) {
    exec_ok("CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO posts VALUES (10, 'Hello')");
    exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
    exec_ok("LINK users(1) TO posts(10) VIA authored");

    auto qr = exec_ok("SELECT title, __node, __depth, __path "
                      "FROM TRAVERSE authored FROM users(1) DIRECTION OUT WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Hello");
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 10);
    ASSERT_EQ(qr.rows[0][3].type_id(), TypeId::PATH);
    const Path& p = qr.rows[0][3].as_path();
    ASSERT_EQ(p.steps.size(), 2u);
    EXPECT_EQ(p.steps[0].node_pk, 1);
    EXPECT_EQ(p.steps[1].node_pk, 10);
    EXPECT_EQ(p.length(), 1);
}

// Regression for GDB-694 (Critical hang found during GDB-680 QA). When a
// heterogeneous edge links a target node whose PK equals the start node's
// PK (users(1) -> posts(1)), the BFS records the node as its own parent
// (visited is intentionally not seeded with the start key for heterogeneous
// edges) and reconstruct_path() used to loop forever, growing the steps
// vector until OOM. GDB-694's depth-bounded reconstruction fixes this:
// one row with a two-step path [1, 1].
TEST_F(QA_GDB680_Trace, HeterogeneousEnrichedTraceSamePkHangs) {
    exec_ok("CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");
    exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    auto qr = exec_ok("SELECT title, __node, __depth, __path "
                      "FROM TRAVERSE authored FROM users(1) DIRECTION OUT WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.rows[0][3].type_id(), TypeId::PATH);
    const Path& p = qr.rows[0][3].as_path();
    EXPECT_EQ(p.steps.size(), 2u) << "path must be start -> target, not an infinite self-chain";
}

// Regression for GDB-694: same root cause as above, MODE EDGES flavor —
// collect_edges() calls reconstruct_path(edge.source_pk) and used to hang
// when source PK == start PK has a self-referential parent_map entry.
// Fixed in GDB-694: one edge row whose path is the trivial [1].
TEST_F(QA_GDB680_Trace, HeterogeneousEdgeModeTraceSamePkHangs) {
    exec_ok("CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");
    exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    auto qr = exec_ok("SELECT __from, __to, __path "
                      "FROM TRAVERSE authored FROM users(1) DIRECTION OUT MODE EDGES WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.rows[0][2].type_id(), TypeId::PATH);
    const Path& p = qr.rows[0][2].as_path();
    EXPECT_EQ(p.steps.size(), 1u) << "path to the start node must be the trivial single step";
    EXPECT_EQ(p.steps[0].node_pk, 1);
}

// ---------------------------------------------------------------------------
// Edge mode + cycles
// ---------------------------------------------------------------------------

TEST_F(QA_GDB680_Trace, EdgeModeTraceWithBackEdgeStaysCycleFree) {
    exec_ok("LINK users(5) TO users(1) VIA follows"); // close the big cycle

    auto qr = exec_ok("SELECT __from, __to, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES WITH TRACE");

    // 6 edges now: the diamond's five plus 5->1.
    ASSERT_EQ(qr.rows.size(), 6u);
    bool saw_back_edge = false;
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row[2].type_id(), TypeId::PATH);
        const Path& p = row[2].as_path();
        expect_cycle_free(p);
        ASSERT_FALSE(p.steps.empty());
        EXPECT_EQ(p.steps.front().node_pk, 1);
        EXPECT_EQ(p.steps.back().node_pk, val_to_int64(row[0]))
            << "edge-mode path must end at the edge's __from node";
        if (val_to_int64(row[0]) == 5 && val_to_int64(row[1]) == 1) {
            saw_back_edge = true;
            EXPECT_EQ(p.steps.size(), 4u) << "path to node 5 must span the full chain";
        }
    }
    EXPECT_TRUE(saw_back_edge) << "the back edge 5->1 must be collected";
}

TEST_F(QA_GDB680_Trace, DirectionBothWithCyclePathsStayAcyclic) {
    exec_ok("LINK users(5) TO users(1) VIA follows");

    auto qr = exec_ok("SELECT __node, __depth, __path "
                      "FROM TRAVERSE follows FROM users(3) DIRECTION BOTH WITH TRACE");

    // BOTH from 3 reaches every other node.
    ASSERT_EQ(qr.rows.size(), 4u);
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row[2].type_id(), TypeId::PATH);
        const Path& p = row[2].as_path();
        expect_cycle_free(p);
        EXPECT_EQ(p.steps.front().node_pk, 3);
        EXPECT_EQ(p.steps.back().node_pk, val_to_int64(row[0]));
        EXPECT_EQ(p.length(), val_to_int64(row[1]));
    }
}

// ---------------------------------------------------------------------------
// Stress: deep chain
// ---------------------------------------------------------------------------

TEST_F(QA_GDB680_Trace, DeepChainTraceStress) {
    constexpr int64_t chain_len = 200;
    exec_ok("CREATE TABLE chain (id INT PRIMARY KEY)");
    for (int64_t i = 1; i <= chain_len; ++i) {
        exec_ok("INSERT INTO chain VALUES (" + std::to_string(i) + ")");
    }
    exec_ok("CREATE EDGE TYPE next_link FROM chain TO chain");
    for (int64_t i = 1; i < chain_len; ++i) {
        exec_ok("LINK chain(" + std::to_string(i) + ") TO chain(" + std::to_string(i + 1) +
                ") VIA next_link");
    }

    auto qr = exec_ok("SELECT __node, __depth, __path "
                      "FROM TRAVERSE next_link FROM chain(1) DIRECTION OUT "
                      "MAX_DEPTH 250 WITH TRACE");

    ASSERT_EQ(qr.rows.size(), static_cast<size_t>(chain_len - 1));
    int64_t max_depth_seen = 0;
    for (const auto& row : qr.rows) {
        const int64_t node = val_to_int64(row[0]);
        const int64_t depth = val_to_int64(row[1]);
        max_depth_seen = std::max(max_depth_seen, depth);
        ASSERT_EQ(row[2].type_id(), TypeId::PATH);
        const Path& p = row[2].as_path();
        ASSERT_EQ(static_cast<int64_t>(p.steps.size()), depth + 1);
        EXPECT_EQ(p.steps.front().node_pk, 1);
        EXPECT_EQ(p.steps.back().node_pk, node);
        EXPECT_EQ(p.steps.back().edge_id, -1);
        // The chain path must be strictly sequential: 1, 2, ..., node.
        for (size_t s = 0; s < p.steps.size(); ++s) {
            ASSERT_EQ(p.steps[s].node_pk, static_cast<int64_t>(s) + 1)
                << "chain path must be strictly sequential";
            if (s + 1 < p.steps.size()) {
                EXPECT_GE(p.steps[s].edge_id, 0) << "non-terminal steps must carry a real edge id";
            }
        }
    }
    EXPECT_EQ(max_depth_seen, chain_len - 1);
}

// ---------------------------------------------------------------------------
// Schema sanity: no __path leakage without TRACE
// ---------------------------------------------------------------------------

TEST_F(QA_GDB680_Trace, NoPathColumnWithoutTrace) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    EXPECT_FALSE(col_index(qr, "__path").has_value())
        << "__path must only exist when WITH TRACE is specified";

    auto standalone = exec_ok("TRAVERSE follows FROM users(1)");
    EXPECT_FALSE(col_index(standalone, "__path").has_value());
}

} // namespace
} // namespace sixseven
