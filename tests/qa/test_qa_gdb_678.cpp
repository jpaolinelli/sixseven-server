// QA adversarial tests for GDB-678: BFS parent tracking and path
// reconstruction with the `trace` flag (__path column) across the standalone,
// enriched, and edge traversal operators.
//
// These tests try to BREAK the implementation: disconnected/unreachable nodes,
// self-loops, multi-edge between the same pair, deep/wide graphs, empty graphs,
// cycles, trace on/off boundary behavior, path correctness at depth boundaries,
// and edge-id alignment / non-integer PK error paths.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/edge_traversal.h"
#include "sixseven/executor/enriched_traversal.h"
#include "sixseven/executor/traversal.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Fixture: homogeneous INT64 graph with a configurable edge set.
// ---------------------------------------------------------------------------
class QA_Gdb678 : public ::testing::Test {
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

        auto eid = graph_->create_edge_type(default_database_id, "follows", table_id_, table_id_,
                                            TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;
    }

    uint64_t link(int64_t from, int64_t to) {
        auto r = graph_->link(default_database_id, "follows", Value(from), Value(to));
        EXPECT_TRUE(r.has_value()) << r.error().message;
        return r.has_value() ? *r : 0;
    }

    // Run a standalone (non-fetch) traced traversal; return node_pk -> Path.
    std::unordered_map<int64_t, Path> traced_paths(int64_t start,
                                                   TraverseDirection dir = TraverseDirection::OUT,
                                                   int32_t max_depth = 100,
                                                   size_t max_visited = 100000) {
        TraversalConfig config;
        config.edge_type = "follows";
        config.start_key = Value(start);
        config.direction = dir;
        config.max_depth = max_depth;
        config.max_visited = max_visited;
        config.trace = true;

        std::vector<OutputColumn> cols;
        cols.push_back({"", "node", TypeId::INT64, false, 0});
        cols.push_back({"", "depth", TypeId::INT64, false, 0});
        cols.push_back({"", "__path", TypeId::PATH, false, 0});
        OutputSchema schema(std::move(cols));

        BoundStatement bound;
        TraversalOperator op(*graph_, std::move(config), std::move(schema), nullptr, bound);
        auto open_result = op.open();
        EXPECT_TRUE(open_result.has_value()) << open_result.error().message;

        std::unordered_map<int64_t, Path> paths;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value())
                break;
            auto& vals = row->value().values;
            EXPECT_EQ(vals.size(), 3u);
            if (vals.size() == 3u && vals[2].type_id() == TypeId::PATH) {
                paths.emplace(vals[0].as_int64(), vals[2].as_path());
            }
        }
        op.close();
        return paths;
    }

    // Assert that a Path is well-formed under the match_shortest_path convention:
    // starts at `start`, ends at `target`, terminal edge_id == -1, no repeated
    // node, and every non-terminal edge_id is a real (>= 0) edge row id.
    static void check_path(const Path& p, int64_t start, int64_t target) {
        ASSERT_FALSE(p.steps.empty());
        EXPECT_EQ(p.steps.front().node_pk, start);
        EXPECT_EQ(p.steps.back().node_pk, target);
        EXPECT_EQ(p.steps.back().edge_id, -1) << "terminal step must have edge_id -1";
        std::unordered_set<int64_t> seen;
        for (size_t i = 0; i < p.steps.size(); ++i) {
            EXPECT_TRUE(seen.insert(p.steps[i].node_pk).second)
                << "repeated node in path: " << p.steps[i].node_pk;
            if (i + 1 < p.steps.size()) {
                EXPECT_GE(p.steps[i].edge_id, 0)
                    << "non-terminal step " << i << " must reference a real edge";
            }
        }
        EXPECT_EQ(p.length(), static_cast<int64_t>(p.steps.size()) - 1);
    }

    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t table_id_ = 0;
};

// --- Empty graph: start node has no edges -> zero result rows, no crash. -----
TEST_F(QA_Gdb678, EmptyGraphNoEdgesYieldsNoRows) {
    auto paths = traced_paths(1);
    EXPECT_TRUE(paths.empty());
}

// --- Self-loop on the start node: must not appear (start is pre-visited). -----
TEST_F(QA_Gdb678, SelfLoopOnStartProducesNoPath) {
    link(1, 1);
    auto paths = traced_paths(1);
    EXPECT_TRUE(paths.empty()) << "self-loop on start node should not yield a result row";
}

// --- Self-loop on a reachable non-start node: path stays acyclic. ------------
TEST_F(QA_Gdb678, SelfLoopOnReachableNode) {
    link(1, 2);
    link(2, 2); // self loop on 2
    auto paths = traced_paths(1);
    ASSERT_EQ(paths.size(), 1u);
    check_path(paths.at(2), 1, 2);
    EXPECT_EQ(paths.at(2).steps.size(), 2u);
}

// --- Disconnected component is unreachable: never appears in results. --------
TEST_F(QA_Gdb678, DisconnectedComponentUnreachable) {
    link(1, 2);
    link(10, 11); // separate component
    auto paths = traced_paths(1);
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_TRUE(paths.count(2));
    EXPECT_FALSE(paths.count(10));
    EXPECT_FALSE(paths.count(11));
}

// --- Multi-edge between same pair: path uses exactly one valid edge id. ------
TEST_F(QA_Gdb678, MultiEdgeBetweenSamePairUsesValidEdgeId) {
    uint64_t e1 = link(1, 2);
    uint64_t e2 = link(1, 2); // parallel edge
    auto paths = traced_paths(1);
    ASSERT_EQ(paths.size(), 1u);
    const Path& p = paths.at(2);
    check_path(p, 1, 2);
    ASSERT_EQ(p.steps.size(), 2u);
    int64_t used = p.steps[0].edge_id;
    EXPECT_TRUE(used == static_cast<int64_t>(e1) || used == static_cast<int64_t>(e2))
        << "path edge id " << used << " must be one of the two real parallel edges (" << e1 << ","
        << e2 << ")";
}

// --- Diamond: BFS picks one shortest path; both legs are length 2. -----------
TEST_F(QA_Gdb678, DiamondShortestPathIsTwoHops) {
    link(1, 2);
    link(1, 3);
    link(2, 4);
    link(3, 4);
    auto paths = traced_paths(1);
    ASSERT_EQ(paths.size(), 3u);
    check_path(paths.at(4), 1, 4);
    EXPECT_EQ(paths.at(4).length(), 2) << "node 4 is two hops away in the diamond";
    // The middle node must be either 2 or 3.
    int64_t mid = paths.at(4).steps[1].node_pk;
    EXPECT_TRUE(mid == 2 || mid == 3);
}

// --- Deep chain: depth-50 path is fully aligned end to end. ------------------
TEST_F(QA_Gdb678, DeepChainPathAlignment) {
    const int64_t depth = 50;
    for (int64_t i = 1; i < depth + 1; ++i) {
        link(i, i + 1);
    }
    auto paths = traced_paths(1);
    ASSERT_EQ(paths.size(), static_cast<size_t>(depth));
    const Path& p = paths.at(depth + 1);
    check_path(p, 1, depth + 1);
    ASSERT_EQ(p.steps.size(), static_cast<size_t>(depth + 1));
    for (int64_t i = 0; i < depth + 1; ++i) {
        EXPECT_EQ(p.steps[i].node_pk, i + 1) << "node at step " << i << " misaligned";
    }
}

// --- Wide fan-out: every leaf is exactly one hop with a valid single-edge path.
TEST_F(QA_Gdb678, WideFanOutAllOneHop) {
    const int64_t width = 200;
    for (int64_t i = 2; i < width + 2; ++i) {
        link(1, i);
    }
    auto paths = traced_paths(1);
    ASSERT_EQ(paths.size(), static_cast<size_t>(width));
    for (int64_t i = 2; i < width + 2; ++i) {
        ASSERT_TRUE(paths.count(i)) << "missing leaf " << i;
        check_path(paths.at(i), 1, i);
        EXPECT_EQ(paths.at(i).length(), 1);
    }
}

// --- Depth-boundary: max_depth cuts the path off exactly at the limit. -------
TEST_F(QA_Gdb678, DepthBoundaryPathLength) {
    for (int64_t i = 1; i < 6; ++i) {
        link(i, i + 1); // chain 1..6
    }
    auto paths = traced_paths(1, TraverseDirection::OUT, 2);
    // Only nodes 2 and 3 reachable within depth 2.
    ASSERT_EQ(paths.size(), 2u);
    ASSERT_TRUE(paths.count(3));
    check_path(paths.at(3), 1, 3);
    EXPECT_EQ(paths.at(3).length(), 2);
    EXPECT_FALSE(paths.count(4));
}

// --- Cycle: a back-edge must not corrupt or lengthen the discovered path. ----
TEST_F(QA_Gdb678, CycleBackEdgeKeepsShortestPath) {
    link(1, 2);
    link(2, 3);
    link(3, 1); // cycle back to start
    auto paths = traced_paths(1);
    ASSERT_EQ(paths.size(), 2u);
    check_path(paths.at(2), 1, 2);
    check_path(paths.at(3), 1, 3);
    EXPECT_EQ(paths.at(3).length(), 2);
    // Start node 1 must never re-appear as a result row.
    EXPECT_FALSE(paths.count(1));
}

// --- Trace OFF (default): no __path column appended at all. ------------------
TEST_F(QA_Gdb678, TraceOffEmitsNoPathColumn) {
    link(1, 2);
    TraversalConfig config;
    config.edge_type = "follows";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;
    // trace defaults false

    std::vector<OutputColumn> cols;
    cols.push_back({"", "node", TypeId::INT64, false, 0});
    cols.push_back({"", "depth", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    TraversalOperator op(*graph_, std::move(config), std::move(schema), nullptr, bound);
    ASSERT_TRUE(op.open().has_value());
    auto row = op.next();
    ASSERT_TRUE(row.has_value());
    ASSERT_TRUE(row->has_value());
    EXPECT_EQ(row->value().values.size(), 2u) << "trace off must not add a column";
    op.close();
}

// --- Re-open after close: parent_map cleared, second run is identical. -------
TEST_F(QA_Gdb678, ReopenAfterCloseIsConsistent) {
    link(1, 2);
    link(2, 3);
    auto first = traced_paths(1);
    auto second = traced_paths(1); // fresh operator each call
    ASSERT_EQ(first.size(), second.size());
    for (const auto& [k, p] : first) {
        ASSERT_TRUE(second.count(k));
        EXPECT_EQ(p, second.at(k)) << "re-run path for node " << k << " differs";
    }
}

// --- Non-integer PK with trace: reconstruct_path must error, not crash. ------
TEST_F(QA_Gdb678, NonIntegerPkWithTraceReturnsError) {
    // Build a separate STRING-keyed edge type.
    TableSchema ts;
    ts.name = "snodes";
    CatalogColumnDef pk_col;
    pk_col.ordinal = 0;
    pk_col.name = "id";
    pk_col.type_id = TypeId::STRING;
    pk_col.nullable = false;
    ts.columns.push_back(pk_col);
    ts.pk_columns = "id";
    auto stid = catalog_->create_table(default_database_id, std::move(ts));
    ASSERT_TRUE(stid.has_value()) << stid.error().message;

    auto eid = graph_->create_edge_type(default_database_id, "sfollows", *stid, *stid,
                                        TypeId::STRING, TypeId::STRING, {});
    ASSERT_TRUE(eid.has_value()) << eid.error().message;
    ASSERT_TRUE(graph_->link(default_database_id, "sfollows", Value(std::string("a")),
                             Value(std::string("b")))
                    .has_value());

    TraversalConfig config;
    config.edge_type = "sfollows";
    config.start_key = Value(std::string("a"));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;
    config.trace = true;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "node", TypeId::STRING, false, 0});
    cols.push_back({"", "depth", TypeId::INT64, false, 0});
    cols.push_back({"", "__path", TypeId::PATH, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    TraversalOperator op(*graph_, std::move(config), std::move(schema), nullptr, bound);
    auto open_result = op.open();
    ASSERT_FALSE(open_result.has_value()) << "string PK + trace must produce an error, not a path";
    EXPECT_EQ(open_result.error().code, StatusCode::INVALID_ARGUMENT);
}

// --- IN direction trace: path is start -> ... -> target along reverse edges. -
TEST_F(QA_Gdb678, InDirectionTracePath) {
    // Edges 2->1, 3->2 : from node 1, IN direction reaches 2 (depth1), 3 (depth2).
    link(2, 1);
    link(3, 2);
    auto paths = traced_paths(1, TraverseDirection::IN);
    ASSERT_EQ(paths.size(), 2u);
    check_path(paths.at(2), 1, 2);
    check_path(paths.at(3), 1, 3);
    EXPECT_EQ(paths.at(3).length(), 2);
}

// ---------------------------------------------------------------------------
// EdgeTraversalOperator (__path to the __from node).
// ---------------------------------------------------------------------------
TEST_F(QA_Gdb678, EdgeTraversalTracePathToFromNode) {
    link(1, 2);
    link(2, 3);

    TraversalConfig config;
    config.edge_type = "follows";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;
    config.trace = true;

    // Schema: __from, __to, __depth, __path  (no edge props).
    std::vector<OutputColumn> cols;
    cols.push_back({"", "__from", TypeId::INT64, false, 0});
    cols.push_back({"", "__to", TypeId::INT64, false, 0});
    cols.push_back({"", "__depth", TypeId::INT64, false, 0});
    cols.push_back({"", "__path", TypeId::PATH, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    EdgeTraversalOperator op(*graph_, std::move(config), std::move(schema), nullptr, bound,
                             /*heterogeneous=*/false);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::unordered_map<int64_t, Path> path_by_from; // __from -> path
    int rows = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value())
            break;
        auto& vals = row->value().values;
        ASSERT_EQ(vals.size(), 4u);
        ASSERT_EQ(vals[3].type_id(), TypeId::PATH);
        int64_t from = vals[0].as_int64();
        path_by_from.emplace(from, vals[3].as_path());
        ++rows;
    }
    op.close();

    ASSERT_EQ(rows, 2); // edges 1->2 and 2->3
    // Path to __from=1 is just [1].
    ASSERT_TRUE(path_by_from.count(1));
    check_path(path_by_from.at(1), 1, 1);
    EXPECT_EQ(path_by_from.at(1).steps.size(), 1u);
    // Path to __from=2 is [1,2].
    ASSERT_TRUE(path_by_from.count(2));
    check_path(path_by_from.at(2), 1, 2);
    EXPECT_EQ(path_by_from.at(2).length(), 1);
}

// --- Edge traversal trace OFF: only 3 meta columns, no path. ----------------
TEST_F(QA_Gdb678, EdgeTraversalTraceOffNoPathColumn) {
    link(1, 2);
    TraversalConfig config;
    config.edge_type = "follows";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "__from", TypeId::INT64, false, 0});
    cols.push_back({"", "__to", TypeId::INT64, false, 0});
    cols.push_back({"", "__depth", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    EdgeTraversalOperator op(*graph_, std::move(config), std::move(schema), nullptr, bound, false);
    ASSERT_TRUE(op.open().has_value());
    auto row = op.next();
    ASSERT_TRUE(row.has_value());
    ASSERT_TRUE(row->has_value());
    EXPECT_EQ(row->value().values.size(), 3u);
    op.close();
}

} // namespace
} // namespace sixseven
