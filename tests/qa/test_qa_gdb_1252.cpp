/// @file test_qa_gdb_1252.cpp
/// @brief QA regression tests for GDB-1252.
///
/// GDB-1252 is the BM25/MATCH mirror of GDB-1249: any query combining a
/// MATCH(...) full-text predicate with a JOIN previously silently ignored the
/// full-text search (no relevance ranking, no _score, no error) and returned
/// the full join output, because try_plan_bm25_scan only runs in the joinless
/// WHERE-pushdown path and a residual MatchExpr evaluates to TRUE by
/// construction. Unlike GDB-1250 (which implemented real NEAREST pushdown),
/// this ticket is a stopgap only: MATCH+JOIN is rejected outright, with no
/// pushdown implementation. Coverage:
///   * MATCH in a WHERE with a JOIN is rejected (single/multiple joins, AND-ed
///     with other predicates, join variants).
///   * MATCH in a JOIN ON clause is rejected.
///   * The derived-table workaround is NOT over-rejected and returns only the
///     matching rows' joined output.
///   * Single-table MATCH + _score regressions remain intact.
///   * NEAREST+MATCH+JOIN gets a specific combination error, not this generic one.

#include "sixseven/common/status.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_qa_helpers.h"

using namespace sixseven;

namespace {

// =============================================================================
// Fixture: books(id, title), reviews(id, book_id, stars), a BM25 index on
// books(title), and vec EMBEDDING on books to exercise the NEAREST+MATCH+JOIN
// combination error path.
// =============================================================================

class QA_GDB1252 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1252";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
        index_manager_ = std::make_unique<IndexManager>(catalog_, *storage_);
        engine_->set_index_manager(index_manager_.get());

        seed_fixture();
    }

    void TearDown() override {
        index_manager_.reset();
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void seed_fixture() {
        exec_ok("CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR)");
        exec_ok("CREATE INDEX idx_title ON books(title) USING bm25");

        // "consciousness" appears in ids 1 and 3 only.
        exec_ok("INSERT INTO books VALUES (1, 'consciousness and the brain')");
        exec_ok("INSERT INTO books VALUES (2, 'a history of gardening')");
        exec_ok("INSERT INTO books VALUES (3, 'consciousness explained fully')");
        exec_ok("INSERT INTO books VALUES (4, 'quiet gardens and streams')");

        exec_ok("CREATE TABLE reviews (id INT PRIMARY KEY, book_id INT, stars INT)");
        exec_ok("INSERT INTO reviews VALUES (10, 1, 5)");
        exec_ok("INSERT INTO reviews VALUES (20, 2, 4)");
        exec_ok("INSERT INTO reviews VALUES (30, 3, 3)");
        exec_ok("INSERT INTO reviews VALUES (40, 4, 2)");
    }

    static std::vector<int32_t> sorted_first_col(const QueryResult& qr) {
        std::vector<int32_t> ids;
        for (const auto& row : qr.rows) {
            EXPECT_FALSE(row.empty());
            if (!row.empty()) {
                ids.push_back(row[0].as_int32());
            }
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    static void expect_invalid(const Result<QueryResult>& result,
                               const std::string& needle,
                               const std::string& sql) {
        ASSERT_FALSE(result.has_value()) << "expected rejection for: " << sql;
        EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT) << sql;
        const std::string& msg = result.error().message;
        EXPECT_NE(msg.find(needle), std::string::npos)
            << "message must contain '" << needle << "': " << msg;
    }

    static void expect_match_join_rejected(const Result<QueryResult>& result,
                                           const std::string& sql) {
        expect_invalid(result, "MATCH", sql);
        ASSERT_FALSE(result.has_value());
        const std::string& msg = result.error().message;
        EXPECT_NE(msg.find("JOIN"), std::string::npos) << msg;
        EXPECT_NE(msg.find("derived table"), std::string::npos) << msg;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<IndexManager> index_manager_;
};

// =============================================================================
// AC1: MATCH in WHERE with a JOIN rejected, naming MATCH, JOIN, and the
// derived-table workaround (single join, multiple joins, AND-ed predicate).
// =============================================================================

TEST_F(QA_GDB1252, MatchInWhereWithSingleJoinRejected) {
    const std::string sql = "SELECT b.id, r.stars FROM books b "
                            "INNER JOIN reviews r ON r.book_id = b.id "
                            "WHERE MATCH(b.title) TO 'consciousness'";
    expect_match_join_rejected(engine_->execute(sql), sql);
}

TEST_F(QA_GDB1252, MatchInWhereWithMultipleJoinsRejected) {
    const std::string sql = "SELECT b.id FROM books b "
                            "INNER JOIN reviews r ON r.book_id = b.id "
                            "INNER JOIN reviews r2 ON r2.book_id = b.id "
                            "WHERE MATCH(b.title) TO 'consciousness'";
    expect_match_join_rejected(engine_->execute(sql), sql);
}

TEST_F(QA_GDB1252, MatchAndedWithOtherPredicateWithJoinRejected) {
    const std::string sql = "SELECT b.id FROM books b "
                            "INNER JOIN reviews r ON r.book_id = b.id "
                            "WHERE MATCH(b.title) TO 'consciousness' AND r.stars > 3";
    expect_match_join_rejected(engine_->execute(sql), sql);
}

// =============================================================================
// AC2: MATCH in a JOIN ON clause is rejected with the same error class.
// =============================================================================

TEST_F(QA_GDB1252, MatchInJoinOnClauseRejected) {
    const std::string sql = "SELECT b.id FROM books b "
                            "INNER JOIN reviews r ON r.book_id = b.id "
                            "AND MATCH(b.title) TO 'consciousness'";
    expect_invalid(engine_->execute(sql), "ON clause", sql);
}

// =============================================================================
// AC3: the derived-table workaround is NOT over-rejected and returns only the
// matching books' joined reviews (exact ids asserted against fixture data).
// =============================================================================

TEST_F(QA_GDB1252, DerivedTableMatchWithJoinSucceeds) {
    const std::string sql = "SELECT nb.id, r.stars FROM "
                            "(SELECT id, _score FROM books WHERE MATCH(title) TO "
                            "'consciousness') nb "
                            "JOIN reviews r ON nb.id = r.book_id";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;

    // Only books 1 and 3 contain "consciousness"; book 2 and 4 must be excluded.
    auto ids = sorted_first_col(*result);
    std::vector<int32_t> expected = {1, 3};
    EXPECT_EQ(ids, expected) << "derived-table MATCH returned the wrong books";
}

// A derived table joined to ANOTHER derived table must still plan and return
// only the matching rows' joined output.
TEST_F(QA_GDB1252, DerivedTableJoinedToDerivedTableSucceeds) {
    const std::string sql =
        "SELECT x.id FROM "
        "(SELECT id, _score FROM books WHERE MATCH(title) TO 'consciousness') x "
        "JOIN (SELECT book_id FROM reviews) y ON x.id = y.book_id";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;
    auto ids = sorted_first_col(*result);
    std::vector<int32_t> expected = {1, 3};
    EXPECT_EQ(ids, expected);
}

// A correlated/uncorrelated subquery in WHERE whose own body contains MATCH
// (the outer join has no MATCH of its own) must NOT be rejected by the
// MATCH+JOIN guard added in GDB-1252: expr_contains_match only walks the
// outer query's own WHERE tree (BinaryExpr/UnaryExpr), so it never descends
// into the IN-subquery's body to find this MATCH. This asserts the guard
// specifically -- it must not produce the "MATCH(...) ... JOIN" error. (Any
// separate limitation in wiring the BM25 index into an IN-subquery's own
// recursive plan is a pre-existing, orthogonal concern outside this ticket's
// scope; if hit, it must surface as a different error, not our JOIN guard.)
TEST_F(QA_GDB1252, MatchInsideWhereSubqueryNotOverRejectedByJoinGuard) {
    const std::string sql =
        "SELECT b.id FROM books b JOIN reviews r ON r.book_id = b.id "
        "WHERE b.id IN (SELECT id FROM books b2 WHERE MATCH(b2.title) TO 'consciousness')";
    auto result = engine_->execute(sql);
    if (!result.has_value()) {
        const std::string& msg = result.error().message;
        EXPECT_EQ(msg.find("is not supported in a query with a JOIN"), std::string::npos)
            << "MATCH+JOIN guard must not fire for MATCH inside a WHERE subquery: " << msg;
        EXPECT_EQ(msg.find("is not supported inside a JOIN ON clause"), std::string::npos)
            << "MATCH+JOIN guard must not fire for MATCH inside a WHERE subquery: " << msg;
    }
}

// =============================================================================
// AC4: single-table MATCH + _score regressions remain intact (no JOIN at all).
// =============================================================================

TEST_F(QA_GDB1252, SingleTableMatchStillWorks) {
    const std::string sql =
        "SELECT id, _score FROM books WHERE MATCH(title) TO 'consciousness' ORDER BY _score DESC";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto ids = sorted_first_col(*result);
    std::vector<int32_t> expected = {1, 3};
    EXPECT_EQ(ids, expected);
}

// A join query with NO match predicate must remain unaffected.
TEST_F(QA_GDB1252, JoinWithoutMatchUnaffected) {
    const std::string sql =
        "SELECT b.id, r.stars FROM books b INNER JOIN reviews r ON r.book_id = b.id";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 4u);
}

// =============================================================================
// Adversarial: boolean nesting, join variants, and the NEAREST+MATCH+JOIN
// three-way combination.
// =============================================================================

// MATCH under NOT, in a query with a JOIN, is still rejected -- the join guard
// fires on any occurrence of MATCH in the WHERE tree, not just top-level AND
// positions (unlike NEAREST, MATCH has no "top-level AND" requirement here
// because this ticket rejects outright rather than attempting pushdown).
TEST_F(QA_GDB1252, MatchUnderNotWithJoinRejected) {
    const std::string sql = "SELECT b.id FROM books b "
                            "JOIN reviews r ON r.book_id = b.id "
                            "WHERE NOT MATCH(b.title) TO 'consciousness'";
    expect_match_join_rejected(engine_->execute(sql), sql);
}

// MATCH under OR, in a query with a JOIN, is still rejected.
TEST_F(QA_GDB1252, MatchUnderOrWithJoinRejected) {
    const std::string sql = "SELECT b.id FROM books b "
                            "JOIN reviews r ON r.book_id = b.id "
                            "WHERE MATCH(b.title) TO 'consciousness' OR r.stars > 3";
    expect_match_join_rejected(engine_->execute(sql), sql);
}

// LEFT JOIN with MATCH in WHERE is still rejected.
TEST_F(QA_GDB1252, MatchWithLeftJoinRejected) {
    const std::string sql = "SELECT b.id FROM books b "
                            "LEFT JOIN reviews r ON r.book_id = b.id "
                            "WHERE MATCH(b.title) TO 'consciousness'";
    expect_match_join_rejected(engine_->execute(sql), sql);
}

// CROSS JOIN with MATCH in WHERE is still rejected.
TEST_F(QA_GDB1252, MatchWithCrossJoinRejected) {
    const std::string sql = "SELECT b.id FROM books b "
                            "CROSS JOIN reviews r "
                            "WHERE MATCH(b.title) TO 'consciousness'";
    expect_match_join_rejected(engine_->execute(sql), sql);
}

// MATCH in the ON clause AND in WHERE simultaneously: ON-clause check fires
// first (checked before the WHERE-level check in plan_select).
TEST_F(QA_GDB1252, MatchInOnAndWhereRejected) {
    const std::string sql =
        "SELECT b.id FROM books b "
        "JOIN reviews r ON r.book_id = b.id AND MATCH(b.title) TO 'consciousness' "
        "WHERE MATCH(b.title) TO 'gardening'";
    expect_invalid(engine_->execute(sql), "ON clause", sql);
}

} // namespace
