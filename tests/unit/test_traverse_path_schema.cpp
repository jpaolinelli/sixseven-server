// GDB-679: Planner and binder __path schema wiring.
//
// Verifies that the `__path` column (TypeId::PATH) is threaded through both the
// planner output schema (GDB-689) and the binder output-column declarations
// (GDB-690) whenever WITH TRACE is requested, and is absent otherwise. Covers:
//
//   * standalone TRAVERSE (planner iterator schema)
//   * standalone TRAVERSE wrapped as a derived table (binder bind_traverse path)
//   * SELECT ... FROM TRAVERSE, NODES mode (__path after __source)
//   * SELECT ... FROM TRAVERSE, MODE EDGES (__path after __depth)
//   * the trace-absent case for every context
//
// These are schema-wiring assertions for the GDB-679 story; the underlying BFS
// path reconstruction and operator-level behavior are covered by GDB-678 tests.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

/// Graph + table fixture mirroring the enriched-traversal suite:
///   users: (1,"Alice") (2,"Bob") (3,"Jane") (4,"Jake") (5,"Zara")
///   follows edges: 1->2, 1->3, 2->4, 3->4, 4->5  (homogeneous users->users)
class TraversePathSchemaTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_traverse_path_schema";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");
        exec_ok("INSERT INTO users VALUES (3, 'Jane')");
        exec_ok("INSERT INTO users VALUES (4, 'Jake')");
        exec_ok("INSERT INTO users VALUES (5, 'Zara')");

        exec_ok("CREATE EDGE TYPE follows FROM users TO users");
        exec_ok("LINK users(1) TO users(2) VIA follows");
        exec_ok("LINK users(1) TO users(3) VIA follows");
        exec_ok("LINK users(2) TO users(4) VIA follows");
        exec_ok("LINK users(3) TO users(4) VIA follows");
        exec_ok("LINK users(4) TO users(5) VIA follows");
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
        return std::move(*result);
    }

    // Index of the first column named `name`, or nullopt if absent.
    static std::optional<size_t> col_index(const QueryResult& qr, const std::string& name) {
        for (size_t i = 0; i < qr.column_names.size(); ++i) {
            if (qr.column_names[i] == name) {
                return i;
            }
        }
        return std::nullopt;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// --------------------------------------------------------------------------
// Standalone TRAVERSE (planner iterator schema — GDB-689 / plan_traverse)
// --------------------------------------------------------------------------

TEST_F(TraversePathSchemaTest, StandaloneTraceAddsPathColumn) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");

    auto path_idx = col_index(qr, "__path");
    ASSERT_TRUE(path_idx.has_value()) << "WITH TRACE must surface a __path column";
    EXPECT_EQ(qr.column_types[*path_idx], TypeId::PATH);

    // __path is the trailing column, immediately after node/depth.
    EXPECT_EQ(*path_idx, qr.column_names.size() - 1);

    // Every result row carries a PATH value.
    ASSERT_FALSE(qr.rows.empty());
    for (const auto& row : qr.rows) {
        EXPECT_EQ(row[*path_idx].type_id(), TypeId::PATH);
    }
}

TEST_F(TraversePathSchemaTest, StandaloneWithoutTraceHasNoPathColumn) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) DIRECTION OUT");

    EXPECT_FALSE(col_index(qr, "__path").has_value())
        << "__path must be absent when WITH TRACE is omitted";
    // Standalone TRAVERSE still exposes node + depth.
    EXPECT_TRUE(col_index(qr, "node").has_value());
    EXPECT_TRUE(col_index(qr, "depth").has_value());
}

// --------------------------------------------------------------------------
// Standalone TRAVERSE as a derived table (binder bind_traverse — GDB-690)
//
// When a standalone TRAVERSE is wrapped in a subquery, the outer query resolves
// column references against the binder's output_columns rather than the planner
// iterator schema. This exercises the bind_traverse() __path declaration.
// --------------------------------------------------------------------------

TEST_F(TraversePathSchemaTest, DerivedTableTraceExposesPathToOuterSelect) {
    auto qr = exec_ok("SELECT __path FROM "
                      "(TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE) AS t");

    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "__path");
    EXPECT_EQ(qr.column_types[0], TypeId::PATH);
    ASSERT_FALSE(qr.rows.empty());
    for (const auto& row : qr.rows) {
        EXPECT_EQ(row[0].type_id(), TypeId::PATH);
    }
}

TEST_F(TraversePathSchemaTest, DerivedTableWithoutTraceCannotSelectPath) {
    // No WITH TRACE => binder declares no __path column => referencing it fails.
    auto result =
        engine_->execute("SELECT __path FROM (TRAVERSE follows FROM users(1) DIRECTION OUT) AS t");
    EXPECT_FALSE(result.has_value())
        << "__path must not resolve when the derived TRAVERSE omits WITH TRACE";
}

// --------------------------------------------------------------------------
// FROM TRAVERSE, NODES mode (planner plan_table_source + binder
// compute_traverse_columns). __path sits after __source.
// --------------------------------------------------------------------------

TEST_F(TraversePathSchemaTest, FromTraverseNodesTracePathAfterSource) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");

    auto source_idx = col_index(qr, "__source");
    auto path_idx = col_index(qr, "__path");
    ASSERT_TRUE(source_idx.has_value());
    ASSERT_TRUE(path_idx.has_value()) << "NODES-mode WITH TRACE must surface __path";
    EXPECT_EQ(qr.column_types[*path_idx], TypeId::PATH);
    EXPECT_EQ(*path_idx, *source_idx + 1) << "__path must come immediately after __source";
}

TEST_F(TraversePathSchemaTest, FromTraverseNodesWithoutTraceHasNoPath) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    EXPECT_TRUE(col_index(qr, "__source").has_value());
    EXPECT_FALSE(col_index(qr, "__path").has_value());
}

// --------------------------------------------------------------------------
// FROM TRAVERSE, MODE EDGES (planner plan_table_source + binder
// compute_traverse_columns). __path sits after __depth.
// --------------------------------------------------------------------------

TEST_F(TraversePathSchemaTest, FromTraverseEdgesTracePathAfterDepth) {
    auto qr =
        exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES WITH TRACE");

    auto depth_idx = col_index(qr, "__depth");
    auto path_idx = col_index(qr, "__path");
    ASSERT_TRUE(depth_idx.has_value());
    ASSERT_TRUE(path_idx.has_value()) << "EDGES-mode WITH TRACE must surface __path";
    EXPECT_EQ(qr.column_types[*path_idx], TypeId::PATH);
    EXPECT_EQ(*path_idx, *depth_idx + 1) << "__path must come immediately after __depth";

    // EDGES mode also exposes __from / __to; __path must not displace them.
    EXPECT_TRUE(col_index(qr, "__from").has_value());
    EXPECT_TRUE(col_index(qr, "__to").has_value());
}

TEST_F(TraversePathSchemaTest, FromTraverseEdgesWithoutTraceHasNoPath) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES");
    EXPECT_TRUE(col_index(qr, "__depth").has_value());
    EXPECT_FALSE(col_index(qr, "__path").has_value());
}

} // namespace
} // namespace sixseven
