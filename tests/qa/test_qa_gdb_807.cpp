/// @file test_qa_gdb_807.cpp
/// QA adversarial tests for GDB-807: De-vacuate case-insensitivity guards in
/// QA_GDB606.
///
/// GDB-807 converts three stale "documenting behavior" guards in test_qa_gdb_606.cpp
/// to unconditional assertions now that GDB-610 landed the case-insensitive
/// OutputSchema::find_column fix. These tests use bootstrap_qa_catalog() so they
/// actually run to completion (unlike the pre-existing GDB606 fixture which is
/// missing that call and silently fails SetUp).
///
/// What this file verifies:
///   1. TraverseEdgePropertyCaseInsensitive: rated.SCORE resolves to 'score'
///   2. UnlinkWhereCaseInsensitiveColumn: SCORE resolves case-insensitively in UNLINK WHERE
///   3. TraverseEdgeTypeCaseInsensitive: RATED.score resolves to 'score'
///   4. Adversarial neighbors: mixed case, ALL UPPER, aLtErNaTiNg, edge type name variants
///   5. Confirm regression: a case-sensitive lookup would miss uppercase — assertions catch it

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
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

#include "test_qa_helpers.h"

namespace sixseven {
namespace {

// ============================================================================
// Fixture — uses bootstrap_qa_catalog so SetUp succeeds
// ============================================================================

class QA_GDB807 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb807";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        // Schema: users --(rated: score DOUBLE, review VARCHAR)--> products
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
        return result ? std::move(*result) : QueryResult{};
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
// GDB-807 AC verification: the three de-vacuated tests, now in a working fixture
// ============================================================================

// GDB807_AC1: rated.SCORE (uppercase property) must resolve to 'score'.
// This is the exact scenario from TraverseEdgePropertyCaseInsensitive.
// If OutputSchema::find_column ever regresses to case-sensitive, exec_ok() will
// fail because execute() returns an error and qr.rows.size() == 0 != 3.
TEST_F(QA_GDB807, GDB807_TraverseEdgePropertyCaseInsensitive) {
    auto qr = exec_ok(
        "SELECT rated.SCORE FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(qr.rows.size(), 3u);

    std::vector<double> scores;
    for (const auto& row : qr.rows) {
        scores.push_back(row[0].as_float64());
    }
    std::sort(scores.begin(), scores.end());
    EXPECT_DOUBLE_EQ(scores[0], 1.5);
    EXPECT_DOUBLE_EQ(scores[1], 3.0);
    EXPECT_DOUBLE_EQ(scores[2], 4.5);
}

// GDB807_AC2: SCORE (uppercase) in UNLINK WHERE must resolve case-insensitively.
// This is the exact scenario from UnlinkWhereCaseInsensitiveColumn.
// Regression: if case-sensitive, engine returns error, exec_ok() fails.
TEST_F(QA_GDB807, GDB807_UnlinkWhereCaseInsensitiveColumn) {
    auto qr = exec_ok("UNLINK users(1) FROM products(20) VIA rated WHERE SCORE < 2.0");
    EXPECT_EQ(qr.affected_rows, 1);

    // Confirm the edge is actually gone.
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

// GDB807_AC3: RATED.score (uppercase edge type) must resolve to 'score'.
// This is the exact scenario from TraverseEdgeTypeCaseInsensitive.
// Regression: if case-sensitive type lookup fails, exec_ok() fires.
TEST_F(QA_GDB807, GDB807_TraverseEdgeTypeCaseInsensitive) {
    auto qr = exec_ok(
        "SELECT RATED.score FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(qr.rows.size(), 3u);

    std::vector<double> scores;
    for (const auto& row : qr.rows) {
        scores.push_back(row[0].as_float64());
    }
    std::sort(scores.begin(), scores.end());
    EXPECT_DOUBLE_EQ(scores[0], 1.5);
    EXPECT_DOUBLE_EQ(scores[1], 3.0);
    EXPECT_DOUBLE_EQ(scores[2], 4.5);
}

// ============================================================================
// Adversarial: neighbors of the three target tests — broader case coverage
// ============================================================================

// Both edge type AND property in ALL CAPS.
TEST_F(QA_GDB807, GDB807_BothEdgeTypeAndPropertyAllCaps) {
    auto qr = exec_ok(
        "SELECT RATED.SCORE FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(qr.rows.size(), 3u);
}

// Mixed case on property: rated.Score (title case).
TEST_F(QA_GDB807, GDB807_TraverseEdgePropertyTitleCase) {
    auto qr = exec_ok(
        "SELECT rated.Score FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(qr.rows.size(), 3u);
}

// Mixed case on edge type: Rated.score.
TEST_F(QA_GDB807, GDB807_TraverseEdgeTypeTitleCase) {
    auto qr = exec_ok(
        "SELECT Rated.score FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(qr.rows.size(), 3u);
}

// Mixed case on string property.
TEST_F(QA_GDB807, GDB807_TraverseEdgePropertyStringFieldCaseInsensitive) {
    auto qr = exec_ok(
        "SELECT rated.REVIEW FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(qr.rows.size(), 3u);

    // Verify the actual values returned are the edge property values.
    std::vector<std::string> reviews;
    for (const auto& row : qr.rows) {
        reviews.push_back(row[0].as_string());
    }
    std::sort(reviews.begin(), reviews.end());
    EXPECT_EQ(reviews[0], "average");
    EXPECT_EQ(reviews[1], "excellent");
    EXPECT_EQ(reviews[2], "terrible");
}

// UNLINK WHERE with mixed case property.
TEST_F(QA_GDB807, GDB807_UnlinkWhereMixedCaseProperty) {
    auto qr = exec_ok("UNLINK users(1) FROM products(20) VIA rated WHERE Score < 2.0");
    EXPECT_EQ(qr.affected_rows, 1);
}

// UNLINK WHERE with uppercase string property comparison.
TEST_F(QA_GDB807, GDB807_UnlinkWhereUppercaseStringProperty) {
    auto qr = exec_ok("UNLINK users(1) FROM products(20) VIA rated "
                       "WHERE REVIEW = 'terrible'");
    EXPECT_EQ(qr.affected_rows, 1);
}

// Traversal direction IN with uppercase property.
TEST_F(QA_GDB807, GDB807_TraverseDirectionInCaseInsensitiveProperty) {
    // Product 10 has 3 incoming rated edges from users 1, 2, 3.
    auto qr = exec_ok("SELECT name, rated.SCORE "
                       "FROM TRAVERSE rated FROM products(10) DIRECTION IN FETCH AS t");
    ASSERT_EQ(qr.rows.size(), 3u);
}

// TRAVERSE BOTH direction with uppercase property (self-referential edge only).
TEST_F(QA_GDB807, GDB807_TraverseDirectionBothCaseInsensitiveProperty) {
    // DIRECTION BOTH is only valid for self-referential edges (same src/dst table).
    // Create a follows edge type on users and verify uppercase property works with BOTH.
    exec_ok("CREATE EDGE TYPE follows (weight DOUBLE) FROM users TO users");
    exec_ok("LINK users(1) TO users(2) VIA follows (weight = 0.8)");
    exec_ok("LINK users(2) TO users(1) VIA follows (weight = 0.6)");

    // From user 1 BOTH — should see outgoing (to user 2) and incoming (from user 2).
    auto qr = exec_ok("SELECT follows.WEIGHT "
                       "FROM TRAVERSE follows FROM users(1) DIRECTION BOTH FETCH AS f");
    ASSERT_GE(qr.rows.size(), 1u); // At minimum one edge.
}

// Multiple uppercase properties in single SELECT.
TEST_F(QA_GDB807, GDB807_TraverseMultipleUppercaseProperties) {
    auto qr = exec_ok("SELECT rated.SCORE, rated.REVIEW "
                       "FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(qr.rows.size(), 3u);
    ASSERT_EQ(qr.column_names.size(), 2u);
}

// Case-insensitive property in WHERE clause of TRAVERSE (not just SELECT).
TEST_F(QA_GDB807, GDB807_TraverseWhereClauseCaseInsensitiveProperty) {
    auto qr = exec_ok("SELECT name, rated.score "
                       "FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t "
                       "WHERE rated.SCORE > 3.0");
    // Only product 10 (score=4.5).
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_DOUBLE_EQ(qr.rows[0][1].as_float64(), 4.5);
}

// Case-insensitive edge type in ORDER BY clause.
TEST_F(QA_GDB807, GDB807_TraverseOrderByCaseInsensitiveProperty) {
    auto qr = exec_ok("SELECT name, rated.score "
                       "FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t "
                       "ORDER BY rated.SCORE ASC");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_DOUBLE_EQ(qr.rows[0][1].as_float64(), 1.5);
    EXPECT_DOUBLE_EQ(qr.rows[1][1].as_float64(), 3.0);
    EXPECT_DOUBLE_EQ(qr.rows[2][1].as_float64(), 4.5);
}

// Bug: edge type name in TRAVERSE FROM is case-sensitive (not fixed by GDB-610).
// GDB-610 fixed OutputSchema::find_column (property access) but the TRAVERSE
// FROM edge type lookup does a case-sensitive catalog lookup.
// TRAVERSE RATED (uppercase) fails with "edge type 'RATED' not found".
// This test documents the current (broken) behavior.
// Filed as QA finding — see GDB-807 QA report.
TEST_F(QA_GDB807, GDB807_TraverseFromUppercaseEdgeTypeName_KnownBug) {
    // Currently fails because edge type lookup in TRAVERSE FROM is case-sensitive.
    // When this bug is fixed, this test should change to exec_ok + ASSERT_EQ(3u).
    exec_err("SELECT rated.score "
             "FROM TRAVERSE RATED FROM users(1) DIRECTION OUT FETCH AS t");
}

// ============================================================================
// Adversarial: confirm the structural non-vacuousness of GDB-807's change.
// If find_column regressed to case-sensitive, the next two tests would produce
// either an error from exec_ok (logged + empty QueryResult) or rows.size()==0.
// The ASSERT_EQ(3u) catches both failure modes.
//
// Sibling test: issue same query with lowercase (correct) — should always pass.
// Pair it with uppercase version to prove the uppercase test adds coverage.
// ============================================================================

TEST_F(QA_GDB807, GDB807_RegressionBaseline_LowercaseProperty) {
    // Baseline: lowercase rated.score always worked.
    auto qr = exec_ok(
        "SELECT rated.score FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(qr.rows.size(), 3u);
}

TEST_F(QA_GDB807, GDB807_RegressionAdversarial_UppercaseProperty) {
    // Adversarial: rated.SCORE must also return 3 rows.
    // In a regression, this would return 0 rows (or error). ASSERT catches it.
    auto qr = exec_ok(
        "SELECT rated.SCORE FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(qr.rows.size(), 3u);
}

} // namespace
} // namespace sixseven
