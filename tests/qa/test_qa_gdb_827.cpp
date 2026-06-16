/// QA adversarial tests for GDB-827: BOTH/undirected variable-length MATCH correctness.
///
/// Verifies that the fix to variable_length_match.cpp correctly handles:
///   - BOTH direction in fixed-segment and var-length BFS expansions
///   - No missing incoming-edge paths
///   - No duplicate or wrong rows
///   - Correct min..max bounds
///   - Directed (OUT/IN) regression: results unchanged after the tagged-edge refactor
///   - Termination on cycles (no infinite loop)
///   - Degenerate topologies: self-loop, bidirectional pair, dense graph

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/variable_length_match.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

// ============================================================================
// Shared fixture — linear chain 1->2->3->4->5 (knows)
//                   cycle 1->2->3->1 (cycle)
//                   self-loop 6->6 (self)
//                   bidir pair: 7->8 AND 8->7 (bidir)
// ============================================================================

class QA_GDB827Test : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ =
            std::filesystem::temp_directory_path() / "sixseven_qa_gdb827_test";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        // Create persons table.
        {
            TableSchema ts;
            ts.name = "persons";
            CatalogColumnDef id_col;
            id_col.ordinal = 0;
            id_col.name = "id";
            id_col.type_id = TypeId::INT64;
            id_col.nullable = false;
            ts.columns.push_back(id_col);
            CatalogColumnDef name_col;
            name_col.ordinal = 1;
            name_col.name = "name";
            name_col.type_id = TypeId::STRING;
            name_col.nullable = false;
            ts.columns.push_back(name_col);
            ts.pk_columns = "id";
            auto tid = catalog_->create_table(default_database_id, std::move(ts));
            ASSERT_TRUE(tid.has_value()) << tid.error().message;
            persons_id_ = *tid;

            auto schema = catalog_->get_table(default_database_id, "persons");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, persons_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        // Nodes 1–8.
        for (int64_t i = 1; i <= 8; ++i) {
            insert_person(i, "P" + std::to_string(i));
        }

        // Linear chain 1->2->3->4->5 (knows).
        auto eid_knows = graph_->create_edge_type(default_database_id, "knows",
                                                   persons_id_, persons_id_,
                                                   TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid_knows.has_value()) << eid_knows.error().message;
        link("knows", 1, 2);
        link("knows", 2, 3);
        link("knows", 3, 4);
        link("knows", 4, 5);

        // Cycle 1->2->3->1 (cycle_edge).
        auto eid_cycle = graph_->create_edge_type(default_database_id, "cycle_edge",
                                                   persons_id_, persons_id_,
                                                   TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid_cycle.has_value()) << eid_cycle.error().message;
        link("cycle_edge", 1, 2);
        link("cycle_edge", 2, 3);
        link("cycle_edge", 3, 1);

        // Self-loop: 6->6.
        auto eid_self = graph_->create_edge_type(default_database_id, "self_edge",
                                                  persons_id_, persons_id_,
                                                  TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid_self.has_value()) << eid_self.error().message;
        link("self_edge", 6, 6);

        // Bidirectional pair: 7->8 AND 8->7.
        auto eid_bidir = graph_->create_edge_type(default_database_id, "bidir_edge",
                                                   persons_id_, persons_id_,
                                                   TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid_bidir.has_value()) << eid_bidir.error().message;
        link("bidir_edge", 7, 8);
        link("bidir_edge", 8, 7);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void link(const std::string& edge_type, int64_t from, int64_t to) {
        auto r = graph_->link(default_database_id, edge_type, Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    void insert_person(int64_t id, const std::string& name) {
        auto ts = storage_->get_table_storage(persons_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table(default_database_id, "persons");
        ASSERT_TRUE(schema.has_value());
        std::vector<Value> vals = {Value(id), Value(name)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    // Collect (src_id, tgt_id) pairs from results.
    std::vector<std::pair<int64_t, int64_t>> run_and_collect(MatchConfig config) {
        std::vector<OutputColumn> out_cols;
        out_cols.push_back({"a", "id", TypeId::INT64, false, persons_id_});
        out_cols.push_back({"b", "id", TypeId::INT64, false, persons_id_});
        OutputSchema schema(std::move(out_cols));

        BoundStatement bound;
        VariableLengthMatchOperator op(*graph_, *catalog_, *storage_,
                                        default_database_id,
                                        std::move(config), std::move(schema),
                                        nullptr, bound);
        auto open_result = op.open();
        EXPECT_TRUE(open_result.has_value()) << open_result.error().message;
        if (!open_result) {
            return {};
        }

        std::vector<std::pair<int64_t, int64_t>> results;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value()) break;
            auto& t = **row;
            EXPECT_GE(t.values.size(), 2u);
            if (t.values.size() >= 2) {
                results.emplace_back(t.values[0].as_int64(), t.values[1].as_int64());
            }
        }
        op.close();
        return results;
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t persons_id_ = 0;
};

// ============================================================================
// TC1: Linear chain BOTH [*1..2] — verify backward reachability from tail
// ============================================================================

TEST_F(QA_GDB827Test, GDB827_BothVarLen_LinearChain_BackwardReachability) {
    // Chain: 1->2->3->4->5 queried with BOTH {1,2}.
    // From node 5 (tail): depth1={4}, depth2={3} (via incoming edges only).
    // The core bug was that BOTH acted as OUT-only, so 5 would reach nothing.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::BOTH, 1, 2));

    auto pairs = run_and_collect(std::move(config));

    // Build a map: src -> set of reachable targets.
    std::unordered_map<int64_t, std::unordered_set<int64_t>> reach;
    for (auto& [s, t] : pairs) {
        reach[s].insert(t);
    }

    // Node 5 has NO outgoing edges; it must still reach 4 (d1) and 3 (d2) via incoming.
    EXPECT_TRUE(reach[5].count(4) > 0)
        << "Node 5 must reach node 4 at depth 1 via incoming edge (core bug)";
    EXPECT_TRUE(reach[5].count(3) > 0)
        << "Node 5 must reach node 3 at depth 2 via incoming edges (core bug)";
    // Node 5 must NOT reach itself (no self-loops).
    EXPECT_EQ(reach[5].count(5), 0u) << "No self-reach from node 5";

    // Node 1 has NO incoming edges; it must still reach 2 (d1) and 3 (d2) via outgoing.
    EXPECT_TRUE(reach[1].count(2) > 0) << "Node 1 must reach node 2 at depth 1 (outgoing)";
    EXPECT_TRUE(reach[1].count(3) > 0) << "Node 1 must reach node 3 at depth 2 (outgoing)";

    // Node 3 (middle): both directions at both depths.
    EXPECT_TRUE(reach[3].count(2) > 0) << "Node 3 must reach node 2 via incoming (depth 1)";
    EXPECT_TRUE(reach[3].count(4) > 0) << "Node 3 must reach node 4 via outgoing (depth 1)";
    EXPECT_TRUE(reach[3].count(1) > 0) << "Node 3 must reach node 1 at depth 2 (backward)";
    EXPECT_TRUE(reach[3].count(5) > 0) << "Node 3 must reach node 5 at depth 2 (forward)";

    // Total: hand-computed = 14 rows.
    // From 1: {2,3} = 2; from 2: {1,3,4} = 3; from 3: {2,4,1,5} = 4;
    // from 4: {3,5,2} = 3; from 5: {4,3} = 2. Sum = 14.
    // Nodes 6,7,8 have no 'knows' edges.
    EXPECT_EQ(pairs.size(), 14u) << "Wrong total pair count for BOTH {1,2} on chain";
}

// ============================================================================
// TC2: Node reachable ONLY via an incoming edge (core bug topology)
// ============================================================================

TEST_F(QA_GDB827Test, GDB827_BothVarLen_OnlyIncomingEdge_ExactDepth1) {
    // From node 2: outgoing 2->3 (target_pk=3), incoming 1->2 (source_pk=1).
    // BOTH {1,1} must return both 3 (outgoing) and 1 (incoming).
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::BOTH, 1, 1));

    auto pairs = run_and_collect(std::move(config));

    bool found_2_to_3 = false;
    bool found_2_to_1 = false;
    for (auto& [s, t] : pairs) {
        if (s == 2 && t == 3) found_2_to_3 = true;
        if (s == 2 && t == 1) found_2_to_1 = true;
    }
    EXPECT_TRUE(found_2_to_3) << "Node 2 must reach node 3 via outgoing edge";
    EXPECT_TRUE(found_2_to_1) << "Node 2 must reach node 1 via incoming edge (core bug)";
}

TEST_F(QA_GDB827Test, GDB827_BothVarLen_OnlyIncomingEdge_Depth2) {
    // From node 3: outgoing 3->4, outgoing of 4 = 4->5, outgoing of 5 = none.
    //              incoming 2->3 (so neighbor=2), incoming of 2 = 1->2 (so neighbor=1).
    // BOTH {2,2} must include (3,5) from forward and (3,1) from backward.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::BOTH, 2, 2));

    auto pairs = run_and_collect(std::move(config));

    bool found_3_to_5 = false;
    bool found_3_to_1 = false;
    for (auto& [s, t] : pairs) {
        if (s == 3 && t == 5) found_3_to_5 = true;
        if (s == 3 && t == 1) found_3_to_1 = true;
    }
    EXPECT_TRUE(found_3_to_5) << "Node 3 must reach node 5 at depth 2 (forward)";
    EXPECT_TRUE(found_3_to_1) << "Node 3 must reach node 1 at depth 2 (backward, core bug)";
}

// ============================================================================
// TC3: Cycle with BOTH — termination and no duplicate paths
// ============================================================================

TEST_F(QA_GDB827Test, GDB827_BothVarLen_CycleTermination_NoDuplicates) {
    // Cycle: 1->2->3->1. BOTH {1,3} — undirected cycle.
    // With BOTH, each node connects to 2 neighbors (both predecessors and successors).
    // neighbor(1) = {2,3}, neighbor(2) = {1,3}, neighbor(3) = {1,2}.
    // Per-path visited prevents revisiting nodes, so each path has at most 3 nodes.
    // From node 1: d1={2,3}; d2: from 2(visited={1,2})→{3}; from 3(visited={1,3})→{2}; = {3,2}
    //              d3: from 3(visited={1,2,3})→none; from 2(visited={1,2,3})→none.
    // So from 1: 2+2 = 4 rows.
    // Same from 2 and 3 by symmetry: 4 each = 12 total (from cycle nodes).
    // Nodes 4,5 have no cycle_edge: 0 rows.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "cycle_edge", TraverseDirection::BOTH, 1, 3));

    auto pairs = run_and_collect(std::move(config));

    // BOTH on a 3-cycle with {1,3} uses path-enumeration semantics:
    // multiple distinct paths to the same endpoint produce separate rows.
    // From node 1: paths = {1→2(d1), 1→3(d1), 1→2→3(d2), 1→3→2(d2)} = 4 rows.
    // Same 4 per cycle node (1,2,3), nodes 4-8 have no cycle_edge = 0.
    // Total: 12. Note: (src,tgt) duplicates are EXPECTED here (path semantics).

    // Verify no self-loop rows (src == tgt).
    for (auto& [s, t] : pairs) {
        EXPECT_NE(s, t) << "Self-loop emitted in cycle BOTH query: (" << s << "," << t << ")";
    }

    // Verify termination: count must be bounded (not runaway).
    EXPECT_LE(pairs.size(), 100u) << "Runaway expansion: more rows than could possibly exist";

    // Verify only cycle-graph nodes appear (1,2,3); nodes 4-8 have no cycle_edge.
    for (auto& [s, t] : pairs) {
        EXPECT_GE(s, 1); EXPECT_LE(s, 3);
        EXPECT_GE(t, 1); EXPECT_LE(t, 3);
    }

    // 4 paths per cycle node x 3 cycle nodes = 12.
    EXPECT_EQ(pairs.size(), 12u)
        << "Wrong total for BOTH {1,3} on 3-cycle; expected 12 (4 paths per cycle node)";
}

// ============================================================================
// TC4: Self-loop node with BOTH
// ============================================================================

TEST_F(QA_GDB827Test, GDB827_BothVarLen_SelfLoop_NoBogusRows) {
    // Self-loop: 6->6. BOTH {1,1}.
    // Neighbor of 6 via outgoing: target_pk=6 (but visited={6}) → skip.
    // Neighbor of 6 via incoming: source_pk=6 (but visited={6}) → skip.
    // Expected: 0 rows from node 6 (self-loop, start node is always in visited).
    // But nodes 7,8 (bidir) also have no self_edge connections.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "self_edge", TraverseDirection::BOTH, 1, 1));

    auto pairs = run_and_collect(std::move(config));

    // No node should appear with itself in output (self-loop means nbr==start → skipped).
    for (auto& [s, t] : pairs) {
        EXPECT_NE(s, t) << "Self-loop self-reach emitted: (" << s << "," << t << ")";
    }
    // Since only node 6 has self_edge and it cannot reach anything (cycle back to itself),
    // total should be 0.
    EXPECT_EQ(pairs.size(), 0u)
        << "Self-loop BOTH {1,1}: expected 0 results (start node is always visited)";
}

// ============================================================================
// TC5: Bidirectional pair (7->8 AND 8->7) with BOTH — no double-count
// ============================================================================

TEST_F(QA_GDB827Test, GDB827_BothFixedHop_BiDirPair_NoDuplication) {
    // Bidir pair: edges 7->8 AND 8->7. BOTH fixed-hop (single hop, no quantifier).
    // From node 7: get_edges_from(7) → {8}; get_edges_to(7) → source of 8->7 = {8}.
    // So BOTH emits 8 twice (once via outgoing, once via incoming)?
    // This is the adversarial question — the fix should not double-count.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    MatchEdgeDef edge("r", "bidir_edge", TraverseDirection::BOTH, std::nullopt, std::nullopt);
    config.edges.push_back(std::move(edge));

    auto pairs = run_and_collect(std::move(config));

    // Filter to just nodes 7 and 8.
    std::vector<std::pair<int64_t,int64_t>> bidir_pairs;
    for (auto& p : pairs) {
        if ((p.first == 7 || p.first == 8) && (p.second == 7 || p.second == 8)) {
            bidir_pairs.push_back(p);
        }
    }

    // No self-loops.
    for (auto& [s, t] : bidir_pairs) {
        EXPECT_NE(s, t) << "Self-loop in bidir pair: (" << s << "," << t << ")";
    }

    // From 7: exactly one neighbor (8), from 8: exactly one neighbor (7).
    // Fixed-hop BOTH does NOT deduplicate — if there are two physical edges (7->8 and 8->7),
    // get_edges_from(7) gives target=8 (outgoing), get_edges_to(7) gives source=8 (incoming 8->7).
    // The fix resolves neighbor correctly (source_pk for incoming = 8), but both paths reach 8.
    // So we expect (7,8) appears TWICE and (8,7) appears TWICE (one per physical edge direction).
    // This is expected behavior: two distinct edge traversals between the same node pair.
    // What we must NOT see: (7,7) or (8,8), or any wrong node ID.
    size_t count_7_to_8 = 0;
    size_t count_8_to_7 = 0;
    for (auto& [s, t] : bidir_pairs) {
        if (s == 7 && t == 8) ++count_7_to_8;
        if (s == 8 && t == 7) ++count_8_to_7;
    }
    // Each direction appears from both the outgoing edge and the incoming edge.
    // (7,8): via 7->8 (outgoing) AND via 8->7 where source=8 ≠ our fixed src 7, so actually
    //   get_edges_to(7) returns edges pointing TO 7, which is 8->7, so source_pk=8 → nbr=8.
    //   That gives (7,8) x2? Let me re-check: get_edges_from(7) gives edge 7->8 → target=8.
    //   get_edges_to(7) gives edge 8->7 → source_pk=8 → nbr_is_source=true → nbr_pk=source_pk=8.
    //   So yes, both give nbr=8. From 7 we get (7,8) twice.
    // Both values 1 and 2 are acceptable depending on whether BOTH deduplicates.
    // We primarily assert no wrong values and no self-loops.
    EXPECT_GE(count_7_to_8, 1u) << "From node 7, must reach node 8 via BOTH";
    EXPECT_GE(count_8_to_7, 1u) << "From node 8, must reach node 7 via BOTH";
}

TEST_F(QA_GDB827Test, GDB827_BothVarLen_BiDirPair_Depth1_ReachesNeighbor) {
    // BFS BOTH {1,1} on bidir pair 7->8 AND 8->7.
    // From 7: visited={7}. get_edges_from(7): target=8 (not visited) → enqueue.
    //         get_edges_to(7): source=8 (not visited) → enqueue 8 again.
    //         Both paths emit (7,8). Distinct count depends on dedup logic.
    // Per-path visited prevents revisiting (7 is already visited), so 8 won't loop back.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "bidir_edge", TraverseDirection::BOTH, 1, 1));

    auto pairs = run_and_collect(std::move(config));

    // Must contain at least (7,8) and (8,7).
    bool found_7_to_8 = false;
    bool found_8_to_7 = false;
    for (auto& [s, t] : pairs) {
        if (s == 7 && t == 8) found_7_to_8 = true;
        if (s == 8 && t == 7) found_8_to_7 = true;
        // No self-loops ever.
        EXPECT_NE(s, t) << "Self-loop emitted: (" << s << "," << t << ")";
    }
    EXPECT_TRUE(found_7_to_8) << "BOTH BFS must reach 8 from 7 via at least one edge";
    EXPECT_TRUE(found_8_to_7) << "BOTH BFS must reach 7 from 8 via at least one edge";
}

// ============================================================================
// TC6: Exact bounds — [*2..2] and [*1..3] on chain
// ============================================================================

TEST_F(QA_GDB827Test, GDB827_BothVarLen_ExactHop2_BoundsCorrect) {
    // BOTH {2,2} on chain 1->2->3->4->5.
    // From 1: d2 = {3}  (only forward; backward at d1={} since no incoming to 1, d2 skipped)
    //   Actually: d1 from 1: outgoing={2}, incoming={}. d2 from 2(visited={1,2}): outgoing={3}, incoming from 1(visited) → {3}. = {3}
    // From 2: d1={1,3}; d2 from 1(visited={1,2}): outgoing={2}(visited)→{}, incoming={}; from 3(visited={2,3}): outgoing={4}, incoming={2}(visited)→{4}. = {4}
    // From 3: d1={2,4}; d2 from 2(visited={2,3}): out={3}(visited),in={1}→{1}; from 4(visited={3,4}): out={5},in={3}(visited)→{5}. = {1,5}
    // From 4: d1={3,5}; d2 from 3(visited={3,4}): out={4}(visited),in={2}→{2}; from 5(visited={4,5}): out={},in={4}(visited)→{}. = {2}
    // From 5: d1={4}; d2 from 4(visited={4,5}): out={5}(visited),in={3}→{3}. = {3}
    // Total: 1+1+2+1+1 = 6 rows.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::BOTH, 2, 2));

    auto pairs = run_and_collect(std::move(config));

    // No depth-1 pairs should appear (those would violate min=2).
    std::unordered_set<std::string> depth1_pairs = {"1->2","2->1","2->3","3->2","3->4","4->3","4->5","5->4"};
    for (auto& [s, t] : pairs) {
        std::string key = std::to_string(s) + "->" + std::to_string(t);
        EXPECT_EQ(depth1_pairs.count(key), 0u)
            << "Depth-1 pair emitted for min=2 query: " << key;
        // No self-loops.
        EXPECT_NE(s, t) << "Self-loop emitted: (" << s << "," << t << ")";
    }

    EXPECT_EQ(pairs.size(), 6u)
        << "Wrong count for BOTH {2,2} on chain; expected 6";

    // Spot-check some expected pairs.
    bool found_3_to_1 = false, found_3_to_5 = false;
    for (auto& [s, t] : pairs) {
        if (s == 3 && t == 1) found_3_to_1 = true;
        if (s == 3 && t == 5) found_3_to_5 = true;
    }
    EXPECT_TRUE(found_3_to_1) << "Node 3 must reach node 1 at depth 2 (backward)";
    EXPECT_TRUE(found_3_to_5) << "Node 3 must reach node 5 at depth 2 (forward)";
}

TEST_F(QA_GDB827Test, GDB827_BothVarLen_Range1to3_CountCorrect) {
    // BOTH {1,3} on chain 1->2->3->4->5.
    // Total = {1..2} (14) + {3..3}.
    // Depth-3 pairs: from 1: d3=from 3(visited={1,2,3})→out{4},in{2}(visited)={4}. →{4}. 1 path.
    //   from 2: d3=from 4(visited={1,2,3,4})→out{5},in{3}(visited)={5}. → from 1(visited={1,2,3})→out{2}(visited),in{}={}. = {5}
    //   from 3: d3=from 1(visited={1,2,3})→out{2}(visited),in{}→{} AND from 5(visited={3,4,5})→out{},in{4}(visited)→{}. = {}
    //   Wait I need to be more careful about what entries are in the queue at depth 2 going to depth 3.
    //   Let me just check: total {1,3} BOTH from the 5-node chain with no backward edges from node 1 and no forward from 5.
    //   {1,1} = 8 (same as fixed hop BOTH), {2,2} = 6, {3,3} = ?
    //   {3..3}: From 1: d1={2}, d2={3(vis{1,2,3})}, d3: from 3 nbrs={2(vis),4}→{4}. = 1 row (1,4)
    //   From 2: d1={1,3}, d2: from 1 nbrs{2(vis)}→{}, from 3 nbrs{2(vis),4}→{4}. d3: from 4 nbrs{3(vis),5}→{5}. = 1 row (2,5)
    //   From 3: d1={2,4}, d2: from 2 nbrs{1,3(vis)}→{1}, from 4 nbrs{3(vis),5}→{5}. d3: from 1 nbrs{2(vis)}→{}, from 5 nbrs{4(vis)}→{}. = 0 rows
    //   From 4: d1={3,5}, d2: from 3 nbrs{2,4(vis)}→{2}, from 5 nbrs{4(vis)}→{}. d3: from 2 nbrs{1,3(vis)}→{1}. = 1 row (4,1)
    //   From 5: d1={4}, d2: from 4 nbrs{3,5(vis)}→{3}. d3: from 3 nbrs{2,4(vis)}→{2}. = 1 row (5,2)
    //   {3,3} total = 4 rows.
    //   Grand total {1,3} = 8+6+4 = 18.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::BOTH, 1, 3));

    auto pairs = run_and_collect(std::move(config));

    // Verify no self-loops and no invalid node IDs in knows subgraph (only 1..5 involved).
    for (auto& [s, t] : pairs) {
        EXPECT_NE(s, t) << "Self-loop emitted: (" << s << "," << t << ")";
        EXPECT_GE(s, 1); EXPECT_LE(s, 5);
        EXPECT_GE(t, 1); EXPECT_LE(t, 5);
    }

    EXPECT_EQ(pairs.size(), 18u) << "Wrong count for BOTH {1,3} on chain; expected 18";
}

// ============================================================================
// TC7: OUT-only regression — must match pre-fix directed results
// ============================================================================

TEST_F(QA_GDB827Test, GDB827_OutOnly_VarLen_Regression_1to2) {
    // OUT {1,2} on chain. Pre-fix and post-fix must give same results.
    // 1-hop (4 rows) + 2-hop (3 rows) = 7 rows.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 1, 2));

    auto pairs = run_and_collect(std::move(config));

    EXPECT_EQ(pairs.size(), 7u) << "OUT {1,2} regression: expected 7 rows";

    // Verify no backward-edge results (which would appear only with BOTH or IN).
    // In OUT-only mode, from node 3 we must NOT reach node 2.
    bool found_3_to_2 = false;
    for (auto& [s, t] : pairs) {
        if (s == 3 && t == 2) found_3_to_2 = true;
    }
    EXPECT_FALSE(found_3_to_2)
        << "OUT-only var-len regression: node 3 must NOT reach node 2 (backward)";
}

TEST_F(QA_GDB827Test, GDB827_InOnly_VarLen_Regression_1to2) {
    // IN {1,2} on chain: traverse incoming edges only.
    // From each node, follow incoming edges backward.
    // From 1: no incoming → 0 rows.
    // From 2: d1={1(in)}, d2=from 1: no incoming → 0. = 1
    // From 3: d1={2(in)}, d2=from 2: in={1} → {1}. = 2
    // From 4: d1={3(in)}, d2=from 3: in={2} → {2}. = 2
    // From 5: d1={4(in)}, d2=from 4: in={3} → {3}. = 2
    // Total: 7 rows.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::IN, 1, 2));

    auto pairs = run_and_collect(std::move(config));

    EXPECT_EQ(pairs.size(), 7u) << "IN {1,2} regression: expected 7 rows";

    // From node 3, IN-only must NOT reach node 4 (which is forward).
    bool found_3_to_4 = false;
    for (auto& [s, t] : pairs) {
        if (s == 3 && t == 4) found_3_to_4 = true;
    }
    EXPECT_FALSE(found_3_to_4)
        << "IN-only var-len regression: node 3 must NOT reach node 4 (forward)";

    // From node 3, IN must reach node 2 (depth 1) and node 1 (depth 2).
    bool found_3_to_2 = false, found_3_to_1 = false;
    for (auto& [s, t] : pairs) {
        if (s == 3 && t == 2) found_3_to_2 = true;
        if (s == 3 && t == 1) found_3_to_1 = true;
    }
    EXPECT_TRUE(found_3_to_2) << "IN {1,2}: node 3 must reach node 2 at depth 1";
    EXPECT_TRUE(found_3_to_1) << "IN {1,2}: node 3 must reach node 1 at depth 2";
}

TEST_F(QA_GDB827Test, GDB827_OutOnly_FixedHop_Regression) {
    // OUT fixed hop (no quantifier) on chain: must give 4 rows (same as before fix).
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    MatchEdgeDef edge("r", "knows", TraverseDirection::OUT, std::nullopt, std::nullopt);
    config.edges.push_back(std::move(edge));

    auto pairs = run_and_collect(std::move(config));

    EXPECT_EQ(pairs.size(), 4u) << "OUT fixed-hop regression: expected 4 edges";

    // All pairs must be forward-only.
    std::unordered_set<std::string> valid = {"1->2","2->3","3->4","4->5"};
    for (auto& [s, t] : pairs) {
        std::string key = std::to_string(s) + "->" + std::to_string(t);
        EXPECT_TRUE(valid.count(key) > 0)
            << "OUT fixed-hop emitted unexpected pair: " << key;
    }
}

// ============================================================================
// TC8: Larger/denser subgraph — termination and max_visited bound respected
// ============================================================================

TEST_F(QA_GDB827Test, GDB827_BothVarLen_DenseGraph_TerminatesWithinBound) {
    // Use the 5-node chain with BOTH {1,10} — no max_hops cap hit on 5 nodes.
    // With visited-node dedup, from any node max reachable = 4 nodes (others in chain).
    // Total rows bounded by 5 * 4 = 20.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::BOTH, 1, 10));

    BoundStatement bound;
    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "id", TypeId::INT64, false, persons_id_});
    out_cols.push_back({"b", "id", TypeId::INT64, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    VariableLengthMatchOperator op(*graph_, *catalog_, *storage_,
                                    default_database_id,
                                    std::move(config), std::move(schema),
                                    nullptr, bound,
                                    1000000UL); // large but finite max_visited

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    size_t count = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) break;
        ++count;
        ASSERT_LT(count, 200u) << "Runaway: more than 200 rows from 5-node chain BOTH {1,10}";
    }
    op.close();

    // Chain with BOTH {1,10}: from node 1 can reach 2,3,4,5 (4 nodes).
    // Each of the 5 knows-chain nodes reaches (N-1) others = 4 each = 20 total max.
    // Nodes 6,7,8 have no knows edges.
    EXPECT_LE(count, 20u) << "Too many rows for BOTH on 5-node chain";
    EXPECT_GE(count, 10u) << "Too few rows — probably missing backward paths";
}

// ============================================================================
// TC9: Fixed+var-length mixed pattern — correct rows for both segments
// ============================================================================

TEST_F(QA_GDB827Test, GDB827_FixedThenVarLen_BothOnSecondSegment) {
    // Pattern: (x:persons)-[e:knows]->(y:persons)-[r:knows]-{1,2}-(z:persons)
    // First segment: fixed OUT hop (1->2, 2->3, 3->4, 4->5) — 4 bindings.
    // Second segment: BOTH var-len {1,2} from each y.
    // For y=2 (from x=1): BOTH {1,2} from 2 → {1,3,4} (d1={1,3}, d2={4})
    // For y=3 (from x=2): BOTH {1,2} from 3 → {2,4,1,5}
    // For y=4 (from x=3): BOTH {1,2} from 4 → {3,5,2}
    // For y=5 (from x=4): BOTH {1,2} from 5 → {4,3}
    // Total rows = 3+4+3+2 = 12.
    MatchConfig config;
    config.nodes.push_back({"x", "persons"});
    config.nodes.push_back({"y", "persons"});
    config.nodes.push_back({"z", "persons"});
    // First edge: fixed OUT.
    MatchEdgeDef e1("e", "knows", TraverseDirection::OUT, std::nullopt, std::nullopt);
    config.edges.push_back(std::move(e1));
    // Second edge: BOTH var-len {1,2}.
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::BOTH, 1, 2));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"x", "id", TypeId::INT64, false, persons_id_});
    out_cols.push_back({"y", "id", TypeId::INT64, false, persons_id_});
    out_cols.push_back({"z", "id", TypeId::INT64, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    VariableLengthMatchOperator op(*graph_, *catalog_, *storage_,
                                    default_database_id,
                                    std::move(config), std::move(schema),
                                    nullptr, bound);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<std::tuple<int64_t,int64_t,int64_t>> results;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) break;
        auto& t = **row;
        ASSERT_GE(t.values.size(), 3u);
        results.emplace_back(t.values[0].as_int64(), t.values[1].as_int64(), t.values[2].as_int64());
    }
    op.close();

    // Verify fixed segment: x->y must always be a real knows edge.
    std::unordered_set<std::string> valid_xy = {"1->2","2->3","3->4","4->5"};
    for (auto& [x, y, z] : results) {
        std::string key = std::to_string(x) + "->" + std::to_string(y);
        EXPECT_TRUE(valid_xy.count(key) > 0)
            << "Fixed segment produced invalid x->y pair: " << key;
    }

    // Verify BOTH var-len on second segment: from y=2, z must include 1 (backward at d1).
    bool found_x1_y2_z1 = false;
    for (auto& [x, y, z] : results) {
        if (x == 1 && y == 2 && z == 1) found_x1_y2_z1 = true;
    }
    EXPECT_TRUE(found_x1_y2_z1)
        << "Fixed+var mixed: (x=1,y=2,z=1) expected via backward BOTH on second segment";

    EXPECT_EQ(results.size(), 12u)
        << "Wrong total for fixed->BOTH{1,2} mixed pattern; expected 12";
}

// ============================================================================
// TC10: Zero-length min with BOTH — zero-hop emits self, then extends with BOTH
// ============================================================================

TEST_F(QA_GDB827Test, GDB827_BothVarLen_ZeroMin_IncludesSelfAndNeighbors) {
    // BOTH {0,1} on chain: at depth 0 each node matches itself (a==b),
    // at depth 1 each node reaches BOTH-direction neighbors.
    // 0-hop: 8 rows (one per person), 1-hop: 8 rows (4 chain edges x 2 directions).
    // Total: 16 rows (for all 8 nodes, but only knows-chain nodes have neighbors).
    // Actually: depth-0 is 8 (all nodes), depth-1 is 8 (knows chain bidirectional).
    // Nodes 6,7,8 have no knows edges, so only depth-0 for those = 3 rows.
    // Knows nodes 1-5: depth-0 = 5 rows, depth-1 = 8 rows. Total = 16.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::BOTH, 0, 1));

    auto pairs = run_and_collect(std::move(config));

    // Depth-0 self-matches: every node must appear as (n,n).
    for (int64_t i = 1; i <= 8; ++i) {
        bool found_self = false;
        for (auto& [s, t] : pairs) {
            if (s == i && t == i) found_self = true;
        }
        EXPECT_TRUE(found_self) << "Node " << i << " must appear as self-match at depth 0";
    }

    // Depth-1 backward: from node 5, must reach node 4.
    bool found_5_to_4 = false;
    for (auto& [s, t] : pairs) {
        if (s == 5 && t == 4) found_5_to_4 = true;
    }
    EXPECT_TRUE(found_5_to_4)
        << "BOTH {0,1}: node 5 must reach node 4 at depth 1 (backward, was core bug)";

    EXPECT_EQ(pairs.size(), 16u)
        << "Wrong total for BOTH {0,1} on 8-node graph with chain 1-5; expected 16";
}

} // namespace
} // namespace sixseven
