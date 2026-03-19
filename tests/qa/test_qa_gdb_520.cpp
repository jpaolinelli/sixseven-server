/// @file test_qa_gdb_520.cpp
/// QA adversarial tests for GDB-520: Eigenvector Centrality algorithm.
///
/// Verifies:
///   AC1: eigenvector_centrality() function registered and callable.
///   AC2: Power iteration converges on known test graphs.
///   AC3: Returns correct scores matching known eigenvector values.
///   AC4: Configurable max iterations and convergence tolerance.
///   AC5: Handles disconnected components gracefully.
///   AC6: Unit tests written and passing.
///   AC7: No regressions.
///
/// Adversarial categories: boundary values, error paths, graph topology
/// edge cases, numerical stability, stress tests, mathematical invariants,
/// bipartite graphs, duplicate edges, self-loops, parameter extremes.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/eigenvector_centrality.h"
#include "sixseven/graph/graph_engine.h"

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

/// Per-node eigenvector result.
struct EigenvectorInfo {
    double score;
    int64_t iterations_used;
};

/// Extract per-node results from algorithm output rows.
std::unordered_map<int64_t, EigenvectorInfo>
to_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, EigenvectorInfo> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 3u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto score = std::get<double>(row.values[1].data());
        auto iters = std::get<int64_t>(row.values[2].data());
        result[node_id] = {score, iters};
    }
    return result;
}

// ============================================================================
// Fixture
// ============================================================================

class GDB520_EigenvectorCentralityQA : public ::testing::Test {
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

    Result<std::vector<AlgorithmRow>> run(const std::string& edge_type) {
        AlgorithmContext ctx{engine_, edge_type, {}};
        return eigenvector_centrality_execute(ctx);
    }

    Result<std::vector<AlgorithmRow>>
    run(const std::string& edge_type, std::unordered_map<std::string, Value> args) {
        AlgorithmContext ctx{engine_, edge_type, std::move(args)};
        return eigenvector_centrality_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ============================================================================
// AC1: Function registered and callable
// ============================================================================

TEST_F(GDB520_EigenvectorCentralityQA, AC1_RegistrationAndLookup) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_eigenvector_centrality_def(),
                                              eigenvector_centrality_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_TRUE(registry.has("eigenvector"));
    auto* entry = registry.find("eigenvector");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "eigenvector");
}

TEST_F(GDB520_EigenvectorCentralityQA, AC1_CaseInsensitiveLookup) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_eigenvector_centrality_def(),
                                              eigenvector_centrality_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_TRUE(registry.has("EIGENVECTOR"));
    EXPECT_TRUE(registry.has("Eigenvector"));
    EXPECT_NE(registry.find("EIGENVECTOR"), nullptr);
}

TEST_F(GDB520_EigenvectorCentralityQA, AC1_DuplicateRegistrationFails) {
    AlgorithmRegistry registry;
    auto r1 = registry.register_algorithm(make_eigenvector_centrality_def(),
                                          eigenvector_centrality_execute);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    auto r2 = registry.register_algorithm(make_eigenvector_centrality_def(),
                                          eigenvector_centrality_execute);
    EXPECT_FALSE(r2.has_value());
}

TEST_F(GDB520_EigenvectorCentralityQA, AC1_OutputSchemaCorrect) {
    auto def = make_eigenvector_centrality_def();
    ASSERT_EQ(def.output_columns.size(), 3u);
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_EQ(def.output_columns[1].name, "score");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::FLOAT64);
    EXPECT_EQ(def.output_columns[2].name, "iterations_used");
    EXPECT_EQ(def.output_columns[2].type_id, TypeId::INT64);
}

TEST_F(GDB520_EigenvectorCentralityQA, AC1_DefaultParameterValues) {
    auto def = make_eigenvector_centrality_def();
    ASSERT_EQ(def.params.size(), 2u);

    // iterations default = 100
    EXPECT_EQ(def.params[0].name, "iterations");
    ASSERT_TRUE(def.params[0].default_value.has_value());
    EXPECT_EQ(std::get<int64_t>(def.params[0].default_value->data()), 100);

    // tolerance default = 1e-6
    EXPECT_EQ(def.params[1].name, "tolerance");
    ASSERT_TRUE(def.params[1].default_value.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(def.params[1].default_value->data()), 1e-6);
}

// ============================================================================
// AC2: Power iteration converges on known test graphs
// ============================================================================

TEST_F(GDB520_EigenvectorCentralityQA, AC2_TriangleConvergesQuickly) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});
    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (const auto& [node, info] : m) {
        EXPECT_LT(info.iterations_used, 50)
            << "K3 should converge well before default 100 iterations";
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, AC2_StarGraphConverges) {
    // Star with 10 spokes
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 2; i <= 11; ++i) {
        edges.push_back({1, i});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 11u);
    for (const auto& [node, info] : m) {
        EXPECT_LT(info.iterations_used, 100)
            << "Star graph should converge before max iterations";
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, AC2_BipartiteGraphConverges) {
    // Complete bipartite K3,3: nodes {1,2,3} <-> {4,5,6}
    // Without (A+I) trick, power iteration oscillates on bipartite graphs.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 3; ++i) {
        for (int64_t j = 4; j <= 6; ++j) {
            edges.push_back({i, j});
        }
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 6u);

    // K3,3 is vertex-transitive: all nodes should have equal scores.
    for (const auto& [node, info] : m) {
        EXPECT_NEAR(info.score, 1.0, 1e-4)
            << "All nodes in K3,3 should have equal score ~1.0";
    }
    // Must have converged, not hit max iterations.
    for (const auto& [node, info] : m) {
        EXPECT_LT(info.iterations_used, 100)
            << "K3,3 should converge with (A+I) trick";
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, AC2_CycleGraphConverges) {
    // Cycle C6 (even cycle = bipartite)
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6}, {6, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 6u);

    // All nodes in a cycle have equal eigenvector centrality.
    for (const auto& [node, info] : m) {
        EXPECT_NEAR(info.score, 1.0, 1e-4)
            << "node " << node << " in C6 should have score ~1.0";
        EXPECT_LT(info.iterations_used, 100)
            << "C6 should converge";
    }
}

// ============================================================================
// AC3: Correct scores matching known eigenvector values
// ============================================================================

TEST_F(GDB520_EigenvectorCentralityQA, AC3_PathP3KnownValues) {
    // Path P3: 1--2--3. Adjacency matrix eigenvectors:
    // Principal eigenvector of (A+I): proportional to [1, sqrt(2), 1].
    // After normalization to max=1: [1/sqrt(2), 1.0, 1/sqrt(2)] ≈ [0.7071, 1.0, 0.7071].
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);

    // Center node should be normalized to 1.0.
    EXPECT_NEAR(m[2].score, 1.0, 1e-4);

    // Endpoints should be symmetric.
    EXPECT_NEAR(m[1].score, m[3].score, 1e-6);

    // Endpoint scores: for undirected path 1-2-3, the principal eigenvector
    // of adjacency matrix is [1, sqrt(2), 1]/2. After max-normalization:
    // endpoints = 1/sqrt(2) ≈ 0.7071.
    EXPECT_NEAR(m[1].score, 1.0 / std::sqrt(2.0), 1e-3);
}

TEST_F(GDB520_EigenvectorCentralityQA, AC3_StarGraphKnownRatios) {
    // Star S4: hub=1, spokes={2,3,4,5}. Hub should have highest score.
    // For star graph: hub eigenvector component / spoke component = sqrt(degree).
    build_graph("knows", {{1, 2}, {1, 3}, {1, 4}, {1, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);

    // Hub has highest score.
    EXPECT_NEAR(m[1].score, 1.0, 1e-6);

    // All spokes equal.
    for (int64_t i = 3; i <= 5; ++i) {
        EXPECT_NEAR(m[2].score, m[i].score, 1e-6);
    }

    // Hub > spoke.
    EXPECT_GT(m[1].score, m[2].score);
}

TEST_F(GDB520_EigenvectorCentralityQA, AC3_CompleteGraphAllEqual) {
    // K5: all nodes equivalent.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 5; ++i) {
        for (int64_t j = i + 1; j <= 5; ++j) {
            edges.push_back({i, j});
        }
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (const auto& [node, info] : m) {
        EXPECT_NEAR(info.score, 1.0, 1e-4)
            << "node " << node << " in K5 should have score ~1.0";
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, AC3_BarabollDumbbell) {
    // Dumbbell graph: two K3 connected by a bridge.
    // {1,2,3} fully connected, {4,5,6} fully connected, bridge 3-4.
    build_graph("knows", {
        {1, 2}, {2, 3}, {1, 3},      // K3 left
        {4, 5}, {5, 6}, {4, 6},      // K3 right
        {3, 4},                        // bridge
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 6u);

    // Bridge nodes (3 and 4) should have highest scores.
    EXPECT_GT(m[3].score, m[1].score);
    EXPECT_GT(m[4].score, m[5].score);

    // Symmetry: left clique mirrors right clique.
    EXPECT_NEAR(m[3].score, m[4].score, 1e-4);
    EXPECT_NEAR(m[1].score, m[5].score, 1e-4);
    EXPECT_NEAR(m[2].score, m[6].score, 1e-4);

    // All scores in valid range.
    for (const auto& [node, info] : m) {
        EXPECT_GE(info.score, 0.0);
        EXPECT_LE(info.score, 1.0 + 1e-10);
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, AC3_ScoresNonNegativeAndMaxOne) {
    // Arbitrary graph: verify invariant holds.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 1}, {1, 3}, {2, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    double max_score = 0.0;
    for (const auto& [node, info] : m) {
        EXPECT_GE(info.score, 0.0) << "node " << node;
        EXPECT_LE(info.score, 1.0 + 1e-10) << "node " << node;
        max_score = std::max(max_score, info.score);
    }
    EXPECT_NEAR(max_score, 1.0, 1e-6);
}

TEST_F(GDB520_EigenvectorCentralityQA, AC3_ResultsSortedByNodeId) {
    build_graph("knows", {{10, 5}, {5, 1}, {1, 20}, {20, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results must be sorted by node_id";
    }
}

// ============================================================================
// AC4: Configurable max iterations and convergence tolerance
// ============================================================================

TEST_F(GDB520_EigenvectorCentralityQA, AC4_MaxIterations1) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}});

    std::unordered_map<std::string, Value> args;
    args["iterations"] = Value(static_cast<int64_t>(1));

    auto result = run("knows", args);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (const auto& [node, info] : m) {
        EXPECT_EQ(info.iterations_used, 1)
            << "with max_iterations=1, should use exactly 1 iteration";
        EXPECT_GE(info.score, 0.0);
        EXPECT_LE(info.score, 1.0 + 1e-10);
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, AC4_MaxIterations2) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}});

    std::unordered_map<std::string, Value> args;
    args["iterations"] = Value(static_cast<int64_t>(2));

    auto result = run("knows", args);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (const auto& [node, info] : m) {
        EXPECT_LE(info.iterations_used, 2);
        EXPECT_GE(info.score, 0.0);
        EXPECT_LE(info.score, 1.0 + 1e-10);
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, AC4_VeryLargeTolerance) {
    // tolerance = 1.0 should converge on first iteration.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    std::unordered_map<std::string, Value> args;
    args["tolerance"] = Value(1.0);

    auto result = run("knows", args);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (const auto& [node, info] : m) {
        EXPECT_LE(info.iterations_used, 2)
            << "with tolerance=1.0, should converge almost immediately";
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, AC4_VeryTightTolerance) {
    // tolerance = 1e-12 should still converge on simple graph but take more iterations.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}});

    std::unordered_map<std::string, Value> args;
    args["tolerance"] = Value(1e-12);
    args["iterations"] = Value(static_cast<int64_t>(10000));

    auto result = run("knows", args);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (const auto& [node, info] : m) {
        EXPECT_LT(info.iterations_used, 10000)
            << "K3 should converge even with very tight tolerance";
        EXPECT_NEAR(info.score, 1.0, 1e-10)
            << "with tight tolerance, K3 scores should be very accurate";
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, AC4_ToleranceZero) {
    // tolerance = 0 means exact convergence required. With floating point,
    // may never converge exactly, so should hit max iterations.
    build_graph("knows", {{1, 2}, {2, 3}});

    std::unordered_map<std::string, Value> args;
    args["tolerance"] = Value(0.0);
    args["iterations"] = Value(static_cast<int64_t>(50));

    auto result = run("knows", args);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    // With tolerance=0, should likely hit max iterations (50).
    // Scores should still be valid.
    for (const auto& [node, info] : m) {
        EXPECT_GE(info.score, 0.0);
        EXPECT_LE(info.score, 1.0 + 1e-10);
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, AC4_InvalidIterationsZero) {
    build_graph("knows", {{1, 2}});

    std::unordered_map<std::string, Value> args;
    args["iterations"] = Value(static_cast<int64_t>(0));

    auto result = run("knows", args);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(GDB520_EigenvectorCentralityQA, AC4_InvalidIterationsNegative) {
    build_graph("knows", {{1, 2}});

    std::unordered_map<std::string, Value> args;
    args["iterations"] = Value(static_cast<int64_t>(-5));

    auto result = run("knows", args);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(GDB520_EigenvectorCentralityQA, AC4_NegativeToleranceFails) {
    build_graph("knows", {{1, 2}});

    std::unordered_map<std::string, Value> args;
    args["tolerance"] = Value(-0.001);

    auto result = run("knows", args);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ============================================================================
// AC5: Handles disconnected components gracefully
// ============================================================================

TEST_F(GDB520_EigenvectorCentralityQA, AC5_TwoDisconnectedPairs) {
    // {1,2} and {3,4} — two isolated edges.
    build_graph("knows", {{1, 2}, {3, 4}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 4u);

    // Each pair is symmetric.
    EXPECT_NEAR(m[1].score, m[2].score, 1e-6);
    EXPECT_NEAR(m[3].score, m[4].score, 1e-6);

    // Global max is 1.0.
    double max_score = 0.0;
    for (const auto& [n, info] : m) {
        max_score = std::max(max_score, info.score);
    }
    EXPECT_NEAR(max_score, 1.0, 1e-6);
}

TEST_F(GDB520_EigenvectorCentralityQA, AC5_DisconnectedK3AndSingleEdge) {
    // Component A: K3 {1,2,3}, Component B: edge {4,5}.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 5u);

    // Within K3, all nodes equal.
    EXPECT_NEAR(m[1].score, m[2].score, 1e-4);
    EXPECT_NEAR(m[2].score, m[3].score, 1e-4);

    // Within edge, both equal.
    EXPECT_NEAR(m[4].score, m[5].score, 1e-6);

    // All valid.
    for (const auto& [n, info] : m) {
        EXPECT_GE(info.score, 0.0);
        EXPECT_LE(info.score, 1.0 + 1e-10);
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, AC5_ManySmallComponents) {
    // 10 disconnected pairs: {1,2}, {3,4}, ..., {19,20}.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 19; i += 2) {
        edges.push_back({i, i + 1});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 20u);

    // All pairs should have equal scores within the pair.
    for (int64_t i = 1; i <= 19; i += 2) {
        EXPECT_NEAR(m[i].score, m[i + 1].score, 1e-6)
            << "pair (" << i << ", " << (i + 1) << ") should be symmetric";
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, AC5_SingleNodeViaSelfLoop) {
    // Self-loop creates a 1-node component. Should get score 0.
    build_graph("knows", {{1, 2}, {3, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    // Node 3 has a self-loop but forms a component of size 1.
    // Implementation treats this as score 0.
    if (m.count(3)) {
        EXPECT_NEAR(m[3].score, 0.0, 1e-6)
            << "Self-loop node should have score 0 (single-node component)";
        EXPECT_EQ(m[3].iterations_used, 0);
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, AC5_EmptyGraphReturnsEmpty) {
    build_graph("knows", {});
    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

// ============================================================================
// Adversarial: Graph topology edge cases
// ============================================================================

TEST_F(GDB520_EigenvectorCentralityQA, Adversarial_SelfLoopOnly) {
    // Graph with only self-loops.
    build_graph("knows", {{1, 1}, {2, 2}, {3, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    // Each node is a 1-node component with score 0.
    for (const auto& [node, info] : m) {
        EXPECT_NEAR(info.score, 0.0, 1e-6)
            << "node " << node << " (self-loop only) should have score 0";
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, Adversarial_SelfLoopPlusRealEdges) {
    // Self-loops mixed with real edges. Self-loop should not affect results significantly.
    build_graph("knows", {{1, 1}, {1, 2}, {2, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    // All three nodes in one component (the self-loop adds 1 to adj[1] for node 1).
    // Scores should be valid.
    for (const auto& [node, info] : m) {
        EXPECT_GE(info.score, 0.0);
        EXPECT_LE(info.score, 1.0 + 1e-10);
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, Adversarial_DuplicateEdges) {
    // Multiple edges between same nodes. Adjacency uses unordered_set so
    // duplicates should be deduplicated.
    build_graph("knows", {{1, 2}, {1, 2}, {1, 2}, {2, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);

    // Should be equivalent to simple path 1-2-3.
    EXPECT_NEAR(m[2].score, 1.0, 1e-4) << "center of path should be ~1.0";
    EXPECT_NEAR(m[1].score, m[3].score, 1e-4) << "endpoints should be equal";
}

TEST_F(GDB520_EigenvectorCentralityQA, Adversarial_LargeNodeIds) {
    // Test with very large node IDs.
    build_graph("knows", {
        {1000000, 2000000},
        {2000000, 3000000},
        {3000000, 1000000},
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);

    // K3: all equal.
    for (const auto& [node, info] : m) {
        EXPECT_NEAR(info.score, 1.0, 1e-4);
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, Adversarial_NegativeNodeIds) {
    // Negative node IDs should work (int64 keys).
    build_graph("knows", {{-1, -2}, {-2, -3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 3u);
    EXPECT_NEAR(m[-2].score, 1.0, 1e-4) << "center node should have highest score";
}

TEST_F(GDB520_EigenvectorCentralityQA, Adversarial_PathGraphP5Symmetry) {
    // Path P5: 1-2-3-4-5. Verify strict ordering: center > inner > endpoints.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);

    // Symmetry pairs.
    EXPECT_NEAR(m[1].score, m[5].score, 1e-6);
    EXPECT_NEAR(m[2].score, m[4].score, 1e-6);

    // Ordering: 3 > 2 > 1.
    EXPECT_GT(m[3].score, m[2].score);
    EXPECT_GT(m[2].score, m[1].score);
}

TEST_F(GDB520_EigenvectorCentralityQA, Adversarial_WheelGraph) {
    // Wheel W5: hub=1, rim 2-3-4-5-6-2, hub connects to all.
    build_graph("knows", {
        {1, 2}, {1, 3}, {1, 4}, {1, 5}, {1, 6},  // hub to rim
        {2, 3}, {3, 4}, {4, 5}, {5, 6}, {6, 2},   // rim cycle
    });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 6u);

    // Hub should have highest score (connected to all, rim nodes connected to hub + 2 rim neighbors).
    EXPECT_NEAR(m[1].score, 1.0, 1e-6);

    // All rim nodes should be equal.
    for (int64_t i = 3; i <= 6; ++i) {
        EXPECT_NEAR(m[2].score, m[i].score, 1e-4)
            << "rim nodes should have equal scores";
    }

    EXPECT_GT(m[1].score, m[2].score);
}

// ============================================================================
// Adversarial: Error paths
// ============================================================================

TEST_F(GDB520_EigenvectorCentralityQA, Error_NonexistentEdgeType) {
    build_graph("knows", {{1, 2}});

    auto result = run("no_such_edge_type");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(GDB520_EigenvectorCentralityQA, Error_StringTypeForIterations) {
    build_graph("knows", {{1, 2}});

    std::unordered_map<std::string, Value> args;
    args["iterations"] = Value(std::string("not_a_number"));

    auto result = run("knows", args);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST_F(GDB520_EigenvectorCentralityQA, Error_StringTypeForTolerance) {
    build_graph("knows", {{1, 2}});

    std::unordered_map<std::string, Value> args;
    args["tolerance"] = Value(std::string("bad"));

    auto result = run("knows", args);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// ============================================================================
// Adversarial: Numerical stability
// ============================================================================

TEST_F(GDB520_EigenvectorCentralityQA, Numerical_NaNNotProduced) {
    // Ensure no NaN or Inf in results for any graph.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}, {6, 6}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (const auto& row : *result) {
        auto score = std::get<double>(row.values[1].data());
        EXPECT_FALSE(std::isnan(score)) << "score must not be NaN";
        EXPECT_FALSE(std::isinf(score)) << "score must not be Inf";
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, Numerical_ScoresDeterministic) {
    // Run twice on same graph, results should be identical.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 1}});

    auto r1 = run("knows");
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    auto r2 = run("knows");
    ASSERT_TRUE(r2.has_value()) << r2.error().message;

    ASSERT_EQ(r1->size(), r2->size());
    for (size_t i = 0; i < r1->size(); ++i) {
        auto s1 = std::get<double>((*r1)[i].values[1].data());
        auto s2 = std::get<double>((*r2)[i].values[1].data());
        EXPECT_DOUBLE_EQ(s1, s2) << "scores should be deterministic";
    }
}

// ============================================================================
// Adversarial: Stress test
// ============================================================================

TEST_F(GDB520_EigenvectorCentralityQA, Stress_ChainOf100Nodes) {
    // Path graph with 100 nodes.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i < 100; ++i) {
        edges.push_back({i, i + 1});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 100u);

    // Middle node (50) should have highest score.
    double max_score = 0.0;
    int64_t max_node = -1;
    for (const auto& [node, info] : m) {
        if (info.score > max_score) {
            max_score = info.score;
            max_node = node;
        }
    }
    EXPECT_NEAR(max_score, 1.0, 1e-6);
    // Max node should be near center (50 or 51).
    EXPECT_GE(max_node, 45);
    EXPECT_LE(max_node, 55);
}

TEST_F(GDB520_EigenvectorCentralityQA, Stress_DenseGraph50Nodes) {
    // K50-ish: all-pairs for 50 nodes.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 50; ++i) {
        for (int64_t j = i + 1; j <= 50; ++j) {
            edges.push_back({i, j});
        }
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 50u);

    // Complete graph: all nodes should have equal score ~1.0.
    for (const auto& [node, info] : m) {
        EXPECT_NEAR(info.score, 1.0, 1e-3)
            << "node " << node << " in K50 should have score ~1.0";
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, Stress_ManyDisconnectedComponents) {
    // 50 disconnected pairs: 100 nodes total.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= 99; i += 2) {
        edges.push_back({i, i + 1});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    EXPECT_EQ(m.size(), 100u);

    // All scores valid.
    for (const auto& [node, info] : m) {
        EXPECT_GE(info.score, 0.0);
        EXPECT_LE(info.score, 1.0 + 1e-10);
    }
}

// ============================================================================
// Adversarial: Output column count invariant
// ============================================================================

TEST_F(GDB520_EigenvectorCentralityQA, OutputRowAlways3Columns) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    for (const auto& row : *result) {
        EXPECT_EQ(row.values.size(), 3u)
            << "every output row must have exactly 3 values";
    }
}

// ============================================================================
// Adversarial: iterations_used correctness
// ============================================================================

TEST_F(GDB520_EigenvectorCentralityQA, IterationsUsedLessOrEqualMax) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}});

    int64_t max_iter = 10;
    std::unordered_map<std::string, Value> args;
    args["iterations"] = Value(max_iter);

    auto result = run("knows", args);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);
    for (const auto& [node, info] : m) {
        EXPECT_LE(info.iterations_used, max_iter)
            << "iterations_used should not exceed max_iterations";
        EXPECT_GE(info.iterations_used, 1)
            << "iterations_used should be >= 1 for multi-node components";
    }
}

TEST_F(GDB520_EigenvectorCentralityQA, IterationsUsedSameWithinComponent) {
    // All nodes in a component should report the same iterations_used.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {4, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto m = to_map(*result);

    // Component {1,2,3} should all have same iterations_used.
    EXPECT_EQ(m[1].iterations_used, m[2].iterations_used);
    EXPECT_EQ(m[2].iterations_used, m[3].iterations_used);

    // Component {4,5} should all have same iterations_used.
    EXPECT_EQ(m[4].iterations_used, m[5].iterations_used);
}

} // namespace
} // namespace sixseven
