// GDB-680: TRACE integration tests — enriched and edge traversal layers.
//
// End-to-end TRACE coverage through the full SQL pipeline (parser -> binder ->
// planner -> executor) using SELECT ... FROM TRAVERSE with real table data:
//
//   * __path column exists and has PATH type when WITH TRACE is used
//   * path length matches __depth for each row
//   * the start node is the first step in every path
//   * paths stay cycle-free when the graph contains a cycle (5 -> 1)
//   * WHERE filters hits but paths to surviving hits remain complete
//   * TRACE + FETCH work together on standalone TRAVERSE
//   * TRACE + MODE EDGES shows the path to each edge's __from node
//   * MAX_DEPTH limits path lengths through the SQL surface
//   * DIRECTION IN produces correct reverse paths through the SQL surface
//
// Operator-level (TraversalOperator) coverage lives in test_trace_traversal.cpp.

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
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

/// Fixture with graph AND table storage, matching the enriched-traversal suite:
///   users: (1, "Alice"), (2, "Bob"), (3, "Jane"), (4, "Jake"), (5, "Zara")
///   follows edges: 1 -> 2, 1 -> 3, 2 -> 4, 3 -> 4, 4 -> 5
class TraceEnrichedTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_trace_enriched";
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

    static std::optional<size_t> col_index(const QueryResult& qr, const std::string& name) {
        for (size_t i = 0; i < qr.column_names.size(); ++i) {
            if (qr.column_names[i] == name) {
                return i;
            }
        }
        return std::nullopt;
    }

    static int64_t val_to_int64(const Value& v) {
        if (v.type_id() == TypeId::INT32) {
            return v.as_int32();
        }
        return v.as_int64();
    }

    static void expect_cycle_free(const Path& path) {
        std::unordered_set<int64_t> seen;
        for (const auto& step : path.steps) {
            EXPECT_TRUE(seen.insert(step.node_pk_as_int64()).second)
                << "path contains a repeated node: " << step.node_pk_as_int64();
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ---------------------------------------------------------------------------
// AC: __path column exists and has PATH type when WITH TRACE is used
// ---------------------------------------------------------------------------

TEST_F(TraceEnrichedTest, PathColumnExistsWithPathType) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");

    auto path_idx = col_index(qr, "__path");
    ASSERT_TRUE(path_idx.has_value()) << "WITH TRACE must surface a __path column";
    EXPECT_EQ(qr.column_types[*path_idx], TypeId::PATH);

    ASSERT_EQ(qr.rows.size(), 4u);
    for (const auto& row : qr.rows) {
        EXPECT_EQ(row[*path_idx].type_id(), TypeId::PATH)
            << "every row must carry a PATH value in __path";
    }
}

// ---------------------------------------------------------------------------
// AC: path length matches __depth for each row
// ---------------------------------------------------------------------------

TEST_F(TraceEnrichedTest, PathLengthMatchesDepthPerRow) {
    auto qr = exec_ok("SELECT __node, __depth, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 4u);
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row[2].type_id(), TypeId::PATH);
        const Path& p = row[2].as_path();
        EXPECT_EQ(p.length(), val_to_int64(row[1]))
            << "node " << val_to_int64(row[0]) << ": path hop count must equal __depth";
    }
}

// ---------------------------------------------------------------------------
// AC: start node is the first step in every path
// ---------------------------------------------------------------------------

TEST_F(TraceEnrichedTest, StartNodeIsFirstStepInEveryPath) {
    auto qr = exec_ok("SELECT __node, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 4u);
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row[1].type_id(), TypeId::PATH);
        const Path& p = row[1].as_path();
        ASSERT_FALSE(p.steps.empty());
        EXPECT_EQ(p.steps.front().node_pk_as_int64(), 1)
            << "path must start at the traversal origin";
        EXPECT_EQ(p.steps.back().node_pk_as_int64(), val_to_int64(row[0]))
            << "path must terminate at the result node";
    }
}

// ---------------------------------------------------------------------------
// AC: paths are cycle-free (add edge 5 -> 1, verify no cycles in paths)
// ---------------------------------------------------------------------------

TEST_F(TraceEnrichedTest, CycleEdgeKeepsPathsAcyclic) {
    exec_ok("LINK users(5) TO users(1) VIA follows");

    auto qr = exec_ok("SELECT __node, __depth, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");

    // The back-edge to the (already visited) start node adds no rows.
    ASSERT_EQ(qr.rows.size(), 4u);
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row[2].type_id(), TypeId::PATH);
        const Path& p = row[2].as_path();
        expect_cycle_free(p);
        EXPECT_EQ(p.length(), val_to_int64(row[1]));
        EXPECT_EQ(p.steps.front().node_pk_as_int64(), 1);
    }
}

// ---------------------------------------------------------------------------
// AC: WHERE filters hits but paths to surviving hits are complete
// ---------------------------------------------------------------------------

TEST_F(TraceEnrichedTest, WhereOnTableColumnKeepsCompletePath) {
    // Filter on enriched table data: only Zara (node 5, depth 3) survives,
    // but her traced path must still span the full chain from the start node.
    auto qr = exec_ok("SELECT name, __depth, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "WHERE name = 'Zara' WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Zara");
    EXPECT_EQ(val_to_int64(qr.rows[0][1]), 3);

    ASSERT_EQ(qr.rows[0][2].type_id(), TypeId::PATH);
    const Path& p = qr.rows[0][2].as_path();
    ASSERT_EQ(p.steps.size(), 4u) << "filtered-out intermediate hops must remain in the path";
    EXPECT_EQ(p.steps[0].node_pk_as_int64(), 1);
    EXPECT_TRUE(p.steps[1].node_pk_as_int64() == 2 || p.steps[1].node_pk_as_int64() == 3)
        << "unexpected intermediate node: " << p.steps[1].node_pk_as_int64();
    EXPECT_EQ(p.steps[2].node_pk_as_int64(), 4);
    EXPECT_EQ(p.steps[3].node_pk_as_int64(), 5);
    EXPECT_EQ(p.length(), 3);
}

TEST_F(TraceEnrichedTest, WhereOnDepthKeepsCompletePaths) {
    // Filter on the __depth meta-column: only depth-2 hits survive (node 4),
    // with a complete two-hop path from the start node.
    auto qr = exec_ok("SELECT __node, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "WHERE __depth = 2 WITH TRACE");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(val_to_int64(qr.rows[0][0]), 4);

    ASSERT_EQ(qr.rows[0][1].type_id(), TypeId::PATH);
    const Path& p = qr.rows[0][1].as_path();
    ASSERT_EQ(p.steps.size(), 3u);
    EXPECT_EQ(p.steps.front().node_pk_as_int64(), 1);
    EXPECT_EQ(p.steps.back().node_pk_as_int64(), 4);
    EXPECT_EQ(p.length(), 2);
}

// ---------------------------------------------------------------------------
// AC: TRACE + FETCH both work together (standalone TRAVERSE)
// ---------------------------------------------------------------------------

TEST_F(TraceEnrichedTest, StandaloneFetchWithTrace) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) DIRECTION OUT FETCH WITH TRACE");

    // FETCH adds the source column; TRACE appends __path after it.
    auto node_idx = col_index(qr, "node");
    auto depth_idx = col_index(qr, "depth");
    auto source_idx = col_index(qr, "source");
    auto path_idx = col_index(qr, "__path");
    ASSERT_TRUE(node_idx.has_value());
    ASSERT_TRUE(depth_idx.has_value());
    ASSERT_TRUE(source_idx.has_value()) << "FETCH must surface a source column";
    ASSERT_TRUE(path_idx.has_value()) << "WITH TRACE must surface a __path column";
    EXPECT_EQ(qr.column_types[*path_idx], TypeId::PATH);

    ASSERT_EQ(qr.rows.size(), 4u);
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row[*path_idx].type_id(), TypeId::PATH);
        const Path& p = row[*path_idx].as_path();
        EXPECT_EQ(p.steps.front().node_pk_as_int64(), 1);
        EXPECT_EQ(p.steps.back().node_pk_as_int64(), val_to_int64(row[*node_idx]));
        EXPECT_EQ(p.length(), val_to_int64(row[*depth_idx]));

        // The FETCH source is the BFS parent: the path's penultimate step.
        ASSERT_GE(p.steps.size(), 2u);
        EXPECT_EQ(val_to_int64(row[*source_idx]), p.steps[p.steps.size() - 2].node_pk_as_int64())
            << "FETCH source must match the path's penultimate step";
    }
}

// ---------------------------------------------------------------------------
// AC: TRACE + MODE EDGES shows the path to each edge's __from node
// ---------------------------------------------------------------------------

TEST_F(TraceEnrichedTest, EdgeModeTracePathEndsAtFromNode) {
    auto qr = exec_ok("SELECT __from, __to, __depth, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES WITH TRACE");

    // 5 edges: 1->2, 1->3, 2->4, 3->4, 4->5.
    ASSERT_EQ(qr.rows.size(), 5u);
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row[3].type_id(), TypeId::PATH);
        const Path& p = row[3].as_path();
        ASSERT_FALSE(p.steps.empty());
        EXPECT_EQ(p.steps.front().node_pk_as_int64(), 1)
            << "edge-mode path must start at the origin";
        EXPECT_EQ(p.steps.back().node_pk_as_int64(), val_to_int64(row[0]))
            << "edge-mode path must end at the edge's __from node";
        expect_cycle_free(p);
    }
}

TEST_F(TraceEnrichedTest, EdgeModeTraceFromStartNodeIsTrivialPath) {
    auto qr = exec_ok("SELECT __from, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES WITH TRACE");

    // Edges leaving the start node carry the trivial single-step path [1].
    size_t start_edges = 0;
    for (const auto& row : qr.rows) {
        if (val_to_int64(row[0]) != 1) {
            continue;
        }
        ++start_edges;
        ASSERT_EQ(row[1].type_id(), TypeId::PATH);
        const Path& p = row[1].as_path();
        ASSERT_EQ(p.steps.size(), 1u);
        EXPECT_EQ(p.steps[0].node_pk_as_int64(), 1);
        EXPECT_EQ(p.length(), 0);
    }
    EXPECT_EQ(start_edges, 2u) << "edges 1->2 and 1->3 must originate at the start node";
}

// ---------------------------------------------------------------------------
// AC: TRACE with MAX_DEPTH limits path lengths correctly
// ---------------------------------------------------------------------------

TEST_F(TraceEnrichedTest, MaxDepthLimitsPathLengths) {
    auto qr = exec_ok("SELECT __node, __depth, __path "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 2 WITH TRACE");

    // Depth limit 2 excludes node 5 (depth 3).
    ASSERT_EQ(qr.rows.size(), 3u);
    for (const auto& row : qr.rows) {
        EXPECT_NE(val_to_int64(row[0]), 5) << "MAX_DEPTH 2 must exclude depth-3 nodes";
        ASSERT_EQ(row[2].type_id(), TypeId::PATH);
        const Path& p = row[2].as_path();
        EXPECT_LE(p.length(), 2) << "no path may exceed MAX_DEPTH hops";
        EXPECT_EQ(p.length(), val_to_int64(row[1]));
    }
}

// ---------------------------------------------------------------------------
// AC: TRACE with DIRECTION IN produces correct reverse paths
// ---------------------------------------------------------------------------

TEST_F(TraceEnrichedTest, DirectionInProducesReversePaths) {
    auto qr = exec_ok("SELECT __node, __depth, __path "
                      "FROM TRAVERSE follows FROM users(5) DIRECTION IN WITH TRACE");

    // Reverse BFS from 5: 4 (depth 1), 2 and 3 (depth 2), 1 (depth 3).
    ASSERT_EQ(qr.rows.size(), 4u);

    bool saw_node_1 = false;
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row[2].type_id(), TypeId::PATH);
        const Path& p = row[2].as_path();
        EXPECT_EQ(p.steps.front().node_pk_as_int64(), 5)
            << "reverse paths must start at the BFS origin";
        EXPECT_EQ(p.steps.back().node_pk_as_int64(), val_to_int64(row[0]));
        EXPECT_EQ(p.length(), val_to_int64(row[1]));
        expect_cycle_free(p);

        if (val_to_int64(row[0]) == 1) {
            saw_node_1 = true;
            // The reverse path to 1 must run 5 -> 4 -> (2 or 3) -> 1.
            ASSERT_EQ(p.steps.size(), 4u);
            EXPECT_EQ(p.steps[1].node_pk_as_int64(), 4);
            EXPECT_TRUE(p.steps[2].node_pk_as_int64() == 2 || p.steps[2].node_pk_as_int64() == 3)
                << "unexpected intermediate node: " << p.steps[2].node_pk_as_int64();
        }
    }
    EXPECT_TRUE(saw_node_1) << "reverse BFS from 5 must reach node 1";
}

} // namespace
} // namespace sixseven
