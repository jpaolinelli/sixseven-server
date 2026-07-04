// QA regression tests for GDB-1195.
//
// GDB-1195 replaced the tautological TriangleCountTest.CountsAreNonNegative
// (which only asserted EXPECT_GE(c, 0) and could not fail for an all-zeros or
// otherwise broken implementation) with TriangleCountTest.CountsMatchKnownTriangle,
// which asserts exact per-node counts and the total triangle count on a
// deterministic graph.
//
// This file adversarially probes the triangle-counting algorithm itself
// (src/graph/triangle_count.cpp) with topologies not exercised by the
// existing dev tests: duplicate edges, mixed directed/undirected edges,
// multiple disconnected triangle clusters, larger asymmetric graphs, and
// the sum-of-counts == 3 * total_triangles invariant. The goal is to confirm
// the strengthened test's discriminating power generalizes -- i.e. that the
// underlying algorithm is genuinely correct, not just correct on the one
// fixture graph asserted in the dev test.

#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/triangle_count.h"

#include <gtest/gtest.h>

#include <unordered_map>
#include <unordered_set>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

TableSchema qa_gdb1195_make_table_schema(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", TypeId::INT64, false, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

Value qa_gdb1195_pk(int64_t v) {
    return Value(v);
}

std::unordered_map<int64_t, int64_t> qa_gdb1195_to_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, int64_t> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 2u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto count = std::get<int64_t>(row.values[1].data());
        result[node_id] = count;
    }
    return result;
}

int64_t qa_gdb1195_total(const std::unordered_map<int64_t, int64_t>& counts) {
    int64_t sum = 0;
    for (const auto& [_, c] : counts) sum += c;
    return sum / 3;
}

// Brute-force O(V^3) triangle counter used as an independent oracle to
// cross-check the production degree-ordered algorithm on adversarial graphs.
std::unordered_map<int64_t, int64_t> qa_gdb1195_brute_force(
    const std::vector<std::pair<int64_t, int64_t>>& edges) {
    std::unordered_map<int64_t, std::unordered_set<int64_t>> adj;
    std::unordered_set<int64_t> nodes;
    for (auto [a, b] : edges) {
        nodes.insert(a);
        nodes.insert(b);
        if (a == b) continue; // ignore self-loops
        adj[a].insert(b);
        adj[b].insert(a);
    }
    std::vector<int64_t> sorted_nodes(nodes.begin(), nodes.end());
    std::sort(sorted_nodes.begin(), sorted_nodes.end());

    std::unordered_map<int64_t, int64_t> counts;
    for (auto n : sorted_nodes) counts[n] = 0;

    for (size_t i = 0; i < sorted_nodes.size(); ++i) {
        for (size_t j = i + 1; j < sorted_nodes.size(); ++j) {
            for (size_t k = j + 1; k < sorted_nodes.size(); ++k) {
                int64_t u = sorted_nodes[i], v = sorted_nodes[j], w = sorted_nodes[k];
                if (adj[u].count(v) && adj[v].count(w) && adj[u].count(w)) {
                    ++counts[u];
                    ++counts[v];
                    ++counts[w];
                }
            }
        }
    }
    return counts;
}

} // namespace

class QaGdb1195TriangleCountTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        auto t = catalog_.create_table(default_database_id, qa_gdb1195_make_table_schema("nodes"));
        ASSERT_TRUE(t.has_value()) << t.error().message;
        table_id_ = *t;
    }

    void build_graph(const std::string& edge_type,
                     const std::vector<std::pair<int64_t, int64_t>>& edges) {
        auto et = engine_.create_edge_type(
            default_database_id, edge_type, table_id_, table_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(et.has_value()) << et.error().message;
        for (auto [src, tgt] : edges) {
            auto link = engine_.link(default_database_id, edge_type, qa_gdb1195_pk(src), qa_gdb1195_pk(tgt));
            ASSERT_TRUE(link.has_value()) << link.error().message;
        }
    }

    Result<std::vector<AlgorithmRow>> run(const std::string& edge_type) {
        AlgorithmContext ctx{engine_, default_database_id, edge_type, {}};
        return triangle_count_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ---------------------------------------------------------------------------
// 1. Discrimination check: the strengthened dev-test assertions must fail
//    for a hypothetical "return all zeros" implementation. We can't swap in
//    a broken implementation, but we replicate exactly what GDB-1195's
//    exact assertions require and confirm the real production values differ
//    from the all-zero / arbitrary-nonnegative-garbage baseline the old test
//    would have silently accepted.
// ---------------------------------------------------------------------------

TEST_F(QaGdb1195TriangleCountTest, KnownTriangleCountsAreNonZeroWhereExpected) {
    build_graph("knows", {{1, 2}, {2, 3}, {3, 1}, {1, 4}, {4, 5}});

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto counts = qa_gdb1195_to_map(*result);

    // An all-zeros implementation would pass the old EXPECT_GE(c, 0) test but
    // must fail here: nodes 1,2,3 must be strictly positive.
    EXPECT_GT(counts[1], 0);
    EXPECT_GT(counts[2], 0);
    EXPECT_GT(counts[3], 0);
    EXPECT_EQ(counts[4], 0);
    EXPECT_EQ(counts[5], 0);
    EXPECT_EQ(counts[1], 1);
    EXPECT_EQ(counts[2], 1);
    EXPECT_EQ(counts[3], 1);
}

// ---------------------------------------------------------------------------
// 2. Duplicate edges must not inflate the count (adjacency is a set).
// ---------------------------------------------------------------------------

TEST_F(QaGdb1195TriangleCountTest, DuplicateEdgesDoNotInflateCount) {
    // Triangle {1,2,3} with the same edges linked multiple times, plus
    // duplicate reverse edges.
    build_graph("knows",
                {
                    {1, 2}, {1, 2}, {1, 2},
                    {2, 1}, {2, 1},
                    {2, 3}, {2, 3},
                    {3, 2},
                    {3, 1}, {3, 1}, {3, 1},
                    {1, 3},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto counts = qa_gdb1195_to_map(*result);

    EXPECT_EQ(counts[1], 1);
    EXPECT_EQ(counts[2], 1);
    EXPECT_EQ(counts[3], 1);
    EXPECT_EQ(qa_gdb1195_total(counts), 1);
}

// ---------------------------------------------------------------------------
// 3. Multiple disconnected triangle clusters -- counts must not leak across
//    components, and per-component correctness must hold independently.
// ---------------------------------------------------------------------------

TEST_F(QaGdb1195TriangleCountTest, DisconnectedTriangleClusters) {
    // Component A: triangle {1,2,3}. Component B: triangle {10,11,12}.
    // Component C: isolated edge {20,21} (no triangle).
    build_graph("knows",
                {
                    {1, 2}, {2, 1}, {2, 3}, {3, 2}, {3, 1}, {1, 3},
                    {10, 11}, {11, 10}, {11, 12}, {12, 11}, {12, 10}, {10, 12},
                    {20, 21}, {21, 20},
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto counts = qa_gdb1195_to_map(*result);

    EXPECT_EQ(counts.size(), 8u);
    for (int64_t n : {1, 2, 3, 10, 11, 12}) {
        EXPECT_EQ(counts[n], 1) << "node " << n;
    }
    EXPECT_EQ(counts[20], 0);
    EXPECT_EQ(counts[21], 0);
    EXPECT_EQ(qa_gdb1195_total(counts), 2);
}

// ---------------------------------------------------------------------------
// 4. Larger asymmetric graph cross-checked against an independent brute-force
//    O(V^3) oracle. This is the strongest adversarial check: it does not
//    hand-derive expected counts, it recomputes them independently.
// ---------------------------------------------------------------------------

TEST_F(QaGdb1195TriangleCountTest, LargerAsymmetricGraphMatchesBruteForceOracle) {
    // A mix of overlapping triangles, a pendant, and a longer cycle with
    // chords, built from directed edges only in one direction to also
    // exercise directed-edge handling.
    std::vector<std::pair<int64_t, int64_t>> edges = {
        {1, 2}, {2, 3}, {3, 1},      // triangle {1,2,3}
        {3, 4}, {4, 5}, {5, 3},      // triangle {3,4,5} sharing node 3
        {2, 4},                     // chord creating triangle {2,3,4} too
        {5, 6}, {6, 7}, {7, 8}, {8, 5}, // 4-cycle, no triangle
        {8, 9},                     // pendant off the cycle
        {9, 10}, {10, 9},           // isolated-ish edge
    };

    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto counts = qa_gdb1195_to_map(*result);

    auto oracle = qa_gdb1195_brute_force(edges);

    ASSERT_EQ(counts.size(), oracle.size());
    for (const auto& [node, expected] : oracle) {
        EXPECT_EQ(counts[node], expected) << "mismatch at node " << node;
    }
    EXPECT_EQ(qa_gdb1195_total(counts), qa_gdb1195_total(oracle));
}

// ---------------------------------------------------------------------------
// 5. Invariant check: sum of per-node counts must always be exactly
//    3 * total_triangles (each triangle credited to all 3 members).
// ---------------------------------------------------------------------------

TEST_F(QaGdb1195TriangleCountTest, SumOfCountsIsThreeTimesTotalTriangles) {
    build_graph("knows",
                {
                    {1, 2}, {1, 3}, {1, 4}, {2, 1}, {2, 3}, {2, 4},
                    {3, 1}, {3, 2}, {3, 4}, {4, 1}, {4, 2}, {4, 3}, // K4
                    {5, 6}, {6, 5}, // dangling edge, no triangle
                });

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto counts = qa_gdb1195_to_map(*result);

    int64_t sum = 0;
    for (const auto& [_, c] : counts) sum += c;

    // K4 has 4 triangles => sum should be 12, total_triangles == 4.
    EXPECT_EQ(sum, 12);
    EXPECT_EQ(sum % 3, 0) << "sum of per-node counts must be divisible by 3";
    EXPECT_EQ(qa_gdb1195_total(counts), 4);
}

// ---------------------------------------------------------------------------
// 6. Mixed directed edges (only some reversed) still yield undirected
//    triangle semantics consistently, matching the brute-force oracle.
// ---------------------------------------------------------------------------

TEST_F(QaGdb1195TriangleCountTest, PartiallyDirectedTriangleMatchesOracle) {
    // Only one direction present for each edge except one reversed pair;
    // the algorithm treats the graph as undirected so this should still
    // form one triangle.
    std::vector<std::pair<int64_t, int64_t>> edges = {
        {1, 2}, {2, 3}, {1, 3}, {3, 1}, // duplicate + reverse on one edge
    };

    build_graph("knows", edges);

    auto result = run("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto counts = qa_gdb1195_to_map(*result);
    auto oracle = qa_gdb1195_brute_force(edges);

    for (const auto& [node, expected] : oracle) {
        EXPECT_EQ(counts[node], expected) << "node " << node;
    }
    EXPECT_EQ(qa_gdb1195_total(counts), 1);
}
