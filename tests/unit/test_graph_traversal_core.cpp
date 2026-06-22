#include "sixseven/common/value.h"
#include "sixseven/executor/graph_traversal_core.h"

#include <gtest/gtest.h>

#include <unordered_set>

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// pk_to_int64
// ---------------------------------------------------------------------------

TEST(GraphTraversalCore, PkToInt64FromInt64) {
    Value v(int64_t{42});
    auto r = pk_to_int64(v);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, int64_t{42});
}

TEST(GraphTraversalCore, PkToInt64FromInt32) {
    Value v(int32_t{7});
    auto r = pk_to_int64(v);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, int64_t{7});
}

TEST(GraphTraversalCore, PkToInt64NegativeInt32) {
    Value v(int32_t{-1});
    auto r = pk_to_int64(v);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, int64_t{-1});
}

TEST(GraphTraversalCore, PkToInt64NullReturnsError) {
    Value v; // null
    auto r = pk_to_int64(v);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(GraphTraversalCore, PkToInt64StringReturnsError) {
    Value v(std::string{"hello"});
    auto r = pk_to_int64(v);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// NodeId / NodeIdHash
// ---------------------------------------------------------------------------

TEST(GraphTraversalCore, NodeIdEqualitySameTableSamePk) {
    NodeId a{1, Value(int64_t{10})};
    NodeId b{1, Value(int64_t{10})};
    EXPECT_EQ(a, b);
}

TEST(GraphTraversalCore, NodeIdInequalityDifferentTable) {
    // C6/C7: two nodes from different tables with the same PK must NOT collide.
    NodeId a{1, Value(int64_t{1})};
    NodeId b{2, Value(int64_t{1})};
    EXPECT_NE(a, b);
}

TEST(GraphTraversalCore, NodeIdInequalityDifferentPk) {
    NodeId a{1, Value(int64_t{1})};
    NodeId b{1, Value(int64_t{2})};
    EXPECT_NE(a, b);
}

TEST(GraphTraversalCore, NodeIdHashSameNodesProduceSameHash) {
    NodeIdHash h;
    NodeId a{3, Value(int64_t{99})};
    NodeId b{3, Value(int64_t{99})};
    EXPECT_EQ(h(a), h(b));
}

TEST(GraphTraversalCore, NodeIdHashDifferentTablesDoNotCollide) {
    // Different tables with the same PK should (with high probability) hash
    // differently — the test verifies that table_id is included in the hash.
    NodeIdHash h;
    NodeId a{1, Value(int64_t{1})};
    NodeId b{2, Value(int64_t{1})};
    // Not a strict requirement (hash collisions can happen), but for these
    // small integer inputs the Fibonacci mixing should produce distinct values.
    EXPECT_NE(h(a), h(b));
}

TEST(GraphTraversalCore, NodeIdUsableInUnorderedSet) {
    std::unordered_set<NodeId, NodeIdHash> s;
    NodeId a{1, Value(int64_t{1})};
    NodeId b{2, Value(int64_t{1})}; // same PK, different table
    NodeId c{1, Value(int64_t{1})}; // duplicate of a

    s.insert(a);
    s.insert(b);
    s.insert(c); // should not increase size — duplicate of a

    EXPECT_EQ(s.size(), 2u); // a and b are distinct; c == a is deduplicated
    EXPECT_TRUE(s.count(a) > 0);
    EXPECT_TRUE(s.count(b) > 0);
}

// ---------------------------------------------------------------------------
// reconstruct_path
// ---------------------------------------------------------------------------

// Helper: build a linear parent chain  start(1) → 2 → 3 → 4.
// parent_map: 2→{1, edge10}, 3→{2, edge11}, 4→{3, edge12}.
// reconstruct_path(4, depth=3) should produce path: 1→2→3→4
// with outgoing edges: step[0].edge_id=edge10, step[1].edge_id=edge11,
// step[2].edge_id=edge12, step[3].edge_id=-1.
ParentMap make_linear_chain() {
    ParentMap pm;
    pm[Value(int64_t{2})] = ParentInfo{Value(int64_t{1}), 10};
    pm[Value(int64_t{3})] = ParentInfo{Value(int64_t{2}), 11};
    pm[Value(int64_t{4})] = ParentInfo{Value(int64_t{3}), 12};
    return pm;
}

TEST(GraphTraversalCore, ReconstructPathLinearChain) {
    auto pm = make_linear_chain();
    auto result = reconstruct_path(Value(int64_t{4}), 3, pm);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto& steps = result->steps;
    ASSERT_EQ(steps.size(), 4u);
    EXPECT_EQ(steps[0].node_pk, int64_t{1});
    EXPECT_EQ(steps[0].edge_id, int64_t{10}); // outgoing: edge to step[1]
    EXPECT_EQ(steps[1].node_pk, int64_t{2});
    EXPECT_EQ(steps[1].edge_id, int64_t{11}); // outgoing: edge to step[2]
    EXPECT_EQ(steps[2].node_pk, int64_t{3});
    EXPECT_EQ(steps[2].edge_id, int64_t{12}); // outgoing: edge to step[3]
    EXPECT_EQ(steps[3].node_pk, int64_t{4});
    EXPECT_EQ(steps[3].edge_id, int64_t{-1}); // terminal step: no outgoing
}

TEST(GraphTraversalCore, ReconstructPathDepthZeroSelf) {
    // Target is start node — depth 0. Should produce a single-step path.
    ParentMap pm; // empty
    auto result = reconstruct_path(Value(int64_t{1}), 0, pm);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->steps.size(), 1u);
    EXPECT_EQ(result->steps[0].node_pk, int64_t{1});
    EXPECT_EQ(result->steps[0].edge_id, int64_t{-1});
}

TEST(GraphTraversalCore, ReconstructPathSingleHop) {
    ParentMap pm;
    pm[Value(int64_t{2})] = ParentInfo{Value(int64_t{1}), 5};
    auto result = reconstruct_path(Value(int64_t{2}), 1, pm);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->steps.size(), 2u);
    EXPECT_EQ(result->steps[0].node_pk, int64_t{1});
    EXPECT_EQ(result->steps[0].edge_id, int64_t{5});
    EXPECT_EQ(result->steps[1].node_pk, int64_t{2});
    EXPECT_EQ(result->steps[1].edge_id, int64_t{-1});
}

// H9/H11 (GDB-694): depth guard prevents infinite loop when the parent map
// contains a cross-table PK collision that makes the start node appear to
// have a parent of itself.
TEST(GraphTraversalCore, ReconstructPathDepthGuardPreventsLoop) {
    // Construct a parent map where node 1 appears to have itself as parent
    // (simulating a cross-table PK collision).
    ParentMap pm;
    pm[Value(int64_t{1})] = ParentInfo{Value(int64_t{1}), 99}; // self-loop
    pm[Value(int64_t{2})] = ParentInfo{Value(int64_t{1}), 10};

    // Reconstruct path to node 2 at depth 1.  Without the depth guard the
    // walk would try to follow 1→1→… forever.  The depth guard stops after 1
    // hop (remaining starts at 1 and hits 0 at node 1, terminating).
    auto result = reconstruct_path(Value(int64_t{2}), 1, pm);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->steps.size(), 2u);
    EXPECT_EQ(result->steps[0].node_pk, int64_t{1});
    EXPECT_EQ(result->steps[1].node_pk, int64_t{2});
}

// H11 (GDB-694): the max_steps guard catches an actual cycle in the parent
// map (not just a cross-table collision) and returns INTERNAL_ERROR.
TEST(GraphTraversalCore, ReconstructPathMaxStepsGuardCycleError) {
    // Build a genuine cycle: 2→3, 3→2.  Neither is the start node.
    ParentMap pm;
    pm[Value(int64_t{2})] = ParentInfo{Value(int64_t{3}), 10};
    pm[Value(int64_t{3})] = ParentInfo{Value(int64_t{2}), 11};

    // Attempt to reconstruct path to node 2 at depth 5 (enough to exhaust
    // the max_steps bound = pm.size()+1 = 3).
    auto result = reconstruct_path(Value(int64_t{2}), 5, pm);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INTERNAL_ERROR);
}

// Verify that reconstruct_path handles int32_t PKs (not just int64_t).
TEST(GraphTraversalCore, ReconstructPathInt32Pks) {
    ParentMap pm;
    pm[Value(int32_t{2})] = ParentInfo{Value(int32_t{1}), 7};
    auto result = reconstruct_path(Value(int32_t{2}), 1, pm);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->steps.size(), 2u);
    // pk_to_int64 widens int32 → int64.
    EXPECT_EQ(result->steps[0].node_pk, int64_t{1});
    EXPECT_EQ(result->steps[1].node_pk, int64_t{2});
}

} // namespace
} // namespace sixseven
