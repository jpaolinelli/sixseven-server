/// @file test_qa_gdb_517.cpp
/// QA adversarial tests for GDB-517: Degree Centrality algorithm.
///
/// Verifies:
///   AC1: degree_centrality() function registered and callable.
///   AC2: Returns correct in-degree, out-degree, total degree per node.
///   AC3: Normalized degree computed correctly.
///   AC4: Direction parameter filters to in/out/all.
///
/// Adversarial categories: boundary values, error paths, graph topology
/// edge cases, numerical stability, stress tests, mathematical invariants.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/degree_centrality.h"
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

/// Degree info for a single node.
struct DegreeInfo {
    int64_t in_degree;
    int64_t out_degree;
    int64_t degree;
    double normalized_degree;
};

/// Extract (node_id, DegreeInfo) from algorithm result rows.
std::unordered_map<int64_t, DegreeInfo> to_degree_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, DegreeInfo> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 5u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto in_deg = std::get<int64_t>(row.values[1].data());
        auto out_deg = std::get<int64_t>(row.values[2].data());
        auto deg = std::get<int64_t>(row.values[3].data());
        auto norm = std::get<double>(row.values[4].data());
        result[node_id] = {in_deg, out_deg, deg, norm};
    }
    return result;
}

/// Verify output rows are sorted by node_id.
void verify_sorted_by_node_id(const std::vector<AlgorithmRow>& rows) {
    for (size_t i = 1; i < rows.size(); ++i) {
        auto prev = std::get<int64_t>(rows[i - 1].values[0].data());
        auto curr = std::get<int64_t>(rows[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
}

/// Verify that all normalized degree values are finite (not NaN or Inf).
void verify_normalized_finite(const std::unordered_map<int64_t, DegreeInfo>& degrees) {
    for (const auto& [node, info] : degrees) {
        EXPECT_FALSE(std::isnan(info.normalized_degree))
            << "node " << node << " has NaN normalized_degree";
        EXPECT_FALSE(std::isinf(info.normalized_degree))
            << "node " << node << " has Inf normalized_degree";
    }
}

/// Verify the invariant: degree = in_degree + out_degree for direction "all".
void verify_degree_equals_in_plus_out(const std::unordered_map<int64_t, DegreeInfo>& degrees) {
    for (const auto& [node, info] : degrees) {
        EXPECT_EQ(info.degree, info.in_degree + info.out_degree)
            << "node " << node << ": degree should equal in_degree + out_degree";
    }
}

// ============================================================================
// Test fixture
// ============================================================================

class QA_GDB517_DegreeCentrality : public ::testing::Test {
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

    Result<std::vector<AlgorithmRow>> run(const std::string& edge_type,
                                          const std::string& direction = "all") {
        AlgorithmContext ctx{engine_, edge_type, {{"direction", Value(std::string(direction))}}};
        return degree_centrality_execute(ctx);
    }

    Result<std::vector<AlgorithmRow>>
    run_raw(const std::string& edge_type,
            std::unordered_map<std::string, Value> args) {
        AlgorithmContext ctx{engine_, edge_type, std::move(args)};
        return degree_centrality_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ============================================================================
// AC1: degree_centrality() function registered and callable
// ============================================================================

TEST(QA_GDB517_Def, AC1_Registration) {
    AlgorithmRegistry registry;
    auto result =
        registry.register_algorithm(make_degree_centrality_def(), degree_centrality_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto* entry = registry.find("degree_centrality");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "degree_centrality");
}

TEST(QA_GDB517_Def, AC1_CaseInsensitiveLookup) {
    AlgorithmRegistry registry;
    (void)registry.register_algorithm(make_degree_centrality_def(), degree_centrality_execute);

    EXPECT_NE(registry.find("DEGREE_CENTRALITY"), nullptr);
    EXPECT_NE(registry.find("Degree_Centrality"), nullptr);
    EXPECT_NE(registry.find("degree_centrality"), nullptr);
}

TEST(QA_GDB517_Def, AC1_OutputSchemaColumns) {
    auto def = make_degree_centrality_def();
    ASSERT_EQ(def.output_columns.size(), 5u);

    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
    EXPECT_FALSE(def.output_columns[0].nullable);

    EXPECT_EQ(def.output_columns[1].name, "in_degree");
    EXPECT_EQ(def.output_columns[1].type_id, TypeId::INT64);

    EXPECT_EQ(def.output_columns[2].name, "out_degree");
    EXPECT_EQ(def.output_columns[2].type_id, TypeId::INT64);

    EXPECT_EQ(def.output_columns[3].name, "degree");
    EXPECT_EQ(def.output_columns[3].type_id, TypeId::INT64);

    EXPECT_EQ(def.output_columns[4].name, "normalized_degree");
    EXPECT_EQ(def.output_columns[4].type_id, TypeId::FLOAT64);
}

TEST(QA_GDB517_Def, AC1_DirectionParamDefinition) {
    auto def = make_degree_centrality_def();
    ASSERT_EQ(def.params.size(), 1u);

    EXPECT_EQ(def.params[0].name, "direction");
    EXPECT_EQ(def.params[0].type_id, TypeId::STRING);
    EXPECT_FALSE(def.params[0].required);
    ASSERT_TRUE(def.params[0].default_value.has_value());
    EXPECT_EQ(std::get<std::string>(def.params[0].default_value->data()), "all");
}

TEST_F(QA_GDB517_DegreeCentrality, AC1_OutputRowStructure) {
    build_graph("knows", {{1, 2}, {2, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value());

    for (const auto& row : *result) {
        ASSERT_EQ(row.values.size(), 5u) << "each row must have exactly 5 columns";
        EXPECT_TRUE(std::holds_alternative<int64_t>(row.values[0].data()));
        EXPECT_TRUE(std::holds_alternative<int64_t>(row.values[1].data()));
        EXPECT_TRUE(std::holds_alternative<int64_t>(row.values[2].data()));
        EXPECT_TRUE(std::holds_alternative<int64_t>(row.values[3].data()));
        EXPECT_TRUE(std::holds_alternative<double>(row.values[4].data()));
    }
}

// ============================================================================
// AC2: Returns correct in-degree, out-degree, total degree per node
// ============================================================================

TEST_F(QA_GDB517_DegreeCentrality, AC2_LinearChain) {
    // 1->2->3->4: verify exact degree values.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 4u);

    // Node 1: out=1, in=0, total=1
    EXPECT_EQ(degrees[1].in_degree, 0);
    EXPECT_EQ(degrees[1].out_degree, 1);
    EXPECT_EQ(degrees[1].degree, 1);

    // Node 2: out=1, in=1, total=2
    EXPECT_EQ(degrees[2].in_degree, 1);
    EXPECT_EQ(degrees[2].out_degree, 1);
    EXPECT_EQ(degrees[2].degree, 2);

    // Node 3: out=1, in=1, total=2
    EXPECT_EQ(degrees[3].in_degree, 1);
    EXPECT_EQ(degrees[3].out_degree, 1);
    EXPECT_EQ(degrees[3].degree, 2);

    // Node 4: out=0, in=1, total=1
    EXPECT_EQ(degrees[4].in_degree, 1);
    EXPECT_EQ(degrees[4].out_degree, 0);
    EXPECT_EQ(degrees[4].degree, 1);
}

TEST_F(QA_GDB517_DegreeCentrality, AC2_BidirectionalEdges) {
    // 1<->2: bidirectional edge.
    build_graph("knows", {{1, 2}, {2, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 2u);

    // Both nodes: in=1, out=1, total=2
    EXPECT_EQ(degrees[1].in_degree, 1);
    EXPECT_EQ(degrees[1].out_degree, 1);
    EXPECT_EQ(degrees[1].degree, 2);
    EXPECT_EQ(degrees[2].in_degree, 1);
    EXPECT_EQ(degrees[2].out_degree, 1);
    EXPECT_EQ(degrees[2].degree, 2);
}

TEST_F(QA_GDB517_DegreeCentrality, AC2_SelfLoopInMultiNodeGraph) {
    // Self-loop on node 2 in a 3-node graph: 1->2, 2->2, 2->3
    build_graph("knows", {{1, 2}, {2, 2}, {2, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 3u);

    // Node 1: out=1, in=0, total=1
    EXPECT_EQ(degrees[1].in_degree, 0);
    EXPECT_EQ(degrees[1].out_degree, 1);

    // Node 2: self-loop adds +1 in and +1 out, plus edge from 1 and to 3
    // in=2 (from 1 and self), out=2 (to self and 3), total=4
    EXPECT_EQ(degrees[2].in_degree, 2);
    EXPECT_EQ(degrees[2].out_degree, 2);
    EXPECT_EQ(degrees[2].degree, 4);

    // Node 3: in=1, out=0, total=1
    EXPECT_EQ(degrees[3].in_degree, 1);
    EXPECT_EQ(degrees[3].out_degree, 0);
}

TEST_F(QA_GDB517_DegreeCentrality, AC2_DuplicateEdges) {
    // Multiple edges between same pair: 1->2 three times.
    build_graph("knows", {{1, 2}, {1, 2}, {1, 2}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 2u);

    // Multigraph: counts all edges.
    EXPECT_EQ(degrees[1].out_degree, 3);
    EXPECT_EQ(degrees[2].in_degree, 3);
    EXPECT_EQ(degrees[1].degree, 3);
    EXPECT_EQ(degrees[2].degree, 3);
}

TEST_F(QA_GDB517_DegreeCentrality, AC2_DegreeEqualsInPlusOutInvariant) {
    // Complex graph: verify degree = in + out for every node.
    build_graph("knows", {{1, 2}, {1, 3}, {2, 3}, {3, 1}, {3, 4}, {4, 2}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    verify_degree_equals_in_plus_out(degrees);
}

TEST_F(QA_GDB517_DegreeCentrality, AC2_SumInDegreeEqualsSumOutDegree) {
    // Fundamental directed graph invariant: sum(in_degree) = sum(out_degree) = |E|
    build_graph("knows", {{1, 2}, {1, 3}, {2, 3}, {3, 1}, {3, 4}, {4, 2}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    int64_t sum_in = 0;
    int64_t sum_out = 0;
    for (const auto& [node, info] : degrees) {
        sum_in += info.in_degree;
        sum_out += info.out_degree;
    }
    EXPECT_EQ(sum_in, sum_out) << "sum of in-degrees must equal sum of out-degrees";
    EXPECT_EQ(sum_in, 6) << "sum must equal number of edges";
}

TEST_F(QA_GDB517_DegreeCentrality, AC2_DisconnectedComponents) {
    // Two disconnected pairs: {1->2, 3->4}
    build_graph("knows", {{1, 2}, {3, 4}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 4u);

    EXPECT_EQ(degrees[1].out_degree, 1);
    EXPECT_EQ(degrees[1].in_degree, 0);
    EXPECT_EQ(degrees[2].out_degree, 0);
    EXPECT_EQ(degrees[2].in_degree, 1);
    EXPECT_EQ(degrees[3].out_degree, 1);
    EXPECT_EQ(degrees[3].in_degree, 0);
    EXPECT_EQ(degrees[4].out_degree, 0);
    EXPECT_EQ(degrees[4].in_degree, 1);
}

TEST_F(QA_GDB517_DegreeCentrality, AC2_EmptyGraphReturnsEmpty) {
    build_graph("knows", {});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

// ============================================================================
// AC3: Normalized degree computed correctly
// ============================================================================

TEST_F(QA_GDB517_DegreeCentrality, AC3_NormalizedDegreeTwoNodes) {
    // N=2, divisor=1. Degree=1 => normalized=1.0.
    build_graph("knows", {{1, 2}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    // normalized = degree / (N-1) = 1 / 1 = 1.0
    EXPECT_DOUBLE_EQ(degrees[1].normalized_degree, 1.0);
    EXPECT_DOUBLE_EQ(degrees[2].normalized_degree, 1.0);
}

TEST_F(QA_GDB517_DegreeCentrality, AC3_NormalizedDegreeSingleNodeSelfLoop) {
    // N=1, divisor=1 (special case). Self-loop: degree=2 => normalized=2.0.
    build_graph("knows", {{1, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 1u);
    EXPECT_DOUBLE_EQ(degrees[1].normalized_degree, 2.0);
}

TEST_F(QA_GDB517_DegreeCentrality, AC3_NormalizedDegreeFormula) {
    // 5-node star: hub=1 with 4 outgoing edges. Verify exact formula.
    build_graph("knows", {{1, 2}, {1, 3}, {1, 4}, {1, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    double N = 5.0;
    double divisor = N - 1.0; // = 4.0

    // Hub: degree=4, normalized=4/4=1.0
    EXPECT_DOUBLE_EQ(degrees[1].normalized_degree, degrees[1].degree / divisor);
    EXPECT_DOUBLE_EQ(degrees[1].normalized_degree, 1.0);

    // Leaf: degree=1, normalized=1/4=0.25
    for (int64_t leaf : {2, 3, 4, 5}) {
        EXPECT_DOUBLE_EQ(degrees[leaf].normalized_degree, degrees[leaf].degree / divisor);
        EXPECT_DOUBLE_EQ(degrees[leaf].normalized_degree, 0.25);
    }
}

TEST_F(QA_GDB517_DegreeCentrality, AC3_NormalizedDegreeCanExceedOne) {
    // Complete K4: each node has degree = 6 (in=3, out=3), divisor=3.
    // Normalized = 6/3 = 2.0 — exceeding 1.0 is valid for directed "all".
    build_graph("knows",
                {{1, 2}, {1, 3}, {1, 4}, {2, 1}, {2, 3}, {2, 4},
                 {3, 1}, {3, 2}, {3, 4}, {4, 1}, {4, 2}, {4, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    for (int64_t node : {1, 2, 3, 4}) {
        EXPECT_DOUBLE_EQ(degrees[node].normalized_degree, 2.0);
    }
}

TEST_F(QA_GDB517_DegreeCentrality, AC3_NormalizedDegreeInDirection) {
    // Direction "in": normalized = in_degree / (N-1).
    build_graph("knows", {{1, 2}, {1, 3}, {2, 3}});

    auto result = run("knows", "in");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    double divisor = 2.0; // N=3, divisor=2

    EXPECT_DOUBLE_EQ(degrees[1].normalized_degree, 0.0 / divisor);
    EXPECT_DOUBLE_EQ(degrees[2].normalized_degree, 1.0 / divisor);
    EXPECT_DOUBLE_EQ(degrees[3].normalized_degree, 2.0 / divisor);
}

TEST_F(QA_GDB517_DegreeCentrality, AC3_NormalizedDegreeOutDirection) {
    // Direction "out": normalized = out_degree / (N-1).
    build_graph("knows", {{1, 2}, {1, 3}, {2, 3}});

    auto result = run("knows", "out");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    double divisor = 2.0; // N=3, divisor=2

    EXPECT_DOUBLE_EQ(degrees[1].normalized_degree, 2.0 / divisor);
    EXPECT_DOUBLE_EQ(degrees[2].normalized_degree, 1.0 / divisor);
    EXPECT_DOUBLE_EQ(degrees[3].normalized_degree, 0.0 / divisor);
}

TEST_F(QA_GDB517_DegreeCentrality, AC3_NormalizedDegreeZero) {
    // A node with degree 0 should have normalized_degree 0.0 exactly.
    // In direction "in", a source-only node has degree 0.
    build_graph("knows", {{1, 2}, {1, 3}});

    auto result = run("knows", "in");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_DOUBLE_EQ(degrees[1].normalized_degree, 0.0);
}

// ============================================================================
// AC4: Direction parameter filters to in/out/all
// ============================================================================

TEST_F(QA_GDB517_DegreeCentrality, AC4_DirectionInFiltersDegreeColumn) {
    build_graph("knows", {{1, 2}, {1, 3}, {2, 3}});

    auto result = run("knows", "in");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    // degree column should equal in_degree
    for (const auto& [node, info] : degrees) {
        EXPECT_EQ(info.degree, info.in_degree) << "node " << node;
    }
}

TEST_F(QA_GDB517_DegreeCentrality, AC4_DirectionOutFiltersDegreeColumn) {
    build_graph("knows", {{1, 2}, {1, 3}, {2, 3}});

    auto result = run("knows", "out");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    // degree column should equal out_degree
    for (const auto& [node, info] : degrees) {
        EXPECT_EQ(info.degree, info.out_degree) << "node " << node;
    }
}

TEST_F(QA_GDB517_DegreeCentrality, AC4_DirectionAllDegreeIsSum) {
    build_graph("knows", {{1, 2}, {1, 3}, {2, 3}});

    auto result = run("knows", "all");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    verify_degree_equals_in_plus_out(degrees);
}

TEST_F(QA_GDB517_DegreeCentrality, AC4_InAndOutColumnsAlwaysPresent) {
    // Even with direction="in", in_degree and out_degree columns still computed.
    build_graph("knows", {{1, 2}, {2, 3}});

    auto in_result = run("knows", "in");
    ASSERT_TRUE(in_result.has_value()) << in_result.error().message;

    auto out_result = run("knows", "out");
    ASSERT_TRUE(out_result.has_value()) << out_result.error().message;

    auto in_degrees = to_degree_map(*in_result);
    auto out_degrees = to_degree_map(*out_result);

    // in_degree and out_degree columns should be identical regardless of direction
    for (const auto& [node, in_info] : in_degrees) {
        EXPECT_EQ(in_info.in_degree, out_degrees[node].in_degree)
            << "node " << node << " in_degree should be the same";
        EXPECT_EQ(in_info.out_degree, out_degrees[node].out_degree)
            << "node " << node << " out_degree should be the same";
    }
}

TEST_F(QA_GDB517_DegreeCentrality, AC4_DefaultDirectionIsAll) {
    // When no direction arg is passed, default to "all".
    build_graph("knows", {{1, 2}, {1, 3}, {2, 3}});

    auto default_result = run_raw("knows", {});
    auto explicit_all = run("knows", "all");
    ASSERT_TRUE(default_result.has_value()) << default_result.error().message;
    ASSERT_TRUE(explicit_all.has_value()) << explicit_all.error().message;

    ASSERT_EQ(default_result->size(), explicit_all->size());
    auto default_degrees = to_degree_map(*default_result);
    auto all_degrees = to_degree_map(*explicit_all);

    for (const auto& [node, info] : default_degrees) {
        EXPECT_EQ(info.degree, all_degrees[node].degree) << "node " << node;
        EXPECT_DOUBLE_EQ(info.normalized_degree, all_degrees[node].normalized_degree)
            << "node " << node;
    }
}

// ============================================================================
// Error paths
// ============================================================================

TEST_F(QA_GDB517_DegreeCentrality, Error_NonexistentEdgeType) {
    build_graph("knows", {{1, 2}});

    auto result = run("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB517_DegreeCentrality, Error_InvalidDirectionString) {
    build_graph("knows", {{1, 2}});

    auto result = run("knows", "invalid");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB517_DegreeCentrality, Error_EmptyDirectionString) {
    build_graph("knows", {{1, 2}});

    auto result = run("knows", "");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB517_DegreeCentrality, Error_DirectionCaseSensitive) {
    // Direction values are case-sensitive: "IN", "Out", "ALL" should fail.
    build_graph("knows", {{1, 2}});

    auto r1 = run("knows", "IN");
    ASSERT_FALSE(r1.has_value()) << "'IN' should be rejected (case-sensitive)";
    EXPECT_EQ(r1.error().code, StatusCode::INVALID_ARGUMENT);

    auto r2 = run("knows", "Out");
    ASSERT_FALSE(r2.has_value()) << "'Out' should be rejected (case-sensitive)";

    auto r3 = run("knows", "ALL");
    ASSERT_FALSE(r3.has_value()) << "'ALL' should be rejected (case-sensitive)";
}

TEST_F(QA_GDB517_DegreeCentrality, Error_DirectionWithWhitespace) {
    build_graph("knows", {{1, 2}});

    auto r1 = run("knows", " in");
    ASSERT_FALSE(r1.has_value()) << "leading whitespace should be rejected";

    auto r2 = run("knows", "out ");
    ASSERT_FALSE(r2.has_value()) << "trailing whitespace should be rejected";
}

TEST_F(QA_GDB517_DegreeCentrality, Error_NonStringDirectionValue) {
    build_graph("knows", {{1, 2}});

    // Pass an integer for direction — should fail with TYPE_ERROR.
    auto result = run_raw("knows", {{"direction", Value(static_cast<int64_t>(42))}});
    ASSERT_FALSE(result.has_value())
        << "integer direction value should fail with type error";
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST_F(QA_GDB517_DegreeCentrality, Error_NullDirectionValue) {
    build_graph("knows", {{1, 2}});

    auto result = run_raw("knows", {{"direction", Value()}});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ============================================================================
// Graph topology edge cases
// ============================================================================

TEST_F(QA_GDB517_DegreeCentrality, Topology_MultipleSelfLoops) {
    // All nodes have self-loops: 1->1, 2->2, 3->3, plus 1->2, 2->3.
    build_graph("knows", {{1, 1}, {2, 2}, {3, 3}, {1, 2}, {2, 3}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    verify_degree_equals_in_plus_out(degrees);
    verify_normalized_finite(degrees);

    // Node 1: out=2(self+to2), in=1(self), total=3
    EXPECT_EQ(degrees[1].in_degree, 1);
    EXPECT_EQ(degrees[1].out_degree, 2);

    // Node 2: out=2(self+to3), in=2(self+from1), total=4
    EXPECT_EQ(degrees[2].in_degree, 2);
    EXPECT_EQ(degrees[2].out_degree, 2);

    // Node 3: out=1(self), in=2(self+from2), total=3
    EXPECT_EQ(degrees[3].in_degree, 2);
    EXPECT_EQ(degrees[3].out_degree, 1);
}

TEST_F(QA_GDB517_DegreeCentrality, Topology_DirectedCycle) {
    // Directed 5-cycle: 1->2->3->4->5->1. All nodes should have equal degree.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 5u);

    for (int64_t node : {1, 2, 3, 4, 5}) {
        EXPECT_EQ(degrees[node].in_degree, 1);
        EXPECT_EQ(degrees[node].out_degree, 1);
        EXPECT_EQ(degrees[node].degree, 2);
        EXPECT_DOUBLE_EQ(degrees[node].normalized_degree, 2.0 / 4.0);
    }
}

TEST_F(QA_GDB517_DegreeCentrality, Topology_ComplexDAG) {
    // DAG with multiple paths: 1->2, 1->3, 2->4, 3->4, 4->5
    build_graph("knows", {{1, 2}, {1, 3}, {2, 4}, {3, 4}, {4, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 5u);

    // Node 4 has highest total degree: in=2(from 2,3), out=1(to 5), total=3
    EXPECT_EQ(degrees[4].in_degree, 2);
    EXPECT_EQ(degrees[4].out_degree, 1);
    EXPECT_EQ(degrees[4].degree, 3);

    // Source node 1: in=0, out=2
    EXPECT_EQ(degrees[1].in_degree, 0);
    EXPECT_EQ(degrees[1].out_degree, 2);

    // Sink node 5: in=1, out=0
    EXPECT_EQ(degrees[5].in_degree, 1);
    EXPECT_EQ(degrees[5].out_degree, 0);
}

TEST_F(QA_GDB517_DegreeCentrality, Topology_LargeNodeIds) {
    constexpr int64_t A = 1000000000LL;
    constexpr int64_t B = 2000000000LL;
    constexpr int64_t C = 3000000000LL;
    build_graph("knows", {{A, B}, {B, C}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 3u);
    verify_sorted_by_node_id(*result);

    EXPECT_EQ(degrees[A].out_degree, 1);
    EXPECT_EQ(degrees[B].in_degree, 1);
    EXPECT_EQ(degrees[B].out_degree, 1);
    EXPECT_EQ(degrees[C].in_degree, 1);
}

TEST_F(QA_GDB517_DegreeCentrality, Topology_NegativeNodeIds) {
    build_graph("knows", {{-10, -5}, {-5, 0}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 3u);
    verify_sorted_by_node_id(*result);

    EXPECT_EQ(degrees[-10].out_degree, 1);
    EXPECT_EQ(degrees[-10].in_degree, 0);
    EXPECT_EQ(degrees[-5].in_degree, 1);
    EXPECT_EQ(degrees[-5].out_degree, 1);
    EXPECT_EQ(degrees[0].in_degree, 1);
    EXPECT_EQ(degrees[0].out_degree, 0);
}

TEST_F(QA_GDB517_DegreeCentrality, Topology_MixedPositiveNegativeIds) {
    build_graph("knows", {{-1, 0}, {0, 1}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value());
    verify_sorted_by_node_id(*result);

    auto degrees = to_degree_map(*result);
    // Node 0 is the bridge: in=1, out=1
    EXPECT_EQ(degrees[0].in_degree, 1);
    EXPECT_EQ(degrees[0].out_degree, 1);
    EXPECT_EQ(degrees[0].degree, 2);
}

// ============================================================================
// Output ordering & determinism
// ============================================================================

TEST_F(QA_GDB517_DegreeCentrality, OutputSortedByNodeId) {
    build_graph("knows", {{5, 3}, {3, 1}, {4, 2}, {7, 6}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value());
    verify_sorted_by_node_id(*result);
}

TEST_F(QA_GDB517_DegreeCentrality, DeterministicOutput) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {1, 3}});

    auto result1 = run("knows");
    auto result2 = run("knows");
    ASSERT_TRUE(result1.has_value());
    ASSERT_TRUE(result2.has_value());

    ASSERT_EQ(result1->size(), result2->size());
    for (size_t i = 0; i < result1->size(); ++i) {
        for (size_t j = 0; j < 5; ++j) {
            if (j < 4) {
                EXPECT_EQ(std::get<int64_t>((*result1)[i].values[j].data()),
                          std::get<int64_t>((*result2)[i].values[j].data()))
                    << "row " << i << " col " << j;
            } else {
                EXPECT_DOUBLE_EQ(std::get<double>((*result1)[i].values[j].data()),
                                 std::get<double>((*result2)[i].values[j].data()))
                    << "row " << i << " col " << j;
            }
        }
    }
}

// ============================================================================
// Numerical stability
// ============================================================================

TEST_F(QA_GDB517_DegreeCentrality, NumericalStability_AllNormalizedFinite) {
    // Dense graph: verify no NaN or Inf in normalized_degree.
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < 20; ++i) {
        for (int64_t j = 0; j < 20; ++j) {
            if (i != j) {
                edges.push_back({i, j});
            }
        }
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    verify_normalized_finite(degrees);

    // Complete K20: each node has in=19, out=19, total=38
    // normalized = 38 / 19 = 2.0
    for (const auto& [node, info] : degrees) {
        EXPECT_DOUBLE_EQ(info.normalized_degree, 2.0) << "node " << node;
    }
}

TEST_F(QA_GDB517_DegreeCentrality, NumericalStability_NormalizedNonNegative) {
    // All normalized degrees should be >= 0.0.
    build_graph("knows", {{1, 2}, {2, 3}, {3, 4}});

    for (const auto& dir : {"in", "out", "all"}) {
        auto result = run("knows", dir);
        ASSERT_TRUE(result.has_value()) << "direction=" << dir;

        auto degrees = to_degree_map(*result);
        for (const auto& [node, info] : degrees) {
            EXPECT_GE(info.normalized_degree, 0.0)
                << "node " << node << " direction=" << dir;
        }
    }
}

// ============================================================================
// Stress tests
// ============================================================================

TEST_F(QA_GDB517_DegreeCentrality, Stress_RingGraph500) {
    // Directed ring of 500 nodes. All should have equal degree.
    constexpr int64_t N = 500;
    std::vector<std::pair<int64_t, int64_t>> edges;
    edges.reserve(N);
    for (int64_t i = 0; i < N; ++i) {
        edges.push_back({i, (i + 1) % N});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), static_cast<size_t>(N));

    for (const auto& [node, info] : degrees) {
        EXPECT_EQ(info.in_degree, 1) << "node " << node;
        EXPECT_EQ(info.out_degree, 1) << "node " << node;
        EXPECT_EQ(info.degree, 2) << "node " << node;
        EXPECT_DOUBLE_EQ(info.normalized_degree, 2.0 / (N - 1.0)) << "node " << node;
    }
    verify_sorted_by_node_id(*result);
}

TEST_F(QA_GDB517_DegreeCentrality, Stress_LinearChain200) {
    // Linear chain of 200 nodes.
    constexpr int64_t N = 200;
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < N - 1; ++i) {
        edges.push_back({i, i + 1});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), static_cast<size_t>(N));
    verify_normalized_finite(degrees);

    // Endpoints: degree=1, interior: degree=2
    EXPECT_EQ(degrees[0].degree, 1);
    EXPECT_EQ(degrees[N - 1].degree, 1);
    for (int64_t i = 1; i < N - 1; ++i) {
        EXPECT_EQ(degrees[i].degree, 2) << "interior node " << i;
    }
}

TEST_F(QA_GDB517_DegreeCentrality, Stress_HubAndSpoke300) {
    // Hub node 0 with 300 outgoing edges.
    constexpr int64_t SPOKES = 300;
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 1; i <= SPOKES; ++i) {
        edges.push_back({0, i});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), static_cast<size_t>(SPOKES + 1));

    // Hub: out=300, in=0, total=300
    EXPECT_EQ(degrees[0].out_degree, SPOKES);
    EXPECT_EQ(degrees[0].in_degree, 0);
    EXPECT_EQ(degrees[0].degree, SPOKES);

    // Normalized: 300 / 300 = 1.0
    EXPECT_DOUBLE_EQ(degrees[0].normalized_degree, 1.0);

    // Spokes: out=0, in=1, total=1
    for (int64_t i = 1; i <= SPOKES; ++i) {
        EXPECT_EQ(degrees[i].out_degree, 0);
        EXPECT_EQ(degrees[i].in_degree, 1);
        EXPECT_EQ(degrees[i].degree, 1);
    }
}

TEST_F(QA_GDB517_DegreeCentrality, Stress_ManyDisconnectedPairs) {
    // 200 disconnected pairs: (0->1), (2->3), ..., (398->399)
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < 400; i += 2) {
        edges.push_back({i, i + 1});
    }
    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto degrees = to_degree_map(*result);
    EXPECT_EQ(degrees.size(), 400u);
    verify_normalized_finite(degrees);

    for (const auto& [node, info] : degrees) {
        EXPECT_EQ(info.degree, 1) << "node " << node;
    }
}

TEST_F(QA_GDB517_DegreeCentrality, Stress_AllDirectionsOnLargeGraph) {
    // Run all three directions on a 100-node chain and verify consistency.
    constexpr int64_t N = 100;
    std::vector<std::pair<int64_t, int64_t>> edges;
    for (int64_t i = 0; i < N - 1; ++i) {
        edges.push_back({i, i + 1});
    }
    build_graph("knows", edges);

    auto r_all = run("knows", "all");
    auto r_in = run("knows", "in");
    auto r_out = run("knows", "out");
    ASSERT_TRUE(r_all.has_value());
    ASSERT_TRUE(r_in.has_value());
    ASSERT_TRUE(r_out.has_value());

    auto d_all = to_degree_map(*r_all);
    auto d_in = to_degree_map(*r_in);
    auto d_out = to_degree_map(*r_out);

    for (int64_t i = 0; i < N; ++i) {
        // in_degree and out_degree columns are always the same
        EXPECT_EQ(d_all[i].in_degree, d_in[i].in_degree);
        EXPECT_EQ(d_all[i].out_degree, d_out[i].out_degree);

        // direction affects degree column
        EXPECT_EQ(d_in[i].degree, d_in[i].in_degree) << "node " << i;
        EXPECT_EQ(d_out[i].degree, d_out[i].out_degree) << "node " << i;
        EXPECT_EQ(d_all[i].degree, d_all[i].in_degree + d_all[i].out_degree) << "node " << i;
    }
}

} // namespace
} // namespace sixseven
