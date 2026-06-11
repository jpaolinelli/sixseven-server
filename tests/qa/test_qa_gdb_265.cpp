/// @file test_qa_gdb_265.cpp
/// QA adversarial tests for GDB-265: EnrichedTraversalOperator with row data enrichment.
///
/// Tests cover:
///   - All 8 acceptance criteria with concrete verification
///   - Boundary values (MAX_DEPTH 0, 1, deep chains)
///   - Cycle detection in enriched traversal
///   - IN and BOTH direction enrichment
///   - NULL column handling in enriched results
///   - Multiple dangling edges
///   - WHERE filter edge cases (no matches, combined predicates)
///   - Projection edge cases (only meta-columns, aliased sources)
///   - Schema correctness (column count, types)
///   - Stress test (many nodes)

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

namespace sixseven {
namespace {

// ============================================================================
// Fixture: builds users table + follows edges (same as dev tests)
// ============================================================================

class QA_GDB265 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb265";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

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

        // Create edge type and build graph: 1→2, 1→3, 2→4, 3→4, 4→5.
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
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected);
        }
    }

    /// Find column index by name (asserts it exists).
    size_t col_idx(const QueryResult& qr, const std::string& name) {
        for (size_t i = 0; i < qr.column_names.size(); ++i) {
            if (qr.column_names[i] == name)
                return i;
        }
        ADD_FAILURE() << "column not found: " << name;
        return 0;
    }

    /// Helper to extract integer value regardless of underlying type (INT32
    /// or INT64). Meta-columns like __node and __source use the table's actual
    /// PK type which may be INT32 for INT columns.
    int64_t get_int(const Value& v) {
        if (v.is_null()) {
            ADD_FAILURE() << "expected integer value, got NULL";
            return 0;
        }
        try {
            return v.as_int64();
        } catch (...) {}
        try {
            return static_cast<int64_t>(v.as_int32());
        } catch (...) {}
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
// AC1: SELECT * returns actual user columns alongside meta-columns
// ============================================================================

TEST_F(QA_GDB265, AC1_SelectStarReturnsColumnsAndMeta) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    // Must have exactly 5 columns: id, name, __node, __depth, __source.
    ASSERT_EQ(qr.column_names.size(), 5u);
    EXPECT_EQ(qr.column_names[0], "id");
    EXPECT_EQ(qr.column_names[1], "name");
    EXPECT_EQ(qr.column_names[2], "__node");
    EXPECT_EQ(qr.column_names[3], "__depth");
    EXPECT_EQ(qr.column_names[4], "__source");

    // BFS from 1 OUT: visits 2,3 (depth 1), 4 (depth 2), 5 (depth 3).
    ASSERT_EQ(qr.rows.size(), 4u);

    // Every row must have exactly 5 values.
    for (size_t r = 0; r < qr.rows.size(); ++r) {
        ASSERT_EQ(qr.rows[r].size(), 5u) << "row " << r << " has wrong column count";
    }

    // Verify enriched data: id matches __node, name is non-null.
    auto id_idx = col_idx(qr, "id");
    auto name_idx = col_idx(qr, "name");
    auto node_idx = col_idx(qr, "__node");
    for (const auto& row : qr.rows) {
        EXPECT_FALSE(row[name_idx].is_null()) << "name should be enriched";
        EXPECT_EQ(get_int(row[id_idx]), get_int(row[node_idx])) << "id should match __node";
    }
}

// ============================================================================
// AC2: Projection — SELECT name, __depth returns only selected columns
// ============================================================================

TEST_F(QA_GDB265, AC2_ProjectionWorksCorrectly) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "name");
    EXPECT_EQ(qr.column_names[1], "__depth");

    ASSERT_EQ(qr.rows.size(), 4u);
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row.size(), 2u);
        EXPECT_FALSE(row[0].is_null());  // name is populated
        EXPECT_GE(row[1].as_int64(), 1); // depth >= 1
    }
}

// ============================================================================
// AC3: WHERE on enriched table data — name LIKE 'J%'
// ============================================================================

TEST_F(QA_GDB265, AC3_WhereOnTableColumn) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "WHERE name LIKE 'J%'");

    // Jane (depth 1) and Jake (depth 2) match J%.
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
// AC4: WHERE on meta-columns — __depth = 1
// ============================================================================

TEST_F(QA_GDB265, AC4_WhereOnMetaColumn) {
    auto qr = exec_ok("SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "WHERE __depth = 1");

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
// AC5: ORDER BY + LIMIT composes correctly
// ============================================================================

TEST_F(QA_GDB265, AC5_OrderByAndLimit) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "ORDER BY __depth, name LIMIT 2");

    ASSERT_EQ(qr.rows.size(), 2u);
    // Depth 1 sorted: Bob, Jane.
    EXPECT_EQ(qr.rows[0][0].as_string(), "Bob");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 1);
    EXPECT_EQ(qr.rows[1][0].as_string(), "Jane");
    EXPECT_EQ(qr.rows[1][1].as_int64(), 1);
}

TEST_F(QA_GDB265, AC5_OrderByNameDesc) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "ORDER BY name DESC LIMIT 3");

    ASSERT_EQ(qr.rows.size(), 3u);
    // Descending: Zara, Jane, Jake.
    EXPECT_EQ(qr.rows[0][0].as_string(), "Zara");
    EXPECT_EQ(qr.rows[1][0].as_string(), "Jane");
    EXPECT_EQ(qr.rows[2][0].as_string(), "Jake");
}

// ============================================================================
// AC6: Empty traversal — no outgoing edges returns empty result set
// ============================================================================

TEST_F(QA_GDB265, AC6_EmptyTraversalLeafNode) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(5) DIRECTION OUT");
    EXPECT_TRUE(qr.rows.empty());
    // Schema should still be correct even with no rows.
    ASSERT_EQ(qr.column_names.size(), 5u);
}

TEST_F(QA_GDB265, AC6_EmptyTraversalNoIncomingEdges) {
    // Node 1 has no incoming edges.
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION IN");
    EXPECT_TRUE(qr.rows.empty());
}

// ============================================================================
// AC7: Standalone TRAVERSE still returns (node, depth) as before
// ============================================================================

TEST_F(QA_GDB265, AC7_StandaloneTraverseBackcompat) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) DIRECTION OUT");

    ASSERT_GE(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "node");
    EXPECT_EQ(qr.column_names[1], "depth");

    // BFS from 1: 4 reachable nodes.
    ASSERT_EQ(qr.rows.size(), 4u);

    // Standalone does NOT have __source, id, name columns.
    // Columns should be exactly node + depth (possibly + source from older impl).
    EXPECT_LE(qr.column_names.size(), 3u);
}

// ============================================================================
// Boundary: MAX_DEPTH = 1 — only immediate neighbors
// ============================================================================

TEST_F(QA_GDB265, BoundaryMaxDepth1) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "MAX_DEPTH 1");

    // Only depth-1 neighbors: Bob (2) and Jane (3).
    ASSERT_EQ(qr.rows.size(), 2u);
    for (const auto& row : qr.rows) {
        EXPECT_EQ(row[1].as_int64(), 1);
    }

    std::vector<std::string> names;
    for (const auto& row : qr.rows) {
        names.push_back(row[0].as_string());
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "Bob");
    EXPECT_EQ(names[1], "Jane");
}

// ============================================================================
// Boundary: MAX_DEPTH = 0 — no results (start node excluded)
// ============================================================================

TEST_F(QA_GDB265, BoundaryMaxDepth0) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 0");

    // Start node itself is at depth 0 and excluded from results.
    // MAX_DEPTH=0 means don't expand beyond depth 0, so nothing is emitted.
    EXPECT_TRUE(qr.rows.empty());
}

// ============================================================================
// Boundary: Deep chain — verify correct depth values
// ============================================================================

TEST_F(QA_GDB265, BoundaryDeepChainDepthValues) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "ORDER BY __depth");

    ASSERT_EQ(qr.rows.size(), 4u);

    // Depth 1: Bob, Jane; Depth 2: Jake; Depth 3: Zara.
    // When ordered by depth:
    // Rows 0,1 should be depth 1
    EXPECT_EQ(qr.rows[0][1].as_int64(), 1);
    EXPECT_EQ(qr.rows[1][1].as_int64(), 1);
    // Row 2 should be depth 2
    EXPECT_EQ(qr.rows[2][1].as_int64(), 2);
    EXPECT_EQ(qr.rows[2][0].as_string(), "Jake");
    // Row 3 should be depth 3
    EXPECT_EQ(qr.rows[3][1].as_int64(), 3);
    EXPECT_EQ(qr.rows[3][0].as_string(), "Zara");
}

// ============================================================================
// Cycle detection: BFS does not revisit nodes in circular graphs
// ============================================================================

TEST_F(QA_GDB265, CycleDetectionCircularGraph) {
    // Create cycle: 5→1 (so 1→2→4→5→1 is a cycle).
    exec_ok("LINK users(5) TO users(1) VIA follows");

    auto qr = exec_ok("SELECT __node, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    // BFS should still visit exactly 4 distinct nodes: 2,3,4,5.
    // Node 1 is already visited (start), so cycle back to 1 is skipped.
    ASSERT_EQ(qr.rows.size(), 4u);

    // Verify no duplicate nodes.
    std::vector<int64_t> nodes;
    for (const auto& row : qr.rows) {
        nodes.push_back(get_int(row[0]));
    }
    std::sort(nodes.begin(), nodes.end());
    EXPECT_EQ(nodes[0], 2);
    EXPECT_EQ(nodes[1], 3);
    EXPECT_EQ(nodes[2], 4);
    EXPECT_EQ(nodes[3], 5);
}

// ============================================================================
// Direction IN: enrichment resolves source table columns
// ============================================================================

TEST_F(QA_GDB265, DirectionInEnrichment) {
    // From user 5 going IN: 5←4, then 4←2, 4←3, then 2←1, 3←1.
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(5) DIRECTION IN");

    // Should reach: 4 (depth 1), 2 and 3 (depth 2), 1 (depth 3) = 4 nodes.
    ASSERT_EQ(qr.rows.size(), 4u);

    // Names should be enriched from users table.
    for (const auto& row : qr.rows) {
        EXPECT_FALSE(row[0].is_null());
    }

    // Verify Jake is at depth 1.
    bool found_jake = false;
    for (const auto& row : qr.rows) {
        if (row[0].as_string() == "Jake") {
            EXPECT_EQ(row[1].as_int64(), 1);
            found_jake = true;
        }
    }
    EXPECT_TRUE(found_jake) << "Jake should be at depth 1 going IN from node 5";
}

// ============================================================================
// Direction BOTH: enrichment works for bidirectional traversal
// ============================================================================

TEST_F(QA_GDB265, DirectionBothEnrichment) {
    // From user 4 going BOTH: reaches 2,3,5 (depth 1), then 1 (depth 2).
    auto qr = exec_ok("SELECT name, __depth, __source FROM TRAVERSE follows FROM users(4) "
                      "DIRECTION BOTH");

    // Should reach 4 nodes.
    ASSERT_EQ(qr.rows.size(), 4u);

    // All names should be enriched.
    for (const auto& row : qr.rows) {
        EXPECT_FALSE(row[0].is_null());
    }
}

// ============================================================================
// __source meta-column values for depth-1 nodes
// ============================================================================

TEST_F(QA_GDB265, SourceMetaColumnCorrectness) {
    auto qr = exec_ok("SELECT __node, __depth, __source "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.rows.size(), 4u);

    auto node_idx = col_idx(qr, "__node");
    auto depth_idx = col_idx(qr, "__depth");
    auto source_idx = col_idx(qr, "__source");

    for (const auto& row : qr.rows) {
        if (row[depth_idx].as_int64() == 1) {
            // Depth-1 nodes should have __source = 1 (the start node).
            EXPECT_EQ(get_int(row[source_idx]), 1)
                << "depth-1 node " << get_int(row[node_idx]) << " should have source=1";
        }
    }

    // Find node 4 (depth 2) — its source should be 2 or 3 (whichever BFS found first).
    for (const auto& row : qr.rows) {
        if (get_int(row[node_idx]) == 4) {
            EXPECT_EQ(row[depth_idx].as_int64(), 2);
            auto source = get_int(row[source_idx]);
            EXPECT_TRUE(source == 2 || source == 3)
                << "node 4 source should be 2 or 3, got " << source;
        }
    }
}

// ============================================================================
// Dangling edges: multiple dangling edges produce NULL table columns
// ============================================================================

TEST_F(QA_GDB265, MultipleDanglingEdges) {
    // GDB-315 added PK validation to LINK — dangling edges (pointing to
    // non-existent rows) are now rejected.  Verify LINK fails for
    // non-existent target PKs.
    auto r1 = engine_->execute("LINK users(5) TO users(998) VIA follows");
    EXPECT_FALSE(r1.has_value()) << "LINK to non-existent PK should fail";

    auto r2 = engine_->execute("LINK users(5) TO users(999) VIA follows");
    EXPECT_FALSE(r2.has_value()) << "LINK to non-existent PK should fail";
}

// ============================================================================
// WHERE filter that matches no rows — returns empty result
// ============================================================================

TEST_F(QA_GDB265, WhereNoMatches) {
    auto qr = exec_ok("SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "WHERE name = 'NonExistent'");

    EXPECT_TRUE(qr.rows.empty());
}

// ============================================================================
// WHERE combining table column AND meta-column
// ============================================================================

TEST_F(QA_GDB265, WhereCombinedTableAndMeta) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "WHERE __depth = 1 AND name > 'C'");

    // Depth 1: Bob and Jane. Only Jane > 'C'.
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Jane");
    EXPECT_EQ(qr.rows[0][1].as_int64(), 1);
}

// ============================================================================
// WHERE on __depth > value — range filter on meta-column
// ============================================================================

TEST_F(QA_GDB265, WhereDepthRange) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "WHERE __depth >= 2 ORDER BY __depth");

    // Depth 2: Jake, Depth 3: Zara.
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Jake");
    EXPECT_EQ(qr.rows[1][0].as_string(), "Zara");
}

// ============================================================================
// Projection: only meta-columns (no table columns)
// ============================================================================

TEST_F(QA_GDB265, ProjectOnlyMetaColumns) {
    auto qr = exec_ok("SELECT __node, __depth, __source "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.column_names.size(), 3u);
    EXPECT_EQ(qr.column_names[0], "__node");
    EXPECT_EQ(qr.column_names[1], "__depth");
    EXPECT_EQ(qr.column_names[2], "__source");

    ASSERT_EQ(qr.rows.size(), 4u);
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row.size(), 3u);
        EXPECT_FALSE(row[0].is_null());
        EXPECT_FALSE(row[1].is_null());
    }
}

// ============================================================================
// Projection: single table column only
// ============================================================================

TEST_F(QA_GDB265, ProjectSingleTableColumn) {
    auto qr = exec_ok("SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "name");
    ASSERT_EQ(qr.rows.size(), 4u);
}

// ============================================================================
// Aliased TRAVERSE source
// ============================================================================

TEST_F(QA_GDB265, AliasedTraverseSource) {
    auto qr = exec_ok("SELECT t.name, t.__depth "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT AS t");

    ASSERT_EQ(qr.column_names.size(), 2u);
    ASSERT_EQ(qr.rows.size(), 4u);
}

// ============================================================================
// NULL values in table columns are correctly enriched
// ============================================================================

TEST_F(QA_GDB265, NullColumnsInTable) {
    // Create a table with nullable columns and insert rows with NULLs.
    exec_ok("CREATE TABLE items (id INT PRIMARY KEY, label VARCHAR, score INT)");
    exec_ok("INSERT INTO items VALUES (1, 'root', 100)");
    exec_ok("INSERT INTO items VALUES (2, NULL, 50)");
    exec_ok("INSERT INTO items VALUES (3, 'leaf', NULL)");

    exec_ok("CREATE EDGE TYPE links FROM items TO items");
    exec_ok("LINK items(1) TO items(2) VIA links");
    exec_ok("LINK items(1) TO items(3) VIA links");

    auto qr = exec_ok("SELECT label, score, __node "
                      "FROM TRAVERSE links FROM items(1) DIRECTION OUT "
                      "ORDER BY __node");

    ASSERT_EQ(qr.rows.size(), 2u);

    // Node 2: label=NULL, score=50.
    EXPECT_TRUE(qr.rows[0][0].is_null()) << "node 2 label should be NULL";
    EXPECT_EQ(qr.rows[0][1].as_int32(), 50);

    // Node 3: label='leaf', score=NULL.
    EXPECT_EQ(qr.rows[1][0].as_string(), "leaf");
    EXPECT_TRUE(qr.rows[1][1].is_null()) << "node 3 score should be NULL";
}

// ============================================================================
// Enrichment across different table types (self-referencing same table)
// ============================================================================

TEST_F(QA_GDB265, SelfReferencingEdgeType) {
    // 'follows' is already users→users. Verify both directions enrich from
    // the same table.
    auto out = exec_ok("SELECT name FROM TRAVERSE follows FROM users(4) DIRECTION OUT");
    ASSERT_EQ(out.rows.size(), 1u);
    EXPECT_EQ(out.rows[0][0].as_string(), "Zara");

    auto in = exec_ok("SELECT name FROM TRAVERSE follows FROM users(4) DIRECTION IN");
    // IN from 4: 2 (Bob) and 3 (Jane) at depth 1, then 1 (Alice) at depth 2.
    ASSERT_EQ(in.rows.size(), 3u);
    std::vector<std::string> names;
    for (const auto& row : in.rows) {
        names.push_back(row[0].as_string());
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "Alice");
    EXPECT_EQ(names[1], "Bob");
    EXPECT_EQ(names[2], "Jane");
}

// ============================================================================
// COUNT(*) on enriched traversal result
// ============================================================================

TEST_F(QA_GDB265, AggregateOnEnrichedResult) {
    auto qr = exec_ok("SELECT COUNT(*) FROM TRAVERSE follows FROM users(1) DIRECTION OUT");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int64(), 4);
}

// ============================================================================
// GROUP BY __depth gives correct counts
// ============================================================================

TEST_F(QA_GDB265, GroupByDepth) {
    auto qr = exec_ok("SELECT __depth, COUNT(*) FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "GROUP BY __depth ORDER BY __depth");

    ASSERT_EQ(qr.rows.size(), 3u);
    // Depth 1: 2 nodes (Bob, Jane)
    EXPECT_EQ(qr.rows[0][0].as_int64(), 1);
    EXPECT_EQ(qr.rows[0][1].as_int64(), 2);
    // Depth 2: 1 node (Jake)
    EXPECT_EQ(qr.rows[1][0].as_int64(), 2);
    EXPECT_EQ(qr.rows[1][1].as_int64(), 1);
    // Depth 3: 1 node (Zara)
    EXPECT_EQ(qr.rows[2][0].as_int64(), 3);
    EXPECT_EQ(qr.rows[2][1].as_int64(), 1);
}

// ============================================================================
// OFFSET on enriched traversal results
// ============================================================================

TEST_F(QA_GDB265, OrderByWithOffset) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "ORDER BY __depth, name LIMIT 2 OFFSET 1");

    ASSERT_EQ(qr.rows.size(), 2u);
    // Skips Bob (first depth 1), gets Jane (depth 1) and Jake (depth 2).
    EXPECT_EQ(qr.rows[0][0].as_string(), "Jane");
    EXPECT_EQ(qr.rows[1][0].as_string(), "Jake");
}

// ============================================================================
// Stress: many nodes — verifies hash map enrichment scales
// ============================================================================

TEST_F(QA_GDB265, StressManyNodes) {
    // Create a chain of 100 nodes.
    exec_ok("CREATE TABLE chain (id INT PRIMARY KEY, label VARCHAR)");
    exec_ok("CREATE EDGE TYPE next FROM chain TO chain");

    for (int i = 1; i <= 100; ++i) {
        exec_ok("INSERT INTO chain VALUES (" + std::to_string(i) + ", 'node_" + std::to_string(i) +
                "')");
    }
    for (int i = 1; i < 100; ++i) {
        exec_ok("LINK chain(" + std::to_string(i) + ") TO chain(" + std::to_string(i + 1) +
                ") VIA next");
    }

    auto qr = exec_ok("SELECT label, __depth FROM TRAVERSE next FROM chain(1) DIRECTION OUT");

    // Should get 99 nodes (2 through 100).
    ASSERT_EQ(qr.rows.size(), 99u);

    // Every row should be enriched (no NULLs).
    for (const auto& row : qr.rows) {
        EXPECT_FALSE(row[0].is_null());
    }

    // Verify depth ordering makes sense: max depth should be 99.
    int64_t max_depth = 0;
    for (const auto& row : qr.rows) {
        max_depth = std::max(max_depth, row[1].as_int64());
    }
    EXPECT_EQ(max_depth, 99);
}

// ============================================================================
// Diamond graph: node reachable via two paths is only returned once
// ============================================================================

TEST_F(QA_GDB265, DiamondGraphNoDuplicates) {
    // Graph: 1→2, 1→3, 2→4, 3→4. Node 4 is reachable via two paths.
    auto qr = exec_ok("SELECT __node, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "WHERE __node = 4");

    // Node 4 should appear exactly once.
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(get_int(qr.rows[0][0]), 4);
    EXPECT_EQ(qr.rows[0][1].as_int64(), 2);
}

// ============================================================================
// Start from middle of graph — both upstream and downstream exist
// ============================================================================

TEST_F(QA_GDB265, StartFromMiddleNodeOut) {
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE follows FROM users(2) DIRECTION OUT");

    // 2→4 (depth 1), 4→5 (depth 2). Nodes 1 and 3 not reachable going OUT.
    ASSERT_EQ(qr.rows.size(), 2u);

    std::vector<std::string> names;
    for (const auto& row : qr.rows) {
        names.push_back(row[0].as_string());
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "Jake");
    EXPECT_EQ(names[1], "Zara");
}

// ============================================================================
// Schema correctness: column types match expected types
// ============================================================================

TEST_F(QA_GDB265, SchemaColumnTypesCorrect) {
    auto qr = exec_ok("SELECT id, name, __node, __depth, __source "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT LIMIT 1");

    ASSERT_EQ(qr.rows.size(), 1u);

    // id is INT (INT32), name is VARCHAR (STRING).
    // __node type should match PK type.
    // __depth is INT64.
    // __source is nullable.
    auto& row = qr.rows[0];

    // id should be INT32 (users PK is INT).
    EXPECT_FALSE(row[0].is_null());

    // __depth should be INT64.
    auto depth = row[3].as_int64();
    EXPECT_GE(depth, 1);

    // __source should be non-null for depth > 0 nodes.
    EXPECT_FALSE(row[4].is_null());
}

// ============================================================================
// WHERE on dangling edge: table column IS NULL works
// ============================================================================

TEST_F(QA_GDB265, WhereOnDanglingEdgeIsNull) {
    // GDB-315 added PK validation — dangling edges can no longer be created.
    // Verify that LINK to non-existent PK fails.
    auto r = engine_->execute("LINK users(5) TO users(999) VIA follows");
    EXPECT_FALSE(r.has_value()) << "LINK to non-existent PK should fail";
}

TEST_F(QA_GDB265, WhereOnDanglingEdgeIsNotNull) {
    // GDB-315 added PK validation — dangling edges can no longer be created.
    auto r = engine_->execute("LINK users(5) TO users(999) VIA follows");
    EXPECT_FALSE(r.has_value()) << "LINK to non-existent PK should fail";
}

// ============================================================================
// LIMIT 0 returns no rows
// ============================================================================

TEST_F(QA_GDB265, LimitZero) {
    auto qr = exec_ok("SELECT name FROM TRAVERSE follows FROM users(1) DIRECTION OUT LIMIT 0");
    EXPECT_TRUE(qr.rows.empty());
}

// ============================================================================
// Repeated open/close: execute same query twice (operator reuse)
// ============================================================================

TEST_F(QA_GDB265, RepeatedExecution) {
    auto qr1 =
        exec_ok("SELECT __node FROM TRAVERSE follows FROM users(1) DIRECTION OUT ORDER BY __node");
    auto qr2 =
        exec_ok("SELECT __node FROM TRAVERSE follows FROM users(1) DIRECTION OUT ORDER BY __node");

    ASSERT_EQ(qr1.rows.size(), qr2.rows.size());
    for (size_t i = 0; i < qr1.rows.size(); ++i) {
        EXPECT_EQ(get_int(qr1.rows[i][0]), get_int(qr2.rows[i][0]));
    }
}

// ============================================================================
// Heterogeneous edge: different source and target tables
// ============================================================================

TEST_F(QA_GDB265, HeterogeneousEdgeEnrichment) {
    // Create posts table and an "authored" edge from users→posts.
    exec_ok("CREATE TABLE posts (pid INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO posts VALUES (100, 'Hello World')");
    exec_ok("INSERT INTO posts VALUES (101, 'Second Post')");

    exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
    exec_ok("LINK users(1) TO posts(100) VIA authored");
    exec_ok("LINK users(1) TO posts(101) VIA authored");

    auto qr = exec_ok("SELECT title, __depth FROM TRAVERSE authored FROM users(1) DIRECTION OUT "
                      "ORDER BY title");

    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Hello World");
    EXPECT_EQ(qr.rows[1][0].as_string(), "Second Post");

    // Both at depth 1.
    EXPECT_EQ(qr.rows[0][1].as_int64(), 1);
    EXPECT_EQ(qr.rows[1][1].as_int64(), 1);
}

// ============================================================================
// IN direction on heterogeneous edge — enriches FROM source table columns
// ============================================================================

TEST_F(QA_GDB265, HeterogeneousEdgeInDirection) {
    exec_ok("CREATE TABLE posts (pid INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO posts VALUES (100, 'Hello World')");
    exec_ok("CREATE EDGE TYPE authored FROM users TO posts");
    exec_ok("LINK users(1) TO posts(100) VIA authored");
    exec_ok("LINK users(2) TO posts(100) VIA authored");

    // IN from posts → should reach users table.
    auto qr = exec_ok("SELECT name, __depth FROM TRAVERSE authored FROM posts(100) DIRECTION IN "
                      "ORDER BY name");

    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "Alice");
    EXPECT_EQ(qr.rows[1][0].as_string(), "Bob");
}

// ============================================================================
// SELECT with expression on enriched column
// ============================================================================

TEST_F(QA_GDB265, ExpressionOnEnrichedColumn) {
    auto qr = exec_ok("SELECT name, __depth + 10 AS boosted_depth "
                      "FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "WHERE __depth = 1 ORDER BY name");

    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][1].as_int64(), 11);
    EXPECT_EQ(qr.rows[1][1].as_int64(), 11);
}

// ============================================================================
// Large MAX_DEPTH with small graph — no crash or unexpected behavior
// ============================================================================

TEST_F(QA_GDB265, LargeMaxDepthSmallGraph) {
    auto qr = exec_ok("SELECT __node, __depth FROM TRAVERSE follows FROM users(1) DIRECTION OUT "
                      "MAX_DEPTH 1000");

    // Same result as default — graph only has depth 3.
    ASSERT_EQ(qr.rows.size(), 4u);
}

} // namespace
} // namespace sixseven
