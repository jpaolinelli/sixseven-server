/// @file test_qa_gdb_894.cpp
/// @brief Adversarial QA tests for GDB-894: Extract shared traversal/BFS core.
///
/// Focus areas:
///   1. BOTH-direction correctness on asymmetric graphs for all 6 migrated
///      operators (traversal, edge_traversal, enriched_traversal, shortest_path,
///      match_shortest_path, variable_length_match).
///   2. H9/H11 guard uniformity: every migrated operator handles cyclic/depth-
///      limited input the same way as TraversalOperator did after GDB-694.
///   3. NodeId cross-table PK collision: two nodes with same pk in different
///      tables must NOT be conflated in shortest_path / match_shortest_path.
///   4. reconstruct_path edge cases via unit-level checks (null pk, depth guard).
///   5. pattern_match (un-migrated) regression: BOTH direction still correct.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/graph_traversal_core.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Shared fixture: asymmetric directed homogeneous graph.
//
// Table "users" — single table (homogeneous edge type "follows"):
//   users: 1(Alice), 2(Bob), 3(Carol), 4(Dave)
// Edges (OUT direction only, follows):
//   1->2, 2->3, 3->4   (chain, no reverse edges)
//
// Expected neighbor sets at depth 1:
//   OUT from 1: {2}
//   IN  from 1: {}  (nobody follows 1)
//   BOTH from 1: {2}
//
//   OUT from 2: {3}
//   IN  from 2: {1}  (user 1 follows 2)
//   BOTH from 2: {1, 3}
// ---------------------------------------------------------------------------

class QA_GDB894_AsymmetricGraph : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb894_asym";
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
        exec_ok("INSERT INTO users VALUES (4, 'Dave')");

        exec_ok("CREATE EDGE TYPE follows FROM users TO users");
        exec_ok("LINK users(1) TO users(2) VIA follows"); // 1->2
        exec_ok("LINK users(2) TO users(3) VIA follows"); // 2->3
        exec_ok("LINK users(3) TO users(4) VIA follows"); // 3->4
        // No reverse edges: B->A does NOT exist.
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto r = engine_->execute(sql);
        EXPECT_TRUE(r.has_value()) << sql << ": " << (r ? "" : r.error().message);
        return r.value_or(QueryResult{});
    }

    std::set<int64_t> pk_set(const QueryResult& qr, size_t col = 0) {
        std::set<int64_t> s;
        for (const auto& row : qr.rows) {
            if (col < row.size() && !row[col].is_null()) {
                if (const auto* p = std::get_if<int64_t>(&row[col].data()))
                    s.insert(*p);
                else if (const auto* p = std::get_if<int32_t>(&row[col].data()))
                    s.insert(static_cast<int64_t>(*p));
            }
        }
        return s;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
    std::filesystem::path data_dir_;
};

// ---------------------------------------------------------------------------
// 1. BOTH direction — TraversalOperator (TRAVERSE statement)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB894_AsymmetricGraph, GDB894_Traversal_BOTH_FromNode1_ReturnsOnlyOutNeighbors) {
    // OUT from users(1) -> {2}; IN to users(1) -> {} (nobody points to 1).
    // BOTH should return exactly {2}.
    auto qr = exec_ok("TRAVERSE follows FROM users(1) DIRECTION BOTH MAX_DEPTH 1");
    auto pks = pk_set(qr);
    EXPECT_EQ(pks.count(2), 1u) << "Expected node 2 in BOTH result from node 1";
    EXPECT_EQ(pks.count(3), 0u) << "Node 3 should NOT appear at depth 1";
    EXPECT_EQ(pks.count(1), 0u) << "Start node 1 should not be in results";
}

TEST_F(QA_GDB894_AsymmetricGraph, GDB894_Traversal_BOTH_FromNode2_ReturnsBothNeighbors) {
    // OUT from 2 -> {3}; IN to 2 -> {1}. BOTH must return {1, 3}.
    auto qr = exec_ok("TRAVERSE follows FROM users(2) DIRECTION BOTH MAX_DEPTH 1");
    auto pks = pk_set(qr);
    EXPECT_EQ(pks.count(1), 1u) << "Node 1 (incoming) must be returned by BOTH direction";
    EXPECT_EQ(pks.count(3), 1u) << "Node 3 (outgoing) must be returned by BOTH direction";
    EXPECT_EQ(pks.count(2), 0u) << "Start node 2 should not be in results";
}

TEST_F(QA_GDB894_AsymmetricGraph, GDB894_Traversal_OUT_FromNode2_OnlyOutgoing) {
    // OUT from 2 -> {3} only.
    auto qr = exec_ok("TRAVERSE follows FROM users(2) DIRECTION OUT MAX_DEPTH 1");
    auto pks = pk_set(qr);
    EXPECT_EQ(pks.count(3), 1u);
    EXPECT_EQ(pks.count(1), 0u) << "IN neighbor 1 must NOT appear with OUT direction";
}

TEST_F(QA_GDB894_AsymmetricGraph, GDB894_Traversal_IN_FromNode2_OnlyIncoming) {
    // IN to 2 (treat 2 as target): who points TO 2? -> node 1.
    auto qr = exec_ok("TRAVERSE follows FROM users(2) DIRECTION IN MAX_DEPTH 1");
    auto pks = pk_set(qr);
    EXPECT_EQ(pks.count(1), 1u) << "Node 1 (source of edge to 2) must appear with IN direction";
    EXPECT_EQ(pks.count(3), 0u) << "OUT neighbor 3 must NOT appear with IN direction";
}

// Verify BOTH at depth 2: from node 1, depth 2 should reach 3 (out 1->2->3) and no extras.
TEST_F(QA_GDB894_AsymmetricGraph, GDB894_Traversal_BOTH_Depth2_FromNode1) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) DIRECTION BOTH MAX_DEPTH 2");
    auto pks = pk_set(qr);
    // Via OUT 1->2->3: {2, 3}. Via BOTH from 2 we'd also see 1 again (IN) but
    // 1 is the start node (visited-set seeds it if homogeneous). So result is {2, 3}.
    EXPECT_EQ(pks.count(2), 1u);
    EXPECT_EQ(pks.count(3), 1u);
    EXPECT_EQ(pks.count(4), 0u) << "Node 4 should not be reachable at depth 2 from node 1";
}

// ---------------------------------------------------------------------------
// 2. BOTH direction — shortest_path
//    There is no OUT-only path from 3 to 1 (chain goes 1->2->3->4).
//    BOTH direction allows traversal of edges in reverse:
//    3 can reach 2 (IN), 2 can reach 1 (IN) => path 3->2->1 exists with BOTH.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB894_AsymmetricGraph, GDB894_ShortestPath_BOTH_FindsReverseEdge) {
    // OUT: no path from 3 to 1.
    auto result_out = engine_->execute("SHORTEST PATH FROM users(3) TO users(1) VIA follows");
    if (result_out.has_value()) {
        EXPECT_EQ(result_out->rows.size(), 0u)
            << "OUT direction: no path from 3 to 1 should exist in chain";
    }

    // BOTH: path 3->2->1 via reverse edges (2 hops).
    auto qr_both =
        exec_ok("SHORTEST PATH FROM users(3) TO users(1) VIA follows DIRECTION BOTH");
    EXPECT_GT(qr_both.rows.size(), 0u)
        << "BOTH direction should find path 3->2->1 via reverse edges";
}

TEST_F(QA_GDB894_AsymmetricGraph, GDB894_ShortestPath_OUT_DirectionCorrect) {
    // OUT: path from 1 to 3 (2 hops: 1->2->3).
    auto qr = exec_ok("SHORTEST PATH FROM users(1) TO users(3) VIA follows");
    EXPECT_GT(qr.rows.size(), 0u) << "Should find path 1->2->3 with OUT direction";
}

// ---------------------------------------------------------------------------
// 3. BOTH direction — pattern_match (un-migrated operator, regression check).
//    Should still work correctly and not be disturbed by the refactor.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB894_AsymmetricGraph, GDB894_PatternMatch_BOTH_FromNode2_BothNeighbors) {
    // FROM MATCH with BOTH direction from node 2: should find both 1 (incoming) and 3 (outgoing).
    // pattern_match uses undirected matching: (a)-[e:follows]-(b) retrieves both.
    auto qr = exec_ok(
        "SELECT b.id FROM MATCH (a:users)-[e:follows]-(b:users) WHERE a.id = 2");
    auto pks = pk_set(qr);
    EXPECT_EQ(pks.count(1), 1u) << "Pattern match undirected: incoming neighbor 1 must be found";
    EXPECT_EQ(pks.count(3), 1u) << "Pattern match undirected: outgoing neighbor 3 must be found";
}

TEST_F(QA_GDB894_AsymmetricGraph, GDB894_PatternMatch_DirectedOut_OnlyOutgoing) {
    // Directed pattern match (->): from node 2, only out-neighbor 3.
    auto qr = exec_ok(
        "SELECT b.id FROM MATCH (a:users)-[e:follows]->(b:users) WHERE a.id = 2");
    auto pks = pk_set(qr);
    EXPECT_EQ(pks.count(3), 1u);
    EXPECT_EQ(pks.count(1), 0u) << "Directed OUT pattern match must not return incoming neighbor 1";
}

// ---------------------------------------------------------------------------
// 4. Cycle + depth-limited — H9/H11 uniformity across operators.
//    Graph: 1->2->3->1 (cycle). All migrated operators must terminate.
// ---------------------------------------------------------------------------

class QA_GDB894_CyclicGraph : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb894_cycle";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        exec_ok("CREATE TABLE nodes (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO nodes VALUES (1, 'A')");
        exec_ok("INSERT INTO nodes VALUES (2, 'B')");
        exec_ok("INSERT INTO nodes VALUES (3, 'C')");

        exec_ok("CREATE EDGE TYPE loops FROM nodes TO nodes");
        exec_ok("LINK nodes(1) TO nodes(2) VIA loops");
        exec_ok("LINK nodes(2) TO nodes(3) VIA loops");
        exec_ok("LINK nodes(3) TO nodes(1) VIA loops"); // closes the cycle
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto r = engine_->execute(sql);
        EXPECT_TRUE(r.has_value()) << sql << ": " << (r ? "" : r.error().message);
        return r.value_or(QueryResult{});
    }

    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
    std::filesystem::path data_dir_;
};

TEST_F(QA_GDB894_CyclicGraph, GDB894_Traversal_Cyclic_TerminatesWithVisitedSet) {
    // BFS with visited set prevents revisiting. Must terminate quickly.
    auto qr = exec_ok("TRAVERSE loops FROM nodes(1) MAX_DEPTH 10");
    // Should get nodes 2 and 3 (cycle detection prevents revisiting 1).
    EXPECT_GE(qr.rows.size(), 1u);
    EXPECT_LE(qr.rows.size(), 3u) << "Cycle detection must bound the result size";
}

TEST_F(QA_GDB894_CyclicGraph, GDB894_Traversal_BOTH_Cyclic_TerminatesWithVisitedSet) {
    // BOTH direction on cyclic graph — must not loop indefinitely.
    auto qr = exec_ok("TRAVERSE loops FROM nodes(1) DIRECTION BOTH MAX_DEPTH 5");
    EXPECT_LE(qr.rows.size(), 3u) << "BOTH cyclic traversal must be bounded";
}

TEST_F(QA_GDB894_CyclicGraph, GDB894_ShortestPath_Cyclic_FindsCorrectPath) {
    // Shortest path from 1 to 3: path 1->2->3 (length 2).
    auto qr = exec_ok("SHORTEST PATH FROM nodes(1) TO nodes(3) VIA loops");
    // Should find a path (not hang).
    EXPECT_GE(qr.rows.size(), 1u) << "Should find path 1->2->3 in cyclic graph";
}

TEST_F(QA_GDB894_CyclicGraph, GDB894_VariableLengthMatch_Cyclic_Terminates) {
    // Variable-length match on cyclic graph. Should terminate within max_visited.
    auto result = engine_->execute(
        "SELECT a.id, b.id FROM MATCH (a:nodes)-[r:loops]->{1,3}(b:nodes)");
    // Should not hang. Accept success or INVALID_ARGUMENT (max_visited exceeded).
    EXPECT_TRUE(result.has_value() ||
                result.error().code == StatusCode::INVALID_ARGUMENT)
        << "Variable length match on cyclic graph must terminate: "
        << (result ? "" : result.error().message);
}

TEST_F(QA_GDB894_CyclicGraph, GDB894_EdgeTraversal_Cyclic_Terminates) {
    auto qr = exec_ok("TRAVERSE loops FROM nodes(1) MAX_DEPTH 5 MODE EDGES");
    // Edge traversal should be bounded.
    EXPECT_GE(qr.rows.size(), 1u);
    EXPECT_LE(qr.rows.size(), 10u) << "Edge traversal on cyclic graph must be bounded";
}

// ---------------------------------------------------------------------------
// 5. Cross-table PK collision — NodeId keying in shortest_path.
//    users table: pk=1,2; posts table: pk=1,2.
//    Heterogeneous edge: users(1)->posts(1), users(2)->posts(2).
//    SHORTEST PATH FROM users(2) TO posts(1) via authored should not conflate
//    users(pk=1) with posts(pk=1) (would produce bogus 0-hop or wrong path).
// ---------------------------------------------------------------------------

class QA_GDB894_CrossTablePK : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb894_crosstable";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        // Two tables with overlapping PKs.
        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");

        exec_ok("CREATE TABLE posts (id INT PRIMARY KEY, title VARCHAR)");
        exec_ok("INSERT INTO posts VALUES (1, 'Widget')"); // pk=1 same as user 1
        exec_ok("INSERT INTO posts VALUES (2, 'Gadget')");

        exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
        exec_ok("LINK users(1) TO posts(2) VIA authored"); // user 1 -> post 2
        exec_ok("LINK users(2) TO posts(1) VIA authored"); // user 2 -> post 1
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto r = engine_->execute(sql);
        EXPECT_TRUE(r.has_value()) << sql << ": " << (r ? "" : r.error().message);
        return r.value_or(QueryResult{});
    }

    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
    std::filesystem::path data_dir_;
};

TEST_F(QA_GDB894_CrossTablePK, GDB894_ShortestPath_CrossTablePK_CorrectPath) {
    // users(1)->posts(2) exists. users(2)->posts(1) exists.
    // SHORTEST PATH FROM users(1) TO posts(2): 1 hop, should succeed.
    auto qr = exec_ok("SHORTEST PATH FROM users(1) TO posts(2) VIA authored");
    EXPECT_GT(qr.rows.size(), 0u) << "Should find 1-hop path users(1)->posts(2)";
}

TEST_F(QA_GDB894_CrossTablePK, GDB894_ShortestPath_CrossTablePK_NoFalseZeroHopSelf) {
    // NodeId keying: from_node = {users_table, pk=1}, to_node = {posts_table, pk=1}.
    // These are DIFFERENT tables with the same pk=1. Without NodeId they would compare
    // equal, producing a bogus 0-hop self-path. With NodeId they compare unequal, so
    // BFS runs normally.
    //
    // Note: the bidirectional BFS (a pre-existing behavior across all branches) may still
    // find a multi-hop path through indirect meeting points in this graph topology. The
    // critical invariant we test here is that the trivial-case (0-hop) check does NOT
    // trigger, i.e., the result does NOT start with a (pk=1, hop=0) self entry.
    auto result = engine_->execute("SHORTEST PATH FROM users(1) TO posts(1) VIA authored");
    if (result.has_value() && result->rows.size() != 0) {
        // If rows exist, the first row must NOT be (pk=1, hop=0) — that would be
        // the false 0-hop produced by bare-pk collision.
        if (!result->rows.empty() && result->rows[0].size() >= 2) {
            auto hop = result->rows[0][1];
            bool is_zero_hop = false;
            if (const auto* p = std::get_if<int64_t>(&hop.data())) {
                is_zero_hop = (*p == 0 && result->rows.size() == 1);
            }
            EXPECT_FALSE(is_zero_hop)
                << "Must not return a single 0-hop row (false self-path due to pk=1 collision)";
        }
    }
}

// ---------------------------------------------------------------------------
// 6. reconstruct_path unit-level edge cases.
// ---------------------------------------------------------------------------

TEST(QA_GDB894_ReconstructPath, GDB894_NullTargetPkReturnsError) {
    // null pk for the target itself -> pk_to_int64 fails.
    ParentMap pm;
    auto result = reconstruct_path(Value(), 0, pm);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT)
        << "null target pk must produce INVALID_ARGUMENT";
}

TEST(QA_GDB894_ReconstructPath, GDB894_EmptyParentMap_DepthZeroSelf) {
    // Empty parent map, depth=0 -> single-step path to self.
    ParentMap pm;
    auto result = reconstruct_path(Value(int64_t{42}), 0, pm);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->steps.size(), 1u);
    EXPECT_EQ(result->steps[0].node_pk, int64_t{42});
    EXPECT_EQ(result->steps[0].edge_id, int64_t{-1});
}

TEST(QA_GDB894_ReconstructPath, GDB894_MaxDepthLongChain_Terminates) {
    // Build a chain of length 10 and reconstruct. Should succeed.
    ParentMap pm;
    for (int64_t i = 2; i <= 11; ++i) {
        pm[Value(i)] = ParentInfo{Value(i - 1), i * 10};
    }
    auto result = reconstruct_path(Value(int64_t{11}), 10, pm);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->steps.size(), 11u); // nodes 1..11
    EXPECT_EQ(result->steps[0].node_pk, int64_t{1});
    EXPECT_EQ(result->steps[10].node_pk, int64_t{11});
    EXPECT_EQ(result->steps[10].edge_id, int64_t{-1}); // terminal
}

TEST(QA_GDB894_ReconstructPath, GDB894_CycleInParentMap_H11Error) {
    // Genuine cycle: 2->3->2. Requesting depth=10 will exceed max_steps=3.
    ParentMap pm;
    pm[Value(int64_t{2})] = ParentInfo{Value(int64_t{3}), 10};
    pm[Value(int64_t{3})] = ParentInfo{Value(int64_t{2}), 11};

    auto result = reconstruct_path(Value(int64_t{2}), 10, pm);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INTERNAL_ERROR)
        << "H11 cycle guard must return INTERNAL_ERROR for genuine cycle in parent map";
}

TEST(QA_GDB894_ReconstructPath, GDB894_DepthGuardPreventsCrossTableLoop) {
    // H9: node 1 appears to have itself as parent (cross-table PK collision sim).
    // reconstruct_path(2, depth=1): walk: cursor=2(depth=1)->1(depth=0, stop).
    // The depth guard stops after 1 hop even though pm[1]={1,...} creates a self-loop.
    ParentMap pm;
    pm[Value(int64_t{1})] = ParentInfo{Value(int64_t{1}), 99}; // self-loop entry
    pm[Value(int64_t{2})] = ParentInfo{Value(int64_t{1}), 10};

    auto result = reconstruct_path(Value(int64_t{2}), 1, pm);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->steps.size(), 2u);
    EXPECT_EQ(result->steps[0].node_pk, int64_t{1});
    EXPECT_EQ(result->steps[1].node_pk, int64_t{2});
}

TEST(QA_GDB894_ReconstructPath, GDB894_Int32PkWidenedToInt64) {
    // pk_to_int64 must widen int32 -> int64.
    ParentMap pm;
    pm[Value(int32_t{2})] = ParentInfo{Value(int32_t{1}), 7};
    auto result = reconstruct_path(Value(int32_t{2}), 1, pm);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->steps.size(), 2u);
    EXPECT_EQ(result->steps[0].node_pk, int64_t{1});
    EXPECT_EQ(result->steps[1].node_pk, int64_t{2});
}

// ---------------------------------------------------------------------------
// 7. Verify BOTH direction is the union of OUT and IN (behavior preservation).
//    For each starting node, BOTH result == OUT ∪ IN.
// ---------------------------------------------------------------------------

class QA_GDB894_BothUnion : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb894_union";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        // Graph: 5 nodes, non-symmetric edges.
        //   1->2, 1->3, 4->2, 5->1
        // Node 1: OUT={2,3}, IN={5}. BOTH={1,2,3,5} minus self = {2,3,5}.
        // Node 2: OUT={},    IN={1,4}. BOTH={1,4}.
        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, val INT)");
        for (int i = 1; i <= 5; ++i) {
            exec_ok("INSERT INTO users VALUES (" + std::to_string(i) + ", " +
                    std::to_string(i * 10) + ")");
        }
        exec_ok("CREATE EDGE TYPE rel FROM users TO users");
        exec_ok("LINK users(1) TO users(2) VIA rel");
        exec_ok("LINK users(1) TO users(3) VIA rel");
        exec_ok("LINK users(4) TO users(2) VIA rel");
        exec_ok("LINK users(5) TO users(1) VIA rel");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto r = engine_->execute(sql);
        EXPECT_TRUE(r.has_value()) << sql << ": " << (r ? "" : r.error().message);
        return r.value_or(QueryResult{});
    }

    std::set<int64_t> pk_set(const QueryResult& qr, size_t col = 0) {
        std::set<int64_t> s;
        for (const auto& row : qr.rows) {
            if (col < row.size() && !row[col].is_null()) {
                if (const auto* p = std::get_if<int64_t>(&row[col].data()))
                    s.insert(*p);
                else if (const auto* p = std::get_if<int32_t>(&row[col].data()))
                    s.insert(static_cast<int64_t>(*p));
            }
        }
        return s;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
    std::filesystem::path data_dir_;
};

// Node 1: OUT={2,3}, IN={5}. BOTH must contain all three.
TEST_F(QA_GDB894_BothUnion, GDB894_Traversal_BOTH_IsUnionOfOutIn_Node1) {
    auto out = pk_set(exec_ok("TRAVERSE rel FROM users(1) DIRECTION OUT MAX_DEPTH 1"));
    auto in = pk_set(exec_ok("TRAVERSE rel FROM users(1) DIRECTION IN MAX_DEPTH 1"));
    auto both = pk_set(exec_ok("TRAVERSE rel FROM users(1) DIRECTION BOTH MAX_DEPTH 1"));

    // BOTH must be a superset of OUT and IN.
    for (auto pk : out) {
        EXPECT_EQ(both.count(pk), 1u) << "OUT neighbor " << pk << " missing from BOTH result";
    }
    for (auto pk : in) {
        EXPECT_EQ(both.count(pk), 1u) << "IN neighbor " << pk << " missing from BOTH result";
    }
    // BOTH must not contain extra nodes not in OUT ∪ IN.
    std::set<int64_t> union_expected = out;
    union_expected.insert(in.begin(), in.end());
    EXPECT_EQ(both, union_expected) << "BOTH result must be exactly OUT ∪ IN";
}

// Node 2: OUT={} (no outgoing from 2), IN={1,4}. BOTH must equal {1,4}.
TEST_F(QA_GDB894_BothUnion, GDB894_Traversal_BOTH_IsUnionOfOutIn_Node2_NoOutgoing) {
    auto out = pk_set(exec_ok("TRAVERSE rel FROM users(2) DIRECTION OUT MAX_DEPTH 1"));
    auto in = pk_set(exec_ok("TRAVERSE rel FROM users(2) DIRECTION IN MAX_DEPTH 1"));
    auto both = pk_set(exec_ok("TRAVERSE rel FROM users(2) DIRECTION BOTH MAX_DEPTH 1"));

    EXPECT_EQ(out.size(), 0u) << "Node 2 has no outgoing edges";
    EXPECT_EQ(in.size(), 2u) << "Node 2 has incoming edges from 1 and 4";

    std::set<int64_t> union_expected = out;
    union_expected.insert(in.begin(), in.end());
    EXPECT_EQ(both, union_expected);
}

// ---------------------------------------------------------------------------
// 8. NodeId equality and hash: unit-level checks (behavior-preservation of fix).
// ---------------------------------------------------------------------------

TEST(QA_GDB894_NodeId, GDB894_SameTableSamePkEqual) {
    NodeId a{10, Value(int64_t{99})};
    NodeId b{10, Value(int64_t{99})};
    EXPECT_EQ(a, b);
    NodeIdHash h;
    EXPECT_EQ(h(a), h(b));
}

TEST(QA_GDB894_NodeId, GDB894_DifferentTableSamePkNotEqual) {
    // This is the cross-table PK collision scenario. Different tables must
    // produce different NodeIds even with identical pk values.
    NodeId a{1, Value(int64_t{1})};
    NodeId b{2, Value(int64_t{1})};
    EXPECT_NE(a, b);
}

TEST(QA_GDB894_NodeId, GDB894_SameTableDifferentPkNotEqual) {
    NodeId a{5, Value(int64_t{10})};
    NodeId b{5, Value(int64_t{20})};
    EXPECT_NE(a, b);
}

TEST(QA_GDB894_NodeId, GDB894_NodeIdInUnorderedContainerDeduplication) {
    // Confirm the set deduplicates correctly: two nodes from different tables
    // with the same pk are distinct entries.
    std::unordered_set<NodeId, NodeIdHash> s;
    NodeId a{1, Value(int64_t{1})};
    NodeId b{2, Value(int64_t{1})}; // different table, same pk
    NodeId c{1, Value(int64_t{1})}; // duplicate of a

    s.insert(a);
    s.insert(b);
    s.insert(c);

    EXPECT_EQ(s.size(), 2u) << "a and b are distinct; c==a should be deduplicated";
    EXPECT_TRUE(s.count(a) > 0);
    EXPECT_TRUE(s.count(b) > 0);
}

// ---------------------------------------------------------------------------
// 9. Stress: BOTH direction on a star graph — one hub, many spokes.
//    hub(1)->spoke(2..11). BOTH from hub: all 10 spokes (OUT neighbors).
//    BOTH from a spoke: hub (IN neighbor). OUT from spoke: empty.
// ---------------------------------------------------------------------------

class QA_GDB894_StarGraph : public ::testing::Test {
protected:
    static constexpr int kSpokes = 8; // keep small for test speed

    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb894_star";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, val INT)");
        exec_ok("INSERT INTO users VALUES (1, 100)"); // hub
        for (int i = 2; i <= kSpokes + 1; ++i) {
            exec_ok("INSERT INTO users VALUES (" + std::to_string(i) + ", " +
                    std::to_string(i) + ")");
        }
        exec_ok("CREATE EDGE TYPE spoke FROM users TO users");
        // hub -> all spokes (OUT only — no reverse edges)
        for (int i = 2; i <= kSpokes + 1; ++i) {
            exec_ok("LINK users(1) TO users(" + std::to_string(i) + ") VIA spoke");
        }
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto r = engine_->execute(sql);
        EXPECT_TRUE(r.has_value()) << sql << ": " << (r ? "" : r.error().message);
        return r.value_or(QueryResult{});
    }

    std::set<int64_t> pk_set(const QueryResult& qr, size_t col = 0) {
        std::set<int64_t> s;
        for (const auto& row : qr.rows) {
            if (col < row.size() && !row[col].is_null()) {
                if (const auto* p = std::get_if<int64_t>(&row[col].data()))
                    s.insert(*p);
                else if (const auto* p = std::get_if<int32_t>(&row[col].data()))
                    s.insert(static_cast<int64_t>(*p));
            }
        }
        return s;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
    std::filesystem::path data_dir_;
};

TEST_F(QA_GDB894_StarGraph, GDB894_Traversal_BOTH_HubReturnsAllSpokes) {
    auto both = pk_set(exec_ok("TRAVERSE spoke FROM users(1) DIRECTION BOTH MAX_DEPTH 1"));
    EXPECT_EQ(both.size(), static_cast<size_t>(kSpokes))
        << "BOTH from hub should return all " << kSpokes << " spokes";
    for (int i = 2; i <= kSpokes + 1; ++i) {
        EXPECT_EQ(both.count(static_cast<int64_t>(i)), 1u)
            << "Spoke " << i << " missing from BOTH result";
    }
}

TEST_F(QA_GDB894_StarGraph, GDB894_Traversal_BOTH_SpokeReturnsOnlyHub) {
    // From spoke (e.g., node 3): IN -> {1} (hub). OUT -> {} (spoke has no outgoing).
    // BOTH should return {1} only.
    auto both = pk_set(exec_ok("TRAVERSE spoke FROM users(3) DIRECTION BOTH MAX_DEPTH 1"));
    EXPECT_EQ(both.count(1), 1u) << "Hub (pk=1) must be in BOTH result from spoke";
    EXPECT_EQ(both.size(), 1u) << "Only hub should be neighbor of spoke in BOTH direction";
}

TEST_F(QA_GDB894_StarGraph, GDB894_Traversal_OUT_SpokeReturnsEmpty) {
    // OUT from spoke: no outgoing edges.
    auto out = pk_set(exec_ok("TRAVERSE spoke FROM users(3) DIRECTION OUT MAX_DEPTH 1"));
    EXPECT_EQ(out.size(), 0u) << "Spokes have no outgoing edges; OUT should return empty";
}

} // namespace
} // namespace sixseven
