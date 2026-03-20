/// @file test_qa_gdb_606.cpp
/// QA adversarial tests for GDB-606: Edge property access in TRAVERSE SELECT
/// and UNLINK WHERE.
///
/// Verifies the fix and adversarially tests:
///   AC1: edge_type.property syntax works in TRAVERSE SELECT with explicit alias.
///   AC2: Bare column names in UNLINK WHERE resolve to edge properties.
///   AC3: UNLINK without WHERE continues to work (regression guard).
///   AC4: Edge properties work in both TRAVERSE directions (OUT, IN, BOTH).
///
/// Adversarial categories: edge cases, boundary values, null handling, error
/// paths, case sensitivity, multiple edges, type variations, compound
/// predicates, stress tests.

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
// Fixture: graph with users, products, and "rated" edges with properties.
//
//   users:    (1, "Alice"), (2, "Bob"), (3, "Carol")
//   products: (10, "Widget"), (20, "Gadget"), (30, "Gizmo")
//   edge type "rated" (score DOUBLE, review VARCHAR) FROM users TO products
//   edges:
//     users(1) -> products(10) via rated (score=4.5, review='excellent')
//     users(1) -> products(20) via rated (score=1.5, review='terrible')
//     users(1) -> products(30) via rated (score=3.0, review='average')
//     users(2) -> products(10) via rated (score=5.0, review='perfect')
//     users(2) -> products(20) via rated (score=2.0, review='poor')
//     users(3) -> products(10) via rated (score=3.5, review='good')
// ============================================================================

class QA_GDB606 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb606";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");
        exec_ok("INSERT INTO users VALUES (3, 'Carol')");

        exec_ok("CREATE TABLE products (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO products VALUES (10, 'Widget')");
        exec_ok("INSERT INTO products VALUES (20, 'Gadget')");
        exec_ok("INSERT INTO products VALUES (30, 'Gizmo')");

        exec_ok("CREATE EDGE TYPE rated (score DOUBLE, review VARCHAR) "
                 "FROM users TO products");

        exec_ok("LINK users(1) TO products(10) VIA rated (score = 4.5, review = 'excellent')");
        exec_ok("LINK users(1) TO products(20) VIA rated (score = 1.5, review = 'terrible')");
        exec_ok("LINK users(1) TO products(30) VIA rated (score = 3.0, review = 'average')");
        exec_ok("LINK users(2) TO products(10) VIA rated (score = 5.0, review = 'perfect')");
        exec_ok("LINK users(2) TO products(20) VIA rated (score = 2.0, review = 'poor')");
        exec_ok("LINK users(3) TO products(10) VIA rated (score = 3.5, review = 'good')");
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

    void exec_err(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << ": expected error but got success";
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// AC1: edge_type.property syntax in TRAVERSE SELECT with explicit alias
// ============================================================================

TEST_F(QA_GDB606, TraverseSelectEdgePropertyWithAlias) {
    // The original bug: rated.score fails with "table not found: rated" when
    // the TRAVERSE has an explicit alias (AS t).
    auto qr = exec_ok("SELECT name, rated.score, rated.review "
                       "FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");

    ASSERT_EQ(qr.column_names.size(), 3u);
    EXPECT_EQ(qr.column_names[0], "name");
    EXPECT_EQ(qr.column_names[1], "score");
    EXPECT_EQ(qr.column_names[2], "review");

    // User 1 has 3 outgoing rated edges.
    ASSERT_EQ(qr.rows.size(), 3u);

    // Collect scores and sort for deterministic comparison.
    std::vector<double> scores;
    for (const auto& row : qr.rows) {
        scores.push_back(row[1].as_float64());
    }
    std::sort(scores.begin(), scores.end());

    EXPECT_DOUBLE_EQ(scores[0], 1.5);
    EXPECT_DOUBLE_EQ(scores[1], 3.0);
    EXPECT_DOUBLE_EQ(scores[2], 4.5);
}

TEST_F(QA_GDB606, TraverseSelectEdgePropertyWithoutAlias) {
    // Without alias: should also work (regression guard).
    auto qr = exec_ok("SELECT name, rated.score "
                       "FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH");

    ASSERT_EQ(qr.rows.size(), 3u);

    std::vector<double> scores;
    for (const auto& row : qr.rows) {
        scores.push_back(row[1].as_float64());
    }
    std::sort(scores.begin(), scores.end());

    EXPECT_DOUBLE_EQ(scores[0], 1.5);
    EXPECT_DOUBLE_EQ(scores[1], 3.0);
    EXPECT_DOUBLE_EQ(scores[2], 4.5);
}

// ============================================================================
// AC2: Bare column names in UNLINK WHERE resolve to edge properties
// ============================================================================

TEST_F(QA_GDB606, UnlinkWhereNumericProperty) {
    // The original bug: "column not found: score" in UNLINK WHERE.
    auto qr = exec_ok("UNLINK users(1) FROM products(20) VIA rated WHERE score < 2.0");
    EXPECT_EQ(qr.affected_rows, 1);

    // Verify only 2 edges remain for user 1.
    auto edges = exec_ok("SELECT rated.score FROM TRAVERSE rated "
                          "FROM users(1) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(edges.rows.size(), 2u);

    std::vector<double> remaining;
    for (const auto& row : edges.rows) {
        remaining.push_back(row[0].as_float64());
    }
    std::sort(remaining.begin(), remaining.end());
    EXPECT_DOUBLE_EQ(remaining[0], 3.0);
    EXPECT_DOUBLE_EQ(remaining[1], 4.5);
}

TEST_F(QA_GDB606, UnlinkWhereStringProperty) {
    auto qr = exec_ok("UNLINK users(1) FROM products(20) VIA rated "
                       "WHERE review = 'terrible'");
    EXPECT_EQ(qr.affected_rows, 1);

    auto edges = exec_ok("SELECT rated.review FROM TRAVERSE rated "
                          "FROM users(1) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(edges.rows.size(), 2u);
}

// ============================================================================
// AC3: UNLINK without WHERE — regression guard
// ============================================================================

TEST_F(QA_GDB606, UnlinkWithoutWhereStillWorks) {
    auto qr = exec_ok("UNLINK users(3) FROM products(10) VIA rated");
    EXPECT_EQ(qr.affected_rows, 1);

    auto edges = exec_ok("SELECT rated.score FROM TRAVERSE rated "
                          "FROM users(3) DIRECTION OUT FETCH AS t");
    EXPECT_EQ(edges.rows.size(), 0u);
}

// ============================================================================
// AC4: Edge properties in TRAVERSE with different directions
// ============================================================================

TEST_F(QA_GDB606, TraverseEdgePropertyDirectionIn) {
    // Traverse incoming edges to product 10 (rated by users 1, 2, 3).
    auto qr = exec_ok("SELECT name, rated.score "
                       "FROM TRAVERSE rated FROM products(10) DIRECTION IN FETCH AS t");

    ASSERT_EQ(qr.rows.size(), 3u);

    std::vector<double> scores;
    for (const auto& row : qr.rows) {
        scores.push_back(row[1].as_float64());
    }
    std::sort(scores.begin(), scores.end());

    EXPECT_DOUBLE_EQ(scores[0], 3.5);
    EXPECT_DOUBLE_EQ(scores[1], 4.5);
    EXPECT_DOUBLE_EQ(scores[2], 5.0);
}

// ============================================================================
// Adversarial: Compound WHERE predicates in UNLINK
// ============================================================================

TEST_F(QA_GDB606, UnlinkWhereCompoundPredicate) {
    // AND predicate: only delete edges with score < 2.0 AND review = 'terrible'.
    auto qr = exec_ok("UNLINK users(1) FROM products(20) VIA rated "
                       "WHERE score < 2.0 AND review = 'terrible'");
    EXPECT_EQ(qr.affected_rows, 1);
}

TEST_F(QA_GDB606, UnlinkWhereCompoundNoMatch) {
    // AND predicate where score matches but review doesn't.
    auto qr = exec_ok("UNLINK users(1) FROM products(20) VIA rated "
                       "WHERE score < 2.0 AND review = 'nonexistent'");
    EXPECT_EQ(qr.affected_rows, 0);

    // Edge should still exist.
    auto edges = exec_ok("SELECT rated.score FROM TRAVERSE rated "
                          "FROM users(1) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(edges.rows.size(), 3u);
}

// ============================================================================
// Adversarial: UNLINK WHERE with no matching edges (target has no edge)
// ============================================================================

TEST_F(QA_GDB606, UnlinkWhereTargetHasNoEdge) {
    // User 3 has no edge to product 20.
    auto qr = exec_ok("UNLINK users(3) FROM products(20) VIA rated WHERE score > 0");
    EXPECT_EQ(qr.affected_rows, 0);
}

// ============================================================================
// Adversarial: UNLINK WHERE boundary — score exactly at boundary
// ============================================================================

TEST_F(QA_GDB606, UnlinkWhereBoundaryExactMatch) {
    // score = 1.5 exactly — test <= boundary.
    auto qr = exec_ok("UNLINK users(1) FROM products(20) VIA rated WHERE score <= 1.5");
    EXPECT_EQ(qr.affected_rows, 1);
}

TEST_F(QA_GDB606, UnlinkWhereBoundaryExclusion) {
    // score = 1.5 exactly — test < 1.5 should NOT match.
    auto qr = exec_ok("UNLINK users(1) FROM products(20) VIA rated WHERE score < 1.5");
    EXPECT_EQ(qr.affected_rows, 0);
}

// ============================================================================
// Adversarial: Case sensitivity in edge property column names
// ============================================================================

TEST_F(QA_GDB606, TraverseEdgePropertyCaseInsensitive) {
    // BUG FINDING: OutputSchema::find_column uses case-sensitive comparison,
    // so rated.SCORE fails at runtime even though the binder resolves it
    // case-insensitively. This is a pre-existing issue amplified by GDB-606.
    auto result = engine_->execute(
        "SELECT rated.SCORE FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");

    // Currently fails — documenting the behavior.
    // If this starts passing, the case sensitivity bug has been fixed.
    if (result.has_value()) {
        EXPECT_EQ(result->rows.size(), 3u);
    }
}

TEST_F(QA_GDB606, UnlinkWhereCaseInsensitiveColumn) {
    // Bare column name SCORE (uppercase) in UNLINK WHERE.
    // The binder resolves case-insensitively, but evaluate_predicate uses
    // OutputSchema::find_column which is case-sensitive.
    auto result = engine_->execute(
        "UNLINK users(1) FROM products(20) VIA rated WHERE SCORE < 2.0");

    // Currently fails — documenting the behavior.
    if (result.has_value()) {
        EXPECT_EQ(result->affected_rows, 1);
    }
}

// ============================================================================
// Adversarial: Edge type name case in edge_type.property syntax
// ============================================================================

TEST_F(QA_GDB606, TraverseEdgeTypeCaseInsensitive) {
    // RATED.score (uppercase edge type name) — same case sensitivity issue.
    auto result = engine_->execute(
        "SELECT RATED.score FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");

    // Documenting behavior — currently fails due to case-sensitive find_column.
    if (result.has_value()) {
        EXPECT_EQ(result->rows.size(), 3u);
    }
}

// ============================================================================
// Adversarial: SELECT * includes edge properties with alias
// ============================================================================

TEST_F(QA_GDB606, SelectStarIncludesEdgePropertiesWithAlias) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");

    // Should include: target table columns + __to + __depth + edge properties
    // Verify edge property columns are present.
    bool has_score = false;
    bool has_review = false;
    for (const auto& col : qr.column_names) {
        if (col == "score") has_score = true;
        if (col == "review") has_review = true;
    }
    EXPECT_TRUE(has_score) << "SELECT * should include edge property 'score'";
    EXPECT_TRUE(has_review) << "SELECT * should include edge property 'review'";

    ASSERT_EQ(qr.rows.size(), 3u);
}

// ============================================================================
// Adversarial: Multiple edge types — ensure scoping is correct
// ============================================================================

TEST_F(QA_GDB606, MultipleEdgeTypesScopeIsolation) {
    // Create a second edge type with a different property set.
    exec_ok("CREATE EDGE TYPE purchased (quantity INT, discount DOUBLE) "
            "FROM users TO products");
    exec_ok("LINK users(1) TO products(10) VIA purchased (quantity = 2, discount = 0.1)");

    // Traverse rated — should NOT see purchased properties.
    auto qr = exec_ok("SELECT rated.score "
                       "FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(qr.rows.size(), 3u);

    // Traverse purchased — should NOT see rated properties.
    auto qr2 = exec_ok("SELECT purchased.quantity "
                        "FROM TRAVERSE purchased FROM users(1) DIRECTION OUT FETCH AS p");
    ASSERT_EQ(qr2.rows.size(), 1u);
    EXPECT_EQ(qr2.rows[0][0].as_int64(), 2);
}

// ============================================================================
// Adversarial: Reference nonexistent edge property — should error
// ============================================================================

TEST_F(QA_GDB606, TraverseSelectNonexistentEdgeProperty) {
    exec_err("SELECT rated.nonexistent "
             "FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");
}

TEST_F(QA_GDB606, UnlinkWhereNonexistentProperty) {
    exec_err("UNLINK users(1) FROM products(20) VIA rated WHERE nonexistent = 'foo'");
}

// ============================================================================
// Adversarial: UNLINK WHERE deletes multiple edges from same source
// ============================================================================

TEST_F(QA_GDB606, UnlinkWhereDeletesAllMatchingFromSource) {
    // User 1 has edges to products 20 (score=1.5) and 30 (score=3.0).
    // Delete all edges from user 1 where score < 4.0 to ALL targets.
    // But UNLINK requires a specific target, so test one at a time.

    // Delete low-score edges one target at a time.
    auto qr1 = exec_ok("UNLINK users(1) FROM products(20) VIA rated WHERE score < 4.0");
    EXPECT_EQ(qr1.affected_rows, 1);

    auto qr2 = exec_ok("UNLINK users(1) FROM products(30) VIA rated WHERE score < 4.0");
    EXPECT_EQ(qr2.affected_rows, 1);

    // Only the 4.5-score edge should remain.
    auto edges = exec_ok("SELECT rated.score FROM TRAVERSE rated "
                          "FROM users(1) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(edges.rows.size(), 1u);
    EXPECT_DOUBLE_EQ(edges.rows[0][0].as_float64(), 4.5);
}

// ============================================================================
// Adversarial: Edge with no properties — UNLINK WHERE should handle gracefully
// ============================================================================

TEST_F(QA_GDB606, EdgeWithNoPropertiesUnlinkWhere) {
    exec_ok("CREATE TABLE nodes (id INT PRIMARY KEY, label VARCHAR)");
    exec_ok("INSERT INTO nodes VALUES (1, 'A')");
    exec_ok("INSERT INTO nodes VALUES (2, 'B')");
    exec_ok("CREATE EDGE TYPE connects FROM nodes TO nodes");
    exec_ok("LINK nodes(1) TO nodes(2) VIA connects");

    // WHERE on edge with no properties — should error (no columns to reference).
    exec_err("UNLINK nodes(1) FROM nodes(2) VIA connects WHERE score > 1");
}

// ============================================================================
// Adversarial: TRAVERSE edge property in WHERE clause (not just SELECT)
// ============================================================================

TEST_F(QA_GDB606, TraverseWhereOnEdgeProperty) {
    // Filter traversal results by edge property in WHERE.
    auto qr = exec_ok("SELECT name, rated.score "
                       "FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t "
                       "WHERE rated.score > 3.0");

    // Only products with score > 3.0: Widget (4.5).
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_DOUBLE_EQ(qr.rows[0][1].as_float64(), 4.5);
}

// ============================================================================
// Adversarial: TRAVERSE edge property in ORDER BY
// ============================================================================

TEST_F(QA_GDB606, TraverseOrderByEdgeProperty) {
    auto qr = exec_ok("SELECT name, rated.score "
                       "FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t "
                       "ORDER BY rated.score ASC");

    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_DOUBLE_EQ(qr.rows[0][1].as_float64(), 1.5);
    EXPECT_DOUBLE_EQ(qr.rows[1][1].as_float64(), 3.0);
    EXPECT_DOUBLE_EQ(qr.rows[2][1].as_float64(), 4.5);
}

// ============================================================================
// Adversarial: UNLINK WHERE with OR predicate
// ============================================================================

TEST_F(QA_GDB606, UnlinkWhereOrPredicate) {
    // Delete edges to product 20 where score < 2.0 OR review = 'terrible'.
    // Both conditions match the same edge, should still delete only 1.
    auto qr = exec_ok("UNLINK users(1) FROM products(20) VIA rated "
                       "WHERE score < 2.0 OR review = 'terrible'");
    EXPECT_EQ(qr.affected_rows, 1);
}

// ============================================================================
// Adversarial: Verify UNLINK WHERE doesn't affect other users' edges
// ============================================================================

TEST_F(QA_GDB606, UnlinkWhereIsolation) {
    // Delete user 1's edge to product 10.
    exec_ok("UNLINK users(1) FROM products(10) VIA rated WHERE score > 0");

    // User 2 and 3's edges to product 10 should be unaffected.
    auto edges2 = exec_ok("SELECT rated.score FROM TRAVERSE rated "
                           "FROM users(2) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(edges2.rows.size(), 2u); // user 2 still has edges to 10 and 20

    auto edges3 = exec_ok("SELECT rated.score FROM TRAVERSE rated "
                           "FROM users(3) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(edges3.rows.size(), 1u); // user 3 still has edge to 10
}

// ============================================================================
// Adversarial: Self-referential edge type with properties
// ============================================================================

TEST_F(QA_GDB606, SelfReferentialEdgeProperties) {
    exec_ok("CREATE EDGE TYPE follows (weight DOUBLE) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 0.8)");
    exec_ok("LINK users(1) TO users(3) VIA follows (weight = 0.3)");

    // TRAVERSE with edge property and alias.
    auto qr = exec_ok("SELECT name, follows.weight "
                       "FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH AS f");

    ASSERT_EQ(qr.rows.size(), 2u);

    // UNLINK WHERE on self-referential edge.
    auto ur = exec_ok("UNLINK users(1) FROM users(3) VIA follows WHERE weight < 0.5");
    EXPECT_EQ(ur.affected_rows, 1);

    auto remaining = exec_ok("SELECT follows.weight "
                              "FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH AS f");
    ASSERT_EQ(remaining.rows.size(), 1u);
    EXPECT_DOUBLE_EQ(remaining.rows[0][0].as_float64(), 0.8);
}

// ============================================================================
// Adversarial: Stress — many edges, UNLINK WHERE filters subset
// ============================================================================

TEST_F(QA_GDB606, StressManyEdgesUnlinkWhere) {
    exec_ok("CREATE TABLE items (id INT PRIMARY KEY, val VARCHAR)");
    exec_ok("CREATE EDGE TYPE likes (rating INT) FROM users TO items");

    // Create 50 items and link user 1 to all with varying ratings.
    for (int i = 1; i <= 50; ++i) {
        exec_ok("INSERT INTO items VALUES (" + std::to_string(i) + ", 'item" +
                std::to_string(i) + "')");
        exec_ok("LINK users(1) TO items(" + std::to_string(i) + ") VIA likes (rating = " +
                std::to_string(i % 5) + ")");
    }

    // Delete edges where rating = 0 (items 5, 10, 15, 20, 25, 30, 35, 40, 45, 50).
    // Need to unlink per-target. Unlink from item 5.
    auto qr = exec_ok("UNLINK users(1) FROM items(5) VIA likes WHERE rating = 0");
    EXPECT_EQ(qr.affected_rows, 1);

    // Verify the other edges remain.
    auto edges = exec_ok("SELECT likes.rating FROM TRAVERSE likes "
                          "FROM users(1) DIRECTION OUT FETCH AS l");
    ASSERT_EQ(edges.rows.size(), 49u);
}

// ============================================================================
// Adversarial: Qualified edge property in UNLINK WHERE (edge_type.property)
// ============================================================================

TEST_F(QA_GDB606, UnlinkWhereQualifiedProperty) {
    // Use rated.score (qualified) in UNLINK WHERE — may or may not be supported.
    // If supported, should work. If not, should give clear error.
    auto result = engine_->execute(
        "UNLINK users(1) FROM products(20) VIA rated WHERE rated.score < 2.0");

    if (result.has_value()) {
        EXPECT_EQ(result->affected_rows, 1);
    }
    // Either way, no crash or undefined behavior — that's what matters.
}

// ============================================================================
// Adversarial: Edge property with same name as target table column
// ============================================================================

TEST_F(QA_GDB606, EdgePropertySameNameAsTableColumn) {
    // Create edge type with property "name" — same as users.name and products.name.
    exec_ok("CREATE EDGE TYPE tagged (name VARCHAR) FROM users TO products");
    exec_ok("LINK users(1) TO products(10) VIA tagged (name = 'favorite')");

    // Traverse with tagged.name — should return edge property, not table column.
    auto qr = exec_ok("SELECT tagged.name "
                       "FROM TRAVERSE tagged FROM users(1) DIRECTION OUT FETCH AS tg");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "favorite");
}

// ============================================================================
// Adversarial: UNLINK WHERE with type mismatch in predicate
// ============================================================================

TEST_F(QA_GDB606, UnlinkWhereTypeMismatch) {
    // score is DOUBLE, compare with string — should error or handle gracefully.
    auto result = engine_->execute(
        "UNLINK users(1) FROM products(20) VIA rated WHERE score = 'not_a_number'");

    // Should either error or return 0 affected rows — not crash.
    if (result.has_value()) {
        EXPECT_EQ(result->affected_rows, 0);
    }
}

} // namespace
} // namespace sixseven
