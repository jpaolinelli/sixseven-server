/// @file test_qa_gdb_694.cpp
/// QA adversarial tests for GDB-694: heterogeneous TRACE must not hang when
/// a target PK equals the start PK (depth-bounded reconstruct_path() in
/// TraversalOperator, EnrichedTraversalOperator, EdgeTraversalOperator).
///
/// Adversarial coverage beyond the dev suite (test_heterogeneous_traversal.cpp)
/// and the GDB-680 regression tests:
///   * collisions among multiple targets (collider mixed with non-colliders)
///   * reverse collisions (target PK equals a *different* source's PK)
///   * PK-aliasing "chains" must not escape the heterogeneous depth-1 cap
///   * IN-direction collisions with multiple colliding sources
///   * dangling-edge + collision (target row deleted after LINK)
///   * standalone TRAVERSE statement form under collision (termination pin)
///   * DIRECTION BOTH on heterogeneous edges (graceful error, no hang)
///   * string-PK heterogeneous targets under TRACE (clean INVALID_ARGUMENT)
///   * homogeneous regressions: cycles + shortcut edges keep path == depth
///   * stress: many colliding pairs, every query bounded and correct

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

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

/// Full SQL-pipeline fixture: users (1..3) and posts tables plus the
/// heterogeneous edge type `authored FROM users TO posts`. Posts rows and
/// links are inserted per test so each scenario controls its collisions.
class QA_GDB694_TraceCollision : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb694";
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

    /// Execute SQL that must fail; returns the error for further inspection.
    Error exec_err(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << ": expected an error but query succeeded";
        if (result.has_value()) {
            return Error{StatusCode::OK, "query unexpectedly succeeded"};
        }
        EXPECT_FALSE(result.error().message.empty()) << sql << ": error message must not be empty";
        return result.error();
    }

    static int64_t val_to_int64(const Value& v) {
        if (v.type_id() == TypeId::INT32) {
            return v.as_int32();
        }
        return v.as_int64();
    }

    /// Assert a path is exactly the two steps [from, to] with a real outgoing
    /// edge on the first step and a -1 terminal edge.
    static void expect_two_step_path(const Path& p, int64_t from, int64_t to) {
        ASSERT_EQ(p.steps.size(), 2u) << "path must be start -> target, not a self-chain";
        EXPECT_EQ(p.steps[0].node_pk, from);
        EXPECT_EQ(p.steps[1].node_pk, to);
        EXPECT_GE(p.steps[0].edge_id, 0) << "start step must carry the outgoing edge id";
        EXPECT_EQ(p.steps[1].edge_id, -1) << "terminal step has no outgoing edge";
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ---------------------------------------------------------------------------
// Collisions mixed with non-colliding targets
// ---------------------------------------------------------------------------

TEST_F(QA_GDB694_TraceCollision, EnrichedOutColliderAmongMultipleTargets) {
    exec_ok("INSERT INTO posts VALUES (1, 'Collider')");
    exec_ok("INSERT INTO posts VALUES (2, 'BobAlias')"); // PK aliases users(2)
    exec_ok("INSERT INTO posts VALUES (10, 'Plain')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(1) TO posts(2) VIA authored");
    exec_ok("LINK users(1) TO posts(10) VIA authored");

    auto qr = exec_ok("SELECT title, __node, __depth, __path "
                      "FROM TRAVERSE authored FROM users(1) DIRECTION OUT WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 3u);
    std::unordered_set<int64_t> nodes;
    for (const auto& row : qr.rows) {
        const int64_t node = val_to_int64(row[1]);
        EXPECT_TRUE(nodes.insert(node).second) << "node " << node << " emitted twice";
        EXPECT_EQ(val_to_int64(row[2]), 1) << "all heterogeneous targets are at depth 1";
        ASSERT_EQ(row[3].type_id(), TypeId::PATH);
        expect_two_step_path(row[3].as_path(), 1, node);
    }
    EXPECT_EQ(nodes.count(1), 1u) << "the colliding target posts(1) must be present";
    EXPECT_EQ(nodes.count(2), 1u);
    EXPECT_EQ(nodes.count(10), 1u);
}

TEST_F(QA_GDB694_TraceCollision, EdgeModeOutColliderAmongMultipleTargets) {
    exec_ok("INSERT INTO posts VALUES (1, 'Collider')");
    exec_ok("INSERT INTO posts VALUES (10, 'Plain')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(1) TO posts(10) VIA authored");

    auto qr = exec_ok("SELECT __from, __to, __depth, __path "
                      "FROM TRAVERSE authored FROM users(1) DIRECTION OUT MODE EDGES WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 2u);
    for (const auto& row : qr.rows) {
        EXPECT_EQ(val_to_int64(row[0]), 1);
        ASSERT_EQ(row[3].type_id(), TypeId::PATH);
        const Path& p = row[3].as_path();
        ASSERT_EQ(p.steps.size(), 1u)
            << "path to the start (__from) node must be the trivial single step";
        EXPECT_EQ(p.steps[0].node_pk, 1);
        EXPECT_EQ(p.steps[0].edge_id, -1);
    }
}

// ---------------------------------------------------------------------------
// IN-direction collisions, including multiple colliding sources
// ---------------------------------------------------------------------------

TEST_F(QA_GDB694_TraceCollision, EnrichedInCollisionWithSecondAuthor) {
    exec_ok("INSERT INTO posts VALUES (1, 'Shared')");
    exec_ok("LINK users(1) TO posts(1) VIA authored"); // collider: users(1) == posts(1)
    exec_ok("LINK users(3) TO posts(1) VIA authored");

    auto qr = exec_ok("SELECT name, __node, __depth, __path "
                      "FROM TRAVERSE authored FROM posts(1) DIRECTION IN WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 2u);
    std::unordered_set<int64_t> sources;
    for (const auto& row : qr.rows) {
        const int64_t node = val_to_int64(row[1]);
        EXPECT_TRUE(sources.insert(node).second);
        EXPECT_EQ(val_to_int64(row[2]), 1);
        ASSERT_EQ(row[3].type_id(), TypeId::PATH);
        expect_two_step_path(row[3].as_path(), 1, node);
    }
    EXPECT_EQ(sources.count(1), 1u) << "the colliding source users(1) must be present";
    EXPECT_EQ(sources.count(3), 1u);
}

TEST_F(QA_GDB694_TraceCollision, EdgeModeInCollisionWithSecondAuthor) {
    exec_ok("INSERT INTO posts VALUES (1, 'Shared')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(3) TO posts(1) VIA authored");

    auto qr = exec_ok("SELECT __from, __to, __depth, __path "
                      "FROM TRAVERSE authored FROM posts(1) DIRECTION IN MODE EDGES WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 2u);
    std::unordered_set<int64_t> froms;
    for (const auto& row : qr.rows) {
        const int64_t from = val_to_int64(row[0]);
        EXPECT_TRUE(froms.insert(from).second);
        EXPECT_EQ(val_to_int64(row[1]), 1);
        EXPECT_EQ(val_to_int64(row[2]), 1) << "__depth is the deeper endpoint (the source)";
        ASSERT_EQ(row[3].type_id(), TypeId::PATH);
        // Path runs from the start posts(1) to the edge's __from node.
        expect_two_step_path(row[3].as_path(), 1, from);
    }
    EXPECT_EQ(froms.count(1), 1u);
    EXPECT_EQ(froms.count(3), 1u);
}

// ---------------------------------------------------------------------------
// Reverse collisions: target PK matches a *different* source's PK
// ---------------------------------------------------------------------------

TEST_F(QA_GDB694_TraceCollision, ReverseCollisionTargetAliasesOtherSource) {
    exec_ok("INSERT INTO posts VALUES (1, 'AliasOfAlice')");
    exec_ok("INSERT INTO posts VALUES (2, 'AliasOfBob')");
    exec_ok("LINK users(1) TO posts(2) VIA authored");
    exec_ok("LINK users(2) TO posts(1) VIA authored");

    // From users(1): only posts(2), path [1, 2].
    auto qr1 = exec_ok("SELECT __node, __depth, __path "
                       "FROM TRAVERSE authored FROM users(1) DIRECTION OUT WITH TRACE");
    ASSERT_EQ(qr1.rows.size(), 1u);
    EXPECT_EQ(val_to_int64(qr1.rows[0][0]), 2);
    EXPECT_EQ(val_to_int64(qr1.rows[0][1]), 1);
    ASSERT_EQ(qr1.rows[0][2].type_id(), TypeId::PATH);
    expect_two_step_path(qr1.rows[0][2].as_path(), 1, 2);

    // From users(2): only posts(1), path [2, 1].
    auto qr2 = exec_ok("SELECT __node, __depth, __path "
                       "FROM TRAVERSE authored FROM users(2) DIRECTION OUT WITH TRACE");
    ASSERT_EQ(qr2.rows.size(), 1u);
    EXPECT_EQ(val_to_int64(qr2.rows[0][0]), 1);
    ASSERT_EQ(qr2.rows[0][2].type_id(), TypeId::PATH);
    expect_two_step_path(qr2.rows[0][2].as_path(), 2, 1);
}

TEST_F(QA_GDB694_TraceCollision, AliasingChainCannotEscapeHeterogeneousDepthCap) {
    // posts(2) aliases users(2), which has its own outgoing edge. If the BFS
    // wrongly treated posts(2) as users(2) it would reach posts(3) at a fake
    // depth 2. The heterogeneous depth-1 cap must prevent that even when the
    // query asks for MAX_DEPTH 5, and every emitted path must stay two-step.
    exec_ok("INSERT INTO posts VALUES (2, 'AliasOfBob')");
    exec_ok("INSERT INTO posts VALUES (3, 'Unreachable')");
    exec_ok("LINK users(1) TO posts(2) VIA authored");
    exec_ok("LINK users(2) TO posts(3) VIA authored");

    auto qr = exec_ok("SELECT __node, __depth, __path "
                      "FROM TRAVERSE authored FROM users(1) DIRECTION OUT MAX_DEPTH 5 WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 1u) << "PK aliasing must not leak phantom multi-hop rows";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 2);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 1);
    ASSERT_EQ(qr.rows[0][2].type_id(), TypeId::PATH);
    expect_two_step_path(qr.rows[0][2].as_path(), 1, 2);
}

// ---------------------------------------------------------------------------
// Dangling edge + collision
// ---------------------------------------------------------------------------

TEST_F(QA_GDB694_TraceCollision, DanglingColliderEdgeStillTerminatesWithNullRow) {
    exec_ok("INSERT INTO posts VALUES (1, 'Doomed')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("DELETE FROM posts WHERE id = 1");

    auto qr = exec_ok("SELECT title, __node, __depth, __path "
                      "FROM TRAVERSE authored FROM users(1) DIRECTION OUT WITH TRACE");

    // The edge survives the row deletion; enrichment fills NULLs but the
    // depth-bounded path must still be the two-step [1, 1].
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_TRUE(qr.rows[0][0].is_null()) << "deleted target row must enrich as NULL";
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 1);
    ASSERT_EQ(qr.rows[0][3].type_id(), TypeId::PATH);
    expect_two_step_path(qr.rows[0][3].as_path(), 1, 1);
}

// ---------------------------------------------------------------------------
// Standalone TRAVERSE statement form (TraversalOperator)
// ---------------------------------------------------------------------------

// The standalone form always seeds the BFS visited set with the start key
// (TraversalOperator has no heterogeneous flag), so a colliding target is
// silently suppressed instead of emitted. This pins the current behavior:
// the query must terminate with 0 rows — never hang. The suppression itself
// is a separate pre-existing inconsistency with the FROM TRAVERSE form
// (see QA report for GDB-694).
TEST_F(QA_GDB694_TraceCollision, StandaloneTraverseCollisionTerminates) {
    exec_ok("INSERT INTO posts VALUES (1, 'Collider')");
    exec_ok("INSERT INTO posts VALUES (10, 'Plain')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(1) TO posts(10) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM users(1) WITH TRACE");

    // Non-colliding target emitted; colliding target suppressed by the
    // visited-set seed. Both outcomes must be bounded and cycle-free.
    ASSERT_EQ(qr.rows.size(), 1u)
        << "standalone TRAVERSE currently suppresses the colliding target";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 10);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 1);
    ASSERT_EQ(qr.rows[0][2].type_id(), TypeId::PATH);
    const Path& p = qr.rows[0][2].as_path();
    ASSERT_EQ(p.steps.size(), 2u);
    EXPECT_EQ(p.steps[0].node_pk, 1);
    EXPECT_EQ(p.steps[1].node_pk, 10);
}

// ---------------------------------------------------------------------------
// DIRECTION BOTH on heterogeneous edges: graceful error, no hang
// ---------------------------------------------------------------------------

TEST_F(QA_GDB694_TraceCollision, DirectionBothHeterogeneousIsGracefulError) {
    exec_ok("INSERT INTO posts VALUES (1, 'Collider')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    auto err = exec_err("SELECT __node, __path "
                        "FROM TRAVERSE authored FROM users(1) DIRECTION BOTH WITH TRACE");
    EXPECT_EQ(err.code, StatusCode::TYPE_ERROR);
    EXPECT_NE(err.message.find("DIRECTION BOTH not supported"), std::string::npos)
        << "actual message: " << err.message;
}

// ---------------------------------------------------------------------------
// Empty results with collision data present
// ---------------------------------------------------------------------------

TEST_F(QA_GDB694_TraceCollision, NonexistentStartKeyYieldsNoRows) {
    exec_ok("INSERT INTO posts VALUES (1, 'Collider')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    auto qr = exec_ok("SELECT __node, __path "
                      "FROM TRAVERSE authored FROM users(42) DIRECTION OUT WITH TRACE");
    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QA_GDB694_TraceCollision, MaxDepthZeroCollisionYieldsNoRows) {
    exec_ok("INSERT INTO posts VALUES (1, 'Collider')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    auto qr = exec_ok("SELECT __node, __path FROM TRAVERSE authored FROM users(1) "
                      "DIRECTION OUT MAX_DEPTH 0 WITH TRACE");
    EXPECT_EQ(qr.rows.size(), 0u) << "MAX_DEPTH 0 must suppress even colliding targets";
}

// ---------------------------------------------------------------------------
// String-PK heterogeneous targets under TRACE
// ---------------------------------------------------------------------------

TEST_F(QA_GDB694_TraceCollision, StringPkTargetTraceFailsCleanly) {
    exec_ok("CREATE TABLE docs (slug VARCHAR PRIMARY KEY, body VARCHAR)");
    exec_ok("INSERT INTO docs VALUES ('intro', 'Hello')");
    exec_ok("CREATE EDGE TYPE wrote FROM users TO docs");
    exec_ok("LINK users(1) TO docs('intro') VIA wrote");

    // Without TRACE the heterogeneous traversal works.
    auto qr = exec_ok("SELECT body, __node "
                      "FROM TRAVERSE wrote FROM users(1) DIRECTION OUT");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Hello");

    // With TRACE the string PK cannot be encoded into PathStep: clean error,
    // not a crash or a hang.
    auto err = exec_err("SELECT body, __node, __path "
                        "FROM TRAVERSE wrote FROM users(1) DIRECTION OUT WITH TRACE");
    EXPECT_EQ(err.code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(err.message.find("integer primary keys"), std::string::npos)
        << "actual message: " << err.message;
}

// ---------------------------------------------------------------------------
// Homogeneous regressions: the depth guard must never truncate valid paths
// ---------------------------------------------------------------------------

TEST_F(QA_GDB694_TraceCollision, HomogeneousCycleWithShortcutKeepsPathEqualToDepth) {
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(2) TO users(3) VIA follows");
    exec_ok("LINK users(3) TO users(1) VIA follows"); // cycle back to start
    exec_ok("LINK users(1) TO users(3) VIA follows"); // shortcut: 3 at depth 1

    auto qr = exec_ok("SELECT __node, __depth, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");

    // Nodes 2 and 3, both at depth 1 (3 via the shortcut, not the long route).
    ASSERT_EQ(qr.rows.size(), 2u);
    for (const auto& row : qr.rows) {
        const int64_t node = val_to_int64(row[0]);
        const int64_t depth = val_to_int64(row[1]);
        EXPECT_EQ(depth, 1) << "node " << node << " must be discovered via the shortest route";
        ASSERT_EQ(row[2].type_id(), TypeId::PATH);
        const Path& p = row[2].as_path();
        ASSERT_EQ(static_cast<int64_t>(p.steps.size()), depth + 1)
            << "path step count must equal depth + 1 for node " << node;
        EXPECT_EQ(p.steps.front().node_pk, 1);
        EXPECT_EQ(p.steps.back().node_pk, node);
    }
}

TEST_F(QA_GDB694_TraceCollision, HomogeneousSelfLoopAndBackEdgesStayBounded) {
    exec_ok("CREATE EDGE TYPE follows FROM users TO users");
    exec_ok("LINK users(1) TO users(1) VIA follows"); // self-loop on start
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(2) TO users(1) VIA follows"); // back edge
    exec_ok("LINK users(2) TO users(2) VIA follows"); // self-loop mid-graph

    auto qr = exec_ok("SELECT __node, __depth, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 1u) << "only users(2) is newly reachable";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 2);
    ASSERT_EQ(qr.rows[0][2].type_id(), TypeId::PATH);
    expect_two_step_path(qr.rows[0][2].as_path(), 1, 2);
}

// ---------------------------------------------------------------------------
// Stress: many colliding pairs
// ---------------------------------------------------------------------------

TEST_F(QA_GDB694_TraceCollision, ManyCollidingPairsAllTerminateWithCorrectPaths) {
    constexpr int64_t pairs = 40;
    for (int64_t i = 4; i <= pairs; ++i) {
        exec_ok("INSERT INTO users VALUES (" + std::to_string(i) + ", 'U" + std::to_string(i) +
                "')");
    }
    for (int64_t i = 1; i <= pairs; ++i) {
        exec_ok("INSERT INTO posts VALUES (" + std::to_string(i) + ", 'P" + std::to_string(i) +
                "')");
    }
    // users(i) -> posts(i) (direct collision) and users(i) -> posts(i+1)
    // (collision with the *next* user's PK).
    for (int64_t i = 1; i <= pairs; ++i) {
        exec_ok("LINK users(" + std::to_string(i) + ") TO posts(" + std::to_string(i) +
                ") VIA authored");
        if (i < pairs) {
            exec_ok("LINK users(" + std::to_string(i) + ") TO posts(" + std::to_string(i + 1) +
                    ") VIA authored");
        }
    }

    for (int64_t i = 1; i <= pairs; ++i) {
        auto qr = exec_ok("SELECT __node, __depth, __path FROM TRAVERSE authored FROM users(" +
                          std::to_string(i) + ") DIRECTION OUT WITH TRACE");
        const size_t expected = (i < pairs) ? 2u : 1u;
        ASSERT_EQ(qr.rows.size(), expected) << "start users(" << i << ")";
        for (const auto& row : qr.rows) {
            const int64_t node = val_to_int64(row[0]);
            EXPECT_TRUE(node == i || node == i + 1);
            EXPECT_EQ(val_to_int64(row[1]), 1);
            ASSERT_EQ(row[2].type_id(), TypeId::PATH);
            expect_two_step_path(row[2].as_path(), i, node);
        }
    }
}

// Edge-mode flavor of the stress scenario: every edge row's __path must be
// the trivial single-step [start] regardless of how many PKs collide.
TEST_F(QA_GDB694_TraceCollision, ManyCollidingPairsEdgeModeTrivialPaths) {
    constexpr int64_t pairs = 20;
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
        auto qr =
            exec_ok("SELECT __from, __to, __path FROM TRAVERSE authored FROM users(" +
                    std::to_string(i) + ") DIRECTION OUT MODE EDGES WITH TRACE");
        ASSERT_EQ(qr.rows.size(), 1u) << "start users(" << i << ")";
        EXPECT_EQ(val_to_int64(qr.rows[0][0]), i);
        EXPECT_EQ(val_to_int64(qr.rows[0][1]), i);
        ASSERT_EQ(qr.rows[0][2].type_id(), TypeId::PATH);
        const Path& p = qr.rows[0][2].as_path();
        ASSERT_EQ(p.steps.size(), 1u);
        EXPECT_EQ(p.steps[0].node_pk, i);
        EXPECT_EQ(p.steps[0].edge_id, -1);
    }
}

} // namespace
} // namespace sixseven
