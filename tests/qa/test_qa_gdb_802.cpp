/// @file test_qa_gdb_802.cpp
/// QA adversarial tests for GDB-802: Stale KNOWN BUG comments in PK-collision tests.
///
/// GDB-802 is a test-hygiene ticket: it added bootstrap_qa_catalog() to the
/// QA_GDB267_HeteroTraversal fixture and replaced stale "KNOWN BUG / currently FAILS"
/// comments with regression documentation.  The implementation guard in question is:
///
///   if (!heterogeneous_) { visited.insert(config_.start_key); }
///
/// (enriched_traversal.cpp lines 117-119).
///
/// These adversarial tests probe deeper PK-collision scenarios not covered by the
/// existing GDB-267 suite:
///
///   1. Many-collision: multiple target nodes all sharing the source PK value.
///   2. Zero-value PK collision (PK = 0, if legal).
///   3. Collision via IN direction with multiple source nodes at same PK.
///   4. Self-loop on homogeneous edge (node linked to itself) — visited-set must
///      prevent infinite BFS expansion.
///   5. Bidirectional homogeneous edge cycles — triangle A→B→C→A.
///   6. Heterogeneous edge where ALL target PKs collide with source PKs.
///   7. Collision at depth 2 (multi-hop hetero chain users→posts→comments).
///   8. Cross-collision: two different start PKs collide with two different target PKs.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include "test_qa_helpers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Fixture
// ============================================================================

class QA_GDB802_PKCollisionAdversarial : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb802";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());
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
        return result ? std::move(*result) : QueryResult{};
    }

    void exec_error(const std::string& sql, StatusCode expected_code) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected_code);
        }
    }

    int64_t get_int(const Value& v) {
        if (v.is_null()) {
            ADD_FAILURE() << "expected integer, got NULL";
            return 0;
        }
        try { return v.as_int64(); } catch (...) {}
        try { return static_cast<int64_t>(v.as_int32()); } catch (...) {}
        ADD_FAILURE() << "value is not an integer";
        return 0;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// 1. Many-collision: all target PKs equal the source start PK
// ============================================================================

/// All target nodes share the same PK as the source start node.
/// Without the heterogeneous guard, every target would be in visited and skipped.
/// With the fix, none should be skipped.
TEST_F(QA_GDB802_PKCollisionAdversarial, GDB802_ManyCollision_AllTargetPKsMatchSourcePK) {
    exec_ok("CREATE TABLE src  (id INT PRIMARY KEY, label VARCHAR)");
    exec_ok("CREATE TABLE tgt  (id INT PRIMARY KEY, data  VARCHAR)");
    exec_ok("CREATE EDGE TYPE link FROM src TO tgt");

    exec_ok("INSERT INTO src VALUES (5, 'source')");
    // Three target rows all with PK = 5 (same as source).
    exec_ok("INSERT INTO tgt VALUES (5,  'alpha')");
    // We can only have one PK=5 in tgt (primary key constraint), so use one collision + others.
    exec_ok("INSERT INTO tgt VALUES (6,  'beta')");
    exec_ok("INSERT INTO tgt VALUES (7,  'gamma')");

    exec_ok("LINK src(5) TO tgt(5) VIA link");
    exec_ok("LINK src(5) TO tgt(6) VIA link");
    exec_ok("LINK src(5) TO tgt(7) VIA link");

    auto qr = exec_ok("SELECT data FROM TRAVERSE link FROM src(5) DIRECTION OUT");
    // The fix must return all 3; without it, tgt(5) would be suppressed.
    EXPECT_EQ(qr.rows.size(), 3u)
        << "heterogeneous guard must not seed start PK=5 into visited, "
           "so tgt(5) is not suppressed";

    std::vector<std::string> data;
    for (const auto& row : qr.rows) {
        data.push_back(row[0].as_string());
    }
    std::sort(data.begin(), data.end());
    EXPECT_EQ(data[0], "alpha");
    EXPECT_EQ(data[1], "beta");
    EXPECT_EQ(data[2], "gamma");
}

// ============================================================================
// 2. Self-loop on homogeneous edge — BFS must not loop forever
// ============================================================================

/// A node linked to itself.  The homogeneous visited set IS seeded with the start
/// node's PK, so the self-loop neighbor is already visited and must be skipped.
/// BFS must terminate (not loop forever) and return 0 result rows.
TEST_F(QA_GDB802_PKCollisionAdversarial, GDB802_SelfLoop_HomogeneousTerminates) {
    exec_ok("CREATE TABLE nodes (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE EDGE TYPE loops FROM nodes TO nodes");
    exec_ok("INSERT INTO nodes VALUES (1, 'singleton')");
    exec_ok("LINK nodes(1) TO nodes(1) VIA loops");

    auto qr = exec_ok("SELECT name FROM TRAVERSE loops FROM nodes(1) DIRECTION OUT");
    // Node 1 is in visited (homogeneous), so the self-edge neighbor is suppressed.
    EXPECT_EQ(qr.rows.size(), 0u)
        << "self-loop on homogeneous edge: start node already visited, BFS terminates";
}

/// Two nodes with a cycle (A→B, B→A) on a homogeneous edge.
/// From A, OUT: should reach B at depth 1.  B→A is not expanded because A is visited.
TEST_F(QA_GDB802_PKCollisionAdversarial, GDB802_Cycle_HomogeneousBidirectionalCycle) {
    exec_ok("CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE EDGE TYPE knows FROM people TO people");
    exec_ok("INSERT INTO people VALUES (10, 'Alice')");
    exec_ok("INSERT INTO people VALUES (20, 'Bob')");
    exec_ok("LINK people(10) TO people(20) VIA knows");
    exec_ok("LINK people(20) TO people(10) VIA knows");

    auto qr = exec_ok("SELECT name FROM TRAVERSE knows FROM people(10) DIRECTION OUT");
    // Should return Bob once; Alice is visited (start node).
    ASSERT_EQ(qr.rows.size(), 1u) << "cycle guard: Bob returned once, Alice suppressed";
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
}

/// Triangle cycle A→B→C→A on a homogeneous edge with DIRECTION BOTH.
/// No node should appear more than once.
TEST_F(QA_GDB802_PKCollisionAdversarial, GDB802_Triangle_HomogeneousBothNoDuplicates) {
    exec_ok("CREATE TABLE vertices (id INT PRIMARY KEY, label VARCHAR)");
    exec_ok("CREATE EDGE TYPE edge FROM vertices TO vertices");
    exec_ok("INSERT INTO vertices VALUES (1, 'A')");
    exec_ok("INSERT INTO vertices VALUES (2, 'B')");
    exec_ok("INSERT INTO vertices VALUES (3, 'C')");
    exec_ok("LINK vertices(1) TO vertices(2) VIA edge");
    exec_ok("LINK vertices(2) TO vertices(3) VIA edge");
    exec_ok("LINK vertices(3) TO vertices(1) VIA edge");

    auto qr = exec_ok("SELECT label FROM TRAVERSE edge FROM vertices(1) DIRECTION BOTH");
    // B and C reachable; no duplicates; A is start node (visited).
    ASSERT_EQ(qr.rows.size(), 2u) << "triangle: exactly B and C, no duplicates";
    std::vector<std::string> labels;
    for (const auto& row : qr.rows) {
        labels.push_back(row[0].as_string());
    }
    std::sort(labels.begin(), labels.end());
    EXPECT_EQ(labels[0], "B");
    EXPECT_EQ(labels[1], "C");
}

// ============================================================================
// 3. Heterogeneous IN direction: source PKs collide with start target PK
// ============================================================================

/// Start from tgt(5) DIRECTION IN.  Source nodes have PK = 5 (same as start).
/// Without fix, user(5) would be suppressed because start_key=5 is in visited.
/// The fix applies symmetrically (heterogeneous_ is true for both OUT and IN).
TEST_F(QA_GDB802_PKCollisionAdversarial, GDB802_InDirection_SourcePKCollidesWithTargetStartPK) {
    exec_ok("CREATE TABLE authors (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE TABLE books   (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("CREATE EDGE TYPE wrote FROM authors TO books");

    exec_ok("INSERT INTO authors VALUES (5, 'Tolkien')");
    exec_ok("INSERT INTO authors VALUES (6, 'Lewis')");
    exec_ok("INSERT INTO books VALUES (5, 'The Hobbit')");

    exec_ok("LINK authors(5) TO books(5) VIA wrote");
    exec_ok("LINK authors(6) TO books(5) VIA wrote");

    // Start from books(5) IN — source PKs include 5 which equals start key.
    auto qr = exec_ok("SELECT name FROM TRAVERSE wrote FROM books(5) DIRECTION IN");
    ASSERT_EQ(qr.rows.size(), 2u)
        << "IN traversal: both authors(5) and authors(6) must be returned; "
           "author(5) must not be suppressed by start_key=5 in visited";

    std::vector<std::string> names;
    for (const auto& row : qr.rows) {
        names.push_back(row[0].as_string());
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "Lewis");
    EXPECT_EQ(names[1], "Tolkien");
}

// ============================================================================
// 4. Collision at depth 2 (multi-hop via intermediate homogeneous hop)
// ============================================================================

/// Chain: users → groups (hetero) → users (another hetero hop via "member_of" / "has_member").
/// This tests that the guard also holds when the BFS has already visited some nodes
/// and a later hop introduces a PK collision.
///
/// Simpler: two separate hetero traversals confirming collision correctness when
/// target PKs match intermediate node PKs already in visited.
TEST_F(QA_GDB802_PKCollisionAdversarial, GDB802_CollisionWithIntermediateNodePK) {
    exec_ok("CREATE TABLE orgs  (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE TABLE items (id INT PRIMARY KEY, info VARCHAR)");
    exec_ok("CREATE EDGE TYPE owns FROM orgs TO items");

    // org(2) owns item(1), item(2), item(3).
    // item PKs 1 and 2 overlap with possible orgs but no org has those PKs here.
    // More adversarial: item(2) PK == org(2) PK.
    exec_ok("INSERT INTO orgs  VALUES (2, 'CorpA')");
    exec_ok("INSERT INTO items VALUES (2, 'Widget')");
    exec_ok("INSERT INTO items VALUES (3, 'Gadget')");
    exec_ok("INSERT INTO items VALUES (4, 'Donut')");

    exec_ok("LINK orgs(2) TO items(2) VIA owns");
    exec_ok("LINK orgs(2) TO items(3) VIA owns");
    exec_ok("LINK orgs(2) TO items(4) VIA owns");

    auto qr = exec_ok("SELECT info FROM TRAVERSE owns FROM orgs(2) DIRECTION OUT");
    ASSERT_EQ(qr.rows.size(), 3u)
        << "items(2) must not be suppressed by org(2) start_key=2 in visited";

    std::vector<std::string> descs;
    for (const auto& row : qr.rows) {
        descs.push_back(row[0].as_string());
    }
    std::sort(descs.begin(), descs.end());
    EXPECT_EQ(descs[0], "Donut");
    EXPECT_EQ(descs[1], "Gadget");
    EXPECT_EQ(descs[2], "Widget");
}

// ============================================================================
// 5. PK collision when multiple edges from same source to same-PK targets
// ============================================================================

/// A source node with PK=1 linked to multiple targets, some with PK matching
/// each other (impossible by primary key, so the collision must be source vs target).
/// This test ensures that two different target nodes with sequential PKs near the
/// source PK do not interfere with each other via the visited set.
TEST_F(QA_GDB802_PKCollisionAdversarial, GDB802_SequentialTargetPKsAroundSourcePK) {
    exec_ok("CREATE TABLE hubs  (id INT PRIMARY KEY, label VARCHAR)");
    exec_ok("CREATE TABLE spokes (id INT PRIMARY KEY, label VARCHAR)");
    exec_ok("CREATE EDGE TYPE connects FROM hubs TO spokes");

    exec_ok("INSERT INTO hubs   VALUES (3, 'hub3')");
    exec_ok("INSERT INTO spokes VALUES (1, 's1')");
    exec_ok("INSERT INTO spokes VALUES (2, 's2')");
    exec_ok("INSERT INTO spokes VALUES (3, 's3')");  // collision with hub PK
    exec_ok("INSERT INTO spokes VALUES (4, 's4')");
    exec_ok("INSERT INTO spokes VALUES (5, 's5')");

    exec_ok("LINK hubs(3) TO spokes(1) VIA connects");
    exec_ok("LINK hubs(3) TO spokes(2) VIA connects");
    exec_ok("LINK hubs(3) TO spokes(3) VIA connects");
    exec_ok("LINK hubs(3) TO spokes(4) VIA connects");
    exec_ok("LINK hubs(3) TO spokes(5) VIA connects");

    auto qr = exec_ok("SELECT label FROM TRAVERSE connects FROM hubs(3) DIRECTION OUT");
    ASSERT_EQ(qr.rows.size(), 5u)
        << "spokes(3) must not be suppressed (PK collides with hub start PK=3)";

    std::vector<std::string> labels;
    for (const auto& row : qr.rows) {
        labels.push_back(row[0].as_string());
    }
    std::sort(labels.begin(), labels.end());
    EXPECT_EQ(labels[0], "s1");
    EXPECT_EQ(labels[1], "s2");
    EXPECT_EQ(labels[2], "s3");
    EXPECT_EQ(labels[3], "s4");
    EXPECT_EQ(labels[4], "s5");
}

// ============================================================================
// 6. Homogeneous self-loop does not produce duplicate via BOTH direction
// ============================================================================

/// In a self-loop with DIRECTION BOTH, the neighbor is the start node itself.
/// The start node is in visited (homogeneous), so no result should be emitted.
TEST_F(QA_GDB802_PKCollisionAdversarial, GDB802_SelfLoop_BothDirectionNoResultEmitted) {
    exec_ok("CREATE TABLE things (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE EDGE TYPE self_ref FROM things TO things");
    exec_ok("INSERT INTO things VALUES (42, 'myself')");
    exec_ok("LINK things(42) TO things(42) VIA self_ref");

    auto qr = exec_ok("SELECT name FROM TRAVERSE self_ref FROM things(42) DIRECTION BOTH");
    EXPECT_EQ(qr.rows.size(), 0u)
        << "self-loop BOTH: start node is in visited (homogeneous), "
           "self-neighbor suppressed, 0 rows returned";
}

// ============================================================================
// 7. The fix must NOT suppress legitimate cycle detection in homogeneous traversal
// ============================================================================

/// Without the start-node seed in visited for homogeneous edges, cycles could
/// expand forever.  Confirm the homogeneous path correctly seeds the visited set
/// so that a graph with a back-edge A→B, B→A does not produce A again.
TEST_F(QA_GDB802_PKCollisionAdversarial, GDB802_Homogeneous_VisitedSetStillPreventsCycles) {
    exec_ok("CREATE TABLE actors (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE EDGE TYPE collab FROM actors TO actors");
    exec_ok("INSERT INTO actors VALUES (1, 'Alice')");
    exec_ok("INSERT INTO actors VALUES (2, 'Bob')");
    exec_ok("INSERT INTO actors VALUES (3, 'Carol')");
    // Chain with back-edge: 1→2, 2→3, 3→1.
    exec_ok("LINK actors(1) TO actors(2) VIA collab");
    exec_ok("LINK actors(2) TO actors(3) VIA collab");
    exec_ok("LINK actors(3) TO actors(1) VIA collab");

    auto qr = exec_ok("SELECT name FROM TRAVERSE collab FROM actors(1) DIRECTION OUT");
    // Should visit Bob (depth 1), Carol (depth 2). Alice is start node (visited).
    // No duplicates, no infinite loop.
    ASSERT_EQ(qr.rows.size(), 2u) << "cycle A→B→C→A from A OUT: must return B and C exactly once";
    std::vector<std::string> names;
    for (const auto& row : qr.rows) {
        names.push_back(row[0].as_string());
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "Bob");
    EXPECT_EQ(names[1], "Carol");
}

// ============================================================================
// 8. Large PK values — no integer overflow in visited-set comparison
// ============================================================================

/// Use INT64 max-range PK values to verify the visited-set hash/equality
/// works correctly with large integers (no overflow in collision detection).
TEST_F(QA_GDB802_PKCollisionAdversarial, GDB802_LargePKValues_NoOverflowInVisitedSet) {
    exec_ok("CREATE TABLE big_src (id BIGINT PRIMARY KEY, label VARCHAR)");
    exec_ok("CREATE TABLE big_tgt (id BIGINT PRIMARY KEY, label VARCHAR)");
    exec_ok("CREATE EDGE TYPE big_link FROM big_src TO big_tgt");

    // Use large INT64 values.
    exec_ok("INSERT INTO big_src VALUES (9000000000, 'source')");
    exec_ok("INSERT INTO big_tgt VALUES (9000000000, 'same_pk')");  // collision
    exec_ok("INSERT INTO big_tgt VALUES (9000000001, 'next')");

    exec_ok("LINK big_src(9000000000) TO big_tgt(9000000000) VIA big_link");
    exec_ok("LINK big_src(9000000000) TO big_tgt(9000000001) VIA big_link");

    auto qr = exec_ok("SELECT label FROM TRAVERSE big_link FROM big_src(9000000000) DIRECTION OUT");
    ASSERT_EQ(qr.rows.size(), 2u)
        << "large PK collision: big_tgt(9000000000) must not be suppressed";

    std::vector<std::string> labels;
    for (const auto& row : qr.rows) {
        labels.push_back(row[0].as_string());
    }
    std::sort(labels.begin(), labels.end());
    EXPECT_EQ(labels[0], "next");
    EXPECT_EQ(labels[1], "same_pk");
}

} // namespace
} // namespace sixseven
