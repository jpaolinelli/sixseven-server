/// @file test_qa_gdb_1297.cpp
/// @brief QA regression tests for GDB-1297.
///
/// GDB-1297 fixed a bug where a BM25 MATCH(...) (or NEAREST/other index-backed
/// predicate) inside an IN/NOT IN subquery failed with "no BM25 index
/// available" whenever the predicate could not be decorrelated into a join at
/// plan time -- i.e. the outer query has a JOIN, or the subquery is genuinely
/// correlated. In both cases expr_evaluator.cpp's eval_in()/eval_exists()/
/// eval_scalar_subquery() construct a fresh runtime-fallback Planner from
/// SubqueryContext, and before the fix that context always carried nullptr
/// index maps (hnsw/btree/hash/bm25/hnsw_rid_maps), regardless of what the
/// top-level Planner actually had loaded.
///
/// The implementer's dev suite (tests/unit/test_bm25_subquery.cpp) covers the
/// core repro (single JOIN forcing the fallback) plus the already-working
/// plan-time-decorrelated shapes. This file adds adversarial coverage the
/// handoff specifically called out: nested subqueries (including a **second**
/// level of runtime fallback, which exercises the recursive nature of the fix
/// -- each Planner threads its OWN index handles into its OWN subquery_ctx_,
/// so a subquery-of-a-subquery must also see them), subqueries under LEFT and
/// multi-way JOINs, the missing-index error path specifically THROUGH the
/// runtime-fallback path (the dev suite only exercises it through the
/// plan-time path), empty-result paths, NULL semantics interacting with the
/// new fallback, and a scale/stress check.
///
/// Per the implementation handoff: EXISTS+MATCH is deliberately NOT exercised
/// here. EXISTS-subquery decorrelation has a separate, still-open
/// silent-wrong-results bug with MATCH (residual MatchExpr evaluating
/// vacuously TRUE in a raw join ON-condition) that is tracked separately and
/// not touched by this PR.
///
/// NOTE on "genuinely correlated" IN/scalar subqueries (the handoff's other
/// named trigger for the runtime-fallback path, alongside "outer query has a
/// JOIN"): investigation while writing this file found that a correlated
/// IN-subquery or scalar subquery whose WHERE clause references the outer
/// query's row by column (e.g. `b.id = r.book_id`) fails at RUNTIME with
/// "column not found: <outer column>" regardless of MATCH/BM25 -- confirmed
/// with a MATCH-free repro. The Binder successfully resolves the correlated
/// reference (bind_with_outer / build_outer_scope both work), but the plain
/// runtime expression evaluator (eval_column_ref in expr_evaluator.cpp) has
/// no knowledge of the Planner's outer_tuple_/outer_schema_ -- that context is
/// only consulted for NEAREST's k/target expressions, not for ordinary WHERE
/// conjuncts evaluated by FilterOperator/Bm25ScanOperator's residual filter.
/// This is a separate, pre-existing bug, independent of GDB-1297's index-map
/// threading fix and NOT introduced by this diff (expr_evaluator.cpp's
/// eval_column_ref / eval_in's bind path are unchanged by it). It is filed
/// separately; correlated-subquery scenarios are therefore not exercised as
/// "this must return correct rows" tests here -- only the JOIN-forced trigger
/// (which does not depend on runtime column correlation) is exercised for
/// nested/deep-chain coverage.

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
#include <utility>
#include <vector>

#include "test_qa_helpers.h"

using namespace sixseven;

namespace {

class QA_GDB1297 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1297";
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
        EXPECT_TRUE(result.has_value())
            << sql << " failed: " << (result ? "" : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    void seed_fixture() {
        exec_ok("CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR)");
        exec_ok("CREATE INDEX idx_title ON books(title) USING bm25");

        // "consciousness" appears in ids 1 and 3 only.
        exec_ok("INSERT INTO books VALUES (1, 'consciousness and the brain')");
        exec_ok("INSERT INTO books VALUES (2, 'a history of gardening')");
        exec_ok("INSERT INTO books VALUES (3, 'consciousness explained fully')");

        exec_ok("CREATE TABLE reviews (id INT PRIMARY KEY, book_id INT, stars INT)");
        exec_ok("INSERT INTO reviews VALUES (10, 1, 5)");
        exec_ok("INSERT INTO reviews VALUES (20, 2, 4)");
        exec_ok("INSERT INTO reviews VALUES (30, 3, 3)");
    }

    static std::vector<int32_t> sorted_first_col(const QueryResult& qr) {
        std::vector<int32_t> ids;
        for (const auto& row : qr.rows) {
            if (!row.empty()) {
                ids.push_back(row[0].as_int32());
            }
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<IndexManager> index_manager_;
};

// =============================================================================
// Nested subqueries: MATCH two levels deep, forced into the runtime fallback
// by an outer JOIN. The inner-most IN is non-correlated, so it decorrelates
// via rewrite_subquery_predicates WITHIN the same fallback Planner instance
// -- this checks that instance's own index-map members (populated by the
// fixed constructor) are usable for further plan-time recursion, not just for
// the single conjunct that triggered the fallback.
// =============================================================================

// Columns are explicitly table-qualified at every level (distinct aliases per
// nesting level) for the same reason the implementer's dev suite qualifies
// them: the outer scan and the decorrelated subquery result share the
// physical table "books", so an unqualified "id" would be ambiguous once the
// SEMI join's combined schema is built -- a pre-existing, orthogonal
// resolution rule, not something GDB-1297 touches.
TEST_F(QA_GDB1297, MatchTwoLevelsDeepNonCorrelatedUnderOuterJoin) {
    const std::string sql =
        "SELECT r.id, r.stars FROM reviews r INNER JOIN books b2 ON b2.id = r.book_id "
        "WHERE r.book_id IN (SELECT bm.id FROM books bm WHERE bm.id IN "
        "(SELECT bi.id FROM books bi WHERE MATCH(bi.title) TO 'consciousness'))";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;
    std::vector<std::pair<int32_t, int32_t>> rows;
    for (auto& row : result->rows) {
        rows.emplace_back(row[0].as_int32(), row[1].as_int32());
    }
    std::sort(rows.begin(), rows.end());
    EXPECT_EQ(rows, (std::vector<std::pair<int32_t, int32_t>>{{10, 5}, {30, 3}}));
}

// A SECOND JOIN, on the MIDDLE subquery, forces a SECOND, independent
// construction of the runtime-fallback Planner (via eval_in called from
// within the FIRST fallback Planner's own FilterOperator, itself triggered by
// the middle query's own JOIN). This is the deepest test of the fix: it
// requires that Planner P1 (built by the top-level eval_in from the
// top-level Planner's SubqueryContext) itself threads its own index handles
// into ITS OWN subquery_ctx_ so that a SECOND eval_in call (evaluating the
// middle query's own JOIN-forced IN against P1's SubqueryContext) can still
// find the BM25 index for the inner-most MATCH. Before the fix, P1's
// subquery_ctx_ always carried nullptr maps (SubqueryContext didn't have the
// fields at all), so this exact shape would fail with "no BM25 index
// available" one level deeper than the dev suite's single-hop repro. (A
// correlation-forced variant of this test was attempted but hits the
// pre-existing, orthogonal runtime-correlation bug documented in the file
// header instead -- JOIN-forcing at both levels avoids that entirely.)
TEST_F(QA_GDB1297, MatchTwoLevelsDeepJoinForcedSecondRuntimeFallback) {
    exec_ok("CREATE TABLE reviews_mid (id INT PRIMARY KEY, book_id INT)");
    exec_ok("INSERT INTO reviews_mid VALUES (1000, 1)");
    exec_ok("INSERT INTO reviews_mid VALUES (2000, 2)");
    exec_ok("INSERT INTO reviews_mid VALUES (3000, 3)");

    const std::string sql =
        "SELECT r.id, r.stars FROM reviews r INNER JOIN books b2 ON b2.id = r.book_id "
        "WHERE r.book_id IN (SELECT bm.id FROM books bm INNER JOIN reviews_mid rm "
        "ON rm.book_id = bm.id "
        "WHERE bm.id IN (SELECT bi.id FROM books bi WHERE MATCH(bi.title) TO 'consciousness'))";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;
    std::vector<std::pair<int32_t, int32_t>> rows;
    for (auto& row : result->rows) {
        rows.emplace_back(row[0].as_int32(), row[1].as_int32());
    }
    std::sort(rows.begin(), rows.end());
    EXPECT_EQ(rows, (std::vector<std::pair<int32_t, int32_t>>{{10, 5}, {30, 3}}));
}

// =============================================================================
// Different JOIN shapes forcing the has_joins runtime-fallback branch.
// =============================================================================

TEST_F(QA_GDB1297, MatchInsideInSubqueryUnderLeftJoin) {
    const std::string sql =
        "SELECT r.id, r.stars FROM reviews r LEFT JOIN books b2 ON b2.id = r.book_id "
        "WHERE r.book_id IN (SELECT id FROM books WHERE MATCH(title) TO 'consciousness')";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;
    auto ids = sorted_first_col(*result);
    EXPECT_EQ(ids, (std::vector<int32_t>{10, 30}));
}

TEST_F(QA_GDB1297, MatchInsideInSubqueryUnderTwoJoins) {
    exec_ok("CREATE TABLE tags (id INT PRIMARY KEY, book_id INT, label VARCHAR)");
    exec_ok("INSERT INTO tags VALUES (100, 1, 'philosophy')");
    exec_ok("INSERT INTO tags VALUES (200, 2, 'nature')");
    exec_ok("INSERT INTO tags VALUES (300, 3, 'philosophy')");

    const std::string sql =
        "SELECT r.id FROM reviews r "
        "INNER JOIN books b2 ON b2.id = r.book_id "
        "INNER JOIN tags t ON t.book_id = r.book_id "
        "WHERE r.book_id IN (SELECT id FROM books WHERE MATCH(title) TO 'consciousness')";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;
    auto ids = sorted_first_col(*result);
    EXPECT_EQ(ids, (std::vector<int32_t>{10, 30}));
}

// =============================================================================
// Missing-index error path THROUGH the runtime-fallback (JOIN forces it).
// The dev suite (MatchInsideInSubqueryStillErrorsWithoutIndex) only exercises
// this error through the plan-time-decorrelated path (no JOIN); this checks
// the SAME error still surfaces correctly when eval_in's fallback Planner is
// the one doing the planning.
// =============================================================================

TEST_F(QA_GDB1297, MatchInsideInSubqueryUnderJoinMissingIndexEntirelyErrors) {
    exec_ok("CREATE TABLE notes (id INT PRIMARY KEY, body VARCHAR)");
    exec_ok("INSERT INTO notes VALUES (1, 'hello world')");

    const std::string sql =
        "SELECT r.id FROM reviews r INNER JOIN books b2 ON b2.id = r.book_id "
        "WHERE r.book_id IN (SELECT id FROM notes WHERE MATCH(body) TO 'hello')";
    auto result = engine_->execute(sql);
    ASSERT_FALSE(result.has_value()) << "expected NOT_FOUND, got a result for: " << sql;
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
    EXPECT_NE(result.error().message.find("BM25"), std::string::npos) << result.error().message;
}

// A BM25 index exists on the table but on a DIFFERENT column than the one
// MATCH() targets -- must still surface the more specific "no BM25 index on
// column 'title'" error through the runtime-fallback path, not a false
// positive and not a crash.
TEST_F(QA_GDB1297, MatchInsideInSubqueryUnderJoinWrongColumnIndexErrors) {
    exec_ok("CREATE TABLE mixed (id INT PRIMARY KEY, title VARCHAR, body VARCHAR)");
    exec_ok("CREATE INDEX idx_body ON mixed(body) USING bm25");
    exec_ok("INSERT INTO mixed VALUES (1, 'consciousness and the brain', 'unrelated text')");

    const std::string sql =
        "SELECT r.id FROM reviews r INNER JOIN books b2 ON b2.id = r.book_id "
        "WHERE r.book_id IN (SELECT id FROM mixed WHERE MATCH(title) TO 'consciousness')";
    auto result = engine_->execute(sql);
    ASSERT_FALSE(result.has_value()) << "expected NOT_FOUND, got a result for: " << sql;
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
    EXPECT_NE(result.error().message.find("title"), std::string::npos) << result.error().message;
}

// =============================================================================
// Empty-result paths, exercised through the runtime-fallback (JOIN present).
// =============================================================================

TEST_F(QA_GDB1297, MatchInsideInSubqueryUnderJoinNoMatchesReturnsEmpty) {
    const std::string sql =
        "SELECT r.id FROM reviews r INNER JOIN books b2 ON b2.id = r.book_id "
        "WHERE r.book_id IN (SELECT id FROM books WHERE MATCH(title) TO 'zzz_no_such_term')";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;
    EXPECT_EQ(result->rows.size(), 0u);
}

TEST_F(QA_GDB1297, MatchInsideInSubqueryUnderJoinEmptyOuterTableReturnsEmpty) {
    exec_ok("CREATE TABLE empty_reviews (id INT PRIMARY KEY, book_id INT, stars INT)");
    const std::string sql =
        "SELECT er.id FROM empty_reviews er INNER JOIN books b2 ON b2.id = er.book_id "
        "WHERE er.book_id IN (SELECT id FROM books WHERE MATCH(title) TO 'consciousness')";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;
    EXPECT_EQ(result->rows.size(), 0u);
}

// NOT IN whose subquery matches nothing: every outer row must be returned,
// through the runtime-fallback path (JOIN present).
TEST_F(QA_GDB1297, MatchInsideNotInSubqueryUnderJoinNoMatchesReturnsAll) {
    const std::string sql =
        "SELECT r.id FROM reviews r INNER JOIN books b2 ON b2.id = r.book_id "
        "WHERE r.book_id NOT IN (SELECT id FROM books WHERE MATCH(title) TO 'zzz_no_such_term')";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;
    auto ids = sorted_first_col(*result);
    EXPECT_EQ(ids, (std::vector<int32_t>{10, 20, 30}));
}

// =============================================================================
// Multiple unconsumed WHERE conjuncts under a JOIN: the has_joins branch
// chains each unconsumed conjunct as its own FilterOperator. Verify a
// MATCH-subquery conjunct AND-ed with a plain sibling predicate are BOTH
// applied (neither dropped nor short-circuited by the chain-building loop).
// =============================================================================

TEST_F(QA_GDB1297, MatchSubqueryConjunctAndedWithSiblingPredicateUnderJoin) {
    const std::string sql = "SELECT r.id FROM reviews r INNER JOIN books b2 ON b2.id = r.book_id "
                            "WHERE r.stars > 3 AND r.book_id IN "
                            "(SELECT id FROM books WHERE MATCH(title) TO 'consciousness')";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;
    // Only review 10 (book 1, stars=5) satisfies both stars > 3 AND book_id
    // matches; review 30 (book 3, stars=3) fails the stars filter.
    auto ids = sorted_first_col(*result);
    EXPECT_EQ(ids, (std::vector<int32_t>{10}));
}

// =============================================================================
// NULL semantics must survive the new runtime-fallback path unchanged: a
// nullable column selected by the MATCH-filtered subquery containing a NULL
// makes "NOT IN" UNKNOWN for every outer row (the classic NOT IN + NULL
// footgun), regardless of which rows actually matched MATCH().
// =============================================================================

TEST_F(QA_GDB1297, NotInSubqueryUnderJoinWithNullInResultSetReturnsNoRows) {
    exec_ok("CREATE TABLE books_nullable (id INT PRIMARY KEY, title VARCHAR, ref_id INT)");
    exec_ok("CREATE INDEX idx_title_nullable ON books_nullable(title) USING bm25");
    // ref_id is nullable; book 1 matches MATCH() but has a NULL ref_id.
    exec_ok("INSERT INTO books_nullable VALUES (1, 'consciousness and the brain', NULL)");
    exec_ok("INSERT INTO books_nullable VALUES (2, 'consciousness explained', 30)");

    const std::string sql =
        "SELECT r.id FROM reviews r INNER JOIN books b2 ON b2.id = r.book_id "
        "WHERE r.book_id NOT IN "
        "(SELECT ref_id FROM books_nullable WHERE MATCH(title) TO 'consciousness')";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;
    EXPECT_EQ(result->rows.size(), 0u)
        << "NOT IN against a result set containing NULL must return 0 rows for every outer row, "
           "even though the MATCH predicate itself matched rows correctly";
}

// =============================================================================
// Scale: many outer rows through the runtime-fallback path, which
// re-constructs a Planner (and re-plans the BM25 scan) once per outer row.
// Checks correctness at scale and gives ASan a wider surface to catch any
// per-row leak or use-after-free in the repeated Planner construction.
// =============================================================================

TEST_F(QA_GDB1297, ManyOuterRowsThroughRuntimeFallbackRemainCorrect) {
    exec_ok("CREATE TABLE many_reviews (id INT PRIMARY KEY, book_id INT, stars INT)");
    constexpr int kRows = 500;
    for (int i = 0; i < kRows; ++i) {
        const int book_id = (i % 3) + 1; // cycles through books 1,2,3
        exec_ok("INSERT INTO many_reviews VALUES (" + std::to_string(i) + ", " +
                std::to_string(book_id) + ", " + std::to_string(i % 5) + ")");
    }

    const std::string sql =
        "SELECT mr.id, mr.book_id FROM many_reviews mr INNER JOIN books b2 ON b2.id = mr.book_id "
        "WHERE mr.book_id IN (SELECT id FROM books WHERE MATCH(title) TO 'consciousness')";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;

    // book_id cycles 1,2,3,1,2,3,...; books matching "consciousness" are 1 and
    // 3, i.e. 2 of every 3 rows.
    size_t expected = 0;
    for (int i = 0; i < kRows; ++i) {
        if ((i % 3) + 1 != 2) {
            ++expected;
        }
    }
    EXPECT_EQ(result->rows.size(), expected);
    for (auto& row : result->rows) {
        int32_t book_id = row[1].as_int32();
        EXPECT_NE(book_id, 2) << "book 2 ('gardening') must never appear in MATCH results";
    }
}

} // namespace
