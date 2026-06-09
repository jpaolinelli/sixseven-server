// GDB-680: TRACE integration tests — basic traversal layer.
//
// End-to-end TRACE coverage at the TraversalOperator level, exercising the
// BFS parent tracking and path reconstruction added in GDB-678 across all
// operator configuration combinations:
//
//   * __path column carries a PATH-typed value for every row
//   * path length matches the depth column for each row
//   * the start node is the first step in every path
//   * paths remain cycle-free when the graph contains a cycle (5 -> 1)
//   * TRACE + FETCH emit (node, depth, source, __path)
//   * MAX_DEPTH bounds the length of every reconstructed path
//   * DIRECTION IN produces correct reverse paths
//   * DIRECTION BOTH paths stay rooted at the start node
//
// Enriched (SELECT ... FROM TRAVERSE), WHERE+TRACE, and MODE EDGES coverage
// lives in test_trace_enriched.cpp.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/traversal.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

/// A single traced BFS row: node PK, depth, optional FETCH source, and path.
struct TracedRow {
    int64_t node = 0;
    int64_t depth = 0;
    Value source; ///< Only populated when fetch is enabled.
    Path path;
};

/// Fixture mirroring the canonical traversal graph used across the suite:
///   1 -> 2, 1 -> 3, 2 -> 4, 3 -> 4, 4 -> 5
class TraceTraversalTest : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        TableSchema ts;
        ts.name = "nodes";
        CatalogColumnDef pk_col;
        pk_col.ordinal = 0;
        pk_col.name = "id";
        pk_col.type_id = TypeId::INT64;
        pk_col.nullable = false;
        ts.columns.push_back(pk_col);
        ts.pk_columns = "id";
        auto tid = catalog_->create_table(default_database_id, std::move(ts));
        ASSERT_TRUE(tid.has_value()) << tid.error().message;
        table_id_ = *tid;

        auto eid = graph_->create_edge_type(
            default_database_id, "follows", table_id_, table_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        link(1, 2);
        link(1, 3);
        link(2, 4);
        link(3, 4);
        link(4, 5);
    }

    void link(int64_t from, int64_t to) {
        auto r = graph_->link(default_database_id, "follows", Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    /// Run a traced BFS and collect all rows keyed by node PK. Asserts that
    /// every row carries a PATH-typed value in the trailing __path column.
    std::unordered_map<int64_t, TracedRow> run_trace(int64_t start,
                                                     TraverseDirection dir = TraverseDirection::OUT,
                                                     int32_t max_depth = 100,
                                                     bool fetch = false) {
        TraversalConfig config;
        config.edge_type = "follows";
        config.start_key = Value(start);
        config.direction = dir;
        config.max_depth = max_depth;
        config.fetch = fetch;
        config.trace = true;

        std::vector<OutputColumn> cols;
        cols.push_back({"", "node", TypeId::INT64, false, 0});
        cols.push_back({"", "depth", TypeId::INT64, false, 0});
        if (fetch) {
            cols.push_back({"", "source", TypeId::INT64, true, 0});
        }
        cols.push_back({"", "__path", TypeId::PATH, false, 0});
        const size_t path_idx = cols.size() - 1;
        OutputSchema schema(std::move(cols));

        BoundStatement bound;
        TraversalOperator op(*graph_, std::move(config), std::move(schema), nullptr, bound);

        auto open_result = op.open();
        EXPECT_TRUE(open_result.has_value()) << open_result.error().message;

        std::unordered_map<int64_t, TracedRow> rows;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row.has_value() || !row->has_value()) {
                break;
            }
            auto& vals = row->value().values;
            EXPECT_EQ(vals.size(), path_idx + 1);
            EXPECT_EQ(vals[path_idx].type_id(), TypeId::PATH)
                << "__path must carry a PATH value for every row";

            TracedRow traced;
            traced.node = vals[0].as_int64();
            traced.depth = vals[1].as_int64();
            if (fetch) {
                traced.source = vals[2];
            }
            traced.path = vals[path_idx].as_path();
            rows.emplace(traced.node, std::move(traced));
        }
        op.close();
        return rows;
    }

    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t table_id_ = 0;
};

/// Assert a path has no repeated node PKs.
void expect_cycle_free(const Path& path) {
    std::unordered_set<int64_t> seen;
    for (const auto& step : path.steps) {
        EXPECT_TRUE(seen.insert(step.node_pk).second)
            << "path contains a repeated node: " << step.node_pk;
    }
}

// ---------------------------------------------------------------------------
// AC: __path column exists and carries PATH values when trace is enabled
// ---------------------------------------------------------------------------

TEST_F(TraceTraversalTest, EveryRowCarriesPathValue) {
    auto rows = run_trace(1);

    // BFS from 1 reaches 2, 3, 4, 5; run_trace already asserts PATH typing.
    ASSERT_EQ(rows.size(), 4u);
    for (const auto& [node, traced] : rows) {
        EXPECT_FALSE(traced.path.steps.empty()) << "path for node " << node << " must be non-empty";
    }
}

// ---------------------------------------------------------------------------
// AC: start node is the first step in every path; the result node is the last
// ---------------------------------------------------------------------------

TEST_F(TraceTraversalTest, StartNodeIsFirstStepInEveryPath) {
    auto rows = run_trace(1);

    ASSERT_EQ(rows.size(), 4u);
    for (const auto& [node, traced] : rows) {
        ASSERT_FALSE(traced.path.steps.empty());
        EXPECT_EQ(traced.path.steps.front().node_pk, 1)
            << "path for node " << node << " must start at the BFS origin";
        EXPECT_EQ(traced.path.steps.back().node_pk, node)
            << "path must terminate at the result node";
        EXPECT_EQ(traced.path.steps.back().edge_id, -1)
            << "terminal step must carry no outgoing edge";
    }
}

// ---------------------------------------------------------------------------
// AC: path length matches the depth column for each row
// ---------------------------------------------------------------------------

TEST_F(TraceTraversalTest, PathLengthMatchesDepth) {
    auto rows = run_trace(1);

    ASSERT_EQ(rows.size(), 4u);
    for (const auto& [node, traced] : rows) {
        EXPECT_EQ(traced.path.length(), traced.depth)
            << "node " << node << ": path hop count must equal BFS depth";
    }

    // Spot-check known depths in the diamond graph.
    EXPECT_EQ(rows.at(2).depth, 1);
    EXPECT_EQ(rows.at(3).depth, 1);
    EXPECT_EQ(rows.at(4).depth, 2);
    EXPECT_EQ(rows.at(5).depth, 3);
}

// ---------------------------------------------------------------------------
// AC: TRACE with MAX_DEPTH limits path lengths correctly
// ---------------------------------------------------------------------------

TEST_F(TraceTraversalTest, MaxDepthLimitsPathLengths) {
    auto rows = run_trace(1, TraverseDirection::OUT, 2);

    // Depth limit 2 excludes node 5 (depth 3).
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows.count(5), 0u);
    for (const auto& [node, traced] : rows) {
        EXPECT_LE(traced.path.length(), 2) << "node " << node << ": path exceeds MAX_DEPTH";
        EXPECT_EQ(traced.path.length(), traced.depth);
    }
    EXPECT_EQ(rows.at(4).path.length(), 2);
}

TEST_F(TraceTraversalTest, MaxDepthOneYieldsSingleHopPaths) {
    auto rows = run_trace(1, TraverseDirection::OUT, 1);

    // Only direct neighbors 2 and 3.
    ASSERT_EQ(rows.size(), 2u);
    for (const auto& [node, traced] : rows) {
        ASSERT_EQ(traced.path.steps.size(), 2u) << "node " << node;
        EXPECT_EQ(traced.path.steps.front().node_pk, 1);
        EXPECT_EQ(traced.path.length(), 1);
        // The non-terminal step must reference a real edge row.
        EXPECT_GE(traced.path.steps.front().edge_id, 0);
    }
}

// ---------------------------------------------------------------------------
// AC: TRACE + FETCH both work together
// ---------------------------------------------------------------------------

TEST_F(TraceTraversalTest, FetchWithTraceEmitsSourceAndPath) {
    auto rows = run_trace(1, TraverseDirection::OUT, 100, /*fetch=*/true);

    ASSERT_EQ(rows.size(), 4u);
    for (const auto& [node, traced] : rows) {
        // FETCH adds the source column; TRACE still appends a valid path.
        EXPECT_EQ(traced.path.length(), traced.depth) << "node " << node;
        EXPECT_EQ(traced.path.steps.front().node_pk, 1);
        EXPECT_EQ(traced.path.steps.back().node_pk, node);

        // The FETCH source is the BFS parent: it must be the second-to-last
        // step of the traced path.
        ASSERT_FALSE(traced.source.is_null()) << "node " << node << ": source must be populated";
        ASSERT_GE(traced.path.steps.size(), 2u);
        EXPECT_EQ(traced.source.as_int64(), traced.path.steps[traced.path.steps.size() - 2].node_pk)
            << "node " << node << ": FETCH source must match the path's penultimate step";
    }
}

// ---------------------------------------------------------------------------
// AC: paths are cycle-free (add edge 5 -> 1, verify no cycles in paths)
// ---------------------------------------------------------------------------

TEST_F(TraceTraversalTest, CycleEdgeProducesCycleFreePaths) {
    link(5, 1); // close the cycle 1 -> ... -> 5 -> 1

    auto rows = run_trace(1);

    // The cycle must not add rows (1 is the start and stays visited) nor
    // corrupt any path.
    ASSERT_EQ(rows.size(), 4u);
    for (const auto& [node, traced] : rows) {
        expect_cycle_free(traced.path);
        EXPECT_EQ(traced.path.length(), traced.depth) << "node " << node;
        EXPECT_EQ(traced.path.steps.front().node_pk, 1);
    }
}

TEST_F(TraceTraversalTest, InnerCycleProducesCycleFreePaths) {
    // A tighter cycle 4 -> 2 inside the diamond must also stay acyclic.
    link(4, 2);

    auto rows = run_trace(1);
    ASSERT_EQ(rows.size(), 4u);
    for (const auto& [node, traced] : rows) {
        expect_cycle_free(traced.path);
        EXPECT_EQ(traced.path.length(), traced.depth) << "node " << node;
    }
}

// ---------------------------------------------------------------------------
// AC: TRACE with DIRECTION IN produces correct reverse paths
// ---------------------------------------------------------------------------

TEST_F(TraceTraversalTest, DirectionInProducesReversePaths) {
    auto rows = run_trace(5, TraverseDirection::IN);

    // Reverse BFS from 5: 4 (depth 1), 2 and 3 (depth 2), 1 (depth 3).
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows.at(4).depth, 1);
    EXPECT_EQ(rows.at(2).depth, 2);
    EXPECT_EQ(rows.at(3).depth, 2);
    EXPECT_EQ(rows.at(1).depth, 3);

    for (const auto& [node, traced] : rows) {
        EXPECT_EQ(traced.path.steps.front().node_pk, 5)
            << "reverse path for node " << node << " must start at the BFS origin";
        EXPECT_EQ(traced.path.steps.back().node_pk, node);
        EXPECT_EQ(traced.path.length(), traced.depth);
        expect_cycle_free(traced.path);
    }

    // The reverse path to 1 must run 5 -> 4 -> (2 or 3) -> 1.
    const Path& p1 = rows.at(1).path;
    ASSERT_EQ(p1.steps.size(), 4u);
    EXPECT_EQ(p1.steps[0].node_pk, 5);
    EXPECT_EQ(p1.steps[1].node_pk, 4);
    EXPECT_TRUE(p1.steps[2].node_pk == 2 || p1.steps[2].node_pk == 3)
        << "unexpected intermediate node: " << p1.steps[2].node_pk;
    EXPECT_EQ(p1.steps[3].node_pk, 1);
}

TEST_F(TraceTraversalTest, DirectionBothPathsStayRootedAtStart) {
    auto rows = run_trace(4, TraverseDirection::BOTH);

    // BOTH from 4 reaches every other node: 2, 3, 5 (depth 1) and 1 (depth 2).
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows.at(2).depth, 1);
    EXPECT_EQ(rows.at(3).depth, 1);
    EXPECT_EQ(rows.at(5).depth, 1);
    EXPECT_EQ(rows.at(1).depth, 2);

    for (const auto& [node, traced] : rows) {
        EXPECT_EQ(traced.path.steps.front().node_pk, 4)
            << "BOTH-direction path for node " << node << " must start at the BFS origin";
        EXPECT_EQ(traced.path.steps.back().node_pk, node);
        EXPECT_EQ(traced.path.length(), traced.depth);
        expect_cycle_free(traced.path);
    }
}

} // namespace
} // namespace sixseven
