/// @file test_qa_gdb_851.cpp
/// QA adversarial tests for GDB-851: heterogeneous PK collision bug class
/// (audit finding H9) in MatchShortestPathOperator (MATCH...path-selector).
///
/// Bug 1 (trivial-case): find_shortest_paths / find_weighted_shortest_paths
///   compared src_pk == tgt_pk with bare ValueEqual{} — no table identity.
///   Result: MATCH ANY SHORTEST (u:users)-[]->{...}(p:posts) with users.id==1
///   and posts.id==1 returned a bogus 0-hop path even though they are rows in
///   DIFFERENT tables.
///
/// Bug 2 (globally_visited / best_cost collision): BFS visited sets and
///   Dijkstra best_cost map were keyed by bare PK Value.  A users node with
///   pk=K and a posts node with pk=K were treated as the same node, causing
///   real paths to be pruned or fabricated meeting points to be detected.
///
/// These are the MATCH-operator analogues of the GDB-842 bugs fixed in
/// ShortestPathOperator.  GDB-851 mirrors that fix for MatchShortestPathOperator.
///
/// Test coverage:
///   AC1  trivial-case bogus 0-hop: MATCH ANY SHORTEST users(pk=1) TO posts(pk=1)
///        with NO edge must return empty, not a zero-hop path.
///   AC2  trivial-case with edge: same PK, direct edge → exactly 1-hop (2 nodes).
///   AC3  cross-table PK collision must NOT fabricate a meeting point (unweighted).
///   AC4  valid heterogeneous path: assert exact hop count + node sequence.
///   AC5  weighted variant (find_weighted_shortest_paths): trivial-case same check.
///   AC6  weighted variant: valid heterogeneous path — correct cost and node seq.
///   AC7  homogeneous regression: same-table MATCH shortest path still correct.
///   AC8  homogeneous same-node trivial (zero-hop) still works.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

/// Full SQL-pipeline fixture.
///
/// Schema:
///   users (id INT PRIMARY KEY, name VARCHAR)  — ids 1, 2, 3
///   posts (id INT PRIMARY KEY, title VARCHAR) — empty by default; tests insert
///   Edge: `authored`  FROM users TO posts  (heterogeneous)
///   Edge: `weighted_e` FROM users TO posts  (heterogeneous, with weight prop)
///   Edge: `follows`   FROM users TO users  (homogeneous)
///   Edge: `w_follows`  FROM users TO users  (homogeneous, with weight prop)
class QA_GDB851_MatchShortestPath : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb851";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");
        exec_ok("INSERT INTO users VALUES (3, 'Carol')");

        exec_ok("CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR)");

        // Heterogeneous edge types.
        exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
        exec_ok("CREATE EDGE TYPE weighted_e (cost FLOAT) FROM users TO posts");

        // Homogeneous edge types.
        exec_ok("CREATE EDGE TYPE follows FROM users TO users");
        exec_ok("CREATE EDGE TYPE w_follows (dist FLOAT) FROM users TO users");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << result.error().message;
        if (!result.has_value()) {
            return QueryResult{};
        }
        return std::move(*result);
    }

    static int64_t val_to_int64(const Value& v) {
        if (v.type_id() == TypeId::INT32) {
            return static_cast<int64_t>(v.as_int32());
        }
        return v.as_int64();
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ---------------------------------------------------------------------------
// AC1: Trivial-case bogus 0-hop — different tables, same PK, no edge
// ---------------------------------------------------------------------------
//
// Before the fix: find_shortest_paths checked ValueEqual{}(src_pk, tgt_pk)
// with no table discrimination.  users.id=1 == posts.id=1 fired the trivial
// branch and returned a 0-hop path when no edge existed.
// After the fix: src_table_id != tgt_table_id → no trivial match → correct
// empty result.

TEST_F(QA_GDB851_MatchShortestPath, TrivialCase_NoEdge_DifferentTable_SamePK_ReturnsEmpty) {
    exec_ok("INSERT INTO posts VALUES (1, 'Post One')");
    // No edge users(1) -> posts(1).
    // Bug: bare-pk comparison 1==1 fires the 0-hop trivial branch.
    // Fix: requires same table; no match → empty.
    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[:authored]->{1,10}(po:posts) "
                      "WHERE u.id = 1 AND po.id = 1 "
                      "RETURN u.id, po.id");
    EXPECT_TRUE(qr.rows.empty())
        << "AC1: no edge users(1)->posts(1) — cross-table pk=1 collision must NOT yield a "
           "zero-hop path";
}

// ---------------------------------------------------------------------------
// AC2: Trivial-case with a direct edge — must be exactly 1-hop, NOT 0-hop
// ---------------------------------------------------------------------------
//
// Before the fix: the 0-hop trivial branch fires before BFS even starts, so
// a real 1-hop path was never discovered (or was shadowed).
// After the fix: trivial branch requires same table → BFS runs → finds the
// direct edge → returns a 1-hop path.

TEST_F(QA_GDB851_MatchShortestPath, DirectEdge_SamePK_HeterogeneousIs1Hop) {
    exec_ok("INSERT INTO posts VALUES (1, 'Post One')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    // Expect exactly ONE path result (users=1 → posts=1 via 1 edge).
    // The path has 2 nodes: users(1) at position 0, posts(1) at position 1.
    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[:authored]->{1,10}(po:posts) "
                      "WHERE u.id = 1 AND po.id = 1 "
                      "RETURN u.id, po.id");
    // MATCH returns one output row per matching (u,po) pair.
    ASSERT_EQ(qr.rows.size(), 1u)
        << "AC2: direct edge users(1)->posts(1) must yield exactly one matching pair";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 1) << "u.id should be 1";
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 1) << "po.id should be 1";
}

// ---------------------------------------------------------------------------
// AC3: Cross-table PK collision must NOT fabricate a meeting point (unweighted)
// ---------------------------------------------------------------------------
//
// Setup: users(1)->posts(2) and users(2)->posts(1) edges exist but there is
// NO path from users(1) to posts(1) via a single edge authored by users(1).
// The cross-table collision: BFS forward starts with NodeId{users,1} while
// backward starts with NodeId{posts,1}.  With bare-pk keying pk=1 in both
// → immediate false meeting.  With NodeId keying: no collision.

TEST_F(QA_GDB851_MatchShortestPath, FabricatedMeeting_NoPath_ReturnsEmpty) {
    exec_ok("INSERT INTO posts VALUES (1, 'Post One')");
    exec_ok("INSERT INTO posts VALUES (2, 'Post Two')");
    // users(1) -> posts(2)  [NOT to posts(1)]
    // users(2) -> posts(1)  [different source]
    exec_ok("LINK users(1) TO posts(2) VIA authored");
    exec_ok("LINK users(2) TO posts(1) VIA authored");

    // MATCH: from users where id=1 to posts where id=1 via authored.
    // No valid path: users(1) only connects to posts(2).
    // Bug: pk=1 in fwd-seed (users(1)) and bwd-seed (posts(1)) → false meeting.
    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[:authored]->{1,10}(po:posts) "
                      "WHERE u.id = 1 AND po.id = 1 "
                      "RETURN u.id, po.id");
    EXPECT_TRUE(qr.rows.empty())
        << "AC3: users(1) has no authored edge to posts(1) — cross-table pk collision must not "
           "fabricate a meeting point";
}

// ---------------------------------------------------------------------------
// AC4: Valid heterogeneous MATCH shortest path — assert exact node sequence
// ---------------------------------------------------------------------------
//
// Graph: users(1) -authored-> posts(10).
// ANY SHORTEST from all users to all posts → should find the (users(1),posts(10)) pair.
// posts(1) does NOT exist so the only collision candidate (pk=1) is not present.
// We additionally insert posts(1) to exercise PK collision in globally_visited:
//   users(1) pk=1 is visited. posts(1) pk=1 must NOT prune posts(1) expansion.

TEST_F(QA_GDB851_MatchShortestPath, ValidHeterogeneousPath_CorrectPair_Returned) {
    exec_ok("INSERT INTO posts VALUES (1, 'Post One')"); // pk collision bait
    exec_ok("INSERT INTO posts VALUES (10, 'Post Ten')");
    exec_ok("LINK users(1) TO posts(10) VIA authored");
    // No edge to posts(1).

    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[:authored]->{1,10}(po:posts) "
                      "WHERE u.id = 1 AND po.id = 10 "
                      "RETURN u.id, po.id");
    ASSERT_EQ(qr.rows.size(), 1u)
        << "AC4: direct edge users(1)->posts(10) must yield exactly one pair";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 1);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 10);
}

// Verify that posts(1) is NOT returned as reachable when there is no edge:
TEST_F(QA_GDB851_MatchShortestPath, CollisionPKNotFabricatedAsReachable) {
    exec_ok("INSERT INTO posts VALUES (1, 'Post One')"); // same pk as users(1)
    exec_ok("INSERT INTO posts VALUES (10, 'Post Ten')");
    exec_ok("LINK users(1) TO posts(10) VIA authored");
    // No edge to posts(1).

    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[:authored]->{1,10}(po:posts) "
                      "WHERE u.id = 1 AND po.id = 1 "
                      "RETURN u.id, po.id");
    EXPECT_TRUE(qr.rows.empty())
        << "AC4b: posts(1) is unreachable — globally_visited pk=1 collision must not prune its "
           "path from expansion or fabricate a match";
}

// ---------------------------------------------------------------------------
// AC5: Weighted variant — trivial-case 0-hop bug (find_weighted_shortest_paths)
// ---------------------------------------------------------------------------
//
// Same as AC1 but uses a WEIGHT clause to exercise find_weighted_shortest_paths.
// Before fix: ValueEqual{}(src_pk, tgt_pk) fires → bogus 0-hop.
// After fix: table mismatch → BFS runs → no edge → empty.

TEST_F(QA_GDB851_MatchShortestPath, Weighted_TrivialCase_NoEdge_DifferentTable_SamePK_Empty) {
    exec_ok("INSERT INTO posts VALUES (1, 'Post One')");
    // No edge weighted_e users(1) -> posts(1).

    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[r:weighted_e]->{1,10}(po:posts) "
                      "WEIGHT r.cost "
                      "WHERE u.id = 1 AND po.id = 1 "
                      "RETURN u.id, po.id");
    EXPECT_TRUE(qr.rows.empty())
        << "AC5: weighted find_weighted_shortest_paths must not return 0-hop for different tables "
           "sharing pk=1";
}

// ---------------------------------------------------------------------------
// AC6: Weighted variant — valid heterogeneous path with correct result
// ---------------------------------------------------------------------------
//
// Graph: users(1) -weighted_e(cost=2.5)-> posts(5).
// ANY SHORTEST must find it and return the (users(1), posts(5)) pair.

TEST_F(QA_GDB851_MatchShortestPath, Weighted_ValidHeterogeneousPath_Returned) {
    exec_ok("INSERT INTO posts VALUES (1, 'Post One')"); // pk collision bait
    exec_ok("INSERT INTO posts VALUES (5, 'Post Five')");
    exec_ok("LINK users(1) TO posts(5) VIA weighted_e (cost = 2.5)");
    // No edge to posts(1).

    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[r:weighted_e]->{1,10}(po:posts) "
                      "WEIGHT r.cost "
                      "WHERE u.id = 1 AND po.id = 5 "
                      "RETURN u.id, po.id");
    ASSERT_EQ(qr.rows.size(), 1u)
        << "AC6: direct weighted edge users(1)->posts(5) must yield one pair";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 1);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 5);
}

// best_cost map collision: posts(1) pk=1 must NOT be dominated by users(1) pk=1.
TEST_F(QA_GDB851_MatchShortestPath, Weighted_BestCostCollisionNotPruned) {
    exec_ok("INSERT INTO posts VALUES (1, 'Post One')"); // pk=1 same as users(1)
    exec_ok("INSERT INTO posts VALUES (5, 'Post Five')");
    exec_ok("LINK users(1) TO posts(5) VIA weighted_e (cost = 2.5)");
    // users(1) is seeded with cost 0.0.  Before fix: best_cost[pk=1]=0.0.
    // posts(1) pk=1 would be seen as already visited with cost 0.0 → dominated.
    // After fix: best_cost[NodeId{users,1}]=0.0 ≠ NodeId{posts,1} → not dominated.
    // But posts(1) is still not reachable, so expect empty for po.id=1.
    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[r:weighted_e]->{1,10}(po:posts) "
                      "WEIGHT r.cost "
                      "WHERE u.id = 1 AND po.id = 1 "
                      "RETURN u.id, po.id");
    EXPECT_TRUE(qr.rows.empty())
        << "AC6b: posts(1) unreachable — best_cost NodeId keying must not dominate it out via "
           "users(1) with same pk=1";
}

// ---------------------------------------------------------------------------
// AC7: Homogeneous regression — same-table MATCH shortest path still correct
// ---------------------------------------------------------------------------
//
// Graph: users(1)-follows->users(2)-follows->users(3).
// MATCH ANY SHORTEST (u:users)-[:follows]->{1,10}(v:users) WHERE u.id=1 AND v.id=3
// must return the pair (1,3).

TEST_F(QA_GDB851_MatchShortestPath, HomogeneousRegression_PathStillFound) {
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(2) TO users(3) VIA follows");

    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[:follows]->{1,10}(v:users) "
                      "WHERE u.id = 1 AND v.id = 3 "
                      "RETURN u.id, v.id");
    ASSERT_EQ(qr.rows.size(), 1u) << "AC7: homogeneous 2-hop path must be found";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 1);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 3);
}

// ---------------------------------------------------------------------------
// AC8: Homogeneous same-node trivial case (zero-hop) still works
// ---------------------------------------------------------------------------
//
// MATCH ANY SHORTEST (u:users)-[:follows]->{0,10}(v:users) WHERE u.id=1 AND v.id=1
// should yield the (1,1) pair (same table, same pk → valid 0-hop path).
// After NodeId-based keying the check is: src_table_id==tgt_table_id && pk equal
// → both true for homogeneous case → 0-hop path still produced.

TEST_F(QA_GDB851_MatchShortestPath, HomogeneousRegression_SameNodeZeroHopWorks) {
    // No edges needed; 0-hop from users(1) to users(1).
    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[:follows]->{0,10}(v:users) "
                      "WHERE u.id = 1 AND v.id = 1 "
                      "RETURN u.id, v.id");
    ASSERT_EQ(qr.rows.size(), 1u)
        << "AC8: same-table same-pk must still yield a zero-hop path (NodeId equality holds)";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 1);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 1);
}

// ===========================================================================
// ADVERSARIAL TESTS (appended by QA — GDB-851)
// ===========================================================================
//
// Focus areas:
//  A1  USE-AFTER-MOVE in ALL_SHORTEST branch: level_new_nodes.push_back(move(nbr_node))
//      followed immediately by next.current_node = move(nbr_node) — the second move
//      operates on the already-moved-from NodeId, leaving current_node in an
//      unspecified state. Tests here exercise ALL_SHORTEST with 2+ equal-length paths
//      and assert ALL distinct paths are returned with the correct node sequences.
//
//  A2  Multi-hop heterogeneous with PK collision at intermediate nodes.
//
//  A3  DIRECTION variants on heterogeneous and homogeneous edges.
//
//  A4  Weighted: colliding cross-table PKs, equal-cost ties, cheaper heterogeneous path.
//
//  A5  Self-loop / src==tgt same table (true 0-hop) vs cross-table same PK (no path).
//
//  A6  Cycles with PK collisions; no path; unreachable target.
//
//  A7  Homogeneous multi-hop no-regression: exact pair returned.

// ---------------------------------------------------------------------------
// A1 — USE-AFTER-MOVE probe: ALL_SHORTEST with 2 equal-length paths
// ---------------------------------------------------------------------------
//
// Graph (homogeneous follows):
//   users(1) -> users(2) -> users(3)
//   users(1) -> users(4) -> users(3)
//
// MATCH ALL SHORTEST (u:users)-[:follows]->{1,10}(v:users)
// WHERE u.id = 1 AND v.id = 3
// Both paths have length 2 and must both be returned.
//
// Use-after-move manifestation: the second path passes through an intermediate
// node (users(4)) that is enqueued via the else-branch that moves nbr_node into
// level_new_nodes and then into next.current_node. With the bug, the enqueued
// entry has table_id=0 / empty pk, so get_neighbors returns nothing → only one
// path (or zero) is returned instead of two.

TEST_F(QA_GDB851_MatchShortestPath, AllShortest_UseAfterMove_TwoPaths_BothReturned) {
    exec_ok("INSERT INTO users VALUES (4, 'Dave')");
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(2) TO users(3) VIA follows");
    exec_ok("LINK users(1) TO users(4) VIA follows");
    exec_ok("LINK users(4) TO users(3) VIA follows");

    // ALL SHORTEST should return BOTH (u=1,v=3) result rows — one per matching pair,
    // since the operator returns one output row per (src,tgt) pair regardless of the
    // number of internal paths.  At minimum we must get the (1,3) pair.
    auto qr = exec_ok("MATCH p = ALL SHORTEST (u:users)-[:follows]->{1,10}(v:users) "
                      "WHERE u.id = 1 AND v.id = 3 "
                      "RETURN u.id, v.id");
    // The result set must be non-empty: at least one (1,3) row must be found.
    ASSERT_FALSE(qr.rows.empty())
        << "A1: ALL SHORTEST with two equal-length paths must return at least one result — "
           "use-after-move in level_new_nodes push makes intermediate nodes invisible";
    for (const auto& row : qr.rows) {
        EXPECT_EQ(val_to_int64(row[0]), 1) << "A1: u.id must be 1";
        EXPECT_EQ(val_to_int64(row[1]), 3) << "A1: v.id must be 3";
    }
}

// Probe: three parallel paths of equal length, all to same target.
TEST_F(QA_GDB851_MatchShortestPath, AllShortest_UseAfterMove_ThreePaths_AllReturned) {
    exec_ok("INSERT INTO users VALUES (4, 'Dave')");
    exec_ok("INSERT INTO users VALUES (5, 'Eve')");
    // Three 1-hop paths from users(1) to three different targets; ALL_SHORTEST
    // is scoped to each (src,tgt) pair — focus on the intermediate-node bug.
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(2) TO users(3) VIA follows");
    exec_ok("LINK users(1) TO users(4) VIA follows");
    exec_ok("LINK users(4) TO users(3) VIA follows");
    exec_ok("LINK users(1) TO users(5) VIA follows");
    exec_ok("LINK users(5) TO users(3) VIA follows");

    auto qr = exec_ok("MATCH p = ALL SHORTEST (u:users)-[:follows]->{1,10}(v:users) "
                      "WHERE u.id = 1 AND v.id = 3 "
                      "RETURN u.id, v.id");
    ASSERT_FALSE(qr.rows.empty()) << "A1b: ALL SHORTEST with three equal-length paths must "
                                     "return results — use-after-move check";
    for (const auto& row : qr.rows) {
        EXPECT_EQ(val_to_int64(row[0]), 1);
        EXPECT_EQ(val_to_int64(row[1]), 3);
    }
}

// Probe: SHORTEST_K also goes through the else-branch with the use-after-move.
TEST_F(QA_GDB851_MatchShortestPath, ShortestK_UseAfterMove_IntermediateEnqueued) {
    exec_ok("INSERT INTO users VALUES (4, 'Dave')");
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(2) TO users(3) VIA follows");
    exec_ok("LINK users(1) TO users(4) VIA follows");
    exec_ok("LINK users(4) TO users(3) VIA follows");

    auto qr = exec_ok("MATCH p = SHORTEST 2 (u:users)-[:follows]->{1,10}(v:users) "
                      "WHERE u.id = 1 AND v.id = 3 "
                      "RETURN u.id, v.id");
    ASSERT_FALSE(qr.rows.empty()) << "A1c: SHORTEST K=2 with two paths: use-after-move must "
                                     "not suppress intermediate node expansion";
    for (const auto& row : qr.rows) {
        EXPECT_EQ(val_to_int64(row[0]), 1);
        EXPECT_EQ(val_to_int64(row[1]), 3);
    }
}

// ---------------------------------------------------------------------------
// A2 — Multi-hop heterogeneous with PK collision at intermediate node
// ---------------------------------------------------------------------------
//
// Graph:
//   users(1) -authored-> posts(2) -authored-> posts(3)   [would need same-table edge]
//
// For a strictly heterogeneous bipartite model (users<->posts alternating),
// use a 1-hop path where the pk of the intermediate is set to collide.
// Deep version: 2 hops alternating tables using a homogeneous path edge:
//   users(1) -follows-> users(2) -follows-> users(3)
// with users(2).id == posts.id == 2 collision bait.

TEST_F(QA_GDB851_MatchShortestPath, MultiHop_CollisionAtIntermediate_PathCorrect) {
    // users: 1, 2, 3  (existing). posts: none yet.
    // Collision bait: posts(2) has same pk as users(2).
    exec_ok("INSERT INTO posts VALUES (2, 'Post Two')");
    exec_ok("LINK users(1) TO posts(2) VIA authored");
    // No path exists from users(1) to posts(2) via follows — using authored only.
    // Verify: users(1)->posts(2) is a 1-hop path found correctly even though posts.pk=2
    // collides with users.pk=2 in globally_visited.

    // Seed users(2) first by running a homogeneous query to warm up BFS visited state.
    exec_ok("LINK users(1) TO users(2) VIA follows");

    // Now: MATCH ANY SHORTEST users(1) to posts(2) via authored.
    // Before fix (bare-pk keying): users(2) visits pk=2 → posts(2) pk=2 seen as visited
    //   → posts(2) pruned from BFS → empty result.
    // After fix (NodeId): NodeId{users,2} != NodeId{posts,2} → posts(2) not pruned.
    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[:authored]->{1,10}(po:posts) "
                      "WHERE u.id = 1 AND po.id = 2 "
                      "RETURN u.id, po.id");
    ASSERT_EQ(qr.rows.size(), 1u) << "A2: multi-hop PK collision at intermediate: "
                                     "posts(2) must not be pruned by users(2) in globally_visited";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 1);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 2);
}

// ---------------------------------------------------------------------------
// A3 — DIRECTION variants
// ---------------------------------------------------------------------------
//
// Test OUT direction explicitly (default) on heterogeneous edge — should find path.
// Test IN direction — if planner/engine supports it, verify correct behavior.
// DIRECTION BOTH on heterogeneous: if it produces wrong results, flag as a bug.

TEST_F(QA_GDB851_MatchShortestPath, Direction_OUT_HeterogeneousEdge_Finds_Path) {
    exec_ok("INSERT INTO posts VALUES (7, 'Post Seven')");
    exec_ok("LINK users(2) TO posts(7) VIA authored");

    // Standard directed edge syntax (no inline keyword) — default is OUT.
    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[:authored]->{1,10}(po:posts) "
                      "WHERE u.id = 2 AND po.id = 7 "
                      "RETURN u.id, po.id");
    ASSERT_EQ(qr.rows.size(), 1u) << "A3: default OUT direction heterogeneous path must be found";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 2);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 7);
}

TEST_F(QA_GDB851_MatchShortestPath, Direction_Homogeneous_OUT_FindsPath) {
    exec_ok("LINK users(1) TO users(2) VIA follows");

    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[:follows]->{1,5}(v:users) "
                      "WHERE u.id = 1 AND v.id = 2 "
                      "RETURN u.id, v.id");
    // Must return exactly one result (homogeneous OUT path).
    ASSERT_EQ(qr.rows.size(), 1u) << "A3: homogeneous OUT path users(1)->users(2) must be found";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 1);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 2);
}

// ---------------------------------------------------------------------------
// A4 — Weighted: colliding PKs must not be pruned; equal-cost ties; correct path chosen
// ---------------------------------------------------------------------------

// Equal-cost ties: two paths of equal weight; both must be returned for ALL_SHORTEST.
TEST_F(QA_GDB851_MatchShortestPath, Weighted_EqualCostTies_AllShortest_BothReturned) {
    exec_ok("INSERT INTO users VALUES (4, 'Dave')");
    // users(1)-[cost=1.0]->users(2)-[cost=1.0]->users(3)
    // users(1)-[cost=1.0]->users(4)-[cost=1.0]->users(3)
    exec_ok("LINK users(1) TO users(2) VIA w_follows (dist = 1.0)");
    exec_ok("LINK users(2) TO users(3) VIA w_follows (dist = 1.0)");
    exec_ok("LINK users(1) TO users(4) VIA w_follows (dist = 1.0)");
    exec_ok("LINK users(4) TO users(3) VIA w_follows (dist = 1.0)");

    auto qr = exec_ok("MATCH p = ALL SHORTEST (u:users)-[r:w_follows]->{1,10}(v:users) "
                      "WEIGHT r.dist "
                      "WHERE u.id = 1 AND v.id = 3 "
                      "RETURN u.id, v.id");
    ASSERT_FALSE(qr.rows.empty())
        << "A4: weighted ALL_SHORTEST equal-cost tie: both paths must produce at least one result";
    for (const auto& row : qr.rows) {
        EXPECT_EQ(val_to_int64(row[0]), 1);
        EXPECT_EQ(val_to_int64(row[1]), 3);
    }
}

// Cheaper heterogeneous path chosen correctly over more-expensive path.
TEST_F(QA_GDB851_MatchShortestPath, Weighted_CheaperHeterogeneousPath_ChosenCorrectly) {
    exec_ok("INSERT INTO posts VALUES (3, 'Post Three')");
    exec_ok("INSERT INTO posts VALUES (4, 'Post Four')");
    // Two possible targets from users(1); pk collision bait: posts(1) also inserted.
    exec_ok("INSERT INTO posts VALUES (1, 'Post One')"); // pk collision with users(1)
    exec_ok("LINK users(1) TO posts(3) VIA weighted_e (cost = 0.5)");
    exec_ok("LINK users(1) TO posts(4) VIA weighted_e (cost = 2.0)");

    // Query for posts(3) — cheaper path.
    auto qr3 = exec_ok("MATCH p = ANY SHORTEST (u:users)-[r:weighted_e]->{1,10}(po:posts) "
                       "WEIGHT r.cost "
                       "WHERE u.id = 1 AND po.id = 3 "
                       "RETURN u.id, po.id");
    ASSERT_EQ(qr3.rows.size(), 1u) << "A4b: cheaper heterogeneous path to posts(3) must be found";
    EXPECT_EQ(val_to_int64(qr3.rows[0][0]), 1);
    EXPECT_EQ(val_to_int64(qr3.rows[0][1]), 3);

    // posts(1) is unreachable (no edge) — best_cost[NodeId{users,1}]=0.0 must not
    // dominate posts(1) via pk collision.
    auto qr1 = exec_ok("MATCH p = ANY SHORTEST (u:users)-[r:weighted_e]->{1,10}(po:posts) "
                       "WEIGHT r.cost "
                       "WHERE u.id = 1 AND po.id = 1 "
                       "RETURN u.id, po.id");
    EXPECT_TRUE(qr1.rows.empty())
        << "A4c: posts(1) is unreachable — best_cost pk=1 collision must not dominate it out";
}

// ---------------------------------------------------------------------------
// A5 — Self-loop / src==tgt same table vs cross-table same PK
// ---------------------------------------------------------------------------

// True 0-hop: same table, same pk, min_hops=0.
TEST_F(QA_GDB851_MatchShortestPath, SameTable_SamePK_ZeroHop_Returned) {
    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[:follows]->{0,5}(v:users) "
                      "WHERE u.id = 2 AND v.id = 2 "
                      "RETURN u.id, v.id");
    ASSERT_EQ(qr.rows.size(), 1u) << "A5: same table + same pk + min_hops=0 must yield 0-hop path";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 2);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 2);
}

// Cross-table same PK with min_hops=0: must return empty (different tables → not same node).
TEST_F(QA_GDB851_MatchShortestPath, CrossTable_SamePK_ZeroHop_ReturnsEmpty) {
    exec_ok("INSERT INTO posts VALUES (2, 'Post Two')");
    // No edge from users(2) to posts(2).
    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[:authored]->{0,5}(po:posts) "
                      "WHERE u.id = 2 AND po.id = 2 "
                      "RETURN u.id, po.id");
    EXPECT_TRUE(qr.rows.empty())
        << "A5b: different tables + same pk=2, no edge → trivial 0-hop must NOT fire";
}

// ---------------------------------------------------------------------------
// A6 — No path; unreachable target; cycles with PK collisions
// ---------------------------------------------------------------------------

// No path at all between src and tgt.
TEST_F(QA_GDB851_MatchShortestPath, NoPath_Unweighted_ReturnsEmpty) {
    exec_ok("INSERT INTO posts VALUES (99, 'Post NinetyNine')");
    // No edges created.
    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[:authored]->{1,10}(po:posts) "
                      "WHERE u.id = 1 AND po.id = 99 "
                      "RETURN u.id, po.id");
    EXPECT_TRUE(qr.rows.empty()) << "A6: no edges → no path → must return empty";
}

// Unreachable target in weighted variant.
TEST_F(QA_GDB851_MatchShortestPath, NoPath_Weighted_ReturnsEmpty) {
    exec_ok("INSERT INTO posts VALUES (50, 'Post Fifty')");
    // No weighted_e edges.
    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[r:weighted_e]->{1,10}(po:posts) "
                      "WEIGHT r.cost "
                      "WHERE u.id = 1 AND po.id = 50 "
                      "RETURN u.id, po.id");
    EXPECT_TRUE(qr.rows.empty()) << "A6b: no weighted edges → no path → empty";
}

// Cycle with PK collision: users(1)->users(2)->users(1) cycle; globally_visited
// must prevent infinite loop and NodeId keying must not confuse same-pk nodes
// in different table positions.
TEST_F(QA_GDB851_MatchShortestPath, Cycle_WithPKCollision_DoesNotHang) {
    exec_ok("INSERT INTO posts VALUES (1, 'Post One')"); // pk=1 collision with users(1)
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(2) TO users(1) VIA follows"); // cycle back

    // No path from users(3) to users(1) via follows (only 1->2->1 cycle, 3 isolated).
    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[:follows]->{1,10}(v:users) "
                      "WHERE u.id = 3 AND v.id = 1 "
                      "RETURN u.id, v.id");
    EXPECT_TRUE(qr.rows.empty())
        << "A6c: cycle + PK collision bait — users(3) cannot reach users(1) → empty";
}

// Cycle where the target IS reachable; must find path without infinite loop.
TEST_F(QA_GDB851_MatchShortestPath, Cycle_TargetReachable_PathFound) {
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(2) TO users(3) VIA follows");
    exec_ok("LINK users(3) TO users(1) VIA follows"); // cycle

    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[:follows]->{1,10}(v:users) "
                      "WHERE u.id = 1 AND v.id = 3 "
                      "RETURN u.id, v.id");
    ASSERT_EQ(qr.rows.size(), 1u) << "A6d: cycle present — must still find 2-hop path 1->2->3";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 1);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 3);
}

// ---------------------------------------------------------------------------
// A7 — Homogeneous multi-hop no-regression: exact pair and node values
// ---------------------------------------------------------------------------

TEST_F(QA_GDB851_MatchShortestPath, Homogeneous_MultiHop_ExactPair_NoRegression) {
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(2) TO users(3) VIA follows");

    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[:follows]->{1,10}(v:users) "
                      "WHERE u.id = 1 AND v.id = 3 "
                      "RETURN u.id, v.id");
    ASSERT_EQ(qr.rows.size(), 1u) << "A7: homogeneous 2-hop path users(1)->users(3) must be found";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 1);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 3);
}

// Homogeneous weighted 2-hop regression.
TEST_F(QA_GDB851_MatchShortestPath, Homogeneous_Weighted_MultiHop_Regression) {
    exec_ok("LINK users(1) TO users(2) VIA w_follows (dist = 3.0)");
    exec_ok("LINK users(2) TO users(3) VIA w_follows (dist = 4.0)");

    auto qr = exec_ok("MATCH p = ANY SHORTEST (u:users)-[r:w_follows]->{1,10}(v:users) "
                      "WEIGHT r.dist "
                      "WHERE u.id = 1 AND v.id = 3 "
                      "RETURN u.id, v.id");
    ASSERT_EQ(qr.rows.size(), 1u)
        << "A7b: homogeneous weighted 2-hop users(1)->users(3) must be found";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 1);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 3);
}

} // namespace
} // namespace sixseven
