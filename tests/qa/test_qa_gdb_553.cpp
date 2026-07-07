/// @file test_qa_gdb_553.cpp
/// QA adversarial tests for GDB-553: plan_from_source() drops variable-length
/// quantifiers for FROM MATCH queries.
///
/// Verifies: FROM MATCH with variable-length edges uses
/// VariableLengthMatchOperator and returns multi-hop results.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "test_qa_helpers.h"

namespace sixseven {
namespace {

// ============================================================================
// Fixture: Query engine with a chain graph
// ============================================================================

class QA_GDB553 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb553";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        bootstrap_qa_catalog(catalog_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        // Create table + edge type + chain graph 1->2->3->4->5->6
        exec_ok("CREATE TABLE persons (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO persons VALUES (1, 'Alice')");
        exec_ok("INSERT INTO persons VALUES (2, 'Bob')");
        exec_ok("INSERT INTO persons VALUES (3, 'Charlie')");
        exec_ok("INSERT INTO persons VALUES (4, 'Diana')");
        exec_ok("INSERT INTO persons VALUES (5, 'Eve')");
        exec_ok("INSERT INTO persons VALUES (6, 'Frank')");
        exec_ok("CREATE EDGE TYPE knows FROM persons TO persons");
        exec_ok("LINK persons(1) TO persons(2) VIA knows");
        exec_ok("LINK persons(2) TO persons(3) VIA knows");
        exec_ok("LINK persons(3) TO persons(4) VIA knows");
        exec_ok("LINK persons(4) TO persons(5) VIA knows");
        exec_ok("LINK persons(5) TO persons(6) VIA knows");
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

    void exec_fail(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// Core fix: FROM MATCH with {min,max} now produces multi-hop results
// ============================================================================

TEST_F(QA_GDB553, FromMatchVariableLength_ReturnsMultiHopResults) {
    // FROM MATCH with {1,3} should return 1, 2, and 3 hop results.
    // Starting from node 1: should reach 2 (1 hop), 3 (2 hops), 4 (3 hops).
    auto result = exec_ok("SELECT a.id, b.id FROM MATCH (a:persons)-[r:knows]->{1,3}(b:persons) "
                          "WHERE a.id = 1");
    EXPECT_GE(result.rows.size(), 3u)
        << "Variable-length {1,3} from node 1 should return at least 3 results";
}

TEST_F(QA_GDB553, FromMatchVariableLength_ExactHops) {
    // {2,2} should return exactly 2-hop results.
    auto result = exec_ok("SELECT a.id, b.id FROM MATCH (a:persons)-[r:knows]->{2,2}(b:persons) "
                          "WHERE a.id = 1");
    // From node 1: 2 hops reaches node 3 (1->2->3). Should get exactly 1
    // result, with b.id == 3. If the {2,2} quantifier is dropped (the
    // GDB-553 regression) the planner falls back to a single-hop match,
    // which also returns exactly 1 row but with b.id == 2 (the 1-hop
    // target) -- asserting the actual b.id value catches that regression,
    // which a bare row-count check cannot.
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0].as_int32(), 1);
    EXPECT_EQ(result.rows[0][1].as_int32(), 3);
}

TEST_F(QA_GDB553, FromMatchVariableLength_LargeRange) {
    // {1,6} should reach all nodes in the chain from node 1.
    auto result = exec_ok("SELECT a.id, b.id FROM MATCH (a:persons)-[r:knows]->{1,6}(b:persons) "
                          "WHERE a.id = 1");
    // Should reach nodes 2,3,4,5,6 = 5 results.
    EXPECT_EQ(result.rows.size(), 5u)
        << "Variable-length {1,6} from node 1 should reach all 5 other nodes";
}

TEST_F(QA_GDB553, FromMatchFixedLength_StillWorks) {
    // Non-variable-length FROM MATCH should still use PatternMatchOperator.
    auto result = exec_ok("SELECT a.id, b.id FROM MATCH (a:persons)-[r:knows]->(b:persons) "
                          "WHERE a.id = 1");
    // Single hop from node 1 should reach node 2.
    EXPECT_GE(result.rows.size(), 1u);
}

TEST_F(QA_GDB553, FromMatchPlusShorthand) {
    // `+` should be equivalent to {1,inf}.
    auto result = exec_ok("SELECT a.id, b.id FROM MATCH (a:persons)-[r:knows]->+(b:persons) "
                          "WHERE a.id = 1");
    EXPECT_GE(result.rows.size(), 5u) << "+ should reach all reachable nodes";
}

TEST_F(QA_GDB553, FromMatchStarShorthand) {
    // `*` should be equivalent to {0,inf}.
    auto result = exec_ok("SELECT a.id, b.id FROM MATCH (a:persons)-[r:knows]->*(b:persons) "
                          "WHERE a.id = 1");
    // Should include self (0 hops) + all reachable = 6 results.
    EXPECT_GE(result.rows.size(), 6u) << "* should include self + all reachable nodes";
}

TEST_F(QA_GDB553, FromMatch_VsStandaloneMatch_SameResults) {
    // FROM MATCH and standalone MATCH RETURN should give the same variable-length results.
    auto from_result =
        exec_ok("SELECT a.id, b.id FROM MATCH (a:persons)-[r:knows]->{1,3}(b:persons) "
                "WHERE a.id = 1 ORDER BY b.id");
    auto match_result = exec_ok("MATCH (a:persons)-[r:knows]->{1,3}(b:persons) "
                                "WHERE a.id = 1 RETURN a.id, b.id ORDER BY b.id");

    EXPECT_EQ(from_result.rows.size(), match_result.rows.size())
        << "FROM MATCH and standalone MATCH should return the same number of results";
}

// ============================================================================
// Edge case: zero min_hops
// ============================================================================

TEST_F(QA_GDB553, FromMatchZeroMin_IncludesSelf) {
    auto result = exec_ok("SELECT a.id, b.id FROM MATCH (a:persons)-[r:knows]->{0,1}(b:persons) "
                          "WHERE a.id = 1");
    // Should include: self (0 hops, a.id=1 b.id=1) + 1-hop (b.id=2).
    EXPECT_GE(result.rows.size(), 2u);
}

} // namespace
} // namespace sixseven
