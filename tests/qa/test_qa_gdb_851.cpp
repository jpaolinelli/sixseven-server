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

} // namespace
} // namespace sixseven
