#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/pagerank.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

#include "test_catalog_helpers.h"

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

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
void verify_scores_sum_to_one(const std::unordered_map<int64_t, double>& scores) {
    double total = 0.0;
    for (const auto& [_, score] : scores) {
        total += score;
    }
    EXPECT_NEAR(total, 1.0, 1e-6) << "PageRank scores should sum to ~1.0";
}

/// Verify that all scores are positive.
void verify_scores_positive(const std::unordered_map<int64_t, double>& scores) {
    for (const auto& [node, score] : scores) {
        EXPECT_GT(score, 0.0) << "node " << node << " should have positive score";
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Algorithm definition tests
// ---------------------------------------------------------------------------

TEST(PageRankDef, OutputSchema) {
    auto def = make_pagerank_def();
    EXPECT_EQ(def.name, "pagerank");
    ASSERT_EQ(def.output_columns.size(), 2u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[1].name, "score");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::FLOAT64);
}

TEST(PageRankDef, Parameters) {
    auto def = make_pagerank_def();
    ASSERT_EQ(def.params.size(), 2u);

    EXPECT_EQ(def.params[0].name, "damping");
    EXPECT_EQ(def.params[0].type_id, TypeId::FLOAT64);
    EXPECT_FALSE(def.params[0].required);
    ASSERT_TRUE(def.params[0].default_value.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(def.params[0].default_value->data()), 0.85);

    EXPECT_EQ(def.params[1].name, "iterations");
    EXPECT_EQ(def.params[1].type_id, TypeId::INT64);
    EXPECT_FALSE(def.params[1].required);
    ASSERT_TRUE(def.params[1].default_value.has_value());
    EXPECT_EQ(std::get<int64_t>(def.params[1].default_value->data()), 20);
}

TEST(PageRankDef, Registration) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_pagerank_def(), pagerank_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto* entry = registry.find("pagerank");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "pagerank");
}

// ---------------------------------------------------------------------------
// Test fixture with a GraphEngine
// ---------------------------------------------------------------------------

class PageRankTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        auto t = catalog_.create_table(default_database_id, make_table_schema("nodes"));
        ASSERT_TRUE(t.has_value()) << t.error().message;
        table_id_ = *t;
    }

    /// Create an edge type and link a list of (src, tgt) pairs.
    void build_graph(const std::string& edge_type,
                     const std::vector<std::pair<int64_t, int64_t>>& edges) {
        auto et = engine_.create_edge_type(
            default_database_id, edge_type, table_id_, table_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(et.has_value()) << et.error().message;

        for (auto [src, tgt] : edges) {
            auto link = engine_.link(default_database_id, edge_type, pk(src), pk(tgt));
            ASSERT_TRUE(link.has_value()) << link.error().message;
        }
    }

    /// Run pagerank with default parameters.
    Result<std::vector<AlgorithmRow>> run_pagerank(const std::string& edge_type) {
        AlgorithmContext ctx{
            engine_,
            default_database_id,
            edge_type,
            {{"damping", Value(0.85)}, {"iterations", Value(static_cast<int64_t>(20))}}};
        return pagerank_execute(ctx);
    }

    /// Run pagerank with custom parameters.
    Result<std::vector<AlgorithmRow>>
    run_pagerank(const std::string& edge_type, double damping, int64_t iterations) {
        AlgorithmContext ctx{engine_,
                             default_database_id,
                             edge_type,
                             {{"damping", Value(damping)}, {"iterations", Value(iterations)}}};
        return pagerank_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ---------------------------------------------------------------------------
// Execution tests on known graphs
// ---------------------------------------------------------------------------

TEST_F(PageRankTest, EmptyGraph) {
    build_graph("knows", {});

    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST_F(PageRankTest, SingleEdge) {
    build_graph("knows", {{1, 2}});

    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 2u);
    verify_scores_sum_to_one(scores);
    verify_scores_positive(scores);

    // Node 2 receives a link from 1, so it should have higher rank.
    EXPECT_GT(scores[2], scores[1]);
}

TEST_F(PageRankTest, CycleGraph) {
    // 1 -> 2 -> 3 -> 1 (triangle cycle)
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 3u);
    verify_scores_sum_to_one(scores);

    // In a symmetric cycle, all nodes should have equal rank.
    double expected = 1.0 / 3.0;
    for (const auto& [node, score] : scores) {
        EXPECT_NEAR(score, expected, 1e-6) << "node " << node << " in cycle should have equal rank";
    }
}

TEST_F(PageRankTest, StarGraph) {
    // 2, 3, 4, 5 all point to 1
    build_graph("knows", {{2, 1}, {3, 1}, {4, 1}, {5, 1}});

    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 5u);
    verify_scores_sum_to_one(scores);
    verify_scores_positive(scores);

    // The hub node (1) should have the highest score.
    for (int64_t leaf : {2, 3, 4, 5}) {
        EXPECT_GT(scores[1], scores[leaf]) << "hub node 1 should outrank leaf " << leaf;
    }
}

TEST_F(PageRankTest, LinearChain) {
    // 1 -> 2 -> 3 -> 4
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}});

    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 4u);
    verify_scores_sum_to_one(scores);
    verify_scores_positive(scores);
}

TEST_F(PageRankTest, ResultsOrderedByNodeId) {
    build_graph("knows", {{3, 1}, {5, 2}, {4, 3}});

    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
}

TEST_F(PageRankTest, CustomDamping) {
    // Linear chain 1 -> 2 -> 3 (node 3 is a dangling node).
    // With damping=0.5 the steady-state scores differ substantially from the
    // default damping=0.85 scores.  We assert against hardcoded expected values
    // (computed analytically) so the test FAILS if the damping parameter is
    // silently ignored.
    //
    // Hardcoded expected (d=0.5, iters=20, converged to steady-state):
    //   node 1 ≈ 0.23529412   node 2 ≈ 0.35294118   node 3 ≈ 0.41176471
    // Hardcoded expected (d=0.85, iters=20, converged to steady-state):
    //   node 1 ≈ 0.18441688   node 2 ≈ 0.34117101   node 3 ≈ 0.47441212
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result_custom = run_pagerank("knows", 0.5, 20);
    ASSERT_TRUE(result_custom.has_value()) << result_custom.error().message;
    auto scores_custom = to_score_map(*result_custom);

    auto result_default = run_pagerank("knows");
    ASSERT_TRUE(result_default.has_value()) << result_default.error().message;
    auto scores_default = to_score_map(*result_default);

    ASSERT_EQ(scores_custom.size(), 3u);
    verify_scores_sum_to_one(scores_custom);

    // Assert hardcoded expected values for damping=0.5.
    // These will NOT match the damping=0.85 run, so if the engine silently
    // ignores the damping parameter this test fails.
    EXPECT_NEAR(scores_custom[1], 0.23529412, 1e-5)
        << "node 1 score with damping=0.5 should be ~0.2353";
    EXPECT_NEAR(scores_custom[2], 0.35294118, 1e-5)
        << "node 2 score with damping=0.5 should be ~0.3529";
    EXPECT_NEAR(scores_custom[3], 0.41176471, 1e-5)
        << "node 3 score with damping=0.5 should be ~0.4118";

    // Explicitly confirm that the custom-damping run differs from the
    // default-damping run by a meaningful amount.
    EXPECT_GT(std::abs(scores_custom[1] - scores_default[1]), 0.04)
        << "node 1 score must differ between damping=0.5 and damping=0.85 by >0.04";
    EXPECT_GT(std::abs(scores_custom[3] - scores_default[3]), 0.05)
        << "node 3 score must differ between damping=0.5 and damping=0.85 by >0.05";
}

TEST_F(PageRankTest, CustomIterations) {
    // Linear chain 1 -> 2 -> 3 (node 3 is a dangling node).
    // With iterations=1 the scores have not yet converged and differ
    // substantially from the fully-converged default (iterations=20) run.
    // We assert against hardcoded expected values so the test FAILS if the
    // iterations parameter is silently ignored.
    //
    // Hardcoded expected (d=0.85, iters=1):
    //   node 1 ≈ 0.14444444   node 2 ≈ 0.42777778   node 3 ≈ 0.42777778
    // Hardcoded expected (d=0.85, iters=20, converged):
    //   node 1 ≈ 0.18441688   node 2 ≈ 0.34117101   node 3 ≈ 0.47441212
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result_1iter = run_pagerank("knows", 0.85, 1);
    ASSERT_TRUE(result_1iter.has_value()) << result_1iter.error().message;
    auto scores_1iter = to_score_map(*result_1iter);

    auto result_default = run_pagerank("knows");
    ASSERT_TRUE(result_default.has_value()) << result_default.error().message;
    auto scores_default = to_score_map(*result_default);

    ASSERT_EQ(scores_1iter.size(), 3u);
    verify_scores_sum_to_one(scores_1iter);

    // Assert hardcoded expected values for iterations=1.
    // These will NOT match the iterations=20 run, so if the engine silently
    // ignores the iterations parameter this test fails.
    EXPECT_NEAR(scores_1iter[1], 0.14444444, 1e-5)
        << "node 1 score after 1 iteration should be ~0.1444";
    EXPECT_NEAR(scores_1iter[2], 0.42777778, 1e-5)
        << "node 2 score after 1 iteration should be ~0.4278";
    EXPECT_NEAR(scores_1iter[3], 0.42777778, 1e-5)
        << "node 3 score after 1 iteration should be ~0.4278";

    // Explicitly confirm that the 1-iteration run differs from the
    // fully-converged run by a meaningful amount.
    EXPECT_GT(std::abs(scores_1iter[2] - scores_default[2]), 0.08)
        << "node 2 score must differ between 1 and 20 iterations by >0.08";
    EXPECT_GT(std::abs(scores_1iter[3] - scores_default[3]), 0.04)
        << "node 3 score must differ between 1 and 20 iterations by >0.04";
}

TEST_F(PageRankTest, DanglingNodes) {
    // Node 2 is a dangling node (no outgoing edges).
    // 1 -> 2
    build_graph("knows", {{1, 2}});

    auto result = run_pagerank("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 2u);
    verify_scores_sum_to_one(scores);
    verify_scores_positive(scores);
}

TEST_F(PageRankTest, NonexistentEdgeTypeFails) {
    build_graph("knows", {{1, 2}});

    auto result = run_pagerank("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(PageRankTest, LargerGraph) {
    // Wikipedia-style example: multiple interconnected nodes.
    build_graph("links",
                {
                    {1, 2},
                    {1, 3},
                    {2, 3},
                    {3, 1},
                    {4, 3},
                });

    auto result = run_pagerank("links");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto scores = to_score_map(*result);
    EXPECT_EQ(scores.size(), 4u);
    verify_scores_sum_to_one(scores);
    verify_scores_positive(scores);

    // Node 3 receives the most incoming links (from 1, 2, and 4).
    EXPECT_GT(scores[3], scores[1]);
    EXPECT_GT(scores[3], scores[2]);
    EXPECT_GT(scores[3], scores[4]);
}
