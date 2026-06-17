// QA adversarial tests for GDB-862: Wasserman-Faust closeness centrality
// fix (directed reachable count, not weak-component size).
//
// Focus areas:
//   1. Multi-hop directed reach: r > 1 but r < nc.
//   2. Universal [0,1] invariant across diverse graph shapes.
//   3. Boundary / robustness: single-node, two-node, isolated sinks, r=0.
//   4. Mutation-grade: exact values that would FAIL if nc-1 were used.

#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/closeness_centrality.h"
#include "sixseven/graph/graph_engine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "test_catalog_helpers.h"

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers (mirror of WassermanFaustTest fixture in unit tests)
// ---------------------------------------------------------------------------

namespace {

static TableSchema make_table_schema_qa(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", TypeId::INT64, false, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

static Value pk_qa(int64_t v) {
    return Value(v);
}

struct WFRow {
    double closeness{0.0};
    int64_t sum_farness{0};
    int64_t reachable_count{0};
    int64_t component_size{0};
    double normalized_closeness{0.0};
};

std::unordered_map<int64_t, WFRow> to_wf_map_qa(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, WFRow> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 6u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto closeness = std::get<double>(row.values[1].data());
        auto sum_farness = std::get<int64_t>(row.values[2].data());
        auto reachable_count = std::get<int64_t>(row.values[3].data());
        auto component_size = std::get<int64_t>(row.values[4].data());
        auto normalized_closeness = std::get<double>(row.values[5].data());
        result[node_id] = {
            closeness, sum_farness, reachable_count, component_size, normalized_closeness};
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class QA_GDB862 : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        auto t = catalog_.create_table(default_database_id, make_table_schema_qa("nodes"));
        ASSERT_TRUE(t.has_value()) << t.error().message;
        table_id_ = *t;
    }

    void build_graph(const std::string& edge_type,
                     const std::vector<std::pair<int64_t, int64_t>>& edges) {
        auto et = engine_.create_edge_type(
            default_database_id, edge_type, table_id_, table_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(et.has_value()) << et.error().message;

        for (auto [src, tgt] : edges) {
            auto link = engine_.link(default_database_id, edge_type, pk_qa(src), pk_qa(tgt));
            ASSERT_TRUE(link.has_value()) << link.error().message;
        }
    }

    Result<std::vector<AlgorithmRow>> run_wf(const std::string& edge_type) {
        std::unordered_map<std::string, Value> args;
        args["variant"] = Value(std::string("wasserman_faust"));
        AlgorithmContext ctx{engine_, default_database_id, edge_type, args};
        return closeness_centrality_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ---------------------------------------------------------------------------
// 1. MULTI-HOP DIRECTED REACH: r > 1 but r < nc
//
// Graph: 1->2->3->4 (directed path), plus 5->1 and 6->2.
// All 6 nodes are weakly connected (nc=6 for all), N=6.
//
// Node 6: directed BFS reaches 2(d=1), 3(d=2), 4(d=3). r=3, nc=6.
//   sum_dist = 1+2+3 = 6
//   wf_norm  = 3/6   = 0.5
//   scaling  = 3/(6-1) = 3/5 = 0.6
//   closeness = 0.6 * 0.5 = 0.3
//   BUGGY (nc-1=5): wf_norm = 5/6, scaling = 5/5 = 1.0, closeness = 5/6 ~ 0.833 -- > 1? no,
//     but wf_norm = 5/6 > 0.5, and the INVARIANT that score <= 1.0 is NOT guaranteed by the
//     BUGGY code. Let's verify: closeness = (5/5)*(5/6) = 5/6 < 1 in this case, but that is
//     because sum_dist is large. The critical test is the exact value: buggy gives 5/6 != 0.3.
//
// Node 5: directed BFS reaches 1(d=1), 2(d=2), 3(d=3), 4(d=4). r=4, nc=6.
//   sum_dist = 1+2+3+4 = 10
//   wf_norm  = 4/10 = 0.4
//   scaling  = 4/5  = 0.8
//   closeness = 0.8 * 0.4 = 0.32
//   BUGGY (nc-1=5): wf_norm = 5/10 = 0.5, scaling = 5/5 = 1.0, closeness = 0.5 != 0.32.
//
// Node 1: directed BFS reaches 2(d=1), 3(d=2), 4(d=3). r=3, nc=6.
//   sum_dist = 6, wf_norm = 3/6 = 0.5, scaling = 3/5, closeness = 0.3
//   BUGGY: wf_norm = 5/6, closeness = 5/6 != 0.3.
//
// Node 2: reaches 3(d=1), 4(d=2). r=2, nc=6. sum_dist=3.
//   wf_norm = 2/3, scaling = 2/5, closeness = (2/5)*(2/3) = 4/15 ~ 0.2667
//   BUGGY: wf_norm = 5/3 (> 1!), closeness = (5/5)*(5/3) = 5/3 > 1 -- IMPOSSIBLE.
//
// Node 3: reaches 4(d=1). r=1, nc=6. sum_dist=1.
//   wf_norm = 1/1 = 1.0, scaling = 1/5, closeness = 0.2
//   BUGGY: wf_norm = 5/1 = 5.0 (>>1!), closeness = (5/5)*5 = 5.0 -- IMPOSSIBLE.
//
// Node 4: pure sink. r=0, closeness=0.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB862, MultiHopDirectedReach_r_gt_1_r_lt_nc_ExactValues) {
    // 1->2->3->4 directed chain, plus 5->1 and 6->2
    build_graph("mhdr", {{1, 2}, {2, 3}, {3, 4}, {5, 1}, {6, 2}});

    auto result = run_wf("mhdr");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map_qa(*result);
    ASSERT_EQ(scores.size(), 6u);

    const double N = 6.0;

    // Universal invariant: all scores in [0, 1].
    for (const auto& [node, r] : scores) {
        EXPECT_GE(r.closeness, 0.0) << "node " << node << " closeness below 0";
        EXPECT_LE(r.closeness, 1.0 + 1e-10)
            << "node " << node << " closeness exceeds 1.0 -- bug GDB-862 not fully fixed";
        EXPECT_FALSE(std::isnan(r.closeness)) << "node " << node << " closeness is NaN";
        EXPECT_FALSE(std::isinf(r.closeness)) << "node " << node << " closeness is Inf";
        EXPECT_GE(r.normalized_closeness, 0.0) << "node " << node;
        EXPECT_LE(r.normalized_closeness, 1.0 + 1e-10) << "node " << node;
    }

    // Node 1: r=3, sum_dist=6, N=6.
    // wf_norm=3/6=0.5, scaling=3/5=0.6, closeness=0.3
    EXPECT_EQ(scores[1].reachable_count, 4); // self + 2,3,4
    EXPECT_EQ(scores[1].sum_farness, 6);
    EXPECT_NEAR(scores[1].normalized_closeness, 3.0 / 6.0, 1e-10);
    EXPECT_NEAR(scores[1].closeness, (3.0 / (N - 1)) * (3.0 / 6.0), 1e-10);
    // Mutation check: buggy nc-1=5 gives closeness=5/6; correct is 3/10=0.3.
    EXPECT_NEAR(scores[1].closeness, 0.3, 1e-10)
        << "node 1 exact value should be 0.3; buggy nc-1 gives ~0.833";

    // Node 2: r=2, sum_dist=3, N=6.
    // wf_norm=2/3, scaling=2/5, closeness=4/15
    EXPECT_EQ(scores[2].reachable_count, 3); // self + 3,4
    EXPECT_EQ(scores[2].sum_farness, 3);
    EXPECT_NEAR(scores[2].normalized_closeness, 2.0 / 3.0, 1e-10);
    EXPECT_NEAR(scores[2].closeness, (2.0 / (N - 1)) * (2.0 / 3.0), 1e-10);
    // Mutation check: buggy nc-1=5 gives wf_norm=5/3 > 1 and closeness = 5/3 > 1.
    EXPECT_NEAR(scores[2].closeness, 4.0 / 15.0, 1e-10)
        << "node 2 exact value should be 4/15; buggy nc-1 gives > 1.0";

    // Node 3: r=1, sum_dist=1, N=6.
    // wf_norm=1.0, scaling=1/5, closeness=0.2
    EXPECT_EQ(scores[3].reachable_count, 2); // self + 4
    EXPECT_EQ(scores[3].sum_farness, 1);
    EXPECT_NEAR(scores[3].normalized_closeness, 1.0, 1e-10);
    EXPECT_NEAR(scores[3].closeness, 1.0 / (N - 1), 1e-10);
    // Mutation check: buggy nc-1=5 gives wf_norm=5 and closeness=5 -- obvious.
    EXPECT_NEAR(scores[3].closeness, 0.2, 1e-10)
        << "node 3 exact value should be 0.2; buggy nc-1 gives 5.0";

    // Node 4: pure sink.
    EXPECT_EQ(scores[4].reachable_count, 1);
    EXPECT_EQ(scores[4].sum_farness, 0);
    EXPECT_DOUBLE_EQ(scores[4].closeness, 0.0);

    // Node 5: r=4, sum_dist=10, N=6.
    // wf_norm=4/10=0.4, scaling=4/5=0.8, closeness=0.32
    EXPECT_EQ(scores[5].reachable_count, 5); // self + 1,2,3,4
    EXPECT_EQ(scores[5].sum_farness, 10);
    EXPECT_NEAR(scores[5].normalized_closeness, 4.0 / 10.0, 1e-10);
    EXPECT_NEAR(scores[5].closeness, (4.0 / (N - 1)) * (4.0 / 10.0), 1e-10);
    EXPECT_NEAR(scores[5].closeness, 0.32, 1e-10)
        << "node 5 exact value should be 0.32; buggy nc-1 gives 0.5";

    // Node 6: r=3, sum_dist=6, N=6.  Same math as node 1.
    EXPECT_EQ(scores[6].reachable_count, 4); // self + 2,3,4
    EXPECT_EQ(scores[6].sum_farness, 6);
    EXPECT_NEAR(scores[6].normalized_closeness, 3.0 / 6.0, 1e-10);
    EXPECT_NEAR(scores[6].closeness, 0.3, 1e-10)
        << "node 6 exact value should be 0.3; buggy nc-1 gives ~0.833";
}

// ---------------------------------------------------------------------------
// 2. UNIVERSAL [0,1] INVARIANT FUZZ-STYLE COVERAGE
//
// Build several qualitatively different directed graph shapes and assert
// that EVERY node's WF closeness is in [0, 1] with no NaN/Inf.
// These shapes are NOT in the unit tests.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB862, UniversalInvariant_DAG_Diamond) {
    // Diamond DAG: 1->2, 1->3, 2->4, 3->4, plus isolated 5->6.
    // N=6. Nodes in the diamond: nc=4 (weakly). 5&6 form a separate component nc=2.
    build_graph("dag_diamond", {{1, 2}, {1, 3}, {2, 4}, {3, 4}, {5, 6}});

    auto result = run_wf("dag_diamond");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map_qa(*result);
    ASSERT_EQ(scores.size(), 6u);

    for (const auto& [node, r] : scores) {
        EXPECT_GE(r.closeness, 0.0) << "node " << node;
        EXPECT_LE(r.closeness, 1.0 + 1e-10) << "node " << node << " > 1.0";
        EXPECT_FALSE(std::isnan(r.closeness)) << "node " << node;
        EXPECT_FALSE(std::isinf(r.closeness)) << "node " << node;
    }

    // Node 1: reaches 2(d=1), 3(d=1), 4(d=2). r=3, sum_dist=4, N=6.
    // wf_norm=3/4=0.75, scaling=3/5=0.6, closeness=0.45.
    EXPECT_EQ(scores[1].reachable_count, 4);
    EXPECT_EQ(scores[1].sum_farness, 4);
    EXPECT_NEAR(scores[1].closeness, 0.45, 1e-10);

    // Node 4: pure sink in the diamond. r=0.
    EXPECT_DOUBLE_EQ(scores[4].closeness, 0.0);
}

TEST_F(QA_GDB862, UniversalInvariant_DirectedCycle) {
    // Directed 4-cycle: 1->2->3->4->1. All reach all others. N=4.
    // Each node: r=3, sum_dist=1+2+3=6. wf_norm=3/6=0.5, scaling=3/3=1.0, closeness=0.5.
    build_graph("dcycle", {{1, 2}, {2, 3}, {3, 4}, {4, 1}});

    auto result = run_wf("dcycle");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map_qa(*result);
    ASSERT_EQ(scores.size(), 4u);

    for (const auto& [node, r] : scores) {
        EXPECT_GE(r.closeness, 0.0) << "node " << node;
        EXPECT_LE(r.closeness, 1.0 + 1e-10) << "node " << node << " > 1.0";
        EXPECT_FALSE(std::isnan(r.closeness)) << "node " << node;
        EXPECT_FALSE(std::isinf(r.closeness)) << "node " << node;
        // All nodes symmetric in a directed cycle.
        EXPECT_NEAR(r.closeness, 0.5, 1e-10) << "node " << node << " in directed 4-cycle";
    }
}

TEST_F(QA_GDB862, UniversalInvariant_StarOutward) {
    // Star-out: hub 1 points to 2,3,4,5 (all leaves). No leaf-to-leaf edges.
    // nc=5 (weakly connected), N=5.
    // Hub: r=4, sum_dist=4, wf_norm=4/4=1.0, scaling=4/4=1.0, closeness=1.0.
    // Leaves: r=0, closeness=0.
    build_graph("star_out", {{1, 2}, {1, 3}, {1, 4}, {1, 5}});

    auto result = run_wf("star_out");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map_qa(*result);
    ASSERT_EQ(scores.size(), 5u);

    for (const auto& [node, r] : scores) {
        EXPECT_GE(r.closeness, 0.0) << "node " << node;
        EXPECT_LE(r.closeness, 1.0 + 1e-10) << "node " << node << " > 1.0";
    }

    // Hub: r=4, sum_dist=4, N=5.
    // wf_norm=1.0, scaling=4/4=1.0, closeness=1.0.
    EXPECT_NEAR(scores[1].closeness, 1.0, 1e-10);

    // Leaves: no outgoing, closeness=0.
    for (int64_t leaf : {2, 3, 4, 5}) {
        EXPECT_DOUBLE_EQ(scores[leaf].closeness, 0.0) << "leaf " << leaf;
    }
}

TEST_F(QA_GDB862, UniversalInvariant_TwoWeakComponentsOneDirectedBridge) {
    // Component A: 1<->2<->3 (bidirectional). Component B: 10<->11.
    // Single directed bridge A->B: 3->10.
    // After adding 3->10: weakly {1,2,3,10,11}. N=5.
    //
    // Node 3: directed BFS reaches 2(d=1 via 3->2), 1(d=2 via 3->2->1),
    //          10(d=1 via 3->10), 11(d=2 via 3->10->11). Wait — check adj.
    //   Edges: 1->2, 2->1, 2->3, 3->2, 3->10, 10->11, 11->10.
    //   From 3: neighbors = {2, 10}.
    //     From 2: neighbors = {1, 3}. 1 unvisited(d=2). 3 already visited.
    //     From 10: neighbors = {11}. 11 unvisited(d=2).
    //   Reachable from 3: {2(d=1), 10(d=1), 1(d=2), 11(d=2)}. r=4, sum_dist=1+1+2+2=6.
    //   nc=5 (all weakly connected now).
    //   wf_norm = 4/6 = 2/3, scaling = 4/(5-1) = 1.0, closeness = 2/3.
    //   BUGGY: nc-1=4, same as r=4 here -- no diff for node 3.
    //
    // Node 1: directed BFS. Edges from 1: {2}. From 2: {1(visited), 3}.
    //   Reachable: 2(d=1), 3(d=2), 10(d=3), 11(d=4). r=4, sum_dist=1+2+3+4=10.
    //   wf_norm=4/10=0.4, scaling=4/4=1.0, closeness=0.4.
    //   BUGGY: nc-1=4 same, no diff -- but we still verify [0,1].
    build_graph("bridge_directed", {{1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 10}, {10, 11}, {11, 10}});

    auto result = run_wf("bridge_directed");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map_qa(*result);
    ASSERT_EQ(scores.size(), 5u);

    for (const auto& [node, r] : scores) {
        EXPECT_GE(r.closeness, 0.0) << "node " << node;
        EXPECT_LE(r.closeness, 1.0 + 1e-10) << "node " << node << " > 1.0";
        EXPECT_FALSE(std::isnan(r.closeness)) << "node " << node;
        EXPECT_FALSE(std::isinf(r.closeness)) << "node " << node;
    }

    // Node 10: directed BFS. Edges from 10: {11}. From 11: {10(visited)}.
    // Reachable from 10: {11(d=1)}. r=1, sum_dist=1, N=5.
    // nc=5 (weakly all together).
    // wf_norm=1.0, scaling=1/4=0.25, closeness=0.25.
    // BUGGY (nc-1=4): wf_norm=4/1=4.0, closeness=4.0 -- obviously wrong.
    EXPECT_EQ(scores[10].reachable_count, 2);
    EXPECT_EQ(scores[10].sum_farness, 1);
    EXPECT_NEAR(scores[10].normalized_closeness, 1.0, 1e-10);
    EXPECT_NEAR(scores[10].closeness, 0.25, 1e-10)
        << "node 10 buggy nc-1=4 would give 4.0, correct is 0.25";
}

// ---------------------------------------------------------------------------
// 3. BOUNDARY / ROBUSTNESS
// ---------------------------------------------------------------------------

TEST_F(QA_GDB862, Boundary_TwoNodeMutualEdge) {
    // Two nodes, bidirectional: 1<->2. N=2, nc=2.
    // Each reaches the other: r=1, sum_dist=1.
    // wf_norm=1/1=1.0, scaling=1/(2-1)=1.0, closeness=1.0.
    build_graph("bidir2", {{1, 2}, {2, 1}});

    auto result = run_wf("bidir2");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map_qa(*result);
    ASSERT_EQ(scores.size(), 2u);

    EXPECT_NEAR(scores[1].closeness, 1.0, 1e-10) << "node 1 in 2-node mutual graph";
    EXPECT_NEAR(scores[2].closeness, 1.0, 1e-10) << "node 2 in 2-node mutual graph";
    EXPECT_FALSE(std::isnan(scores[1].closeness));
    EXPECT_FALSE(std::isnan(scores[2].closeness));
}

TEST_F(QA_GDB862, Boundary_TwoNodeOneDirection) {
    // 1->2 only. N=2. Node 1: r=1, sum_dist=1, closeness=1.0.
    // Node 2: r=0, closeness=0. Verified in unit tests but confirm exact here.
    build_graph("onedir2", {{1, 2}});

    auto result = run_wf("onedir2");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map_qa(*result);
    ASSERT_EQ(scores.size(), 2u);

    EXPECT_NEAR(scores[1].closeness, 1.0, 1e-10);
    EXPECT_DOUBLE_EQ(scores[2].closeness, 0.0);
    EXPECT_DOUBLE_EQ(scores[2].normalized_closeness, 0.0);

    // No NaN or Inf anywhere (especially the sink node with r=0, sum_dist=0).
    EXPECT_FALSE(std::isnan(scores[2].closeness));
    EXPECT_FALSE(std::isinf(scores[2].closeness));
    EXPECT_FALSE(std::isnan(scores[2].normalized_closeness));
    EXPECT_FALSE(std::isinf(scores[2].normalized_closeness));
}

TEST_F(QA_GDB862, Boundary_EmptyGraph_ReturnsEmpty) {
    // Empty edge set: all_nodes empty -> return empty result immediately.
    // No div-by-zero on total_nodes (which would be 0, making total_nodes-1 = -1).
    build_graph("empty_e", {});

    auto result = run_wf("empty_e");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(QA_GDB862, Boundary_IsolatedSink_NoDivByZeroOrNaN) {
    // Chain: 1->2->3->4. Node 4 is a pure sink: r=0, sum_dist=0, sum_dist guard fires.
    // N=4. All nodes weakly connected (nc=4).
    // Node 4: r=0, guard: wf_closeness=0 (no div-by-zero on sum_dist=0).
    build_graph("chain4", {{1, 2}, {2, 3}, {3, 4}});

    auto result = run_wf("chain4");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map_qa(*result);
    ASSERT_EQ(scores.size(), 4u);

    // Sink node 4: must be exactly 0 with no NaN or Inf.
    EXPECT_DOUBLE_EQ(scores[4].closeness, 0.0);
    EXPECT_DOUBLE_EQ(scores[4].normalized_closeness, 0.0);
    EXPECT_FALSE(std::isnan(scores[4].closeness));
    EXPECT_FALSE(std::isinf(scores[4].closeness));
    EXPECT_FALSE(std::isnan(scores[4].normalized_closeness));
    EXPECT_FALSE(std::isinf(scores[4].normalized_closeness));

    // Node 3: r=1, sum_dist=1, N=4.
    // wf_norm=1.0, scaling=1/3, closeness=1/3.
    EXPECT_NEAR(scores[3].closeness, 1.0 / 3.0, 1e-10);

    // Node 2: r=2, sum_dist=1+2=3, N=4.
    // wf_norm=2/3, scaling=2/3, closeness=4/9.
    EXPECT_NEAR(scores[2].closeness, 4.0 / 9.0, 1e-10);

    // Node 1: r=3, sum_dist=1+2+3=6, N=4.
    // wf_norm=3/6=0.5, scaling=3/3=1.0, closeness=0.5.
    EXPECT_NEAR(scores[1].closeness, 0.5, 1e-10);

    // All in [0,1].
    for (const auto& [node, r] : scores) {
        EXPECT_LE(r.closeness, 1.0 + 1e-10) << "node " << node;
        EXPECT_GE(r.closeness, 0.0) << "node " << node;
    }
}

// ---------------------------------------------------------------------------
// 4. MUTATION-GRADE CONFIRMATION
//
// The original bug: both WF factors used nc-1 (weak-component size minus 1)
// instead of r (directed-reachable count excluding self).
//
// These tests assert EXACT values that differ from what nc-1 would produce.
// They would FAIL if the fix were reverted.
// We verify that neither value "accidentally passes" because the tolerance
// range is wide enough to accept both old and new values.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB862, MutationGrade_NodeWith_r_LessThan_nc_ExactValue) {
    // Exact reproduction of the bug report counterexample from GDB-862 ticket:
    // Edges: 1->2 and 3->2. Weak component {1,2,3}, N=3.
    // Node 1: nc=3, reachable={2}, r=1, sum_dist=1.
    //   CORRECT:  wf_norm=1/1=1.0, scaling=1/(3-1)=0.5, closeness=0.5.
    //   BUGGY:    wf_norm=(3-1)/1=2.0, scaling=(3-1)/(3-1)=1.0, closeness=2.0.
    // The values 0.5 and 2.0 are far apart; a tolerance of 1e-10 rules out confusion.
    build_graph("bug_repro", {{1, 2}, {3, 2}});

    auto result = run_wf("bug_repro");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map_qa(*result);
    ASSERT_EQ(scores.size(), 3u);

    // The exact value 0.5 is incompatible with 2.0 (the buggy result).
    EXPECT_NEAR(scores[1].closeness, 0.5, 1e-10)
        << "node 1: correct=0.5, buggy=2.0; tolerance 1e-10 rules out confusion";
    EXPECT_NEAR(scores[3].closeness, 0.5, 1e-10) << "node 3: correct=0.5, buggy=2.0";
    EXPECT_DOUBLE_EQ(scores[2].closeness, 0.0) << "node 2 (sink): must remain 0.0";

    // Regression: closeness must be <= 1.0.
    EXPECT_LE(scores[1].closeness, 1.0 + 1e-10);
    EXPECT_LE(scores[3].closeness, 1.0 + 1e-10);
}

TEST_F(QA_GDB862, MutationGrade_StarWithSharedSink_ExactValues) {
    // 1->5, 2->5, 3->5, 4->5. Weak component all 5 nodes, N=5.
    // Each source: r=1, sum_dist=1.
    //   CORRECT: wf_norm=1.0, scaling=1/(5-1)=0.25, closeness=0.25.
    //   BUGGY:   wf_norm=(5-1)/1=4.0, scaling=(5-1)/(5-1)=1.0, closeness=4.0.
    // Gap: 0.25 vs 4.0.
    build_graph("star_sink_5", {{1, 5}, {2, 5}, {3, 5}, {4, 5}});

    auto result = run_wf("star_sink_5");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map_qa(*result);
    ASSERT_EQ(scores.size(), 5u);

    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_NEAR(scores[node].closeness, 0.25, 1e-10)
            << "node " << node << ": correct=0.25, buggy=4.0";
        EXPECT_LE(scores[node].closeness, 1.0 + 1e-10) << "node " << node << " > 1.0";
    }
    EXPECT_DOUBLE_EQ(scores[5].closeness, 0.0) << "sink node 5";
}

TEST_F(QA_GDB862, MutationGrade_MultiHopPath_InternalNode_ExactValue) {
    // Directed path 1->2->3->4->5, plus extra node 6->1 (so nc={1,2,3,4,5,6}).
    // N=6. Node 2: r=3 (reaches 3,4,5), nc=6.
    //   CORRECT: wf_norm=3/6=0.5, scaling=3/5=0.6, closeness=0.3
    //     (sum_dist for node 2: 3(d=1)+4(d=2)+5(d=3) => 1+2+3=6)
    //   BUGGY:   wf_norm=(6-1)/6=5/6, scaling=(6-1)/5=1.0, closeness=5/6~0.833.
    // Gap: 0.3 vs 5/6. A 1e-10 tolerance on the exact assertion catches the bug.
    build_graph("path6_internal", {{1, 2}, {2, 3}, {3, 4}, {4, 5}, {6, 1}});

    auto result = run_wf("path6_internal");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_wf_map_qa(*result);
    ASSERT_EQ(scores.size(), 6u);

    // Node 2: r=3, sum_dist=1+2+3=6, N=6.
    // wf_norm=3/6=0.5, scaling=3/5, closeness=3/10=0.3.
    EXPECT_EQ(scores[2].reachable_count, 4);
    EXPECT_EQ(scores[2].sum_farness, 6);
    EXPECT_NEAR(scores[2].closeness, 0.3, 1e-10) << "node 2: correct=0.3, buggy nc-1 gives ~0.833";

    // Also verify it's in [0,1].
    for (const auto& [node, r] : scores) {
        EXPECT_LE(r.closeness, 1.0 + 1e-10) << "node " << node << " > 1.0";
        EXPECT_GE(r.closeness, 0.0) << "node " << node;
    }
}

// ---------------------------------------------------------------------------
// 5. SIBLING NON-REGRESSION: WF scores do not affect standard closeness.
//    The standard and WF variants share most of the code path up to the
//    variant branch. Verify that on the exact same graph, standard closeness
//    returns the mathematically correct values independently.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB862, SiblingNonRegression_StandardUnaffected) {
    // Same directed path chain used above. Standard closeness should give
    // r/(sum_dist) based on directed BFS only (no scaling by N-1).
    // 1->2->3->4, N=4. Standard (within-component-only).
    // Node 1: reachable={2,3,4}, sum_dist=1+2+3=6, std_closeness=3/6=0.5.
    // Node 2: reachable={3,4}, sum_dist=1+2=3, std_closeness=2/3.
    // Node 3: reachable={4}, sum_dist=1, std_closeness=1/1=1.0.
    // Node 4: sink, std_closeness=0.
    build_graph("chain_std", {{1, 2}, {2, 3}, {3, 4}});

    std::unordered_map<std::string, Value> args;
    args["variant"] = Value(std::string("standard"));
    AlgorithmContext ctx{engine_, default_database_id, "chain_std", args};
    auto result = closeness_centrality_execute(ctx);

    ASSERT_TRUE(result.has_value()) << result.error().message;

    std::unordered_map<int64_t, double> std_scores;
    for (const auto& row : *result) {
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto closeness = std::get<double>(row.values[1].data());
        std_scores[node_id] = closeness;
    }

    ASSERT_EQ(std_scores.size(), 4u);
    EXPECT_NEAR(std_scores[1], 0.5, 1e-10) << "standard closeness node 1";
    EXPECT_NEAR(std_scores[2], 2.0 / 3.0, 1e-10) << "standard closeness node 2";
    EXPECT_NEAR(std_scores[3], 1.0, 1e-10) << "standard closeness node 3";
    EXPECT_DOUBLE_EQ(std_scores[4], 0.0) << "standard closeness node 4";

    // Standard variant: closeness never exceeds 1.0 either.
    for (const auto& [node, c] : std_scores) {
        EXPECT_LE(c, 1.0 + 1e-10) << "standard node " << node;
        EXPECT_GE(c, 0.0) << "standard node " << node;
    }
}
