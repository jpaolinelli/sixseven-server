#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/traversal.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace sixseven {
namespace {

/// Test fixture for BFS traversal tests.
///
/// Builds a small graph:
///   1 → 2, 1 → 3, 2 → 4, 3 → 4, 4 → 5
class TraversalTest : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = std::make_unique<Catalog>();
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        // Create a dummy table so catalog has a table_id.
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

        // Create edge type.
        auto eid = graph_->create_edge_type(
            "follows", table_id_, table_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Build graph: 1→2, 1→3, 2→4, 3→4, 4→5
        link(1, 2);
        link(1, 3);
        link(2, 4);
        link(3, 4);
        link(4, 5);
    }

    void link(int64_t from, int64_t to) {
        auto r = graph_->link("follows", Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    /// Helper to run a BFS and collect all node PKs.
    std::vector<int64_t> run_bfs(int64_t start,
                                 TraverseDirection dir = TraverseDirection::OUT,
                                 int32_t max_depth = 100,
                                 size_t max_visited = 100000) {
        TraversalConfig config;
        config.edge_type = "follows";
        config.start_key = Value(start);
        config.direction = dir;
        config.max_depth = max_depth;
        config.max_visited = max_visited;

        std::vector<OutputColumn> cols;
        cols.push_back({"", "node", TypeId::INT64, false, 0});
        cols.push_back({"", "depth", TypeId::INT64, false, 0});
        OutputSchema schema(std::move(cols));

        BoundStatement bound;
        TraversalOperator op(*graph_, std::move(config), std::move(schema), nullptr, bound);

        auto open_result = op.open();
        EXPECT_TRUE(open_result.has_value()) << open_result.error().message;

        std::vector<int64_t> nodes;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value()) {
                break;
            }
            nodes.push_back(row->value().values[0].as_int64());
        }
        op.close();
        return nodes;
    }

    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t table_id_ = 0;
};

TEST_F(TraversalTest, BasicOutwardTraversal) {
    auto nodes = run_bfs(1);
    // BFS from 1: should reach 2, 3, 4, 5
    ASSERT_EQ(nodes.size(), 4);
    // Level 1: 2, 3 (order may vary)
    std::vector<int64_t> level1(nodes.begin(), nodes.begin() + 2);
    std::sort(level1.begin(), level1.end());
    EXPECT_EQ(level1[0], 2);
    EXPECT_EQ(level1[1], 3);
    // Level 2: 4
    EXPECT_EQ(nodes[2], 4);
    // Level 3: 5
    EXPECT_EQ(nodes[3], 5);
}

TEST_F(TraversalTest, DepthLimit) {
    auto nodes = run_bfs(1, TraverseDirection::OUT, 1);
    // MAX_DEPTH=1: only immediate neighbors
    ASSERT_EQ(nodes.size(), 2);
    std::sort(nodes.begin(), nodes.end());
    EXPECT_EQ(nodes[0], 2);
    EXPECT_EQ(nodes[1], 3);
}

TEST_F(TraversalTest, DepthLimitTwo) {
    auto nodes = run_bfs(1, TraverseDirection::OUT, 2);
    // MAX_DEPTH=2: 2, 3, 4 (but not 5)
    ASSERT_EQ(nodes.size(), 3);
    std::sort(nodes.begin(), nodes.end());
    EXPECT_EQ(nodes[0], 2);
    EXPECT_EQ(nodes[1], 3);
    EXPECT_EQ(nodes[2], 4);
}

TEST_F(TraversalTest, InwardTraversal) {
    auto nodes = run_bfs(5, TraverseDirection::IN);
    // Reverse from 5: should reach 4, then 2 and 3, then 1
    ASSERT_EQ(nodes.size(), 4);
    EXPECT_EQ(nodes[0], 4); // depth 1
    // depth 2: 2, 3
    std::vector<int64_t> level2(nodes.begin() + 1, nodes.begin() + 3);
    std::sort(level2.begin(), level2.end());
    EXPECT_EQ(level2[0], 2);
    EXPECT_EQ(level2[1], 3);
    EXPECT_EQ(nodes[3], 1); // depth 3
}

TEST_F(TraversalTest, BothDirectionTraversal) {
    auto nodes = run_bfs(4, TraverseDirection::BOTH);
    // From 4 in BOTH: should reach 5 (out), 2 and 3 (in), then 1 (in from 2,3)
    ASSERT_EQ(nodes.size(), 4);
}

TEST_F(TraversalTest, LeafNodeTraversal) {
    auto nodes = run_bfs(5, TraverseDirection::OUT);
    // Node 5 has no outgoing edges
    EXPECT_TRUE(nodes.empty());
}

TEST_F(TraversalTest, CycleHandling) {
    // Add a cycle: 5 → 1
    link(5, 1);

    auto nodes = run_bfs(1, TraverseDirection::OUT);
    // Should still terminate and visit each node once.
    ASSERT_EQ(nodes.size(), 4);
    std::sort(nodes.begin(), nodes.end());
    EXPECT_EQ(nodes[0], 2);
    EXPECT_EQ(nodes[1], 3);
    EXPECT_EQ(nodes[2], 4);
    EXPECT_EQ(nodes[3], 5);
}

TEST_F(TraversalTest, MemoryBoundedVisitedSet) {
    auto nodes = run_bfs(1, TraverseDirection::OUT, 100, 3);
    // max_visited=3: start node (1) + at most 2 more nodes
    EXPECT_LE(nodes.size(), 2u);
}

TEST_F(TraversalTest, TraversalResultsIncludeDepth) {
    TraversalConfig config;
    config.edge_type = "follows";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "node", TypeId::INT64, false, 0});
    cols.push_back({"", "depth", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    TraversalOperator op(*graph_, std::move(config), std::move(schema), nullptr, bound);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value());

    // Collect (node, depth) pairs.
    std::vector<std::pair<int64_t, int64_t>> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value());
        if (!row->has_value()) {
            break;
        }
        auto& vals = row->value().values;
        results.emplace_back(vals[0].as_int64(), vals[1].as_int64());
    }
    op.close();

    ASSERT_EQ(results.size(), 4);
    // Depth 1: nodes 2, 3
    for (size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(results[i].second, 1);
    }
    // Depth 2: node 4
    EXPECT_EQ(results[2].second, 2);
    // Depth 3: node 5
    EXPECT_EQ(results[3].second, 3);
}

TEST_F(TraversalTest, FetchModeIncludesSourcePk) {
    TraversalConfig config;
    config.edge_type = "follows";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 1;
    config.fetch = true;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "node", TypeId::INT64, false, 0});
    cols.push_back({"", "depth", TypeId::INT64, false, 0});
    cols.push_back({"", "source", TypeId::INT64, true, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    TraversalOperator op(*graph_, std::move(config), std::move(schema), nullptr, bound);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value());

    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value());
        if (!row->has_value()) {
            break;
        }
        auto& vals = row->value().values;
        ASSERT_EQ(vals.size(), 3);
        // Source PK should be 1 (the start node).
        EXPECT_EQ(vals[2].as_int64(), 1);
    }
    op.close();
}

TEST_F(TraversalTest, EdgeCollection) {
    TraversalConfig config;
    config.edge_type = "follows";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;
    config.collect_edges = true;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "node", TypeId::INT64, false, 0});
    cols.push_back({"", "depth", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    TraversalOperator op(*graph_, std::move(config), std::move(schema), nullptr, bound);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value());

    // Drain results.
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value());
        if (!row->has_value()) {
            break;
        }
    }

    // Check collected edges.
    const auto& edges = op.collected_edges();
    // BFS from 1 visits 4 nodes via 4 edges (1→2, 1→3, one of 2→4/3→4, 4→5).
    EXPECT_EQ(edges.size(), 4);

    op.close();
}

TEST_F(TraversalTest, NonexistentStartNode) {
    auto nodes = run_bfs(999, TraverseDirection::OUT);
    // Node 999 doesn't exist — no outgoing edges.
    EXPECT_TRUE(nodes.empty());
}

TEST_F(TraversalTest, WhereFilterExcludesNodes) {
    // Build a WHERE predicate: node > 3
    // This should exclude nodes 2 and 3 from the BFS results.
    auto col_ref = std::make_unique<ColumnRefExpr>();
    col_ref->column = "node";

    auto literal = std::make_unique<LiteralExpr>();
    literal->kind = LiteralKind::INTEGER;
    literal->value = "3";

    auto predicate = std::make_unique<BinaryExpr>();
    predicate->op = BinaryOp::GREATER;
    predicate->lhs = std::move(col_ref);
    predicate->rhs = std::move(literal);

    TraversalConfig config;
    config.edge_type = "follows";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "node", TypeId::INT64, false, 0});
    cols.push_back({"", "depth", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    TraversalOperator op(*graph_, std::move(config), std::move(schema), predicate.get(), bound);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<int64_t> nodes;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        nodes.push_back(row->value().values[0].as_int64());
    }
    op.close();

    // BFS from 1 visits 2, 3, 4, 5 — but WHERE node > 3 keeps only 4 and 5.
    ASSERT_EQ(nodes.size(), 2);
    std::sort(nodes.begin(), nodes.end());
    EXPECT_EQ(nodes[0], 4);
    EXPECT_EQ(nodes[1], 5);
}

} // namespace
} // namespace sixseven
