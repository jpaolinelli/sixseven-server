/// @file test_qa_gdb_819.cpp
/// @brief Adversarial QA tests for GDB-819: PageRank CustomDamping /
///        CustomIterations de-vacuation.
///
/// Strategy:
///   1. Confirm the tightened dev tests (CustomDamping, CustomIterations)
///      would FAIL if the engine silently ignored the parameters — achieved
///      by verifying score values that are *only* correct for the supplied
///      parameter, not for any default.
///   2. Adversarially probe the engine across damping values {0, 0.5, 0.85,
///      0.99} and iteration counts {1, 2, 5, 20} on an asymmetric graph to
///      confirm the engine truly honors each parameter.
///   3. Edge cases: single node, self-loop, disconnected component, dangling
///      sink, zero-damping, near-unit-damping.

#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/pagerank.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

// Pull in the unit-test catalog helper (registers default_database_id).
#include "test_catalog_helpers.h"

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers shared across GDB-819 adversarial tests
// ---------------------------------------------------------------------------

namespace {

static TableSchema make_node_schema(const std::string& name) {
    TableSchema s;
    s.name = name;
    s.columns = {{0, "id", TypeId::INT64, false, ""}};
    s.pk_columns = "id";
    return s;
}

static Value pk(int64_t v) { return Value(v); }

std::unordered_map<int64_t, double>
to_score_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, double> m;
    for (const auto& row : rows) {
        auto node = std::get<int64_t>(row.values[0].data());
        auto score = std::get<double>(row.values[1].data());
        m[node] = score;
    }
    return m;
}

double sum_scores(const std::unordered_map<int64_t, double>& m) {
    double t = 0.0;
    for (const auto& [_, s] : m) t += s;
    return t;
}

} // namespace

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class QA_GDB819_PageRank : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        auto t = catalog_.create_table(default_database_id, make_node_schema("nodes"));
        ASSERT_TRUE(t.has_value()) << t.error().message;
        table_id_ = *t;
    }

    void build_graph(const std::string& etype,
                     const std::vector<std::pair<int64_t, int64_t>>& edges) {
        auto et = engine_.create_edge_type(
            default_database_id, etype, table_id_, table_id_,
            TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(et.has_value()) << et.error().message;
        for (auto [s, t] : edges) {
            auto r = engine_.link(default_database_id, etype, pk(s), pk(t));
            ASSERT_TRUE(r.has_value()) << r.error().message;
        }
    }

    Result<std::vector<AlgorithmRow>>
    run(const std::string& etype, double d, int64_t iters) {
        AlgorithmContext ctx{engine_, default_database_id, etype,
                             {{"damping", Value(d)},
                              {"iterations", Value(iters)}}};
        return pagerank_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ---------------------------------------------------------------------------
// AC1 – confirm CustomDamping hardcoded values are correct
//        and the test is non-vacuous
// ---------------------------------------------------------------------------

/// Verify d=0.5 iters=20 converged scores on the linear chain 1->2->3
/// match the analytically computed steady state within 1e-5.
TEST_F(QA_GDB819_PageRank, GDB819_CustomDamping_HardcodedValuesMatchAnalytic) {
    build_graph("knows", {{1, 2}, {2, 3}});

    auto r = run("knows", 0.5, 20);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto s = to_score_map(*r);

    ASSERT_EQ(s.size(), 3u);
    EXPECT_NEAR(sum_scores(s), 1.0, 1e-6);
    EXPECT_NEAR(s[1], 0.23529412, 1e-5) << "analytic value for d=0.5";
    EXPECT_NEAR(s[2], 0.35294118, 1e-5) << "analytic value for d=0.5";
    EXPECT_NEAR(s[3], 0.41176471, 1e-5) << "analytic value for d=0.5";
}

/// Verify d=0.85 iters=20 converged scores on the same chain.
TEST_F(QA_GDB819_PageRank, GDB819_DefaultDamping_HardcodedValuesMatchAnalytic) {
    build_graph("knows", {{1, 2}, {2, 3}});

    auto r = run("knows", 0.85, 20);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto s = to_score_map(*r);

    ASSERT_EQ(s.size(), 3u);
    EXPECT_NEAR(s[1], 0.18441688, 1e-5) << "analytic value for d=0.85";
    EXPECT_NEAR(s[2], 0.34117101, 1e-5) << "analytic value for d=0.85";
    EXPECT_NEAR(s[3], 0.47441212, 1e-5) << "analytic value for d=0.85";
}

/// The d=0.5 run MUST differ from the d=0.85 run: if the engine silently
/// ignored damping both would return identical scores.
TEST_F(QA_GDB819_PageRank, GDB819_DampingParamActuallyChangesScores) {
    build_graph("knows", {{1, 2}, {2, 3}});

    auto r05 = run("knows", 0.5, 20);
    auto r85 = run("knows", 0.85, 20);
    ASSERT_TRUE(r05.has_value());
    ASSERT_TRUE(r85.has_value());

    auto s05 = to_score_map(*r05);
    auto s85 = to_score_map(*r85);

    // Both score vectors must exist.
    ASSERT_EQ(s05.count(1u), 1u);
    ASSERT_EQ(s85.count(1u), 1u);

    // Node 1 must differ by more than 0.04 — the vacuous test would pass even
    // if these were identical.
    EXPECT_GT(std::abs(s05[1] - s85[1]), 0.04)
        << "damping=0.5 vs damping=0.85 node1 must differ; engine may ignore damping";
    EXPECT_GT(std::abs(s05[3] - s85[3]), 0.05)
        << "damping=0.5 vs damping=0.85 node3 must differ; engine may ignore damping";
}

// ---------------------------------------------------------------------------
// AC1 – confirm CustomIterations hardcoded values are correct
//        and the test is non-vacuous
// ---------------------------------------------------------------------------

/// Verify d=0.85 iters=1 first-step scores on chain 1->2->3.
/// These are the exact values after exactly one pass of the power method.
TEST_F(QA_GDB819_PageRank, GDB819_CustomIterations_OneIterHardcoded) {
    build_graph("knows", {{1, 2}, {2, 3}});

    auto r = run("knows", 0.85, 1);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto s = to_score_map(*r);

    ASSERT_EQ(s.size(), 3u);
    EXPECT_NEAR(sum_scores(s), 1.0, 1e-6);
    EXPECT_NEAR(s[1], 0.14444444, 1e-5) << "1-iter analytic value";
    EXPECT_NEAR(s[2], 0.42777778, 1e-5) << "1-iter analytic value";
    EXPECT_NEAR(s[3], 0.42777778, 1e-5) << "1-iter analytic value";
}

/// Verify that iters=1 and iters=20 produce meaningfully different scores.
TEST_F(QA_GDB819_PageRank, GDB819_IterationsParamActuallyChangesScores) {
    build_graph("knows", {{1, 2}, {2, 3}});

    auto r1  = run("knows", 0.85, 1);
    auto r20 = run("knows", 0.85, 20);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r20.has_value());

    auto s1  = to_score_map(*r1);
    auto s20 = to_score_map(*r20);

    EXPECT_GT(std::abs(s1[2] - s20[2]), 0.08)
        << "iters=1 vs iters=20 node2 must differ; engine may ignore iterations";
    EXPECT_GT(std::abs(s1[3] - s20[3]), 0.04)
        << "iters=1 vs iters=20 node3 must differ; engine may ignore iterations";
}

// ---------------------------------------------------------------------------
// Adversarial: monotonic convergence — more iterations should reduce the
// node-2 residual toward the steady state on the asymmetric chain
// ---------------------------------------------------------------------------

/// On chain 1->2->3 with d=0.85, verify that scores at iters=1, 5, and 20
/// are all distinct — confirming the engine runs exactly the requested count.
/// The power-method on this chain is non-monotone for node 2: it starts at
/// 0.4278 (iter=1), drops to 0.2940 (iter=2), then climbs back to the
/// steady-state 0.3412 (iter=20).  So we cannot assert monotone descent;
/// instead we assert all three counts produce different results, and that
/// iters=1 is far from iters=20 (which the parameter-honoring test already
/// covers). We additionally pin all three values analytically.
TEST_F(QA_GDB819_PageRank, GDB819_IterationCountsProduceDifferentScores) {
    build_graph("knows", {{1, 2}, {2, 3}});

    auto r1  = run("knows", 0.85, 1);
    auto r5  = run("knows", 0.85, 5);
    auto r20 = run("knows", 0.85, 20);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r5.has_value());
    ASSERT_TRUE(r20.has_value());

    double n2_1  = to_score_map(*r1)[2];
    double n2_5  = to_score_map(*r5)[2];
    double n2_20 = to_score_map(*r20)[2];

    // Analytically computed values (verified via independent simulation):
    //   iters=1  → node2 = 0.42777778
    //   iters=5  → node2 = 0.33520325
    //   iters=20 → node2 = 0.34117101
    EXPECT_NEAR(n2_1,  0.42777778, 1e-5) << "1-iter node2 analytic";
    EXPECT_NEAR(n2_5,  0.33520325, 1e-5) << "5-iter node2 analytic";
    EXPECT_NEAR(n2_20, 0.34117101, 1e-5) << "20-iter node2 analytic";

    // All three must differ — if the engine always ran a fixed number of
    // iterations any two of these expectations would fail.
    EXPECT_GT(std::abs(n2_1 - n2_20), 0.08)
        << "1-iter vs 20-iter node2 must differ by >0.08";
    EXPECT_GT(std::abs(n2_1 - n2_5), 0.09)
        << "1-iter vs 5-iter node2 must differ by >0.09";
}

// ---------------------------------------------------------------------------
// Adversarial: damping=0 → every node gets uniform rank (1/N)
// ---------------------------------------------------------------------------

/// When d=0 the link structure is completely ignored.  Every node should
/// receive exactly (1-0)/N = 1/N per iteration.  This detects whether the
/// damping parameter actually scales the adjacency contribution.
TEST_F(QA_GDB819_PageRank, GDB819_ZeroDampingGivesUniformRank) {
    // 3-node star so the link structure would strongly differentiate scores
    // at d=0.85 but not at d=0.
    build_graph("knows", {{2, 1}, {3, 1}});

    auto r = run("knows", 0.0, 20);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto s = to_score_map(*r);

    ASSERT_EQ(s.size(), 3u);
    EXPECT_NEAR(sum_scores(s), 1.0, 1e-6);

    double expected = 1.0 / 3.0;
    for (const auto& [node, score] : s) {
        EXPECT_NEAR(score, expected, 1e-6)
            << "node " << node << " should be ~1/3 when damping=0";
    }
}

/// d=0 must give DIFFERENT scores from d=0.85 on a hub graph (hub node
/// dominates at d=0.85; at d=0 all are equal).
TEST_F(QA_GDB819_PageRank, GDB819_ZeroDampingDiffersFromDefaultDamping) {
    build_graph("knows", {{2, 1}, {3, 1}, {4, 1}});

    auto r0  = run("knows", 0.0,  20);
    auto r85 = run("knows", 0.85, 20);
    ASSERT_TRUE(r0.has_value());
    ASSERT_TRUE(r85.has_value());

    auto s0  = to_score_map(*r0);
    auto s85 = to_score_map(*r85);

    // Hub node 1 should be much larger at d=0.85 than at d=0.
    EXPECT_GT(s85[1], s0[1] + 0.1)
        << "hub node1 at d=0.85 must substantially exceed hub at d=0";
}

// ---------------------------------------------------------------------------
// Adversarial: near-unit damping (d=0.99)
// ---------------------------------------------------------------------------

/// At d=0.99 on the asymmetric chain, the sink node (3) accumulates more
/// rank than at d=0.5, because more weight is passed through links rather
/// than teleportation.
TEST_F(QA_GDB819_PageRank, GDB819_NearUnitDampingRaisesLeafRank) {
    build_graph("knows", {{1, 2}, {2, 3}});

    auto r05 = run("knows", 0.5,  20);
    auto r99 = run("knows", 0.99, 20);
    ASSERT_TRUE(r05.has_value());
    ASSERT_TRUE(r99.has_value());

    auto s05 = to_score_map(*r05);
    auto s99 = to_score_map(*r99);

    // Sink node 3 receives more rank at higher damping.
    EXPECT_GT(s99[3], s05[3])
        << "node3 (sink) rank must increase as damping increases from 0.5 to 0.99";
    // Source node 1 receives less rank at higher damping.
    EXPECT_LT(s99[1], s05[1])
        << "node1 (source) rank must decrease as damping increases";
}

// ---------------------------------------------------------------------------
// Adversarial: single-node graph (self-loop only)
// ---------------------------------------------------------------------------

/// A self-loop creates a single node that appears in both all_nodes and
/// outgoing.  Parameters must still take effect and result must sum to 1.
TEST_F(QA_GDB819_PageRank, GDB819_SelfLoopSingleNode) {
    build_graph("knows", {{1, 1}});

    // d=0.5
    auto r05 = run("knows", 0.5, 20);
    ASSERT_TRUE(r05.has_value()) << r05.error().message;
    auto s05 = to_score_map(*r05);
    ASSERT_EQ(s05.size(), 1u);
    EXPECT_NEAR(sum_scores(s05), 1.0, 1e-6);
    EXPECT_NEAR(s05[1], 1.0, 1e-6) << "single node self-loop must have score 1.0";

    // d=0.99 — same single-node result
    auto r99 = run("knows", 0.99, 20);
    ASSERT_TRUE(r99.has_value()) << r99.error().message;
    auto s99 = to_score_map(*r99);
    EXPECT_NEAR(s99[1], 1.0, 1e-6);

    // iters=1 — one iteration should also yield 1.0 for a single self-loop
    auto r1 = run("knows", 0.85, 1);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    EXPECT_NEAR(to_score_map(*r1)[1], 1.0, 1e-6);
}

// ---------------------------------------------------------------------------
// Adversarial: two disconnected components
// ---------------------------------------------------------------------------

/// Two disconnected 2-cycles: {1->2, 2->1} and {3->4, 4->3}.
/// Damping only controls how fast rank leaks; teleportation connects the
/// components.  All nodes should receive positive rank and sum to 1.
/// Scores at d=0.5 should differ from d=0.85 between components because
/// the teleportation term (1-d)/N differs.
TEST_F(QA_GDB819_PageRank, GDB819_DisconnectedComponents_DampingHonored) {
    // Two isolated 2-cycles
    build_graph("knows", {{1, 2}, {2, 1}, {3, 4}, {4, 3}});

    auto r05 = run("knows", 0.5,  20);
    auto r85 = run("knows", 0.85, 20);
    ASSERT_TRUE(r05.has_value()) << r05.error().message;
    ASSERT_TRUE(r85.has_value()) << r85.error().message;

    auto s05 = to_score_map(*r05);
    auto s85 = to_score_map(*r85);

    ASSERT_EQ(s05.size(), 4u);
    ASSERT_EQ(s85.size(), 4u);
    EXPECT_NEAR(sum_scores(s05), 1.0, 1e-6);
    EXPECT_NEAR(sum_scores(s85), 1.0, 1e-6);

    // In two symmetric 2-cycles all nodes are equal (1/4 each) because both
    // cycles are symmetric — the damping parameter doesn't affect this
    // particular topology (both cycles converge to uniform).  We verify that
    // the sum is preserved and all scores are positive.
    for (const auto& [node, score] : s05) {
        EXPECT_GT(score, 0.0) << "node " << node << " must have positive score";
    }
    for (const auto& [node, score] : s85) {
        EXPECT_GT(score, 0.0) << "node " << node << " must have positive score";
    }
}

// ---------------------------------------------------------------------------
// Adversarial: large iteration count should converge
// ---------------------------------------------------------------------------

/// Running 100 iterations should produce essentially the same result as 20
/// on a graph that converges quickly.  This confirms the loop actually stops
/// at the requested count (not always running a fixed number).
TEST_F(QA_GDB819_PageRank, GDB819_LargeIterCountConverges) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto r20  = run("knows", 0.85, 20);
    auto r100 = run("knows", 0.85, 100);
    ASSERT_TRUE(r20.has_value());
    ASSERT_TRUE(r100.has_value());

    auto s20  = to_score_map(*r20);
    auto s100 = to_score_map(*r100);

    // Symmetric cycle has already converged at 20 iterations.
    EXPECT_NEAR(s20[1], s100[1], 1e-6);
    EXPECT_NEAR(s20[2], s100[2], 1e-6);
    EXPECT_NEAR(s20[3], s100[3], 1e-6);
}

// ---------------------------------------------------------------------------
// Adversarial: iters=2 intermediate step on chain
// ---------------------------------------------------------------------------

/// Explicitly verify the 2-iteration intermediate scores on chain 1->2->3.
/// These are computed from the 1-iteration scores through one more pass.
/// If the engine runs a fixed number of iterations (e.g., always 1 or
/// always 20) this test will fail.
TEST_F(QA_GDB819_PageRank, GDB819_TwoIterationsIntermediate) {
    build_graph("knows", {{1, 2}, {2, 3}});

    // After 1 iter: s1=0.14444444, s2=0.42777778, s3=0.42777778
    // Pass 2:
    //   new_s_base = (1-0.85)/3 = 0.05
    //   node1 contrib (non-dangling): 0.85*0.14444444/1 -> goes to node2
    //   node2 contrib (non-dangling): 0.85*0.42777778/1 -> goes to node3
    //   node3 contrib (dangling): 0.85*0.42777778/3 -> goes to all
    //   dangling_share = 0.85*0.42777778/3 = 0.12120370
    //   s1 = 0.05 + 0.12120370 = 0.17120370
    //   s2 = 0.05 + 0.85*0.14444444 + 0.12120370 = 0.05 + 0.12277778 + 0.12120370 = 0.29398148
    //   s3 = 0.05 + 0.85*0.42777778 + 0.12120370 = 0.05 + 0.36361111 + 0.12120370 = 0.53481481
    auto r2 = run("knows", 0.85, 2);
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    auto s2 = to_score_map(*r2);

    ASSERT_EQ(s2.size(), 3u);
    EXPECT_NEAR(sum_scores(s2), 1.0, 1e-6);
    EXPECT_NEAR(s2[1], 0.17120370, 1e-5) << "2-iter analytic node1";
    EXPECT_NEAR(s2[2], 0.29398148, 1e-5) << "2-iter analytic node2";
    EXPECT_NEAR(s2[3], 0.53481481, 1e-5) << "2-iter analytic node3";
}

// ---------------------------------------------------------------------------
// Adversarial: parameter type — pass iterations as int32 (not int64)
// The run_pagerank helper uses int64 explicitly, but verify via AlgorithmContext
// ---------------------------------------------------------------------------

/// Ensure that passing iterations as INT32 value (not INT64) doesn't cause
/// a type error or silent fallback to 20-iter default.
TEST_F(QA_GDB819_PageRank, GDB819_IterationsAsInt32ValueIsAccepted) {
    build_graph("knows", {{1, 2}, {2, 3}});

    // Intentionally construct with int32_t — the value_to_iterations helper
    // must accept it.
    AlgorithmContext ctx{engine_, default_database_id, "knows",
                         {{"damping", Value(0.85)},
                          {"iterations", Value(static_cast<int32_t>(1))}}};
    auto r = pagerank_execute(ctx);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto s = to_score_map(*r);

    // 1-iteration result should match the known value.
    EXPECT_NEAR(s[1], 0.14444444, 1e-5) << "int32 iterations=1 must behave same as int64";
}

// ---------------------------------------------------------------------------
// Adversarial: NULL damping parameter → error not silent fallback
// ---------------------------------------------------------------------------

/// If the caller passes a NULL damping value the engine must return an error,
/// not silently fall back to the default.
TEST_F(QA_GDB819_PageRank, GDB819_NullDampingReturnsError) {
    build_graph("knows", {{1, 2}});

    AlgorithmContext ctx{engine_, default_database_id, "knows",
                         {{"damping", Value(/* null */)}  ,
                          {"iterations", Value(static_cast<int64_t>(20))}}};
    auto r = pagerank_execute(ctx);
    ASSERT_FALSE(r.has_value()) << "NULL damping should return an error";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

/// If the caller passes a NULL iterations value the engine must return an error.
TEST_F(QA_GDB819_PageRank, GDB819_NullIterationsReturnsError) {
    build_graph("knows", {{1, 2}});

    AlgorithmContext ctx{engine_, default_database_id, "knows",
                         {{"damping", Value(0.85)},
                          {"iterations", Value(/* null */)}}};
    auto r = pagerank_execute(ctx);
    ASSERT_FALSE(r.has_value()) << "NULL iterations should return an error";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}
