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
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

/// Test fixture for enriched traversal via SELECT ... FROM TRAVERSE.
///
/// Builds graph AND actual table storage with row data:
///   users: (1, "Alice"), (2, "Bob"), (3, "Jane"), (4, "Jake"), (5, "Zara")
///   edges: 1→2, 1→3, 2→4, 3→4, 4→5
class EnrichedTraversalTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_enriched_trav";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        // Create table and insert rows.
        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");
        exec_ok("INSERT INTO users VALUES (3, 'Jane')");
        exec_ok("INSERT INTO users VALUES (4, 'Jake')");
        exec_ok("INSERT INTO users VALUES (5, 'Zara')");

        // Create edge type and build graph.
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

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        ASSERT_FALSE(result.has_value()) << sql << " should have failed";
        EXPECT_EQ(result.error().code, expected);
    }

    static int64_t val_to_int64(const Value& v) {
        if (v.type_id() == TypeId::INT32)
            return v.as_int32();
        return v.as_int64();
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// 1. BasicEnrichment — SELECT * returns target columns + meta-columns
// ============================================================================

TEST_F(EnrichedTraversalTest, BasicEnrichment) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    // Should have: id, name, __node, __depth, __source
    ASSERT_GE(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "name");

    // Find meta-column positions.
    size_t node_idx = 0;
    size_t depth_idx = 0;
    size_t source_idx = 0;
    for (size_t i = 0; i < qr.column_names.size(); ++i) {
        if (qr.column_names[i] == "__node")
            node_idx = i;
        if (qr.column_names[i] == "__depth")
            depth_idx = i;
        if (qr.column_names[i] == "__source")
            source_idx = i;
    }

    // BFS from 1: visits 2, 3 (depth 1), 4 (depth 2), 5 (depth 3)
    ASSERT_EQ(qr.rows.size(), 4u);

    // Verify enriched data: id column matches __node meta-column.
    for (const auto& row : qr.rows) {
        EXPECT_EQ(row[0].as_int32(), static_cast<int32_t>(val_to_int64(row[node_idx])))
            << "id should match __node for row";
    }

    // Verify depth 1 nodes have source=1.
    for (const auto& row : qr.rows) {
        if (val_to_int64(row[depth_idx]) == 1) {
            EXPECT_EQ(val_to_int64(row[source_idx]), 1);
        }
    }

    // Verify name column is populated (not null).
    for (const auto& row : qr.rows) {
        EXPECT_FALSE(row[1].is_null()) << "name should be enriched from table";
    }
}

// ============================================================================
// 2. ColumnSelection — SELECT name, __depth returns only those columns
// ============================================================================

TEST_F(EnrichedTraversalTest, ColumnSelection) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "name");
    EXPECT_EQ(qr.column_names[1], "__depth");

    ASSERT_EQ(qr.rows.size(), 4u);

    // Depth-1 names should be Bob and Jane (order may vary).
    std::vector<std::string> depth1_names;
    for (const auto& row : qr.rows) {
        if (val_to_int64(row[1]) == 1) {
            depth1_names.push_back(row[0].as_string());
        }
    }
    std::sort(depth1_names.begin(), depth1_names.end());
    ASSERT_EQ(depth1_names.size(), 2u);
    EXPECT_EQ(depth1_names[0], "Bob");
    EXPECT_EQ(depth1_names[1], "Jane");
}

// ============================================================================
// 3. WhereOnTableColumn — WHERE name LIKE 'J%' filters on enriched data
// ============================================================================

TEST_F(EnrichedTraversalTest, WhereOnTableColumn) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "WHERE name LIKE 'J%'");

    // "Jane" (depth 1) and "Jake" (depth 2) match J%.
    ASSERT_EQ(qr.rows.size(), 2u);

    std::vector<std::string> names;
    for (const auto& row : qr.rows) {
        names.push_back(row[0].as_string());
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "Jake");
    EXPECT_EQ(names[1], "Jane");
}

// ============================================================================
// 4. WhereOnDepth — WHERE __depth = 1 filters on meta-column
// ============================================================================

TEST_F(EnrichedTraversalTest, WhereOnDepth) {
    auto qr = exec_ok("SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "WHERE __depth = 1");

    // Depth 1: Bob and Jane.
    ASSERT_EQ(qr.rows.size(), 2u);

    std::vector<std::string> names;
    for (const auto& row : qr.rows) {
        names.push_back(row[0].as_string());
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "Bob");
    EXPECT_EQ(names[1], "Jane");
}

// ============================================================================
// 5. OrderByAndLimit — ORDER BY __depth, name LIMIT 2
// ============================================================================

TEST_F(EnrichedTraversalTest, OrderByAndLimit) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "ORDER BY __depth, name LIMIT 2");

    ASSERT_EQ(qr.rows.size(), 2u);
    // Depth 1 sorted: Bob, Jane — limit 2 picks both.
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 1);
    EXPECT_EQ(qr.rows[1][0].as_string(), "Jane");
    EXPECT_EQ(val_to_int64(qr.rows[1][1]), 1);
}

// ============================================================================
// 6. EmptyTraversal — no outgoing edges returns empty result set
// ============================================================================

TEST_F(EnrichedTraversalTest, EmptyTraversal) {
    // Node 5 has no outgoing edges.
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(5) DIRECTION OUT");
    EXPECT_TRUE(qr.rows.empty());
}

// ============================================================================
// 7. DanglingEdge — PK in graph but not in table gives NULLs for table cols
// ============================================================================

TEST_F(EnrichedTraversalTest, DanglingEdge) {
    // Create a dangling edge via the graph engine directly (bypassing
    // execute_link validation which now rejects non-existent PKs).
    auto link_result = graph_engine_->link(
        default_database_id, "follows", Value(int64_t{5}), Value(int64_t{999}), {});
    ASSERT_TRUE(link_result.has_value()) << link_result.error().message;

    auto qr = exec_ok(
        "SELECT id, name, __node, __depth FROM TRAVERSE follows FROM users(5) DIRECTION OUT");

    // Should get 1 row for node 999.
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(val_to_int64(qr.rows[0][2]), 999); // __node = 999

    // Table columns should be NULL for the dangling edge.
    EXPECT_TRUE(qr.rows[0][0].is_null()) << "id should be NULL for dangling edge";
    EXPECT_TRUE(qr.rows[0][1].is_null()) << "name should be NULL for dangling edge";
}

// ============================================================================
// 8. StandaloneBackcompat — standalone TRAVERSE still returns (node, depth)
// ============================================================================

TEST_F(EnrichedTraversalTest, StandaloneBackcompat) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) DIRECTION OUT");

    // Standalone TRAVERSE returns node, depth columns.
    ASSERT_GE(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "node");
    EXPECT_EQ(qr.column_names[1], "depth");

    // BFS from 1: 4 reachable nodes.
    ASSERT_EQ(qr.rows.size(), 4u);
}

// ============================================================================
// 9. TraceAddsPathColumn — WITH TRACE surfaces a __path column (GDB-678)
// ============================================================================

TEST_F(EnrichedTraversalTest, TraceAddsPathColumn) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");

    // __path must be present in the projected columns.
    size_t path_idx = qr.column_names.size();
    size_t node_idx = qr.column_names.size();
    for (size_t i = 0; i < qr.column_names.size(); ++i) {
        if (qr.column_names[i] == "__path") {
            path_idx = i;
        }
        if (qr.column_names[i] == "__node") {
            node_idx = i;
        }
    }
    ASSERT_LT(path_idx, qr.column_names.size()) << "__path column missing";
    ASSERT_LT(node_idx, qr.column_names.size());

    ASSERT_EQ(qr.rows.size(), 4u);

    // Every row carries a PATH value whose final node equals __node.
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row[path_idx].type_id(), TypeId::PATH) << "__path should be a PATH value";
        const Path& p = row[path_idx].as_path();
        ASSERT_FALSE(p.steps.empty());
        EXPECT_EQ(p.steps.front().node_pk, 1) << "path must start at the source node";
        EXPECT_EQ(p.steps.back().node_pk, val_to_int64(row[node_idx]))
            << "path must end at the result node";
    }
}

// ============================================================================
// 10. TracePathLengthMatchesDepth — path hop count equals __depth (GDB-678)
// ============================================================================

TEST_F(EnrichedTraversalTest, TracePathLengthMatchesDepth) {
    auto qr = exec_ok("SELECT __depth, __path FROM TRAVERSE follows FROM users(1) "
                      "DIRECTION OUT WITH TRACE");

    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "__depth");
    EXPECT_EQ(qr.column_names[1], "__path");
    ASSERT_EQ(qr.rows.size(), 4u);

    for (const auto& row : qr.rows) {
        const int64_t depth = val_to_int64(row[0]);
        ASSERT_EQ(row[1].type_id(), TypeId::PATH);
        const Path& p = row[1].as_path();
        // A path reaching a node at depth d has d edges (d + 1 nodes).
        EXPECT_EQ(p.length(), depth) << "path length must equal traversal depth";
    }
}

// ============================================================================
// 11. NoTraceNoPathColumn — without WITH TRACE there is no __path (GDB-678)
// ============================================================================

TEST_F(EnrichedTraversalTest, NoTraceNoPathColumn) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
    for (const auto& name : qr.column_names) {
        EXPECT_NE(name, "__path") << "__path must not appear without WITH TRACE";
    }
}

} // namespace
} // namespace sixseven
