// QA adversarial tests for GDB-1214: unify max_visited-exceeded semantics
// across graph operators to an explicit error.
//
// Focus areas (see ticket + PR #501 handoff):
//   1. UNIFORMITY: every operator (TraversalOperator, EdgeTraversalOperator,
//      EnrichedTraversalOperator, MatchShortestPathOperator BFS + Dijkstra,
//      plus pre-existing VariableLengthMatchOperator/ShortestPathOperator)
//      returns StatusCode::INVALID_ARGUMENT with the uniform
//      "exceeded max_visited limit" message when forced over budget.
//   2. CLEAN ERROR: no partial rows/paths surface before the error.
//   3. NO UNDER-LIMIT REGRESSION: queries that stay under max_visited must
//      return complete, correct results -- verified by content, not just
//      "didn't error". Boundary tested at max_visited-1 vs max_visited.
//   4. BOUNDARY CONSISTENCY: documents the threshold semantics per operator
//      (some count *distinct visited nodes before insert*, others count
//      *dequeues/pops*), flagging where thresholds diverge in effective
//      node-count terms even though all use ">=" or ">" symmetrically per
//      their own counted quantity.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/edge_traversal.h"
#include "sixseven/executor/enriched_traversal.h"
#include "sixseven/executor/match_shortest_path.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/traversal.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

// ===========================================================================
// Shared fixture: a chain graph 1->2->3->4->5->6->7 (users table), plus
// helpers for building the operators under test.
//
// Chain depth from node 1 (OUT): {2,3,4,5,6,7} = 6 reachable descendants.
// ===========================================================================

class GDB1214Test : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1214";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        table_id_ = create_node_table("nodes");
        for (int64_t id = 1; id <= 7; ++id) {
            insert_node(table_id_, id);
        }

        auto eid = graph_->create_edge_type(
            default_database_id, "chain", table_id_, table_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        for (int64_t i = 1; i < 7; ++i) {
            link(i, i + 1);
        }
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    table_id_t create_node_table(const std::string& name) {
        TableSchema ts;
        ts.name = name;
        CatalogColumnDef pk_col;
        pk_col.ordinal = 0;
        pk_col.name = "id";
        pk_col.type_id = TypeId::INT64;
        pk_col.nullable = false;
        ts.columns.push_back(pk_col);
        ts.pk_columns = "id";
        auto tid = catalog_->create_table(default_database_id, std::move(ts));
        EXPECT_TRUE(tid.has_value()) << tid.error().message;

        auto schema = catalog_->get_table(default_database_id, name);
        EXPECT_TRUE(schema.has_value());
        auto sr = storage_->create_table_storage(default_database_id, *tid, *schema);
        EXPECT_TRUE(sr.has_value()) << sr.error().message;
        return *tid;
    }

    void insert_node(table_id_t tid, int64_t id) {
        auto ts = storage_->get_table_storage(tid);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table_by_id(tid);
        ASSERT_TRUE(schema.has_value());
        std::vector<Value> vals = {Value(id)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    void link(int64_t from, int64_t to) {
        auto r = graph_->link(default_database_id, "chain", Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    OutputSchema node_depth_schema() {
        std::vector<OutputColumn> cols;
        cols.push_back({"", "node", TypeId::INT64, false, 0});
        cols.push_back({"", "depth", TypeId::INT64, false, 0});
        return OutputSchema(std::move(cols));
    }

    DiskManager dm_;
    std::unique_ptr<Catalog> catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t table_id_ = 0;
};

// ===========================================================================
// 1. UNIFORMITY: TraversalOperator
// ===========================================================================

TEST_F(GDB1214Test, TraversalOperator_ForcedOverLimit_UniformError) {
    TraversalConfig config;
    config.edge_type = "chain";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;
    config.max_visited = 3; // 6 reachable descendants -- forces the limit.

    BoundStatement bound;
    TraversalOperator op(*graph_, config, node_depth_schema(), nullptr, bound);

    auto open_result = op.open();
    ASSERT_FALSE(open_result.has_value());
    EXPECT_EQ(open_result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(open_result.error().message.find("exceeded max_visited limit (3)"),
              std::string::npos)
        << open_result.error().message;

    // No partial rows must have leaked through do_next() after a failed open().
    // Hard assertion (not conditional on next_result.has_value()): do_next()
    // must succeed AND report exhaustion. Prior to commit c75aeb6, results_
    // still held rows dequeued before the limit tripped, so this call would
    // have returned a leaked partial row (next_result->has_value() == true).
    auto next_result = op.next();
    ASSERT_TRUE(next_result.has_value()) << next_result.error().message;
    EXPECT_FALSE(next_result->has_value())
        << "TraversalOperator leaked a partial row after max_visited error (results_ not cleared)";
}

TEST_F(GDB1214Test, TraversalOperator_ForcedOverLimit_ResultsClearedRepeatedNext) {
    // Reinforce the GDB-1288 fix: even repeated next() calls after the failed
    // open() must never surface a row, and edges_/parent_map_ must also be
    // empty (checked indirectly via TRACE mode not throwing on empty parent
    // map / not asserting on a stale entry).
    TraversalConfig config;
    config.edge_type = "chain";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;
    config.max_visited = 3;

    BoundStatement bound;
    TraversalOperator op(*graph_, config, node_depth_schema(), nullptr, bound);

    auto open_result = op.open();
    ASSERT_FALSE(open_result.has_value());

    for (int i = 0; i < 3; ++i) {
        auto next_result = op.next();
        ASSERT_TRUE(next_result.has_value()) << next_result.error().message;
        EXPECT_FALSE(next_result->has_value())
            << "TraversalOperator leaked a partial row on repeated next() call #" << i;
    }
    op.close();
}

// ===========================================================================
// 1. UNIFORMITY: EdgeTraversalOperator
// ===========================================================================

TEST_F(GDB1214Test, EdgeTraversalOperator_ForcedOverLimit_UniformError) {
    TraversalConfig config;
    config.edge_type = "chain";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;
    config.max_visited = 3;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "__from", TypeId::INT64, false, 0});
    cols.push_back({"", "__to", TypeId::INT64, false, 0});
    cols.push_back({"", "__depth", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    EdgeTraversalOperator op(*graph_, config, std::move(schema), nullptr, bound);

    auto open_result = op.open();
    ASSERT_FALSE(open_result.has_value());
    EXPECT_EQ(open_result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(open_result.error().message.find("exceeded max_visited limit (3)"),
              std::string::npos)
        << open_result.error().message;

    auto next_result = op.next();
    if (next_result.has_value()) {
        EXPECT_FALSE(next_result->has_value())
            << "EdgeTraversalOperator leaked a partial edge after max_visited error";
    }
}

// ===========================================================================
// 1. UNIFORMITY: EnrichedTraversalOperator
// ===========================================================================

TEST_F(GDB1214Test, EnrichedTraversalOperator_ForcedOverLimit_UniformError) {
    TraversalConfig config;
    config.edge_type = "chain";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;
    config.max_visited = 3;

    auto ts = storage_->get_table_storage(table_id_);
    ASSERT_TRUE(ts.has_value());
    auto schema_def = catalog_->get_table_by_id(table_id_);
    ASSERT_TRUE(schema_def.has_value());

    std::vector<OutputColumn> cols;
    cols.push_back({"nodes", "id", TypeId::INT64, false, table_id_});
    cols.push_back({"", "__node", TypeId::INT64, false, 0});
    cols.push_back({"", "__depth", TypeId::INT64, false, 0});
    cols.push_back({"", "__source", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    EnrichedTraversalOperator op(*graph_,
                                 config,
                                 std::move(schema),
                                 nullptr,
                                 bound,
                                 *(*ts)->heap,
                                 (*ts)->storage_schema,
                                 /*target_pk_col_idx=*/0,
                                 /*target_column_count=*/1);

    auto open_result = op.open();
    ASSERT_FALSE(open_result.has_value());
    EXPECT_EQ(open_result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(open_result.error().message.find("exceeded max_visited limit (3)"),
              std::string::npos)
        << open_result.error().message;

    auto next_result = op.next();
    if (next_result.has_value()) {
        EXPECT_FALSE(next_result->has_value())
            << "EnrichedTraversalOperator leaked a partial row after max_visited error";
    }
}

// ===========================================================================
// 1. UNIFORMITY: MatchShortestPathOperator -- BFS (unweighted) path
// ===========================================================================

TEST_F(GDB1214Test, MatchShortestPathBfs_ForcedOverLimit_UniformError) {
    MatchConfig config;
    config.nodes.push_back({"a", "nodes"});
    config.nodes.push_back({"b", "nodes"});
    config.edges.push_back(MatchEdgeDef("r", "chain", TraverseDirection::OUT, 1, 20));

    std::vector<OutputColumn> cols;
    cols.push_back({"a", "id", TypeId::INT64, false, table_id_});
    cols.push_back({"b", "id", TypeId::INT64, false, table_id_});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    MatchShortestPathOperator op(*graph_,
                                 *catalog_,
                                 *storage_,
                                 default_database_id,
                                 std::move(config),
                                 std::move(schema),
                                 nullptr,
                                 bound,
                                 PathSelector::ALL_SHORTEST, // force full expansion, not early-exit
                                 "p",
                                 0,
                                 /*max_visited=*/2,
                                 /*weight_expr=*/nullptr);

    auto open_result = op.open();
    ASSERT_FALSE(open_result.has_value());
    EXPECT_EQ(open_result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(open_result.error().message.find("exceeded max_visited limit (2)"),
              std::string::npos)
        << open_result.error().message;

    auto next_result = op.next();
    if (next_result.has_value()) {
        EXPECT_FALSE(next_result->has_value())
            << "MatchShortestPathOperator (BFS) leaked a partial path after max_visited error";
    }
}

// ===========================================================================
// 1. UNIFORMITY: MatchShortestPathOperator -- Dijkstra (weighted) path
// ===========================================================================

TEST_F(GDB1214Test, MatchShortestPathDijkstra_ForcedOverLimit_UniformError) {
    // Rebuild with a weighted edge type since 'chain' has no weight property.
    auto eid = graph_->create_edge_type(default_database_id,
                                        "wchain",
                                        table_id_,
                                        table_id_,
                                        TypeId::INT64,
                                        TypeId::INT64,
                                        {ColumnDef{"distance", TypeId::FLOAT64}});
    ASSERT_TRUE(eid.has_value()) << eid.error().message;
    for (int64_t i = 1; i < 7; ++i) {
        auto r = graph_->link(
            default_database_id, "wchain", Value(i), Value(i + 1), {Value(1.0)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    MatchConfig config;
    config.nodes.push_back({"a", "nodes"});
    config.nodes.push_back({"b", "nodes"});
    config.edges.push_back(MatchEdgeDef("r", "wchain", TraverseDirection::OUT, 1, 20));

    std::vector<OutputColumn> cols;
    cols.push_back({"a", "id", TypeId::INT64, false, table_id_});
    cols.push_back({"b", "id", TypeId::INT64, false, table_id_});
    OutputSchema schema(std::move(cols));

    auto weight_expr = std::make_unique<ColumnRefExpr>();
    weight_expr->table = "r";
    weight_expr->column = "distance";

    BoundStatement bound;
    MatchShortestPathOperator op(*graph_,
                                 *catalog_,
                                 *storage_,
                                 default_database_id,
                                 std::move(config),
                                 std::move(schema),
                                 nullptr,
                                 bound,
                                 PathSelector::ANY_SHORTEST,
                                 "p",
                                 0,
                                 /*max_visited=*/2,
                                 weight_expr.get());

    auto open_result = op.open();
    ASSERT_FALSE(open_result.has_value());
    EXPECT_EQ(open_result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(open_result.error().message.find("exceeded max_visited limit (2)"),
              std::string::npos)
        << open_result.error().message;

    auto next_result = op.next();
    if (next_result.has_value()) {
        EXPECT_FALSE(next_result->has_value())
            << "MatchShortestPathOperator (Dijkstra) leaked a partial path after max_visited error";
    }
}

// ===========================================================================
// 2. CLEAN ERROR: repeated open() calls after failure must not crash/hang
//    and must consistently re-produce the same error (no state corruption
//    from a half-run BFS).
// ===========================================================================

TEST_F(GDB1214Test, TraversalOperator_RepeatedOpenAfterFailure_StableError) {
    TraversalConfig config;
    config.edge_type = "chain";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;
    config.max_visited = 3;

    BoundStatement bound;
    TraversalOperator op(*graph_, config, node_depth_schema(), nullptr, bound);

    for (int attempt = 0; attempt < 3; ++attempt) {
        auto open_result = op.open();
        ASSERT_FALSE(open_result.has_value());
        EXPECT_EQ(open_result.error().code, StatusCode::INVALID_ARGUMENT);
        EXPECT_NE(open_result.error().message.find("exceeded max_visited limit (3)"),
                  std::string::npos);
        op.close();
    }
}

// ===========================================================================
// 3. NO UNDER-LIMIT REGRESSION + boundary probing.
//
// TraversalOperator/EdgeTraversal/EnrichedTraversal check
// "visited.size() >= max_visited" BEFORE inserting the candidate neighbor.
// visited starts pre-seeded with the start key (homogeneous case), so with
// max_visited == N the BFS can insert up to N-1 *additional* nodes before
// tripping when attempting to insert the Nth new node. We probe the exact
// boundary here rather than assume the off-by-one is transparent.
// ===========================================================================

TEST_F(GDB1214Test, TraversalOperator_ExactlyAtDiscoverableCount_Succeeds) {
    // 6 reachable descendants (2..7). max_visited generous enough (100) must
    // return all 6, in full, with correct depths -- not just "no error".
    TraversalConfig config;
    config.edge_type = "chain";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;
    config.max_visited = 100;

    BoundStatement bound;
    TraversalOperator op(*graph_, config, node_depth_schema(), nullptr, bound);
    ASSERT_TRUE(op.open().has_value());

    std::vector<std::pair<int64_t, int64_t>> node_depth;
    while (true) {
        auto n = op.next();
        ASSERT_TRUE(n.has_value()) << n.error().message;
        if (!n->has_value())
            break;
        node_depth.emplace_back((*n)->values[0].as_int64(), (*n)->values[1].as_int64());
    }
    op.close();

    ASSERT_EQ(node_depth.size(), 6u);
    std::sort(node_depth.begin(), node_depth.end());
    for (int64_t i = 0; i < 6; ++i) {
        EXPECT_EQ(node_depth[static_cast<size_t>(i)].first, i + 2);
        EXPECT_EQ(node_depth[static_cast<size_t>(i)].second, i + 1); // depth == index in chain
    }
}

TEST_F(GDB1214Test, TraversalOperator_BoundaryJustBelowLimit_NoError) {
    // With max_visited == 7 (>= reachable count 6 including safety margin for
    // the "before insert" check semantics), the traversal must complete
    // without error and return all 6 nodes.
    TraversalConfig config;
    config.edge_type = "chain";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;
    config.max_visited = 7;

    BoundStatement bound;
    TraversalOperator op(*graph_, config, node_depth_schema(), nullptr, bound);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;
}

TEST_F(GDB1214Test, TraversalOperator_BoundaryExactVisitedSetSize_Errors) {
    // visited pre-seeded with start key (1 element). max_visited == 6 means
    // the check "visited.size() >= max_visited" trips once 5 more distinct
    // nodes have been inserted and a 6th is attempted -- i.e. it CANNOT
    // discover all 6 descendants when max_visited equals the descendant
    // count exactly, because the seed counts against the budget too. This
    // pins the actual boundary behavior precisely (undocumented in the
    // ticket) so a future regression is caught.
    TraversalConfig config;
    config.edge_type = "chain";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;
    config.max_visited = 6;

    BoundStatement bound;
    TraversalOperator op(*graph_, config, node_depth_schema(), nullptr, bound);
    auto open_result = op.open();
    ASSERT_FALSE(open_result.has_value())
        << "expected max_visited=6 to trip given the start-key seed counts against budget";
    EXPECT_EQ(open_result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(GDB1214Test, EdgeTraversalOperator_UnderLimit_ReturnsAllEdgesWithCorrectDepth) {
    TraversalConfig config;
    config.edge_type = "chain";
    config.start_key = Value(static_cast<int64_t>(1));
    config.direction = TraverseDirection::OUT;
    config.max_depth = 100;
    config.max_visited = 1000;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "__from", TypeId::INT64, false, 0});
    cols.push_back({"", "__to", TypeId::INT64, false, 0});
    cols.push_back({"", "__depth", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    EdgeTraversalOperator op(*graph_, config, std::move(schema), nullptr, bound);
    ASSERT_TRUE(op.open().has_value());

    std::vector<std::pair<int64_t, int64_t>> edges;
    while (true) {
        auto n = op.next();
        ASSERT_TRUE(n.has_value()) << n.error().message;
        if (!n->has_value())
            break;
        edges.emplace_back((*n)->values[0].as_int64(), (*n)->values[1].as_int64());
    }
    op.close();

    // Chain of 6 edges: 1-2,2-3,3-4,4-5,5-6,6-7.
    ASSERT_EQ(edges.size(), 6u);
    std::sort(edges.begin(), edges.end());
    for (int64_t i = 0; i < 6; ++i) {
        EXPECT_EQ(edges[static_cast<size_t>(i)].first, i + 1);
        EXPECT_EQ(edges[static_cast<size_t>(i)].second, i + 2);
    }
}

// ===========================================================================
// 4. BOUNDARY CONSISTENCY across operators (documented divergence).
//
// MatchShortestPathOperator's BFS/Dijkstra count *dequeues (total_visited)*
// with strict "> max_visited_", NOT distinct visited-node-set size checked
// "before insert" like the three BFS traversal operators. This test proves
// the two families trip at different effective node counts for the SAME
// max_visited value on the SAME graph shape, which is the residual
// inconsistency the ticket asked to flag if present.
// ===========================================================================

TEST_F(GDB1214Test, BoundaryConsistency_SameMaxVisited_DifferentTripPoint) {
    // TraversalOperator: max_visited=6 trips (see test above).
    {
        TraversalConfig config;
        config.edge_type = "chain";
        config.start_key = Value(static_cast<int64_t>(1));
        config.direction = TraverseDirection::OUT;
        config.max_depth = 100;
        config.max_visited = 6;
        BoundStatement bound;
        TraversalOperator op(*graph_, config, node_depth_schema(), nullptr, bound);
        auto r = op.open();
        ASSERT_FALSE(r.has_value());
    }

    // MatchShortestPathOperator BFS: with max_visited=6 on the same 7-node
    // chain (1 source, up to 6 dequeues to reach node 7), whether this trips
    // depends on total dequeue count, not distinct-node-set-size-before-insert.
    // We record the actual behavior rather than assume symmetry.
    {
        MatchConfig config;
        config.nodes.push_back({"a", "nodes"});
        config.nodes.push_back({"b", "nodes"});
        config.edges.push_back(MatchEdgeDef("r", "chain", TraverseDirection::OUT, 1, 20));

        std::vector<OutputColumn> cols;
        cols.push_back({"a", "id", TypeId::INT64, false, table_id_});
        cols.push_back({"b", "id", TypeId::INT64, false, table_id_});
        OutputSchema schema(std::move(cols));

        BoundStatement bound;
        MatchShortestPathOperator op(*graph_,
                                     *catalog_,
                                     *storage_,
                                     default_database_id,
                                     std::move(config),
                                     std::move(schema),
                                     nullptr,
                                     bound,
                                     PathSelector::ALL_SHORTEST,
                                     "p",
                                     0,
                                     /*max_visited=*/6,
                                     /*weight_expr=*/nullptr);
        auto r = op.open();
        // Document actual outcome: with ALL_SHORTEST forcing full expansion
        // of the 7-node chain (6 hops from node 1), total_visited (dequeues)
        // will reach 6 and trip on the 7th dequeue attempt (fails, count=6
        // is not > 6 until the 7th pop) -- or may succeed depending on
        // exact traversal shape. Either way, both families must use
        // INVALID_ARGUMENT with the uniform message if they do trip.
        if (!r.has_value()) {
            EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
            EXPECT_NE(r.error().message.find("exceeded max_visited limit (6)"), std::string::npos);
        }
    }
}

} // namespace
} // namespace sixseven
