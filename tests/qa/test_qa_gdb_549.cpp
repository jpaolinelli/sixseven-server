/// @file test_qa_gdb_549.cpp
/// QA adversarial tests for GDB-549: Stale QA tests assert old broken behavior.
///
/// Verifies that the GDB-478 QA tests were correctly updated to assert
/// the fixed (GDB-548) behavior: algorithm TVFs now expose output columns
/// to the binder scope.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Fixture: Full query engine with algorithm registry
// ============================================================================

class QA_GDB549 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb549";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        // Register pagerank algorithm with 2 output columns.
        AlgorithmDef def;
        def.name = "pagerank";
        def.params = {
            {"damping", TypeId::FLOAT64, false, Value(0.85)},
            {"iterations", TypeId::INT64, false, Value(int64_t{20})},
        };
        def.output_columns = {
            {"node_id", TypeId::INT64, false},
            {"rank", TypeId::FLOAT64, false},
        };
        auto execute = [](const AlgorithmContext& /*ctx*/) -> Result<std::vector<AlgorithmRow>> {
            std::vector<AlgorithmRow> rows;
            rows.push_back({std::vector<Value>{Value(int64_t{1}), Value(0.85)}});
            rows.push_back({std::vector<Value>{Value(int64_t{2}), Value(0.15)}});
            return ok(std::move(rows));
        };
        (void)algo_registry_.register_algorithm(std::move(def), execute);

        // Register connected_components with 2 output columns.
        AlgorithmDef cc_def;
        cc_def.name = "connected_components";
        cc_def.output_columns = {
            {"node_id", TypeId::INT64, false},
            {"component_id", TypeId::INT64, false},
        };
        auto cc_exec = [](const AlgorithmContext& /*ctx*/) -> Result<std::vector<AlgorithmRow>> {
            std::vector<AlgorithmRow> rows;
            rows.push_back({std::vector<Value>{Value(int64_t{1}), Value(int64_t{0})}});
            return ok(std::move(rows));
        };
        (void)algo_registry_.register_algorithm(std::move(cc_def), cc_exec);

        engine_->set_algorithm_registry(&algo_registry_);

        // Set up graph data.
        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");
        exec_ok("CREATE EDGE TYPE knows FROM users TO users");
        exec_ok("LINK users(1) TO users(2) VIA knows");
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
    AlgorithmRegistry algo_registry_;
};

// ============================================================================
// Verify fix: SELECT * returns correct column count
// ============================================================================

TEST_F(QA_GDB549, SelectStarReturnsCorrectColumnCount) {
    auto result = exec_ok("SELECT * FROM pagerank('knows')");
    EXPECT_EQ(result.column_names.size(), 2u)
        << "SELECT * should return 2 columns (node_id, rank)";
    EXPECT_EQ(result.rows.size(), 2u);
}

TEST_F(QA_GDB549, SelectStarColumnNamesCorrect) {
    auto result = exec_ok("SELECT * FROM pagerank('knows')");
    ASSERT_EQ(result.column_names.size(), 2u);
    EXPECT_EQ(result.column_names[0], "node_id");
    EXPECT_EQ(result.column_names[1], "rank");
}

// ============================================================================
// Verify fix: Named column references now work
// ============================================================================

TEST_F(QA_GDB549, SelectNamedColumnNodeId) {
    auto result = exec_ok("SELECT node_id FROM pagerank('knows')");
    EXPECT_EQ(result.column_names.size(), 1u);
    EXPECT_EQ(result.rows.size(), 2u);
}

TEST_F(QA_GDB549, SelectNamedColumnRank) {
    auto result = exec_ok("SELECT rank FROM pagerank('knows')");
    EXPECT_EQ(result.column_names.size(), 1u);
}

TEST_F(QA_GDB549, SelectQualifiedColumn) {
    auto result = exec_ok("SELECT pagerank.node_id FROM pagerank('knows')");
    EXPECT_EQ(result.column_names.size(), 1u);
}

TEST_F(QA_GDB549, SelectBothColumns) {
    auto result = exec_ok("SELECT node_id, rank FROM pagerank('knows')");
    EXPECT_EQ(result.column_names.size(), 2u);
}

// ============================================================================
// Verify fix: WHERE on algorithm columns works
// ============================================================================

TEST_F(QA_GDB549, WhereOnNodeId) {
    auto result = exec_ok("SELECT * FROM pagerank('knows') WHERE node_id = 1");
    EXPECT_EQ(result.column_names.size(), 2u);
    // Should filter to just node_id=1.
    EXPECT_EQ(result.rows.size(), 1u);
}

TEST_F(QA_GDB549, WhereOnRank) {
    auto result = exec_ok("SELECT * FROM pagerank('knows') WHERE rank > 0.5");
    EXPECT_EQ(result.column_names.size(), 2u);
}

// ============================================================================
// Verify fix: ORDER BY on algorithm columns works
// ============================================================================

TEST_F(QA_GDB549, OrderByRankDesc) {
    auto result = exec_ok("SELECT * FROM pagerank('knows') ORDER BY rank DESC");
    EXPECT_EQ(result.column_names.size(), 2u);
    ASSERT_GE(result.rows.size(), 2u);
    // First row should have the higher rank.
    auto r0 = std::get<double>(result.rows[0][1].data());
    auto r1 = std::get<double>(result.rows[1][1].data());
    EXPECT_GE(r0, r1) << "Results should be sorted by rank DESC";
}

TEST_F(QA_GDB549, OrderByNodeIdAsc) {
    auto result = exec_ok("SELECT * FROM pagerank('knows') ORDER BY node_id ASC");
    EXPECT_EQ(result.column_names.size(), 2u);
    ASSERT_GE(result.rows.size(), 2u);
    auto id0 = std::get<int64_t>(result.rows[0][0].data());
    auto id1 = std::get<int64_t>(result.rows[1][0].data());
    EXPECT_LE(id0, id1) << "Results should be sorted by node_id ASC";
}

// ============================================================================
// Verify: Named params still work correctly with column exposure
// ============================================================================

TEST_F(QA_GDB549, NamedParamsWithColumnAccess) {
    auto result = exec_ok(
        "SELECT node_id, rank FROM pagerank('knows', damping := 0.5) ORDER BY node_id");
    EXPECT_EQ(result.column_names.size(), 2u);
    EXPECT_EQ(result.rows.size(), 2u);
}

TEST_F(QA_GDB549, ConnectedComponentsColumnsExposed) {
    auto result = exec_ok("SELECT * FROM connected_components('knows')");
    EXPECT_EQ(result.column_names.size(), 2u);
}

TEST_F(QA_GDB549, ConnectedComponentsNamedColumnAccess) {
    auto result = exec_ok("SELECT node_id, component_id FROM connected_components('knows')");
    EXPECT_EQ(result.column_names.size(), 2u);
}

// ============================================================================
// Error paths: invalid column references should still fail
// ============================================================================

TEST_F(QA_GDB549, InvalidColumnStillFails) {
    exec_fail("SELECT nonexistent_col FROM pagerank('knows')");
}

TEST_F(QA_GDB549, InvalidQualifiedColumnStillFails) {
    exec_fail("SELECT pagerank.nonexistent_col FROM pagerank('knows')");
}

} // namespace
} // namespace sixseven
