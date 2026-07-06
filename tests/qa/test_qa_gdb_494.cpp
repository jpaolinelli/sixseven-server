/// @file test_qa_gdb_494.cpp
/// QA adversarial tests for GDB-494: Fix edge-key collision in Louvain
/// community detection.
///
/// The bug: edge deduplication used a lossy int64_t key
///   key = (lo << 32) | (hi & 0xFFFFFFFF)
/// which caused collisions when distinct node IDs shared the same lower
/// 32 bits (e.g., node 1 vs node 2^32+1). The fix replaces this with a
/// collision-free std::set<std::pair<int64_t, int64_t>>.
///
/// Adversarial categories:
///   - Edge-key collision scenarios (the core bug)
///   - Large/boundary node IDs
///   - Multiple collision patterns in the same graph
///   - Correctness verification after deduplication
///   - Stress tests with many potentially-colliding edges

#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/louvain.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "test_qa_helpers.h"

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

/// Extract (node_id, community_id) pairs from algorithm result rows.
std::unordered_map<int64_t, int64_t> to_community_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, int64_t> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 2u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto community_id = std::get<int64_t>(row.values[1].data());
        result[node_id] = community_id;
    }
    return result;
}

/// Verify that community IDs are contiguous starting from 0.
void verify_contiguous_ids(const std::unordered_map<int64_t, int64_t>& communities) {
    if (communities.empty())
        return;
    std::unordered_set<int64_t> unique_ids;
    for (const auto& [_, comm] : communities) {
        unique_ids.insert(comm);
    }
    int64_t max_id = -1;
    for (int64_t id : unique_ids) {
        EXPECT_GE(id, 0) << "community IDs should be non-negative";
        max_id = std::max(max_id, id);
    }
    EXPECT_EQ(max_id, static_cast<int64_t>(unique_ids.size()) - 1)
        << "community IDs should be contiguous from 0";
}

/// Verify that nodes in the same group share a community ID, and nodes in
/// different groups have different community IDs.
void verify_communities(const std::unordered_map<int64_t, int64_t>& communities,
                        const std::vector<std::vector<int64_t>>& expected_groups) {
    for (const auto& group : expected_groups) {
        if (group.empty())
            continue;
        auto it = communities.find(group[0]);
        ASSERT_NE(it, communities.end()) << "node " << group[0] << " not found";
        int64_t expected_comm = it->second;
        for (int64_t n : group) {
            auto jt = communities.find(n);
            ASSERT_NE(jt, communities.end()) << "node " << n << " not found";
            EXPECT_EQ(jt->second, expected_comm)
                << "nodes " << group[0] << " and " << n << " should share community";
        }
    }
    for (size_t i = 0; i < expected_groups.size(); ++i) {
        for (size_t j = i + 1; j < expected_groups.size(); ++j) {
            if (expected_groups[i].empty() || expected_groups[j].empty())
                continue;
            auto ci = communities.at(expected_groups[i][0]);
            auto cj = communities.at(expected_groups[j][0]);
            EXPECT_NE(ci, cj) << "groups " << i << " and " << j << " should differ";
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class QA_GDB494 : public ::testing::Test {
protected:
    void SetUp() override {
        bootstrap_qa_catalog(catalog_);
        auto t = catalog_.create_table(default_database_id, make_table_schema("nodes"));
        ASSERT_TRUE(t.has_value()) << t.error().message;
        table_id_ = *t;
    }

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

    Result<std::vector<AlgorithmRow>> run_cd(const std::string& edge_type) {
        AlgorithmContext ctx{
            engine_,
            default_database_id,
            edge_type,
            {{"resolution", Value(1.0)}, {"max_iterations", Value(static_cast<int64_t>(10))}}};
        return community_detect_execute(ctx);
    }

    Result<std::vector<AlgorithmRow>>
    run_cd(const std::string& edge_type, double resolution, int64_t max_iterations) {
        AlgorithmContext ctx{
            engine_,
            default_database_id,
            edge_type,
            {{"resolution", Value(resolution)}, {"max_iterations", Value(max_iterations)}}};
        return community_detect_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

// ===========================================================================
// Core collision scenario: the exact bug pattern
// ===========================================================================

TEST_F(QA_GDB494, Collision_NodeIdDiffersOnlyInUpper32Bits) {
    // The original bug: edge (0, 1) and edge (0, 2^32+1) collide because
    // (2^32+1) & 0xFFFFFFFF == 1.
    // With the fix, both edges must be present.
    int64_t id_small = 1;
    int64_t id_large = (1LL << 32) + 1; // 4294967297

    build_graph("knows", {{0, id_small}, {0, id_large}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    // All three nodes must appear (bug would have dropped one edge).
    EXPECT_EQ(communities.size(), 3u);
    EXPECT_NE(communities.find(0), communities.end());
    EXPECT_NE(communities.find(id_small), communities.end());
    EXPECT_NE(communities.find(id_large), communities.end());

    // All connected through node 0 — single community.
    verify_communities(communities, {{0, id_small, id_large}});
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB494, Collision_MultipleHiValuesWithSameLower32) {
    // Multiple distinct nodes that share the same lower 32 bits.
    // All connected to hub node 0.
    int64_t base = 42;
    int64_t id_a = base;               // lower 32 bits = 42
    int64_t id_b = (1LL << 32) + base; // lower 32 bits = 42
    int64_t id_c = (2LL << 32) + base; // lower 32 bits = 42
    int64_t id_d = (3LL << 32) + base; // lower 32 bits = 42

    build_graph("knows", {{0, id_a}, {0, id_b}, {0, id_c}, {0, id_d}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    // 5 nodes: hub 0 + 4 colliding nodes.
    EXPECT_EQ(communities.size(), 5u);
    verify_communities(communities, {{0, id_a, id_b, id_c, id_d}});
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB494, Collision_SymmetricPairCollision) {
    // Edge (A, B) and edge (C, D) where (min(A,B), max(A,B)) would
    // collide with (min(C,D), max(C,D)) under the old scheme.
    // Both lo values are 10, hi values 100 and 2^32+100.
    int64_t hi_a = 100;
    int64_t hi_b = (1LL << 32) + 100;

    build_graph("knows", {{10, hi_a}, {10, hi_b}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    EXPECT_EQ(communities.size(), 3u);
    verify_communities(communities, {{10, hi_a, hi_b}});
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB494, Collision_BothLoAndHiCanCollide) {
    // More complex collision: different lo values but same lo lower 32 bits,
    // AND same hi lower 32 bits.
    // Edge (lo=5, hi=10) vs Edge (lo=2^32+5, hi=2^32+10)
    int64_t lo_a = 5;
    int64_t hi_a = 10;
    int64_t lo_b = (1LL << 32) + 5;
    int64_t hi_b = (1LL << 32) + 10;

    // Two disconnected edges that would collide under old scheme.
    build_graph("knows", {{lo_a, hi_a}, {lo_b, hi_b}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    // 4 nodes: two disconnected pairs.
    EXPECT_EQ(communities.size(), 4u);
    // Each pair should be in its own community.
    EXPECT_EQ(communities[lo_a], communities[hi_a]);
    EXPECT_EQ(communities[lo_b], communities[hi_b]);
    EXPECT_NE(communities[lo_a], communities[lo_b]);
    verify_contiguous_ids(communities);
}

// ===========================================================================
// Node ID boundary values near 2^32
// ===========================================================================

TEST_F(QA_GDB494, Boundary_ExactlyAt2Pow32) {
    // Node ID = 2^32 exactly.
    int64_t at_boundary = 1LL << 32; // 4294967296
    build_graph("knows", {{0, 1}, {0, at_boundary}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    EXPECT_EQ(communities.size(), 3u);
    verify_communities(communities, {{0, 1, at_boundary}});
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB494, Boundary_JustBelow2Pow32) {
    // Node ID = 2^32 - 1 = 4294967295 (max uint32).
    int64_t below = (1LL << 32) - 1;
    build_graph("knows", {{0, below}, {below, 1}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    EXPECT_EQ(communities.size(), 3u);
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB494, Boundary_JustAbove2Pow32) {
    // Node ID = 2^32 + 1.
    int64_t above = (1LL << 32) + 1;
    // Edge (1, above) where above & 0xFFFFFFFF == 1: direct collision with
    // a hypothetical edge (1, 1) which would be a self-loop.
    build_graph("knows", {{0, 1}, {0, above}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    EXPECT_EQ(communities.size(), 3u);
    EXPECT_NE(communities.find(above), communities.end());
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB494, Boundary_LargeNodeIdsNear2Pow48) {
    // Node IDs near 2^48 boundary.
    int64_t base = 1LL << 48;
    build_graph("knows", {{base, base + 1}, {base + 1, base + 2}, {base + 2, base}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    verify_communities(communities, {{base, base + 1, base + 2}});
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB494, Boundary_NodeIdsNearInt64Max) {
    // Node IDs near the maximum int64_t value.
    int64_t max_id = std::numeric_limits<int64_t>::max();
    int64_t near_max = max_id - 2;
    build_graph("knows", {{near_max, near_max + 1}, {near_max + 1, near_max + 2}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    EXPECT_EQ(communities.size(), 3u);
    verify_contiguous_ids(communities);
}

// ===========================================================================
// Mixed collision and non-collision edges in the same graph
// ===========================================================================

TEST_F(QA_GDB494, Mixed_CollidingAndNormalEdgesInSameGraph) {
    // Graph with edges that would collide under the old scheme mixed with
    // normal edges. Verifies deduplication doesn't interfere with normal
    // edge processing.
    //
    // Two separate cliques, one containing a colliding-ID node.
    int64_t id_collide = (1LL << 32) + 2; // lower 32 bits = 2

    // Clique A: 1-2-3 (normal IDs)
    // Clique B: id_collide-100-101 (one colliding ID)
    build_graph("knows",
                {
                    {1, 2},
                    {2, 3},
                    {3, 1},
                    {id_collide, 100},
                    {100, 101},
                    {101, id_collide},
                });

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    // 6 nodes total — two disconnected triangles.
    EXPECT_EQ(communities.size(), 6u);
    EXPECT_NE(communities.find(id_collide), communities.end());
    // Each clique forms its own community.
    verify_communities(communities, {{1, 2, 3}, {id_collide, 100, 101}});
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB494, Mixed_TwoCliquesOneWithCollision) {
    // Two cliques: one normal, one with colliding node IDs.
    // Clique A: 1-2-3 (normal)
    // Clique B: uses node IDs that would collide under old scheme
    int64_t b1 = 100;
    int64_t b2 = (1LL << 32) + 100; // collides with b1 in lower 32
    int64_t b3 = (2LL << 32) + 100; // collides with b1 in lower 32

    build_graph("knows",
                {
                    // Clique A
                    {1, 2},
                    {2, 3},
                    {3, 1},
                    // Clique B (all three pairs)
                    {b1, b2},
                    {b2, b3},
                    {b3, b1},
                });

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    EXPECT_EQ(communities.size(), 6u);
    verify_communities(communities, {{1, 2, 3}, {b1, b2, b3}});
    verify_contiguous_ids(communities);
}

// ===========================================================================
// Reverse-direction edges with colliding IDs
// ===========================================================================

TEST_F(QA_GDB494, Collision_ReverseEdgesWithCollidingIds) {
    // Both directions of an edge with colliding IDs.
    int64_t big = (1LL << 32) + 5;
    // Edges: (5, big) and (big, 5) — same undirected edge.
    // Also: (5, 10) to form a connected triple.
    build_graph("knows", {{5, big}, {big, 5}, {5, 10}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    EXPECT_EQ(communities.size(), 3u);
    // All connected through node 5.
    verify_communities(communities, {{5, big, 10}});
    verify_contiguous_ids(communities);
}

// ===========================================================================
// Verify deduplication correctness: same undirected edge added multiple ways
// ===========================================================================

TEST_F(QA_GDB494, Dedup_SameEdgeBothDirectionsCountedOnce) {
    // Edge (1,2) and (2,1) — should be one undirected edge.
    // Edge (1,3) alone — one undirected edge.
    // Total weight from node 1: should be 2.0 (degree to 2 and to 3).
    build_graph("knows", {{1, 2}, {2, 1}, {1, 3}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    EXPECT_EQ(communities.size(), 3u);
    verify_communities(communities, {{1, 2, 3}});
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB494, Dedup_DuplicateEdgesDoNotDoubleWeight) {
    // Two disconnected pairs: (1,2) x3 duplicates and (3,4).
    // Even with duplicates, the deduplication should keep only one edge per pair.
    build_graph("knows", {{1, 2}, {1, 2}, {1, 2}, {3, 4}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    EXPECT_EQ(communities.size(), 4u);
    EXPECT_EQ(communities[1], communities[2]);
    EXPECT_EQ(communities[3], communities[4]);
    EXPECT_NE(communities[1], communities[3]);
    verify_contiguous_ids(communities);
}

// ===========================================================================
// Self-loops with colliding IDs (should be ignored)
// ===========================================================================

TEST_F(QA_GDB494, SelfLoop_CollidingIdSelfLoopIgnored) {
    // Node IDs that are identical (self-loop) should be ignored.
    // Also include a real edge to verify the graph works.
    int64_t big = (1LL << 32) + 1;
    build_graph("knows", {{big, big}, {big, 0}, {0, 1}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    EXPECT_EQ(communities.size(), 3u);
    verify_communities(communities, {{big, 0, 1}});
    verify_contiguous_ids(communities);
}

// ===========================================================================
// Output ordering with large IDs
// ===========================================================================

TEST_F(QA_GDB494, OutputSorted_WithLargeIds) {
    // Verify output is sorted by node_id even with very large IDs.
    int64_t big = (1LL << 32) + 50;
    build_graph("knows", {{big, 1}, {1, 0}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    ASSERT_EQ(result->size(), 3u);
    for (size_t i = 1; i < result->size(); ++i) {
        auto prev = std::get<int64_t>((*result)[i - 1].values[0].data());
        auto curr = std::get<int64_t>((*result)[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
    verify_contiguous_ids(to_community_map(*result));
}

// ===========================================================================
// Stress: many edges with potentially colliding keys
// ===========================================================================

TEST_F(QA_GDB494, Stress_ManyCollidingEdges) {
    // Hub node 0 connected to 20 nodes that all share the same lower 32 bits.
    // Under the old scheme, all 20 edges would hash to the same key as edge
    // (0, 7), causing 19 edges to be silently dropped.
    std::vector<std::pair<int64_t, int64_t>> edges;
    std::vector<int64_t> all_nodes = {0};

    for (int i = 0; i < 20; ++i) {
        int64_t node = (static_cast<int64_t>(i) << 32) + 7;
        edges.push_back({0, node});
        all_nodes.push_back(node);
    }

    build_graph("knows", edges);

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    // All 21 nodes must appear.
    EXPECT_EQ(communities.size(), 21u);
    // All connected through hub 0 — should be one community.
    verify_communities(communities, {all_nodes});
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB494, Stress_TwoCliquesWithCollidingInterCluster) {
    // Two K5 cliques where the bridge edge nodes have colliding lower 32 bits.
    int64_t offset = 1LL << 32;
    std::vector<std::pair<int64_t, int64_t>> edges;

    // Clique A: nodes 1..5
    for (int64_t i = 1; i <= 5; ++i) {
        for (int64_t j = i + 1; j <= 5; ++j) {
            edges.push_back({i, j});
        }
    }
    // Clique B: nodes offset+1 .. offset+5
    // Lower 32 bits match clique A nodes (1..5).
    for (int64_t i = offset + 1; i <= offset + 5; ++i) {
        for (int64_t j = i + 1; j <= offset + 5; ++j) {
            edges.push_back({i, j});
        }
    }
    // Bridge: node 5 to node offset+1 (colliding lower 32 bits with 5 and 1)
    edges.push_back({5, offset + 1});

    build_graph("knows", edges);

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    EXPECT_EQ(communities.size(), 10u);

    // Clique A internal consistency.
    for (int64_t i = 2; i <= 5; ++i) {
        EXPECT_EQ(communities[1], communities[i]) << "node " << i << " in clique A";
    }
    // Clique B internal consistency.
    for (int64_t i = offset + 2; i <= offset + 5; ++i) {
        EXPECT_EQ(communities[offset + 1], communities[i]) << "node " << i << " in clique B";
    }
    // Two cliques are separate.
    EXPECT_NE(communities[1], communities[offset + 1]);
    verify_contiguous_ids(communities);
}

// ===========================================================================
// Negative node IDs that collide in lower 32 bits
// ===========================================================================

TEST_F(QA_GDB494, Collision_NegativeIdsSameLower32) {
    // Negative IDs: -1 and -(2^32 + 1).
    // In two's complement, these have different bit patterns but could collide
    // in a truncated scheme.
    int64_t neg_a = -1;
    int64_t neg_b = -(1LL << 32) - 1;
    build_graph("knows", {{0, neg_a}, {0, neg_b}});

    auto result = run_cd("knows");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    EXPECT_EQ(communities.size(), 3u);
    verify_communities(communities, {{0, neg_a, neg_b}});
    verify_contiguous_ids(communities);
}

// ===========================================================================
// Resolution parameter still works correctly with fixed dedup
// ===========================================================================

TEST_F(QA_GDB494, Resolution_HighResWithCollidingIds) {
    // Very high resolution with colliding IDs — each node stays isolated.
    int64_t big = (1LL << 32) + 1;
    build_graph("knows", {{0, 1}, {0, big}, {1, big}});

    auto result = run_cd("knows", 100.0, 10);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    EXPECT_EQ(communities.size(), 3u);
    verify_contiguous_ids(communities);
}

TEST_F(QA_GDB494, Resolution_ZeroWithCollidingIds) {
    // Zero resolution with colliding IDs — everything merges.
    int64_t big = (1LL << 32) + 1;
    build_graph("knows", {{0, 1}, {0, big}, {1, big}});

    auto result = run_cd("knows", 0.0, 10);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto communities = to_community_map(*result);

    EXPECT_EQ(communities.size(), 3u);
    verify_communities(communities, {{0, 1, big}});
    verify_contiguous_ids(communities);
}

// ===========================================================================
// Determinism: same result on repeated runs with colliding IDs
// ===========================================================================

TEST_F(QA_GDB494, Determinism_RepeatedRunsWithCollidingIds) {
    int64_t big = (1LL << 32) + 3;
    build_graph("knows", {{0, 1}, {0, big}, {1, 2}, {2, big}});

    auto r1 = run_cd("knows");
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    auto r2 = run_cd("knows");
    ASSERT_TRUE(r2.has_value()) << r2.error().message;

    ASSERT_EQ(r1->size(), r2->size());
    for (size_t i = 0; i < r1->size(); ++i) {
        for (size_t j = 0; j < (*r1)[i].values.size(); ++j) {
            EXPECT_EQ(std::get<int64_t>((*r1)[i].values[j].data()),
                      std::get<int64_t>((*r2)[i].values[j].data()))
                << "row " << i << " col " << j << " differs between runs";
        }
    }
}
