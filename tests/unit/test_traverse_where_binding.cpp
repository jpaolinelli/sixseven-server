// GDB-695: Binder: standalone TRAVERSE WHERE can reference traversal columns.
//
// Before this fix, Binder::bind_traverse() bound the standalone TRAVERSE WHERE
// expression in a scope chained only to the parent scope, so every reference to
// the traversal's own output columns (node, depth, source, __path) failed at
// bind time with "column not found". The executor (TraversalOperator) already
// implemented WHERE post-filtering against its output schema; this suite
// verifies the feature is now reachable from SQL end-to-end.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

/// Graph + table fixture mirroring the traverse-path-schema suite:
///   users: (1,"Alice") (2,"Bob") (3,"Jane") (4,"Jake") (5,"Zara")
///   follows edges: 1->2, 1->3, 2->4, 3->4, 4->5  (homogeneous users->users)
///
/// BFS from users(1), DIRECTION OUT:
///   depth 1: nodes 2, 3
///   depth 2: node 4
///   depth 3: node 5
class TraverseWhereBindingTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_traverse_where_bind";
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
        return result ? std::move(*result) : QueryResult{};
    }

    // Index of the first column with the given name, or nullopt if absent.
    static std::optional<size_t> col_index(const QueryResult& qr, const std::string& name) {
        for (size_t i = 0; i < qr.column_names.size(); ++i) {
            if (qr.column_names[i] == name) {
                return i;
            }
        }
        return std::nullopt;
    }

    // Widen an integer Value (INT32 node PKs, INT64 depth) to int64_t.
    static int64_t to_int64(const Value& v) {
        return v.type_id() == TypeId::INT32 ? static_cast<int64_t>(v.as_int32()) : v.as_int64();
    }

    // Sorted values of an integer column across all rows.
    static std::vector<int64_t> int64_column(const QueryResult& qr, size_t idx) {
        std::vector<int64_t> out;
        out.reserve(qr.rows.size());
        for (const auto& row : qr.rows) {
            out.push_back(to_int64(row[idx]));
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// --------------------------------------------------------------------------
// Happy path: WHERE on traversal columns binds and filters.
// --------------------------------------------------------------------------

TEST_F(TraverseWhereBindingTest, WhereOnDepthFiltersRows) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE depth = 1");

    auto node_idx = col_index(qr, "node");
    auto depth_idx = col_index(qr, "depth");
    ASSERT_TRUE(node_idx.has_value());
    ASSERT_TRUE(depth_idx.has_value());

    ASSERT_EQ(qr.rows.size(), 2u) << "depth = 1 must keep exactly the two depth-1 rows";
    EXPECT_EQ(int64_column(qr, *node_idx), (std::vector<int64_t>{2, 3}));
    for (const auto& row : qr.rows) {
        EXPECT_EQ(to_int64(row[*depth_idx]), 1);
    }
}

TEST_F(TraverseWhereBindingTest, WhereOnNodeFiltersRows) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE node = 4");

    auto node_idx = col_index(qr, "node");
    auto depth_idx = col_index(qr, "depth");
    ASSERT_TRUE(node_idx.has_value());
    ASSERT_TRUE(depth_idx.has_value());

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(to_int64(qr.rows[0][*node_idx]), 4);
    EXPECT_EQ(to_int64(qr.rows[0][*depth_idx]), 2);
}

TEST_F(TraverseWhereBindingTest, WhereWithComparisonOperator) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE depth > 1");

    auto node_idx = col_index(qr, "node");
    ASSERT_TRUE(node_idx.has_value());
    // depth 2 -> node 4, depth 3 -> node 5.
    EXPECT_EQ(int64_column(qr, *node_idx), (std::vector<int64_t>{4, 5}));
}

TEST_F(TraverseWhereBindingTest, WhereQualifiedByEdgeTypeResolves) {
    // Traversal columns are exposed under the edge-type alias.
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE follows.depth = 1");

    auto node_idx = col_index(qr, "node");
    ASSERT_TRUE(node_idx.has_value());
    EXPECT_EQ(int64_column(qr, *node_idx), (std::vector<int64_t>{2, 3}));
}

TEST_F(TraverseWhereBindingTest, WhereMatchingNoRowsReturnsEmpty) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE depth > 99");
    EXPECT_TRUE(qr.rows.empty());
}

// --------------------------------------------------------------------------
// WHERE combined with FETCH and WITH TRACE.
// --------------------------------------------------------------------------

TEST_F(TraverseWhereBindingTest, WhereOnSourceWithFetch) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE source = 1 FETCH");

    auto node_idx = col_index(qr, "node");
    auto source_idx = col_index(qr, "source");
    ASSERT_TRUE(node_idx.has_value());
    ASSERT_TRUE(source_idx.has_value());

    // Only nodes discovered directly from node 1: nodes 2 and 3.
    EXPECT_EQ(int64_column(qr, *node_idx), (std::vector<int64_t>{2, 3}));
    for (const auto& row : qr.rows) {
        EXPECT_EQ(to_int64(row[*source_idx]), 1);
    }
}

TEST_F(TraverseWhereBindingTest, WhereOnDepthWithTrace) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) WHERE depth = 2 WITH TRACE");

    auto node_idx = col_index(qr, "node");
    auto path_idx = col_index(qr, "__path");
    ASSERT_TRUE(node_idx.has_value());
    ASSERT_TRUE(path_idx.has_value()) << "WITH TRACE must still surface __path";

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(to_int64(qr.rows[0][*node_idx]), 4);
    EXPECT_EQ(qr.rows[0][*path_idx].type_id(), TypeId::PATH);
}

// --------------------------------------------------------------------------
// Error conditions: columns not declared for this traversal stay unresolvable.
// --------------------------------------------------------------------------

TEST_F(TraverseWhereBindingTest, WhereOnUnknownColumnStillFailsToBind) {
    auto result = engine_->execute("TRAVERSE follows FROM users(1) WHERE bogus_col = 1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST_F(TraverseWhereBindingTest, WhereOnSourceWithoutFetchFailsToBind) {
    // `source` is only declared when FETCH is present.
    auto result = engine_->execute("TRAVERSE follows FROM users(1) WHERE source = 1");
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraverseWhereBindingTest, WhereOnPathWithoutTraceFailsToBind) {
    // `__path` is only declared when WITH TRACE is present.
    auto result = engine_->execute("TRAVERSE follows FROM users(1) WHERE __path = 1");
    EXPECT_FALSE(result.has_value());
}

} // namespace
} // namespace sixseven
