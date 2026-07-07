/// @file test_qa_gdb_696.cpp
/// QA adversarial tests for GDB-696: standalone TRAVERSE must not drop
/// heterogeneous targets whose PK equals the start PK. The fix threads a
/// `heterogeneous` flag from Planner::plan_traverse() into TraversalConfig and
/// skips seeding the BFS visited set with the start key in
/// TraversalOperator::run_bfs() (mirroring GDB-304).
///
/// Adversarial coverage beyond the dev suite (test_heterogeneous_traversal.cpp):
///   * exact ticket repro plus consistency with the SELECT ... FROM TRAVERSE form
///   * IN-direction collisions with multiple colliding sources
///   * FETCH and FETCH + WITH TRACE combined under collision
///   * TRACE path edge ids (outgoing edge on start step, -1 terminal)
///   * duplicate parallel edges must still dedupe (visited set intact)
///   * explicit MAX_DEPTH must not let PK-aliasing chains escape the
///     heterogeneous depth-1 cap now that the seed is gone
///   * MAX_DEPTH 0 boundary
///   * WHERE post-filters (GDB-695) interacting with the colliding row
///   * homogeneous regressions: self-loop and cycle back-edges must still be
///     suppressed by the visited-set seed
///   * DIRECTION BOTH on the standalone form (accepted, unlike FROM TRAVERSE):
///     bounded, terminating, no duplicate emission
///   * empty edge sets and nonexistent start keys
///   * stress: many colliding pairs, every traversal bounded and correct

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

/// Full SQL-pipeline fixture: users (1..3), posts, the heterogeneous edge
/// `authored FROM users TO posts`, and the homogeneous edge
/// `follows FROM users TO users`. Posts rows and links are inserted per test
/// so each scenario controls its own collisions.
class QA_GDB696_StandaloneTraverse : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb696";
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

    static int64_t val_to_int64(const Value& v) {
        if (v.type_id() == TypeId::INT32) {
            return v.as_int32();
        }
        return v.as_int64();
    }

    /// Collect column 0 (node) of every row as a sorted vector of int64.
    static std::vector<int64_t> sorted_nodes(const QueryResult& qr) {
        std::vector<int64_t> nodes;
        nodes.reserve(qr.rows.size());
        for (const auto& row : qr.rows) {
            nodes.push_back(val_to_int64(row[0]));
        }
        std::sort(nodes.begin(), nodes.end());
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
// Acceptance: exact ticket repro (OUT) and the symmetric IN case
// ---------------------------------------------------------------------------

// Ticket repro: users(1) authored posts(1) and posts(10). The colliding target
// posts(1) must be emitted, not silently dropped as "already visited".
TEST_F(QA_GDB696_StandaloneTraverse, OutCollisionEmitsBothTargets) {
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");
    exec_ok("INSERT INTO posts VALUES (10, 'Plain')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(1) TO posts(10) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM users(1)");

    ASSERT_EQ(qr.rows.size(), 2u) << "colliding target posts(1) must not be dropped";
    EXPECT_EQ(sorted_nodes(qr), (std::vector<int64_t>{1, 10}));
    for (const auto& row : qr.rows) {
        EXPECT_EQ(val_to_int64(row[1]), 1) << "all heterogeneous targets are at depth 1";
    }
}

// Symmetric IN case from the ticket: TRAVERSE authored FROM posts(1)
// DIRECTION IN must emit users(1) despite the PK collision.
TEST_F(QA_GDB696_StandaloneTraverse, InCollisionEmitsCollidingSource) {
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM posts(1) DIRECTION IN");

    ASSERT_EQ(qr.rows.size(), 1u) << "colliding source users(1) must not be dropped";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 1);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 1);
}

// The two query forms must agree on the same graph: standalone TRAVERSE node
// set == SELECT __node FROM TRAVERSE ... DIRECTION OUT.
TEST_F(QA_GDB696_StandaloneTraverse, AgreesWithFromTraverseForm) {
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");
    exec_ok("INSERT INTO posts VALUES (10, 'Plain')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(1) TO posts(10) VIA authored");

    auto standalone = exec_ok("TRAVERSE authored FROM users(1)");
    auto from_form = exec_ok("SELECT __node FROM TRAVERSE authored FROM users(1) DIRECTION OUT");

    EXPECT_EQ(sorted_nodes(standalone), sorted_nodes(from_form))
        << "standalone TRAVERSE and FROM TRAVERSE disagree on the same graph";
}

// ---------------------------------------------------------------------------
// IN-direction: multiple colliding sources
// ---------------------------------------------------------------------------

TEST_F(QA_GDB696_StandaloneTraverse, InDirectionMultipleSourcesIncludingCollider) {
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(2) TO posts(1) VIA authored");
    exec_ok("LINK users(3) TO posts(1) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM posts(1) DIRECTION IN");

    ASSERT_EQ(qr.rows.size(), 3u) << "all authors including the colliding users(1) must appear";
    EXPECT_EQ(sorted_nodes(qr), (std::vector<int64_t>{1, 2, 3}));
}

// ---------------------------------------------------------------------------
// FETCH and FETCH + TRACE under collision
// ---------------------------------------------------------------------------

TEST_F(QA_GDB696_StandaloneTraverse, FetchCollisionEmitsNodeDepthSource) {
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");
    exec_ok("INSERT INTO posts VALUES (10, 'Plain')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(1) TO posts(10) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM users(1) FETCH");

    ASSERT_EQ(qr.rows.size(), 2u);
    bool saw_collider = false;
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row.size(), 3u) << "FETCH emits node, depth, source";
        EXPECT_EQ(val_to_int64(row[1]), 1);
        EXPECT_EQ(val_to_int64(row[2]), 1) << "source of every depth-1 row is the start node";
        if (val_to_int64(row[0]) == 1) {
            saw_collider = true;
        }
    }
    EXPECT_TRUE(saw_collider) << "FETCH must include the colliding target";
}

TEST_F(QA_GDB696_StandaloneTraverse, FetchWithTraceCollisionEmitsAllColumns) {
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");
    exec_ok("INSERT INTO posts VALUES (10, 'Plain')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(1) TO posts(10) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM users(1) FETCH WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 2u);
    bool saw_collider = false;
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row.size(), 4u) << "FETCH + TRACE emits node, depth, source, __path";
        EXPECT_EQ(val_to_int64(row[1]), 1);
        EXPECT_EQ(val_to_int64(row[2]), 1);
        ASSERT_EQ(row[3].type_id(), TypeId::PATH);
        const Path& p = row[3].as_path();
        ASSERT_EQ(p.steps.size(), 2u) << "path must be start -> target, not a self-chain";
        EXPECT_EQ(p.steps[0].node_pk_as_int64(), 1);
        EXPECT_EQ(p.steps[1].node_pk_as_int64(), val_to_int64(row[0]));
        if (val_to_int64(row[0]) == 1) {
            saw_collider = true;
        }
    }
    EXPECT_TRUE(saw_collider);
}

// TRACE path edge ids must be well-formed for the colliding row: the start
// step carries a real outgoing edge id and the terminal step carries -1.
TEST_F(QA_GDB696_StandaloneTraverse, TraceCollisionPathEdgeIdsWellFormed) {
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM users(1) WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.rows[0][2].type_id(), TypeId::PATH);
    const Path& p = qr.rows[0][2].as_path();
    ASSERT_EQ(p.steps.size(), 2u);
    EXPECT_EQ(p.steps[0].node_pk_as_int64(), 1);
    EXPECT_EQ(p.steps[1].node_pk_as_int64(), 1);
    EXPECT_GE(p.steps[0].edge_id, 0) << "start step must carry the outgoing edge id";
    EXPECT_EQ(p.steps[1].edge_id, -1) << "terminal step has no outgoing edge";
}

// ---------------------------------------------------------------------------
// Visited-set integrity: dedupe must survive the removed seed
// ---------------------------------------------------------------------------

// Two parallel edges to the same colliding target: the target must be emitted
// exactly once (the visited set still dedupes targets among themselves).
TEST_F(QA_GDB696_StandaloneTraverse, DuplicateParallelEdgesEmitCollidingTargetOnce) {
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM users(1)");

    ASSERT_EQ(qr.rows.size(), 1u) << "parallel edges must not duplicate the target row";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 1);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 1);
}

// ---------------------------------------------------------------------------
// Depth boundaries: aliasing chains must not escape the heterogeneous cap
// ---------------------------------------------------------------------------

// With the seed gone, node pk 1 re-enters the BFS queue as a depth-1 result.
// Edge adjacency is keyed by PK only, so expanding it would walk users(1)'s
// outgoing edges again at depth 2. The planner's heterogeneous depth-1 cap
// must prevent that even when the user asks for MAX_DEPTH 5.
TEST_F(QA_GDB696_StandaloneTraverse, ExplicitMaxDepthCannotEscapeHeterogeneousCap) {
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");
    exec_ok("INSERT INTO posts VALUES (10, 'Plain')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(1) TO posts(10) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM users(1) MAX_DEPTH 5");

    ASSERT_EQ(qr.rows.size(), 2u) << "PK-aliasing chain must not add depth-2 rows";
    for (const auto& row : qr.rows) {
        EXPECT_EQ(val_to_int64(row[1]), 1) << "no row may exceed the heterogeneous depth cap";
    }
}

// MAX_DEPTH 0 boundary: the start node is never emitted and nothing expands.
TEST_F(QA_GDB696_StandaloneTraverse, MaxDepthZeroEmitsNothing) {
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM users(1) MAX_DEPTH 0");

    EXPECT_EQ(qr.rows.size(), 0u);
}

// ---------------------------------------------------------------------------
// WHERE post-filters (GDB-695) interacting with the colliding row
// ---------------------------------------------------------------------------

TEST_F(QA_GDB696_StandaloneTraverse, WhereFilterSelectsCollidingNode) {
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");
    exec_ok("INSERT INTO posts VALUES (10, 'Plain')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(1) TO posts(10) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM users(1) WHERE node = 1");

    ASSERT_EQ(qr.rows.size(), 1u) << "WHERE node = 1 must match the colliding target";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 1);
}

TEST_F(QA_GDB696_StandaloneTraverse, WhereFilterExcludesCollidingNode) {
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");
    exec_ok("INSERT INTO posts VALUES (10, 'Plain')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(1) TO posts(10) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM users(1) WHERE node <> 1");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 10);
}

// ---------------------------------------------------------------------------
// Homogeneous regressions: the visited-set seed must still apply
// ---------------------------------------------------------------------------

// Self-loop on a homogeneous edge: the start node must not be emitted as its
// own depth-1 neighbor (seed retained when heterogeneous == false).
TEST_F(QA_GDB696_StandaloneTraverse, HomogeneousSelfLoopStillSuppressed) {
    exec_ok("LINK users(1) TO users(1) VIA follows");

    auto qr = exec_ok("TRAVERSE follows FROM users(1)");

    EXPECT_EQ(qr.rows.size(), 0u) << "homogeneous self-loop must not re-emit the start node";
}

// Cycle back to the start on a homogeneous edge: 1 -> 2 -> 1. Node 2 is
// emitted once; the back-edge to the seeded start is suppressed.
TEST_F(QA_GDB696_StandaloneTraverse, HomogeneousCycleBackEdgeStillSuppressed) {
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(2) TO users(1) VIA follows");

    auto qr = exec_ok("TRAVERSE follows FROM users(1)");

    ASSERT_EQ(qr.rows.size(), 1u) << "cycle back-edge must not re-emit the start node";
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 2);
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 1);
}

// Homogeneous TRACE on the same cycle: still one row with a clean 2-step path.
TEST_F(QA_GDB696_StandaloneTraverse, HomogeneousCycleTraceStillBounded) {
    exec_ok("LINK users(1) TO users(2) VIA follows");
    exec_ok("LINK users(2) TO users(1) VIA follows");

    auto qr = exec_ok("TRAVERSE follows FROM users(1) WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.rows[0][2].type_id(), TypeId::PATH);
    const Path& p = qr.rows[0][2].as_path();
    ASSERT_EQ(p.steps.size(), 2u);
    EXPECT_EQ(p.steps[0].node_pk_as_int64(), 1);
    EXPECT_EQ(p.steps[1].node_pk_as_int64(), 2);
}

// ---------------------------------------------------------------------------
// DIRECTION BOTH on the standalone form
// ---------------------------------------------------------------------------

// Unlike the FROM TRAVERSE form (which rejects DIRECTION BOTH on heterogeneous
// edges in the binder), the standalone form accepts it. With the seed removed
// the IN-side neighbor of the start (its own PK) collides with the OUT-side
// target. Pin the behavior: bounded, terminating, no duplicate rows, and the
// node set equals the OUT result.
TEST_F(QA_GDB696_StandaloneTraverse, DirectionBothHeterogeneousBoundedNoDuplicates) {
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");
    exec_ok("INSERT INTO posts VALUES (10, 'Plain')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");
    exec_ok("LINK users(1) TO posts(10) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM users(1) DIRECTION BOTH");

    EXPECT_EQ(sorted_nodes(qr), (std::vector<int64_t>{1, 10}))
        << "DIRECTION BOTH must terminate with the OUT node set, no duplicates";
    for (const auto& row : qr.rows) {
        EXPECT_EQ(val_to_int64(row[1]), 1);
    }
}

// DIRECTION BOTH with TRACE under collision: every emitted path is a bounded
// two-step path (the GDB-694 depth guard keeps reconstruction cycle-free).
TEST_F(QA_GDB696_StandaloneTraverse, DirectionBothHeterogeneousTraceBoundedPaths) {
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM users(1) DIRECTION BOTH WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.rows[0][2].type_id(), TypeId::PATH);
    const Path& p = qr.rows[0][2].as_path();
    ASSERT_EQ(p.steps.size(), 2u) << "collision path must stay bounded under BOTH";
    EXPECT_EQ(p.steps[0].node_pk_as_int64(), 1);
    EXPECT_EQ(p.steps[1].node_pk_as_int64(), 1);
}

// ---------------------------------------------------------------------------
// Empty edge sets and nonexistent start keys
// ---------------------------------------------------------------------------

TEST_F(QA_GDB696_StandaloneTraverse, NoEdgesZeroRows) {
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");

    auto qr = exec_ok("TRAVERSE authored FROM users(1)");

    EXPECT_EQ(qr.rows.size(), 0u);
}

TEST_F(QA_GDB696_StandaloneTraverse, NonexistentStartKeyZeroRows) {
    exec_ok("INSERT INTO posts VALUES (1, 'Hello')");
    exec_ok("LINK users(1) TO posts(1) VIA authored");

    auto qr = exec_ok("TRAVERSE authored FROM users(999)");

    EXPECT_EQ(qr.rows.size(), 0u) << "unlinked start key must yield no rows, not an error";
}

// ---------------------------------------------------------------------------
// Stress: many colliding pairs
// ---------------------------------------------------------------------------

// 100 users(i) -> posts(i) colliding pairs. Every standalone traversal must
// emit exactly its own colliding target at depth 1 — bounded and correct.
TEST_F(QA_GDB696_StandaloneTraverse, StressManyCollidingPairs) {
    const int64_t pairs = 100;
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
        auto qr = exec_ok("TRAVERSE authored FROM users(" + std::to_string(i) + ")");
        ASSERT_EQ(qr.rows.size(), 1u) << "start users(" << i << ")";
        EXPECT_EQ(val_to_int64(qr.rows[0][0]), i);
        EXPECT_EQ(val_to_int64(qr.rows[0][1]), 1);
    }
}

} // namespace
} // namespace sixseven
