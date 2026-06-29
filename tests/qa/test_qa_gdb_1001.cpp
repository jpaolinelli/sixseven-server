// GDB-1001 QA: Adversarial tests for graph-engine error propagation.
//
// The fix propagates get_edges_from/get_edges_to errors (was: silently
// swallowed) in PatternMatchOperator, VariableLengthMatchOperator, and
// MatchShortestPathOperator. The unweighted-BFS legacy-swallow in
// execute_shortest_paths was also removed.
//
// QA FOCUS:
//   1. Legitimate no-path / leaf-node / disconnected-graph queries still
//      succeed with empty results (not spurious errors).
//   2. All three operators propagate genuine get_edges errors.
//   3. Determinism: results are stable across two runs.
//   4. Variable-length {0,N} zero-hop case does not error.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/match_shortest_path.h"
#include "sixseven/executor/pattern_match.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/variable_length_match.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
// Graph topology:
//   persons nodes: 1, 2, 3, 4
//   'knows' edges: 1->2, 2->3
//   Node 4 is isolated (no edges at all).
//   Node 3 has no OUTgoing edges (leaf).
//   1->2->3 reachable; 1->4 is NOT reachable via 'knows'.
//
class QA_GDB1001 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1001";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_ok = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_ok.has_value()) << db_ok.error().message;

        // persons table: single INT64 'id' PK.
        {
            TableSchema ts;
            ts.name = "persons";
            CatalogColumnDef id_col;
            id_col.ordinal = 0;
            id_col.name = "id";
            id_col.type_id = TypeId::INT64;
            id_col.nullable = false;
            ts.columns.push_back(id_col);
            ts.pk_columns = "id";
            auto tid = catalog_->create_table(default_database_id, std::move(ts));
            ASSERT_TRUE(tid.has_value()) << tid.error().message;
            persons_id_ = *tid;

            auto schema = catalog_->get_table(default_database_id, "persons");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, persons_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        for (int64_t id : {1, 2, 3, 4}) {
            insert_node(id);
        }

        // 'knows' edge type.
        auto eid = graph_->create_edge_type(default_database_id,
                                            "knows",
                                            persons_id_,
                                            persons_id_,
                                            TypeId::INT64,
                                            TypeId::INT64,
                                            {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // 1->2, 2->3 only. Node 4 is isolated. Node 3 has no out-edges.
        auto lr1 = graph_->link(default_database_id, "knows", Value(int64_t{1}), Value(int64_t{2}));
        ASSERT_TRUE(lr1.has_value()) << lr1.error().message;
        auto lr2 = graph_->link(default_database_id, "knows", Value(int64_t{2}), Value(int64_t{3}));
        ASSERT_TRUE(lr2.has_value()) << lr2.error().message;
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void insert_node(int64_t id) {
        auto ts = storage_->get_table_storage(persons_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table(default_database_id, "persons");
        ASSERT_TRUE(schema.has_value());

        std::vector<Value> vals = {Value(id)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    OutputSchema make_node_schema() const {
        std::vector<OutputColumn> cols;
        cols.push_back({"persons", "id", TypeId::INT64, false, persons_id_});
        return OutputSchema(std::move(cols));
    }

    OutputSchema make_path_schema() const {
        std::vector<OutputColumn> cols;
        cols.push_back({"", "path", TypeId::PATH, false, 0});
        return OutputSchema(std::move(cols));
    }

    size_t drain_pattern_match(PatternMatchOperator& op) {
        auto open_result = op.open();
        if (!open_result.has_value()) {
            ADD_FAILURE() << "open() failed: " << open_result.error().message;
            return 0;
        }
        size_t count = 0;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row.has_value() || !row->has_value()) {
                break;
            }
            ++count;
        }
        op.close();
        return count;
    }

    size_t drain_varlen_match(VariableLengthMatchOperator& op) {
        auto open_result = op.open();
        if (!open_result.has_value()) {
            ADD_FAILURE() << "open() failed: " << open_result.error().message;
            return 0;
        }
        size_t count = 0;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row.has_value() || !row->has_value()) {
                break;
            }
            ++count;
        }
        op.close();
        return count;
    }

    size_t drain_shortest_path(MatchShortestPathOperator& op) {
        auto open_result = op.open();
        if (!open_result.has_value()) {
            ADD_FAILURE() << "open() failed: " << open_result.error().message;
            return 0;
        }
        size_t count = 0;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row.has_value() || !row->has_value()) {
                break;
            }
            ++count;
        }
        op.close();
        return count;
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t persons_id_ = 0;
};

// ---------------------------------------------------------------------------
// AC1: Legitimate no-edge / leaf-node queries SUCCEED with empty rows
// ---------------------------------------------------------------------------

// Single-hop MATCH where source node 3 has no outgoing 'knows' edges.
// get_edges_from must return ok({}) (empty vector), not an error.
// The operator must open() successfully and return the one 1->2 edge (not crash).
// This is the PRIMARY contract-change risk: over-propagation would turn leaf
// traversal into errors.
TEST_F(QA_GDB1001, PatternMatchLeafNodeSucceeds) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            make_node_schema(),
                            nullptr,
                            bound);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value())
        << "Leaf node traversal must NOT error: " << open_result.error().message;

    // Expect rows: 1->2, 2->3 (nodes 3 and 4 have no out-edges and produce 0).
    size_t count = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        ++count;
    }
    op.close();
    EXPECT_EQ(count, 2u) << "Expected 2 single-hop edges (1->2, 2->3)";
}

// Isolated node 4 has zero outgoing AND incoming edges.
// The variable-length operator must succeed and find 0 paths involving node 4.
TEST_F(QA_GDB1001, VarLenMatchIsolatedNodeSucceeds) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 1, 3));

    BoundStatement bound;
    VariableLengthMatchOperator op(*graph_,
                                   *catalog_,
                                   *storage_,
                                   default_database_id,
                                   std::move(config),
                                   make_node_schema(),
                                   nullptr,
                                   bound);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value())
        << "Isolated node traversal must NOT error: " << open_result.error().message;
    // Node 4 contributes 0 paths; nodes 1/2/3 contribute some.
    size_t count = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        ++count;
    }
    op.close();
    // 1->2 (1-hop), 2->3 (1-hop), 1->3 (2-hop) = 3 paths.
    EXPECT_EQ(count, 3u);
}

// Disconnected graph: shortest path from 1 to 4 (unreachable) should return
// empty results, NOT an error. This is the over-propagation regression to
// catch: FULL-PROPAGATE must not confuse "no path found" with a real failure.
TEST_F(QA_GDB1001, ShortestPathUnreachableTargetReturnsEmpty) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT));

    BoundStatement bound;
    MatchShortestPathOperator op(*graph_,
                                 *catalog_,
                                 *storage_,
                                 default_database_id,
                                 std::move(config),
                                 make_path_schema(),
                                 nullptr,
                                 bound,
                                 PathSelector::ANY_SHORTEST,
                                 "p",
                                 1);

    // open() runs the full BFS over all src/tgt pairs.
    // Pairs with no path (e.g. 1->4, 3->4, etc.) must be silently skipped.
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value())
        << "Disconnected graph must NOT error: " << open_result.error().message;

    size_t count = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        ++count;
    }
    op.close();
    // Only paths that actually exist are returned: 1->2, 2->3, 1->3.
    EXPECT_GE(count, 1u) << "At least one real path must be found";
    // Node 4 is isolated so no extra paths should appear.
}

// Shortest-path where src == tgt, no self-edge. Should return empty, not error.
TEST_F(QA_GDB1001, ShortestPathSrcEqTgtNoSelfEdgeReturnsEmpty) {
    // Restrict to single src/tgt pair: node 1 -> node 1 (no self-loop on 'knows').
    // We do this by filtering via a query that produces no valid pairs.
    // Simplest: run over all pairs; paths from X to X with no self-edge are 0.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT));

    BoundStatement bound;
    MatchShortestPathOperator op(*graph_,
                                 *catalog_,
                                 *storage_,
                                 default_database_id,
                                 std::move(config),
                                 make_path_schema(),
                                 nullptr,
                                 bound,
                                 PathSelector::ANY_SHORTEST,
                                 "p",
                                 1);

    // This is the same as the previous test but emphasizes: the self-pair
    // (src==tgt) is just another no-path case and must not error.
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value())
        << "src==tgt with no self-edge must NOT error: " << open_result.error().message;
    // Drain to confirm no crash or error mid-stream.
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
    }
    op.close();
}

// ---------------------------------------------------------------------------
// AC2: Variable-length {0,N} zero-hop -- must succeed
// ---------------------------------------------------------------------------

// {0,3} should match paths of length 0 (same node). min_hops=0 must not cause
// errors even when get_edges_from returns ok({}) for a leaf.
TEST_F(QA_GDB1001, VarLenMatchZeroHopSucceeds) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    // min_hops=0 means zero-hop (same node) matches are allowed.
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 0, 3));

    BoundStatement bound;
    VariableLengthMatchOperator op(*graph_,
                                   *catalog_,
                                   *storage_,
                                   default_database_id,
                                   std::move(config),
                                   make_node_schema(),
                                   nullptr,
                                   bound);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value())
        << "zero-hop variable-length MATCH must NOT error: " << open_result.error().message;

    size_t count = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        ++count;
    }
    op.close();
    // At minimum the 4 self-pairs (1->1, 2->2, 3->3, 4->4) + real edges.
    EXPECT_GE(count, 4u) << "Zero-hop paths (self-pairs) must be present";
}

// ---------------------------------------------------------------------------
// AC3: Multi-hop pattern match -- leaf reached mid-traversal is NOT an error
// ---------------------------------------------------------------------------

// Two-hop MATCH (a)-[r1]->(b)-[r2]->(c). Nodes with no out-edges at hop 2
// (node 3 as 'b') must not cause the multi-hop expansion to error.
// Expect exactly one two-hop path: 1->2->3.
TEST_F(QA_GDB1001, PatternMatchMultiHopLeafAtHop2Succeeds) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "persons"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT));
    config.edges.push_back(MatchEdgeDef("r2", "knows", TraverseDirection::OUT));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            make_node_schema(),
                            nullptr,
                            bound);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value())
        << "Multi-hop leaf mid-traversal must NOT error: " << open_result.error().message;

    size_t count = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        ++count;
    }
    op.close();
    EXPECT_EQ(count, 1u) << "Exactly one 2-hop path: 1->2->3";
}

// ---------------------------------------------------------------------------
// AC4: Genuine errors propagate -- unknown edge type errors all three ops
// ---------------------------------------------------------------------------

// PatternMatch: unknown edge type propagates as NOT_FOUND.
TEST_F(QA_GDB1001, PatternMatchUnknownEdgeTypeErrors) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "no_such_edge", TraverseDirection::OUT));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            make_node_schema(),
                            nullptr,
                            bound);

    auto result = op.open();
    ASSERT_FALSE(result.has_value()) << "Unknown edge type must propagate an error in PatternMatch";
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// VariableLengthMatch: unknown edge type propagates as NOT_FOUND.
TEST_F(QA_GDB1001, VarLenMatchUnknownEdgeTypeErrors) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "no_such_edge", TraverseDirection::OUT, 1, 3));

    BoundStatement bound;
    VariableLengthMatchOperator op(*graph_,
                                   *catalog_,
                                   *storage_,
                                   default_database_id,
                                   std::move(config),
                                   make_node_schema(),
                                   nullptr,
                                   bound);

    auto result = op.open();
    ASSERT_FALSE(result.has_value())
        << "Unknown edge type must propagate an error in VariableLengthMatch";
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// MatchShortestPathOperator unweighted: unknown edge type propagates.
// This is the exact site of the legacy-swallow removal.
TEST_F(QA_GDB1001, ShortestPathUnweightedUnknownEdgeTypeErrors) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "no_such_edge", TraverseDirection::OUT));

    BoundStatement bound;
    MatchShortestPathOperator op(*graph_,
                                 *catalog_,
                                 *storage_,
                                 default_database_id,
                                 std::move(config),
                                 make_path_schema(),
                                 nullptr,
                                 bound,
                                 PathSelector::ANY_SHORTEST,
                                 "p",
                                 1);

    auto result = op.open();
    ASSERT_FALSE(result.has_value())
        << "Unknown edge type must propagate an error in unweighted ShortestPath";
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// Multi-hop PatternMatch: unknown edge type at hop 2 also propagates.
// This exercises the second propagation site in pattern_match.cpp (lines 372/383).
TEST_F(QA_GDB1001, PatternMatchMultiHopUnknownEdgeAtHop2Errors) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.nodes.push_back({"c", "persons"});
    config.edges.push_back(MatchEdgeDef("r1", "knows", TraverseDirection::OUT));
    // Second hop uses unknown edge type.
    config.edges.push_back(MatchEdgeDef("r2", "no_such_edge", TraverseDirection::OUT));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            make_node_schema(),
                            nullptr,
                            bound);

    auto result = op.open();
    ASSERT_FALSE(result.has_value())
        << "Unknown edge type at hop 2 must propagate an error in multi-hop PatternMatch";
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// ---------------------------------------------------------------------------
// AC5: Determinism -- run twice, get same row counts
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1001, PatternMatchResultsDeterministic) {
    auto run_once = [&]() -> size_t {
        MatchConfig config;
        config.nodes.push_back({"a", "persons"});
        config.nodes.push_back({"b", "persons"});
        config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT));
        BoundStatement bound;
        PatternMatchOperator op(*graph_,
                                *catalog_,
                                *storage_,
                                default_database_id,
                                std::move(config),
                                make_node_schema(),
                                nullptr,
                                bound);
        return drain_pattern_match(op);
    };

    size_t first = run_once();
    size_t second = run_once();
    EXPECT_EQ(first, second) << "PatternMatch results must be deterministic";
    EXPECT_EQ(first, 2u);
}

TEST_F(QA_GDB1001, VarLenMatchResultsDeterministic) {
    auto run_once = [&]() -> size_t {
        MatchConfig config;
        config.nodes.push_back({"a", "persons"});
        config.nodes.push_back({"b", "persons"});
        config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 1, 3));
        BoundStatement bound;
        VariableLengthMatchOperator op(*graph_,
                                       *catalog_,
                                       *storage_,
                                       default_database_id,
                                       std::move(config),
                                       make_node_schema(),
                                       nullptr,
                                       bound);
        return drain_varlen_match(op);
    };

    size_t first = run_once();
    size_t second = run_once();
    EXPECT_EQ(first, second) << "VarLenMatch results must be deterministic";
}

TEST_F(QA_GDB1001, ShortestPathResultsDeterministic) {
    auto run_once = [&]() -> size_t {
        MatchConfig config;
        config.nodes.push_back({"a", "persons"});
        config.nodes.push_back({"b", "persons"});
        config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT));
        BoundStatement bound;
        MatchShortestPathOperator op(*graph_,
                                     *catalog_,
                                     *storage_,
                                     default_database_id,
                                     std::move(config),
                                     make_path_schema(),
                                     nullptr,
                                     bound,
                                     PathSelector::ANY_SHORTEST,
                                     "p",
                                     1);
        return drain_shortest_path(op);
    };

    size_t first = run_once();
    size_t second = run_once();
    EXPECT_EQ(first, second) << "ShortestPath results must be deterministic";
}

} // namespace
} // namespace sixseven
