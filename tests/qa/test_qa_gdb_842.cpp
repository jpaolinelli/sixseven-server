/// @file test_qa_gdb_842.cpp
/// QA adversarial tests for GDB-842: heterogeneous PK collision bug class in
/// ShortestPathOperator::run_bidirectional_bfs().
///
/// Bug 1 (trivial-case): from==to check on bare pk returned a bogus 0-hop
///   path when from and to were rows in DIFFERENT tables sharing the same pk
///   value (e.g. users.1 vs posts.1). Fixed by keying on (table_id, pk).
///
/// Bug 2 (fabricated meeting point): fwd_visited/bwd_visited were keyed by
///   bare pk, so a forward-BFS neighbor from the target table whose pk
///   happened to equal a source-table node already in bwd_visited caused a
///   false meeting-point detection — fabricating or corrupting paths.
///
/// Precedent: GDB-694/695/696 fixed the identical bug class for TRAVERSE.
///
/// Test coverage:
///   * trivial-case returns bogus zero-hop path (reproduces bug 1)
///   * colliding PKs do NOT fabricate a meeting point (bug 2)
///   * valid heterogeneous path found correctly (assert hops/node sequence)
///   * no-path result is empty, not a fabricated path
///   * direction IN heterogeneous shortest path works correctly
///   * DIRECTION BOTH on heterogeneous edge is rejected at plan time
///   * homogeneous regression: same-table shortest path still works
///   * homogeneous same-node trivial case still works
///   * cross-table pk collision stress: many pairs, none fabricate a path

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

/// Full SQL-pipeline fixture:
///   users  (id INT PRIMARY KEY, name VARCHAR)  — pre-populated 1..3
///   posts  (id INT PRIMARY KEY, title VARCHAR) — empty; tests insert as needed
///   Edge: `authored` FROM users TO posts (heterogeneous)
///   Edge: `follows`  FROM users TO users (homogeneous)
class QA_GDB842_ShortestPath : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb842";
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
        exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
        exec_ok("CREATE EDGE TYPE follows FROM users TO users");
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

    /// Execute SQL and assert that it fails (returns an error).
    void exec_err(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "Expected error for: " << sql;
    }

    static int64_t val_to_int64(const Value& v) {
        if (v.type_id() == TypeId::INT32) {
            return static_cast<int64_t>(v.as_int32());
        }
        return v.as_int64();
    }

    /// Collect the node column (col 0) of every result row as int64 in order.
    static std::vector<int64_t> path_nodes(const QueryResult& qr) {
        std::vector<int64_t> nodes;
        nodes.reserve(qr.rows.size());
        for (const auto& row : qr.rows) {
            nodes.push_back(val_to_int64(row[0]));
        }
        return nodes;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ---------------------------------------------------------------------------
// Bug 1: trivial-case check must be heterogeneity-aware
// ---------------------------------------------------------------------------

// Before the fix: SHORTEST PATH FROM users(1) TO posts(1) returned a zero-hop path
// because the bare-pk equality check (1 == 1) fired even though users.1 and
// posts.1 are rows in DIFFERENT tables.  The correct result is a real path
// (length 1) or no-path if no edge exists.
TEST_F(QA_GDB842_ShortestPath, TrivialCaseBugNotFiredForDifferentTables_NoEdge) {
    exec_ok("INSERT INTO posts VALUES (1, 'Post One')");

    // No edge users(1) -> posts(1) yet.
    // Before fix: returns a bogus zero-hop path.  After fix: empty (no path).
    auto qr = exec_ok("SHORTEST PATH FROM users(1) TO posts(1) VIA authored");
    EXPECT_TRUE(qr.rows.empty())
        << "users.1 != posts.1 — cross-table pk collision must not produce a zero-hop path";
}

TEST_F(QA_GDB842_ShortestPath, TrivialCaseBugNotFiredForDifferentTables_WithEdge) {
    exec_ok("INSERT INTO posts VALUES (1, 'Post One')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    // There IS a direct edge users(1) -> posts(1).  Must return a 1-hop path,
    // NOT a zero-hop path (which the bug returned before the pk==pk trivial
    // check was made heterogeneity-aware).
    auto qr = exec_ok("SHORTEST PATH FROM users(1) TO posts(1) VIA authored");
    auto nodes = path_nodes(qr);
    // Expect: start=users(1), end=posts(1) — two rows (hop 0 and hop 1).
    ASSERT_EQ(nodes.size(), 2u) << "direct edge must yield a 1-hop path (2 rows), not zero-hop";
    EXPECT_EQ(nodes[0], 1) << "first node is users(1)";
    EXPECT_EQ(nodes[1], 1) << "last node is posts(1)";

    // Check hop values: row[1] must be 0 then 1.
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 0);
    EXPECT_EQ(val_to_int64(qr.rows[1][1]), 1);
}

// ---------------------------------------------------------------------------
// Bug 2: colliding PKs across tables must NOT fabricate a meeting point
// ---------------------------------------------------------------------------

// Scenario: users(1) authored posts(10).  No edge from posts(10) back to
// users(1).  Before the fix: backward BFS seeds bwd_visited with posts(1).
// When forward BFS reaches posts(10) it checks fwd_visited (bare pk 10) vs
// bwd_visited (bare pk 1) — no match, correct.  BUT if we have posts(1) in
// bwd_visited and users(1) in fwd_visited, a forward expansion that produces
// target pk=1 would falsely match the bwd_visited entry for posts(1) pk=1.
// This test uses SHORTEST PATH FROM users(2) TO posts(1) with an intermediate edge
// users(2)->posts(2), with posts(2).id != posts(1).id, so no path exists.
// With the bug a fabricated meeting at pk=2 (posts(2) pk collides with
// nothing here) would be suppressed; but the cross-table collision between
// users(1) in fwd and posts(1) in bwd could be triggered.
TEST_F(QA_GDB842_ShortestPath, FabricatedMeetingPointNotCreated) {
    exec_ok("INSERT INTO posts VALUES (1, 'Post One')");
    exec_ok("INSERT INTO posts VALUES (2, 'Post Two')");
    // users(1) -> posts(1).  users(2) -> posts(2).  No path from users(2) to posts(1).
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(2) TO posts(2) VIA authored");

    // Forward BFS from users(2) via authored (OUT) reaches posts(2) at depth 1.
    // posts(2).pk == 2.  Backward BFS from posts(1) via authored (IN) reaches
    // users whose pk is 1.  pk=2 != pk=1, so no meeting point — correct.
    // But if the bug is present and pk=2 is in bwd_visited as a bare pk due to
    // some collision, a false path would appear.
    auto qr = exec_ok("SHORTEST PATH FROM users(2) TO posts(1) VIA authored");
    EXPECT_TRUE(qr.rows.empty())
        << "no path from users(2) to posts(1) — fabricated meeting point must not occur";
}

// Adversarial: users(1) is in fwd_visited at the start, and posts(1) is bwd's
// seed.  Without table discrimination: pk=1 in both visited sets → false
// meeting at the start.  After fix: NodeId{users, 1} != NodeId{posts, 1}.
TEST_F(QA_GDB842_ShortestPath, FabricatedMeetingAtSeedPkCollision) {
    exec_ok("INSERT INTO posts VALUES (1, 'Post One')");
    // No edge at all from users(1) to posts(1).
    // Bare-pk bug: fwd seeds pk=1, bwd seeds pk=1 — immediate false meeting.
    // After fix: NodeId{users,1} != NodeId{posts,1} — no meeting.
    auto qr = exec_ok("SHORTEST PATH FROM users(1) TO posts(1) VIA authored");
    EXPECT_TRUE(qr.rows.empty())
        << "no edges exist — cross-table pk collision must not fabricate an immediate meeting";
}

// ---------------------------------------------------------------------------
// Valid heterogeneous path: assert correct node sequence and hop count
// ---------------------------------------------------------------------------

// Graph: users(1) -authored-> posts(10).  Asking for SHORTEST PATH from
// users(1) to posts(10): expect [users(1) at hop 0, posts(10) at hop 1].
TEST_F(QA_GDB842_ShortestPath, ValidHeterogeneousPathDirectEdge) {
    exec_ok("INSERT INTO posts VALUES (10, 'Post Ten')");
    exec_ok("LINK users(1) TO posts(10) VIA authored");

    auto qr = exec_ok("SHORTEST PATH FROM users(1) TO posts(10) VIA authored");
    auto nodes = path_nodes(qr);
    ASSERT_EQ(nodes.size(), 2u) << "direct 1-hop path must have 2 rows";
    EXPECT_EQ(nodes[0], 1) << "first node: users(1)";
    EXPECT_EQ(nodes[1], 10) << "last node: posts(10)";
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 0);
    EXPECT_EQ(val_to_int64(qr.rows[1][1]), 1);
}

// Same graph but users(1) colliding pk: posts(1) exists but not the target.
// Asking for SHORTEST PATH FROM users(1) TO posts(99) — no edge to posts(99).
TEST_F(QA_GDB842_ShortestPath, ValidHeterogeneousNoPathWhenTargetUnreachable) {
    exec_ok("INSERT INTO posts VALUES (1, 'Post One')");
    exec_ok("INSERT INTO posts VALUES (99, 'Post 99')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    // No edge to posts(99).

    auto qr = exec_ok("SHORTEST PATH FROM users(1) TO posts(99) VIA authored");
    EXPECT_TRUE(qr.rows.empty())
        << "posts(99) is unreachable — result must be empty, not a fabricated path";
}

// ---------------------------------------------------------------------------
// IN direction heterogeneous shortest path
// ---------------------------------------------------------------------------

// Graph: users(1) authored posts(5).  SHORTEST PATH FROM posts(5) TO users(1)
// DIRECTION IN: backward from source = users, forward from target = posts.
// For IN direction forward BFS follows IN edges (returns source PKs), bwd
// follows OUT edges (returns target PKs).
TEST_F(QA_GDB842_ShortestPath, InDirectionHeterogeneousValidPath) {
    exec_ok("INSERT INTO posts VALUES (5, 'Post Five')");
    exec_ok("LINK users(1) TO posts(5) VIA authored");

    auto qr = exec_ok("SHORTEST PATH FROM posts(5) TO users(1) VIA authored DIRECTION IN");
    auto nodes = path_nodes(qr);
    ASSERT_EQ(nodes.size(), 2u) << "1-hop IN path must have 2 rows";
    EXPECT_EQ(nodes[0], 5) << "start: posts(5)";
    EXPECT_EQ(nodes[1], 1) << "end: users(1)";
}

// ---------------------------------------------------------------------------
// DIRECTION BOTH on heterogeneous edge is rejected at plan time
// ---------------------------------------------------------------------------

TEST_F(QA_GDB842_ShortestPath, DirectionBothHeterogeneousIsRejected) {
    exec_ok("INSERT INTO posts VALUES (1, 'Post One')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    // Must produce an error, not a (possibly fabricated) path.
    exec_err("SHORTEST PATH FROM users(1) TO posts(1) VIA authored DIRECTION BOTH");
}

// ---------------------------------------------------------------------------
// Homogeneous regression: same-table shortest path must still work
// ---------------------------------------------------------------------------

// Regression: the NodeId-based keying must not break homogeneous edges where
// both sides share the same table_id.  The existing shortest-path unit tests
// cover the basic cases; this QA test exercises the same path through the
// SQL pipeline.
TEST_F(QA_GDB842_ShortestPath, HomogeneousShortestPathStillCorrect) {
    // users(1) -follows-> users(2) -follows-> users(3)
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(2) TO users(3) VIA follows");

    auto qr = exec_ok("SHORTEST PATH FROM users(1) TO users(3) VIA follows");
    auto nodes = path_nodes(qr);
    ASSERT_EQ(nodes.size(), 3u) << "2-hop homogeneous path must have 3 rows";
    EXPECT_EQ(nodes[0], 1);
    EXPECT_EQ(nodes[1], 2);
    EXPECT_EQ(nodes[2], 3);
}

TEST_F(QA_GDB842_ShortestPath, HomogeneousSameNodeTrivialCase) {
    // users(1) TO users(1) — same table AND same pk → valid 0-hop path.
    auto qr = exec_ok("SHORTEST PATH FROM users(1) TO users(1) VIA follows");
    auto nodes = path_nodes(qr);
    ASSERT_EQ(nodes.size(), 1u) << "same-table same-pk is a valid 0-hop path";
    EXPECT_EQ(nodes[0], 1);
}

TEST_F(QA_GDB842_ShortestPath, HomogeneousNoPathStillEmpty) {
    // No follows edges at all → no path.
    auto qr = exec_ok("SHORTEST PATH FROM users(1) TO users(3) VIA follows");
    EXPECT_TRUE(qr.rows.empty());
}

// ---------------------------------------------------------------------------
// Stress: many colliding pairs across tables
// ---------------------------------------------------------------------------

// 20 users(i) -authored-> posts(i) for i in 1..20.  Every SHORTEST PATH
// users(i) TO posts(i) must return the correct 1-hop path, NOT a zero-hop
// path from the trivial-case bug or a fabricated path from the meeting-point
// bug.  Cross-table collisions are rampant (every i appears in both tables).
TEST_F(QA_GDB842_ShortestPath, StressManyCollidingPairsAllReturnCorrectOneHopPath) {
    const int64_t pairs = 20;
    for (int64_t i = 4; i <= pairs; ++i) {
        exec_ok("INSERT INTO users VALUES (" + std::to_string(i) + ", 'U" + std::to_string(i) +
                "')");
    }
    for (int64_t i = 1; i <= pairs; ++i) {
        exec_ok("INSERT INTO posts VALUES (" + std::to_string(i) + ", 'P" + std::to_string(i) +
                "')");
        exec_ok("LINK users(" + std::to_string(i) + ") TO posts(" + std::to_string(i) +
                ") VIA authored");
    }

    for (int64_t i = 1; i <= pairs; ++i) {
        std::string sql = "SHORTEST PATH FROM users(" + std::to_string(i) + ") TO posts(" +
                          std::to_string(i) + ") VIA authored";
        auto qr = exec_ok(sql);
        auto nodes = path_nodes(qr);
        ASSERT_EQ(nodes.size(), 2u)
            << "pair " << i << ": expected 1-hop path (2 rows), got " << nodes.size();
        EXPECT_EQ(nodes[0], i) << "pair " << i << ": start node (users side)";
        EXPECT_EQ(nodes[1], i) << "pair " << i << ": end node (posts side)";
        EXPECT_EQ(val_to_int64(qr.rows[0][1]), 0) << "pair " << i << ": hop 0";
        EXPECT_EQ(val_to_int64(qr.rows[1][1]), 1) << "pair " << i << ": hop 1";
    }
}

// Stress no-path: users(i) TO posts(j) where j > pairs — unreachable.
// Must return empty, not a fabricated path due to cross-table pk collision.
TEST_F(QA_GDB842_ShortestPath, StressNoPathAmongCollidingPairs) {
    const int64_t pairs = 10;
    for (int64_t i = 4; i <= pairs; ++i) {
        exec_ok("INSERT INTO users VALUES (" + std::to_string(i) + ", 'U" + std::to_string(i) +
                "')");
    }
    for (int64_t i = 1; i <= pairs; ++i) {
        exec_ok("INSERT INTO posts VALUES (" + std::to_string(i) + ", 'P" + std::to_string(i) +
                "')");
    }
    // Link users(i) -> posts(i+1) only (no link to posts(i) itself, so
    // SHORTEST PATH FROM users(i) TO posts(i) should be empty).
    for (int64_t i = 1; i < pairs; ++i) {
        exec_ok("LINK users(" + std::to_string(i) + ") TO posts(" + std::to_string(i + 1) +
                ") VIA authored");
    }

    // users(1) TO posts(1): no direct edge (only edge is users(1)->posts(2)).
    auto qr = exec_ok("SHORTEST PATH FROM users(1) TO posts(1) VIA authored");
    EXPECT_TRUE(qr.rows.empty())
        << "no edge users(1)->posts(1) — fabricated path via pk collision must not appear";
}

} // namespace
} // namespace sixseven
