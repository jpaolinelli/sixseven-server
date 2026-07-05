/// QA adversarial tests for GDB-1211: Remove dead
/// VariableLengthMatchOperator::get_neighbors.
///
/// GDB-1211 is a pure dead-code deletion: `get_neighbors` was a private,
/// non-virtual member function with zero call sites (execute_variable_length
/// calls graph_engine_.get_edges_from/get_edges_to directly instead). This
/// file exists to independently verify:
///
///   1. The deletion is compile-time provable dead code (no call sites) --
///      confirmed via source grep, not re-litigated here.
///   2. Variable-length MATCH end-to-end behavior is UNCHANGED by the
///      deletion: fixed-hop, {min,max} ranges, all three directions
///      (OUT/IN/BOTH), cycles, zero-hop, unbounded upper, and no-path cases
///      all still produce correct results through the live inline
///      expansion code in execute_variable_length().
///   3. The LIVE BOTH-direction path (the one that matters, since the dead
///      method is gone) is adversarially re-verified independent of the
///      existing GDB-827/GDB-1001 dev+QA suites, using a different graph
///      topology to avoid false confidence from shared fixtures.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/variable_length_match.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

// ============================================================================
// Fixture: a different topology from GDB-827/GDB-1001 to avoid
// coincidental correctness masking a bug.
//
//   Star graph:      hub(10) -> leaf1(11), hub(10) -> leaf2(12)
//   Diamond:         20 -> 21, 20 -> 22, 21 -> 23, 22 -> 23  (edge type "diamond")
//   Self-loop:       30 -> 30                                (edge type "selfloop")
//   Isolated node:   40 (no edges at all)
// ============================================================================
class QA_GDB1211 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1211";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        {
            TableSchema ts;
            ts.name = "nodes";
            CatalogColumnDef id_col;
            id_col.ordinal = 0;
            id_col.name = "id";
            id_col.type_id = TypeId::INT64;
            id_col.nullable = false;
            ts.columns.push_back(id_col);
            ts.pk_columns = "id";
            auto tid = catalog_->create_table(default_database_id, std::move(ts));
            ASSERT_TRUE(tid.has_value()) << tid.error().message;
            nodes_id_ = *tid;

            auto schema = catalog_->get_table(default_database_id, "nodes");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, nodes_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        for (int64_t id : {10, 11, 12, 20, 21, 22, 23, 30, 40}) {
            insert_node(id);
        }

        auto star_id = graph_->create_edge_type(
            default_database_id, "star", nodes_id_, nodes_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(star_id.has_value()) << star_id.error().message;
        link("star", 10, 11);
        link("star", 10, 12);

        auto diamond_id = graph_->create_edge_type(default_database_id,
                                                    "diamond",
                                                    nodes_id_,
                                                    nodes_id_,
                                                    TypeId::INT64,
                                                    TypeId::INT64,
                                                    {});
        ASSERT_TRUE(diamond_id.has_value()) << diamond_id.error().message;
        link("diamond", 20, 21);
        link("diamond", 20, 22);
        link("diamond", 21, 23);
        link("diamond", 22, 23);

        auto self_id = graph_->create_edge_type(default_database_id,
                                                 "selfloop",
                                                 nodes_id_,
                                                 nodes_id_,
                                                 TypeId::INT64,
                                                 TypeId::INT64,
                                                 {});
        ASSERT_TRUE(self_id.has_value()) << self_id.error().message;
        link("selfloop", 30, 30);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void link(const std::string& edge_type, int64_t from, int64_t to) {
        auto r = graph_->link(default_database_id, edge_type, Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    void insert_node(int64_t id) {
        auto ts = storage_->get_table_storage(nodes_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        std::vector<Value> vals = {Value(id)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    std::vector<Tuple> run_vl_match(MatchConfig config, OutputSchema schema,
                                     size_t max_visited = 100000) {
        BoundStatement bound;
        VariableLengthMatchOperator op(*graph_,
                                       *catalog_,
                                       *storage_,
                                       default_database_id,
                                       std::move(config),
                                       std::move(schema),
                                       nullptr,
                                       bound,
                                       max_visited);
        auto open_result = op.open();
        EXPECT_TRUE(open_result.has_value()) << open_result.error().message;

        std::vector<Tuple> results;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value())
                break;
            results.push_back(std::move(**row));
        }
        op.close();
        return results;
    }

    OutputSchema id_id_schema() {
        std::vector<OutputColumn> out_cols;
        out_cols.push_back({"a", "id", TypeId::INT64, false, nodes_id_});
        out_cols.push_back({"b", "id", TypeId::INT64, false, nodes_id_});
        return OutputSchema(std::move(out_cols));
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t nodes_id_ = 0;
};

// ============================================================================
// 1. Fixed-hop OUT direction: baseline, unaffected by deletion.
// ============================================================================
TEST_F(QA_GDB1211, FixedHopOutDirectionStar) {
    MatchConfig config;
    config.nodes.push_back({"a", "nodes"});
    config.nodes.push_back({"b", "nodes"});
    config.edges.push_back(MatchEdgeDef("r", "star", TraverseDirection::OUT, std::nullopt, std::nullopt));

    auto results = run_vl_match(std::move(config), id_id_schema());

    std::unordered_set<int64_t> targets;
    for (const auto& t : results) {
        ASSERT_GE(t.values.size(), 2u);
        EXPECT_EQ(t.values[0].as_int64(), 10);
        targets.insert(t.values[1].as_int64());
    }
    EXPECT_EQ(targets.size(), 2u);
    EXPECT_TRUE(targets.count(11) > 0);
    EXPECT_TRUE(targets.count(12) > 0);
}

// ============================================================================
// 2. Fixed-hop IN direction: baseline, unaffected by deletion.
// ============================================================================
TEST_F(QA_GDB1211, FixedHopInDirectionStar) {
    MatchConfig config;
    config.nodes.push_back({"a", "nodes"});
    config.nodes.push_back({"b", "nodes"});
    config.edges.push_back(MatchEdgeDef("r", "star", TraverseDirection::IN, std::nullopt, std::nullopt));

    auto results = run_vl_match(std::move(config), id_id_schema());

    // IN direction from leaf1/leaf2 reaches hub; from hub there is no incoming edge.
    bool found_leaf1_to_hub = false;
    bool found_leaf2_to_hub = false;
    for (const auto& t : results) {
        int64_t src = t.values[0].as_int64();
        int64_t tgt = t.values[1].as_int64();
        EXPECT_NE(src, tgt) << "IN direction must not self-loop";
        if (src == 11 && tgt == 10)
            found_leaf1_to_hub = true;
        if (src == 12 && tgt == 10)
            found_leaf2_to_hub = true;
    }
    EXPECT_TRUE(found_leaf1_to_hub);
    EXPECT_TRUE(found_leaf2_to_hub);
}

// ============================================================================
// 3. LIVE BOTH-direction, diamond topology: verify no missing incoming edges,
//    correct row bindings (no bogus self-loops), and correct multi-path fanout.
// ============================================================================
TEST_F(QA_GDB1211, BothDirectionDiamondFixedHop) {
    MatchConfig config;
    config.nodes.push_back({"a", "nodes"});
    config.nodes.push_back({"b", "nodes"});
    config.edges.push_back(
        MatchEdgeDef("r", "diamond", TraverseDirection::BOTH, std::nullopt, std::nullopt));

    auto results = run_vl_match(std::move(config), id_id_schema());

    std::unordered_set<std::string> valid_pairs = {
        "20->21", "21->20", "20->22", "22->20", "21->23", "23->21", "22->23", "23->22"};
    std::unordered_set<std::string> found;
    for (const auto& t : results) {
        int64_t src = t.values[0].as_int64();
        int64_t tgt = t.values[1].as_int64();
        EXPECT_NE(src, tgt) << "BOTH direction produced a bogus self-loop pair";
        std::string pair = std::to_string(src) + "->" + std::to_string(tgt);
        EXPECT_TRUE(valid_pairs.count(pair) > 0) << "Emitted invalid pair: " << pair;
        found.insert(pair);
    }
    for (const auto& p : valid_pairs) {
        EXPECT_TRUE(found.count(p) > 0) << "Missing expected BOTH-direction pair: " << p;
    }
    EXPECT_EQ(found.size(), valid_pairs.size());
}

// ============================================================================
// 4. LIVE BOTH-direction variable-length BFS: node 23 (a sink with two
//    incoming edges, no outgoing) must be reachable as a *source* only via
//    incoming-edge traversal. This directly exercises the code path the
//    dead get_neighbors reportedly mishandled.
// ============================================================================
TEST_F(QA_GDB1211, BothDirectionVarLenFromSinkReachesBothParents) {
    MatchConfig config;
    config.nodes.push_back({"a", "nodes"});
    config.nodes.push_back({"b", "nodes"});
    config.edges.push_back(MatchEdgeDef("r", "diamond", TraverseDirection::BOTH, 1, 2));

    auto results = run_vl_match(std::move(config), id_id_schema());

    std::unordered_set<int64_t> reachable_from_23;
    for (const auto& t : results) {
        if (t.values[0].as_int64() == 23) {
            reachable_from_23.insert(t.values[1].as_int64());
        }
    }
    // From sink 23: depth1 via incoming = {21, 22}; depth2 = {20} (via 21 or 22).
    EXPECT_TRUE(reachable_from_23.count(21) > 0) << "BOTH must traverse incoming edge 21->23";
    EXPECT_TRUE(reachable_from_23.count(22) > 0) << "BOTH must traverse incoming edge 22->23";
    EXPECT_TRUE(reachable_from_23.count(20) > 0) << "BOTH must reach depth-2 ancestor 20";
}

// ============================================================================
// 5. Cycle termination is unaffected: self-loop with BOTH + wide hop range
//    must terminate (per-path visited set), not hang or blow max_visited.
// ============================================================================
TEST_F(QA_GDB1211, SelfLoopBothDirectionTerminates) {
    MatchConfig config;
    config.nodes.push_back({"a", "nodes"});
    config.nodes.push_back({"b", "nodes"});
    config.edges.push_back(MatchEdgeDef("r", "selfloop", TraverseDirection::BOTH, 1, 50));

    // Small max_visited: if traversal fails to terminate via the visited-set
    // this will error out with INVALID_ARGUMENT instead of hanging.
    BoundStatement bound;
    VariableLengthMatchOperator op(*graph_,
                                   *catalog_,
                                   *storage_,
                                   default_database_id,
                                   std::move(config),
                                   id_id_schema(),
                                   nullptr,
                                   bound,
                                   /*max_visited=*/1000);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    size_t count = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value())
            break;
        ++count;
        ASSERT_LT(count, 1000u);
    }
    op.close();
    // Self-loop 30->30 with min_hops=1: the per-path visited-set marks 30 as
    // visited at depth 0, so the depth-1 self-edge is correctly pruned as a
    // revisit -- zero results is the correct terminating behavior (not a hang,
    // not an error). This is the key assertion: the operator terminates
    // cleanly instead of looping or exceeding max_visited.
    EXPECT_EQ(count, 0u);
}

// ============================================================================
// 6. Isolated node: no edges in any direction (OUT/IN/BOTH) => no results,
//    no crash, no error. Exercises the "no call sites left dangling" case
//    where a neighbor lookup legitimately returns empty.
// ============================================================================
TEST_F(QA_GDB1211, IsolatedNodeBothDirectionNoResults) {
    MatchConfig config;
    config.nodes.push_back({"a", "nodes"});
    config.nodes.push_back({"b", "nodes"});
    // Reuse "star" edge type; node 40 participates in no edges of this type.
    config.edges.push_back(MatchEdgeDef("r", "star", TraverseDirection::BOTH, 1, 5));

    auto results = run_vl_match(std::move(config), id_id_schema());

    for (const auto& t : results) {
        EXPECT_NE(t.values[0].as_int64(), 40) << "Isolated node must not appear as a source";
        EXPECT_NE(t.values[1].as_int64(), 40) << "Isolated node must not appear as a target";
    }
}

// ============================================================================
// 7. Zero-hop / unbounded upper bound sanity on BOTH direction: {0,} should
//    include the trivial 0-hop self match for every node plus all BOTH-
//    direction expansions up to the implicit cap.
// ============================================================================
TEST_F(QA_GDB1211, ZeroHopUnboundedUpperBothDirection) {
    MatchConfig config;
    config.nodes.push_back({"a", "nodes"});
    config.nodes.push_back({"b", "nodes"});
    config.edges.push_back(
        MatchEdgeDef("r", "diamond", TraverseDirection::BOTH, 0, std::nullopt));

    auto results = run_vl_match(std::move(config), id_id_schema());

    bool found_zero_hop_20 = false;
    for (const auto& t : results) {
        if (t.values[0].as_int64() == 20 && t.values[1].as_int64() == 20) {
            found_zero_hop_20 = true;
        }
    }
    EXPECT_TRUE(found_zero_hop_20) << "min_hops=0 must include the trivial self-match";
}

// ============================================================================
// 8. No-path case: two disjoint edge-type islands (star vs diamond) must
//    yield zero cross-island pairs when queried through a type they don't
//    share -- confirms directed/BOTH traversal doesn't silently invent edges.
// ============================================================================
TEST_F(QA_GDB1211, NoPathAcrossDisjointEdgeTypes) {
    MatchConfig config;
    config.nodes.push_back({"a", "nodes"});
    config.nodes.push_back({"b", "nodes"});
    config.edges.push_back(MatchEdgeDef("r", "star", TraverseDirection::BOTH, 1, 10));

    auto results = run_vl_match(std::move(config), id_id_schema());

    for (const auto& t : results) {
        int64_t src = t.values[0].as_int64();
        int64_t tgt = t.values[1].as_int64();
        bool src_in_star = (src == 10 || src == 11 || src == 12);
        bool tgt_in_star = (tgt == 10 || tgt == 11 || tgt == 12);
        EXPECT_TRUE(src_in_star && tgt_in_star)
            << "star-typed traversal must not cross into diamond/selfloop nodes: "
            << src << "->" << tgt;
    }
}

} // namespace
} // namespace sixseven
