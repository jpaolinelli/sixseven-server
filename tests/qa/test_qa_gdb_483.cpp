/// @file test_qa_gdb_483.cpp
/// QA adversarial tests for GDB-483: PageRank algorithm.
///
/// Verifies:
///   AC1: Iterative power method implementation.
///   AC2: Configurable damping and iterations.
///   AC3: Output schema: (node_id INT64, score FLOAT64).
///   AC4: Scores sum to ~1.0.
///   AC5: Unit tests on known graphs.
///
/// Adversarial categories: boundary values, error paths, edge cases, stress,
/// graph topology corner cases.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/pagerank.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Helpers
// ============================================================================

static TableSchema make_table_schema(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", TypeId::INT64, false, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

static Value pk(int64_t v) {
    return Value(v);
}

/// Extract (node_id, score) pairs from algorithm result rows.
std::unordered_map<int64_t, double> to_score_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, double> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 2u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto score = std::get<double>(row.values[1].data());
        result[node_id] = score;
    }
    return result;
}

/// Verify that all scores sum to approximately 1.0.
void verify_scores_sum_to_one(const std::unordered_map<int64_t, double>& scores,
                              double tolerance = 1e-6) {
    double total = 0.0;
    for (const auto& [_, score] : scores) {
        total += score;
    }
    EXPECT_NEAR(total, 1.0, tolerance) << "PageRank scores should sum to ~1.0";
}

/// Verify that all scores are positive.
void verify_scores_positive(const std::unordered_map<int64_t, double>& scores) {
    for (const auto& [node, score] : scores) {
        EXPECT_GT(score, 0.0) << "node " << node << " should have positive score";
    }
}

/// Verify output rows are sorted by node_id.
void verify_sorted_by_node_id(const std::vector<AlgorithmRow>& rows) {
    for (size_t i = 1; i < rows.size(); ++i) {
        auto prev = std::get<int64_t>(rows[i - 1].values[0].data());
        auto curr = std::get<int64_t>(rows[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
}

// ============================================================================
// Test fixture
// ============================================================================

class QA_GDB483_PageRank : public ::testing::Test {
protected:
    void SetUp() override {
        auto t = catalog_.create_table(default_database_id, make_table_schema("nodes"));
        ASSERT_TRUE(t.has_value()) << t.error().message;
        table_id_ = *t;
    }

    void build_graph(const std::string& edge_type,
                     const std::vector<std::pair<int64_t, int64_t>>& edges) {
        auto et = engine_.create_edge_type(default_database_id, 
            edge_type, table_id_, table_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(et.has_value()) << et.error().message;

        for (auto [src, tgt] : edges) {
            auto link = engine_.link(edge_type, pk(src), pk(tgt));
            ASSERT_TRUE(link.has_value()) << link.error().message;
        }
    }

    Result<std::vector<AlgorithmRow>> run_pagerank(const std::string& edge_type,
                                                   double damping = 0.85,
                                                   int64_t iterations = 20) {
        AlgorithmContext ctx{
            engine_,
            edge_type,
            {{"damping", Value(damping)}, {"iterations", Value(iterations)}}};
        return pagerank_execute(ctx);
    }

    /// Run pagerank with raw named_args (for testing type edge cases).
    Result<std::vector<AlgorithmRow>>
    run_pagerank_raw(const std::string& edge_type,
                     std::unordered_map<std::string, Value> args) {
        AlgorithmContext ctx{engine_, edge_type, std::move(args)};
        return pagerank_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ============================================================================
// AC1: Iterative power method implementation
// ============================================================================

TEST_F(QA_GDB483_PageRank, AC1_ConvergenceBehavior) {
    // A simple triangle cycle should converge to uniform distribution.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    // Run with few iterations.
    auto result_5 = run_pagerank("knows", 0.85, 5);
    ASSERT_TRUE(result_5.has_value()) << result_5.error().message;
    auto scores_5 = to_score_map(*result_5);

    // Run with many iterations.
    auto result_100 = run_pagerank("knows", 0.85, 100);
    ASSERT_TRUE(result_100.has_value()) << result_100.error().message;
    auto scores_100 = to_score_map(*result_100);

    // With more iterations, scores should be closer to the true stationary
    // distribution (1/3 each for a cycle).
    double expected = 1.0 / 3.0;
    for (int64_t node : {1, 2, 3}) {
        double err_5 = std::abs(scores_5[node] - expected);
        double err_100 = std::abs(scores_100[node] - expected);
        EXPECT_LE(err_100, err_5 + 1e-12)
            << "more iterations should converge closer for node " << node;
    }
}

TEST_F(QA_GDB483_PageRank, AC1_DeterministicOutput) {
    // Same input should produce the exact same output.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {1, 3}});

    auto result1 = run_pagerank("knows");
    ASSERT_TRUE(result1.has_value());
    auto result2 = run_pagerank("knows");
    ASSERT_TRUE(result2.has_value());

    ASSERT_EQ(result1->size(), result2->size());
    for (size_t i = 0; i < result1->size(); ++i) {
        auto score1 = std::get<double>((*result1)[i].values[1].data());
        auto score2 = std::get<double>((*result2)[i].values[1].data());
        EXPECT_DOUBLE_EQ(score1, score2) << "run " << i << " should be deterministic";
    }
}

// ============================================================================
// AC2: Configurable damping and iterations
// ============================================================================

TEST_F(QA_GDB483_PageRank, AC2_DampingZero) {
    // damping = 0 means pure teleportation — all scores should be uniform.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run_pagerank("knows", 0.0, 20);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    verify_scores_sum_to_one(scores);

    // With damping = 0, all scores should be exactly 1/N.
    double expected = 1.0 / 3.0;
    for (const auto& [node, score] : scores) {
        EXPECT_NEAR(score, expected, 1e-10)
            << "damping=0 should give uniform scores for node " << node;
    }
}

TEST_F(QA_GDB483_PageRank, AC2_DampingOne) {
    // damping = 1 means no teleportation. For a cycle, should converge to uniform.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run_pagerank("knows", 1.0, 50);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    verify_scores_sum_to_one(scores);
}

TEST_F(QA_GDB483_PageRank, AC2_DampingHighVsLow) {
    // Higher damping means link structure matters more.
    // Star graph: 2, 3, 4 all point to 1.
    build_graph("knows", {{2, 1}, {3, 1}, {4, 1}});

    auto result_low = run_pagerank("knows", 0.3, 50);
    ASSERT_TRUE(result_low.has_value());
    auto scores_low = to_score_map(*result_low);

    auto result_high = run_pagerank("knows", 0.95, 50);
    ASSERT_TRUE(result_high.has_value());
    auto scores_high = to_score_map(*result_high);

    // With higher damping, hub node 1 should have relatively higher score.
    double ratio_low = scores_low[1] / scores_low[2];
    double ratio_high = scores_high[1] / scores_high[2];
    EXPECT_GT(ratio_high, ratio_low)
        << "higher damping should amplify hub advantage";
}

TEST_F(QA_GDB483_PageRank, AC2_IterationsOne) {
    // Single iteration should still produce valid scores.
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run_pagerank("knows", 0.85, 1);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    verify_scores_sum_to_one(scores);
    verify_scores_positive(scores);
}

TEST_F(QA_GDB483_PageRank, AC2_IterationsZero) {
    // Zero iterations → initial uniform scores (1/N each).
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run_pagerank("knows", 0.85, 0);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 3u);

    // With 0 iterations, all scores should be 1/N (initial value).
    double expected = 1.0 / 3.0;
    for (const auto& [node, score] : scores) {
        EXPECT_NEAR(score, expected, 1e-10)
            << "0 iterations should give initial uniform scores for node " << node;
    }
    verify_scores_sum_to_one(scores);
}

TEST_F(QA_GDB483_PageRank, AC2_NegativeIterations) {
    // Negative iterations: loop doesn't execute, same as 0 iterations.
    build_graph("knows", {{1, 2}});

    auto result = run_pagerank("knows", 0.85, -5);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 2u);

    double expected = 1.0 / 2.0;
    for (const auto& [node, score] : scores) {
        EXPECT_NEAR(score, expected, 1e-10)
            << "negative iterations should give initial uniform scores";
    }
}

// ============================================================================
// AC3: Output schema: (node_id INT64, score FLOAT64)
// ============================================================================

TEST_F(QA_GDB483_PageRank, AC3_OutputSchemaDefinition) {
    auto def = make_pagerank_def();
    ASSERT_EQ(def.output_columns.size(), 2u);

    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_FALSE(def.output_columns[0].nullable);

    EXPECT_EQ(def.output_columns[1].name, "score");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::FLOAT64);
    EXPECT_FALSE(def.output_columns[1].nullable);
}

TEST_F(QA_GDB483_PageRank, AC3_OutputRowStructure) {
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value());

    for (const auto& row : *result) {
        ASSERT_EQ(row.values.size(), 2u) << "each row must have exactly 2 columns";
        // First column: node_id as int64_t.
        EXPECT_TRUE(std::holds_alternative<int64_t>(row.values[0].data()));
        // Second column: score as double.
        EXPECT_TRUE(std::holds_alternative<double>(row.values[1].data()));
    }
}

TEST_F(QA_GDB483_PageRank, AC3_OutputSortedByNodeId) {
    build_graph("knows", {{5, 1}, {3, 7}, {7, 5}, {1, 3}});

    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value());
    verify_sorted_by_node_id(*result);
}

// ============================================================================
// AC4: Scores sum to ~1.0
// ============================================================================

TEST_F(QA_GDB483_PageRank, AC4_ScoresSumToOne_SingleEdge) {
    build_graph("knows", {{1, 2}});
    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value());
    verify_scores_sum_to_one(to_score_map(*result));
}

TEST_F(QA_GDB483_PageRank, AC4_ScoresSumToOne_Cycle) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 1}});
    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value());
    verify_scores_sum_to_one(to_score_map(*result));
}

TEST_F(QA_GDB483_PageRank, AC4_ScoresSumToOne_Star) {
    build_graph("knows", {{2, 1}, {3, 1}, {4, 1}, {5, 1}, {6, 1}});
    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value());
    verify_scores_sum_to_one(to_score_map(*result));
}

TEST_F(QA_GDB483_PageRank, AC4_ScoresSumToOne_AllDangling) {
    // 1->2, 1->3: nodes 2 and 3 are both dangling (no outgoing edges).
    build_graph("knows", {{1, 2}, {1, 3}});
    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value());
    verify_scores_sum_to_one(to_score_map(*result));
}

TEST_F(QA_GDB483_PageRank, AC4_ScoresSumToOne_CompleteGraph) {
    // K4: every node links to every other node.
    build_graph("knows",
                {{1, 2}, {1, 3}, {1, 4}, {2, 1}, {2, 3}, {2, 4}, {3, 1}, {3, 2}, {3, 4},
                 {4, 1}, {4, 2}, {4, 3}});
    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value());
    auto scores = to_score_map(*result);
    verify_scores_sum_to_one(scores);

    // Complete graph should give uniform scores.
    double expected = 0.25;
    for (const auto& [node, score] : scores) {
        EXPECT_NEAR(score, expected, 1e-6) << "node " << node << " in K4 should have 0.25";
    }
}

TEST_F(QA_GDB483_PageRank, AC4_ScoresSumToOne_Disconnected) {
    // Two disconnected components: 1->2 and 3->4.
    build_graph("knows", {{1, 2}, {3, 4}});
    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value());
    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 4u);
    verify_scores_sum_to_one(scores);
}

TEST_F(QA_GDB483_PageRank, AC4_ScoresSumToOne_VaryingDamping) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {4, 2}});

    for (double d : {0.0, 0.15, 0.5, 0.85, 0.95, 1.0}) {
        auto result = run_pagerank("knows", d, 50);
        ASSERT_TRUE(result.has_value()) << "damping=" << d;
        verify_scores_sum_to_one(to_score_map(*result));
    }
}

// ============================================================================
// Graph topology edge cases
// ============================================================================

TEST_F(QA_GDB483_PageRank, SelfLoop) {
    // Node 1 points to itself and to node 2.
    build_graph("knows", {{1, 1}, {1, 2}});

    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 2u);
    verify_scores_sum_to_one(scores);
    verify_scores_positive(scores);
}

TEST_F(QA_GDB483_PageRank, PureSelfLoop) {
    // Single node with only a self-loop.
    build_graph("knows", {{1, 1}});

    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 1u);
    EXPECT_NEAR(scores[1], 1.0, 1e-10) << "single node should have score 1.0";
}

TEST_F(QA_GDB483_PageRank, DuplicateEdges) {
    // Same edge added multiple times. The GraphEngine deduplicates edges,
    // so the effective graph is 1<->2 (one edge each direction).
    build_graph("knows", {{1, 2}, {1, 2}, {1, 2}, {2, 1}});

    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 2u);
    verify_scores_sum_to_one(scores);

    // After dedup, 1<->2 is symmetric, so scores should be equal.
    EXPECT_NEAR(scores[1], scores[2], 1e-6)
        << "symmetric 1<->2 after dedup should give equal scores";
}

TEST_F(QA_GDB483_PageRank, DisconnectedComponents) {
    // Two separate triangles.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}, {5, 6}, {6, 4}});

    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 6u);
    verify_scores_sum_to_one(scores);
    verify_scores_positive(scores);

    // Nodes within each triangle should have similar scores due to symmetry.
    EXPECT_NEAR(scores[1], scores[2], 1e-6);
    EXPECT_NEAR(scores[2], scores[3], 1e-6);
    EXPECT_NEAR(scores[4], scores[5], 1e-6);
    EXPECT_NEAR(scores[5], scores[6], 1e-6);

    // Both triangles are symmetric, so all 6 nodes should have equal rank.
    EXPECT_NEAR(scores[1], scores[4], 1e-6);
}

TEST_F(QA_GDB483_PageRank, LinearChainLong) {
    // 1 -> 2 -> 3 -> ... -> 10 (long chain).
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i < 10; ++i) {
        edges.push_back({i, i + 1});
    }
    build_graph("knows", edges);

    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 10u);
    verify_scores_sum_to_one(scores);
    verify_scores_positive(scores);
}

TEST_F(QA_GDB483_PageRank, BidirectionalEdges) {
    // 1 <-> 2 <-> 3 (bidirectional chain).
    build_graph("knows", {{1, 2}, {2, 1}, {2, 3}, {3, 2}});

    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    verify_scores_sum_to_one(scores);

    // Middle node (2) has more connections and should rank higher.
    EXPECT_GT(scores[2], scores[1]);
    EXPECT_GT(scores[2], scores[3]);
    // Endpoints should have equal rank due to symmetry.
    EXPECT_NEAR(scores[1], scores[3], 1e-6);
}

TEST_F(QA_GDB483_PageRank, HubAndAuthority) {
    // Node 1 points to many nodes (hub), node 10 receives from many nodes (authority).
    // 1->10, 2->10, 3->10, 4->10, 5->10, 1->2, 1->3, 1->4, 1->5
    build_graph("knows",
                {{1, 10}, {2, 10}, {3, 10}, {4, 10}, {5, 10}, {1, 2}, {1, 3}, {1, 4}, {1, 5}});

    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    verify_scores_sum_to_one(scores);

    // Node 10 receives the most incoming links (5), should have highest score.
    for (int64_t n : {1, 2, 3, 4, 5}) {
        EXPECT_GT(scores[10], scores[n]) << "authority node 10 should outrank node " << n;
    }
}

// ============================================================================
// Error paths
// ============================================================================

TEST_F(QA_GDB483_PageRank, NonexistentEdgeType) {
    build_graph("knows", {{1, 2}});
    auto result = run_pagerank("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB483_PageRank, EmptyGraph) {
    build_graph("knows", {});
    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(QA_GDB483_PageRank, StringDampingValueFails) {
    build_graph("knows", {{1, 2}});
    // Pass a string value for damping (should fail with type error).
    auto result = run_pagerank_raw("knows",
                                   {{"damping", Value(std::string("not_a_number"))},
                                    {"iterations", Value(static_cast<int64_t>(20))}});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST_F(QA_GDB483_PageRank, StringIterationsValueFails) {
    build_graph("knows", {{1, 2}});
    // Pass a string value for iterations (should fail with type error).
    auto result = run_pagerank_raw("knows",
                                   {{"damping", Value(0.85)},
                                    {"iterations", Value(std::string("ten"))}});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST_F(QA_GDB483_PageRank, NullDampingValueFails) {
    build_graph("knows", {{1, 2}});
    auto result = run_pagerank_raw("knows",
                                   {{"damping", Value()},
                                    {"iterations", Value(static_cast<int64_t>(20))}});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB483_PageRank, NullIterationsValueFails) {
    build_graph("knows", {{1, 2}});
    auto result = run_pagerank_raw("knows",
                                   {{"damping", Value(0.85)},
                                    {"iterations", Value()}});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ============================================================================
// Parameter type coercion
// ============================================================================

TEST_F(QA_GDB483_PageRank, DampingAsInteger) {
    // Pass damping as int64_t (should be coerced to double).
    build_graph("knows", {{1, 2}, {2, 1}});
    auto result = run_pagerank_raw("knows",
                                   {{"damping", Value(static_cast<int64_t>(1))},
                                    {"iterations", Value(static_cast<int64_t>(10))}});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto scores = to_score_map(*result);
    verify_scores_sum_to_one(scores);
}

TEST_F(QA_GDB483_PageRank, IterationsAsFloat) {
    // Pass iterations as double — should fail since iterations expects integer.
    build_graph("knows", {{1, 2}});
    auto result = run_pagerank_raw("knows",
                                   {{"damping", Value(0.85)},
                                    {"iterations", Value(10.0)}});
    ASSERT_FALSE(result.has_value())
        << "iterations parameter should reject floating-point values";
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST_F(QA_GDB483_PageRank, DefaultParametersWhenOmitted) {
    // Pass no named args — should use defaults (damping=0.85, iterations=20).
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});
    auto result = run_pagerank_raw("knows", {});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    verify_scores_sum_to_one(scores);
}

// ============================================================================
// Algorithm definition tests
// ============================================================================

TEST(QA_GDB483_Def, Registration) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_pagerank_def(), pagerank_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto* entry = registry.find("pagerank");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "pagerank");
    EXPECT_EQ(entry->def.params.size(), 2u);
    EXPECT_EQ(entry->def.output_columns.size(), 2u);
}

TEST(QA_GDB483_Def, CaseInsensitiveLookup) {
    AlgorithmRegistry registry;
    (void)registry.register_algorithm(make_pagerank_def(), pagerank_execute);

    EXPECT_NE(registry.find("PAGERANK"), nullptr);
    EXPECT_NE(registry.find("PageRank"), nullptr);
    EXPECT_NE(registry.find("pagerank"), nullptr);
    EXPECT_NE(registry.find("pAgErAnK"), nullptr);
}

TEST(QA_GDB483_Def, ParameterDefaults) {
    auto def = make_pagerank_def();

    // damping default = 0.85
    ASSERT_TRUE(def.params[0].default_value.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(def.params[0].default_value->data()), 0.85);

    // iterations default = 20
    ASSERT_TRUE(def.params[1].default_value.has_value());
    EXPECT_EQ(std::get<int64_t>(def.params[1].default_value->data()), 20);
}

// ============================================================================
// Boundary values: negative damping
// ============================================================================

TEST_F(QA_GDB483_PageRank, NegativeDamping) {
    // Negative damping is mathematically invalid for PageRank.
    // The implementation doesn't validate this, so scores may go negative
    // or not sum to 1.0. This test documents the behavior.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run_pagerank("knows", -0.5, 20);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    // With negative damping, (1-d)/N > 1/N and d*contribution is negative.
    // Scores should still sum to ~1.0 mathematically.
    verify_scores_sum_to_one(scores);
}

TEST_F(QA_GDB483_PageRank, DampingGreaterThanOne) {
    // Damping > 1.0 is mathematically invalid.
    // (1-d)/N becomes negative.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run_pagerank("knows", 1.5, 20);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    // Scores should still sum to ~1.0 for this symmetric graph.
    verify_scores_sum_to_one(scores);
}

// ============================================================================
// Stress tests
// ============================================================================

TEST_F(QA_GDB483_PageRank, StressLargeGraph) {
    // Build a graph with 500 nodes in a ring.
    constexpr int64_t N = 500;
    std::vector<std::pair<int64_t, int64_t>> edges;
    edges.reserve(N);
    for (int64_t i = 0; i < N; ++i) {
        edges.push_back({i, (i + 1) % N});
    }
    build_graph("knows", edges);

    auto result = run_pagerank("knows", 0.85, 50);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), static_cast<size_t>(N));
    verify_scores_sum_to_one(scores);

    // Ring graph: all nodes should have equal rank.
    double expected = 1.0 / N;
    for (const auto& [node, score] : scores) {
        EXPECT_NEAR(score, expected, 1e-6) << "node " << node << " in ring should have 1/N";
    }
}

TEST_F(QA_GDB483_PageRank, StressManyIterations) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    // 10000 iterations should not crash or produce NaN/Inf.
    auto result = run_pagerank("knows", 0.85, 10000);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    verify_scores_sum_to_one(scores);
    for (const auto& [node, score] : scores) {
        EXPECT_FALSE(std::isnan(score)) << "score for node " << node << " is NaN";
        EXPECT_FALSE(std::isinf(score)) << "score for node " << node << " is Inf";
    }
}

TEST_F(QA_GDB483_PageRank, StressDenseGraph) {
    // Dense graph: 20 nodes, each pointing to all others.
    constexpr int64_t N = 20;
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < N; ++i) {
        for (int64_t j = 0; j < N; ++j) {
            if (i != j) {
                edges.push_back({i, j});
            }
        }
    }
    build_graph("knows", edges);

    auto result = run_pagerank("knows", 0.85, 30);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), static_cast<size_t>(N));
    verify_scores_sum_to_one(scores);

    // Complete graph: all nodes should have equal rank.
    double expected = 1.0 / N;
    for (const auto& [node, score] : scores) {
        EXPECT_NEAR(score, expected, 1e-6);
    }
}

} // namespace
} // namespace sixseven
