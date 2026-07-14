// GDB-1297: BM25 MATCH(...) inside an IN-subquery.
//
// A non-correlated `... WHERE col IN (SELECT ... WHERE MATCH(...))` with no
// JOIN in the outer query is decorrelated into a SEMI join at plan time
// (Planner::rewrite_subquery_predicates), which recurses through the same
// Planner instance -- so it already saw bm25_indexes_ correctly before this
// fix. The bug is in a second, separate path: when the IN-subquery predicate
// CANNOT be decorrelated at plan time -- e.g. the outer query has a JOIN
// (handled by the has_joins predicate-pushdown branch in Planner::plan_select,
// which chains any unconsumed conjunct as a plain runtime FilterOperator
// instead of calling rewrite_subquery_predicates), or the subquery is
// genuinely correlated -- the predicate is re-evaluated per outer row by
// expr_evaluator.cpp's eval_in(), which constructs its OWN fresh Planner from
// SubqueryContext. SubqueryContext previously carried only catalog/storage/
// graph_engine/provider_registry, so that fallback Planner's bm25_indexes_
// (and sibling hnsw_indexes_/btree_indexes_/hash_indexes_/hnsw_rid_maps_)
// were always nullptr, and try_plan_bm25_scan failed with "no BM25 index
// available" even though the table has one. This file locks in the JOIN
// shape (the smallest reliable repro of the runtime-fallback path) plus
// several plan-time-decorrelated shapes that were already correct and must
// stay that way.

#include "sixseven/common/status.h"
#include "sixseven/common/value.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "query_engine_fixture.h"

using namespace sixseven;

namespace {

class Bm25SubqueryTest : public QueryEngineFixture {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_bm25_subquery";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        make_stack();
        run_bootstrap();
        rebuild_indexes();
        seed();
    }

    void TearDown() override {
        reset_stack();
        std::filesystem::remove_all(data_dir_);
    }

    void seed() {
        exec_ok("CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR)");
        exec_ok("CREATE INDEX idx_title ON books(title) USING bm25");
        exec_ok("INSERT INTO books VALUES (1, 'consciousness and the brain')");
        exec_ok("INSERT INTO books VALUES (2, 'a history of gardening')");
        exec_ok("INSERT INTO books VALUES (3, 'consciousness explained')");

        exec_ok("CREATE TABLE reviews (id INT PRIMARY KEY, book_id INT, stars INT)");
        exec_ok("INSERT INTO reviews VALUES (10, 1, 5)");
        exec_ok("INSERT INTO reviews VALUES (20, 2, 4)");
        exec_ok("INSERT INTO reviews VALUES (30, 3, 3)");
    }
};

// The exact shape called out in GDB-1297: MATCH inside an IN-subquery over
// the same table must find the BM25 index and filter correctly. Columns are
// table-qualified because the outer and subquery FROM are the same physical
// table ("books"), so an unqualified `id` would be ambiguous across the two
// scans once combined for the SEMI join -- a pre-existing, orthogonal
// resolution rule unrelated to the bm25_indexes_ propagation bug under test.
TEST_F(Bm25SubqueryTest, MatchInsideInSubquerySameTable) {
    auto r = exec_ok("SELECT books.id FROM books WHERE books.id IN (SELECT id FROM books WHERE "
                     "MATCH(title) TO 'consciousness')");
    ASSERT_EQ(r.rows.size(), 2u);
    std::vector<int32_t> ids;
    for (auto& row : r.rows) {
        ids.push_back(row[0].as_int32());
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<int32_t>{1, 3}));
}

// A more realistic shape: filter a second table by membership in a BM25
// full-text match over the first. Book 2 ("gardening") must be excluded, so
// review 20 (book_id = 2) must not appear.
TEST_F(Bm25SubqueryTest, MatchInsideInSubqueryFiltersOuterTable) {
    auto r = exec_ok("SELECT id, stars FROM reviews WHERE book_id IN "
                     "(SELECT id FROM books WHERE MATCH(title) TO 'consciousness')");
    ASSERT_EQ(r.rows.size(), 2u);
    std::vector<int32_t> ids;
    for (auto& row : r.rows) {
        ids.push_back(row[0].as_int32());
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<int32_t>{10, 30}));
}

// NOT IN with a MATCH subquery must also plan a real BM25 scan (ANTI join
// path) rather than erroring on a missing index.
TEST_F(Bm25SubqueryTest, MatchInsideNotInSubquery) {
    auto r = exec_ok("SELECT id, stars FROM reviews WHERE book_id NOT IN "
                     "(SELECT id FROM books WHERE MATCH(title) TO 'consciousness')");
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0].as_int32(), 20);
}

// GDB-1297 core repro: the outer query has a JOIN, so the IN-subquery's
// MATCH predicate cannot be decorrelated into a join at plan time and is
// instead re-planned per outer row by expr_evaluator.cpp's eval_in(). Before
// the fix this failed outright with "no BM25 index available" despite
// idx_title existing; confirmed by temporarily reverting the fix and
// observing this exact query fail with that message.
TEST_F(Bm25SubqueryTest, MatchInsideInSubqueryUnderOuterJoin) {
    auto r = exec_ok("SELECT r.id, r.stars FROM reviews r INNER JOIN books b2 ON b2.id = r.book_id "
                     "WHERE r.book_id IN (SELECT id FROM books WHERE MATCH(title) TO "
                     "'consciousness')");
    ASSERT_EQ(r.rows.size(), 2u);
    std::vector<std::pair<int32_t, int32_t>> rows;
    for (auto& row : r.rows) {
        rows.emplace_back(row[0].as_int32(), row[1].as_int32());
    }
    std::sort(rows.begin(), rows.end());
    // Review 10 (book 1, "consciousness and the brain") and review 30 (book 3,
    // "consciousness explained") qualify; review 20 (book 2, "gardening") does not.
    EXPECT_EQ(rows, (std::vector<std::pair<int32_t, int32_t>>{{10, 5}, {30, 3}}));
}

// NOT IN variant of the same JOIN-forced runtime-fallback path (ANTI-join
// semantics evaluated via eval_in's negation branch).
TEST_F(Bm25SubqueryTest, MatchInsideNotInSubqueryUnderOuterJoin) {
    auto r = exec_ok("SELECT r.id, r.stars FROM reviews r INNER JOIN books b2 ON b2.id = r.book_id "
                     "WHERE r.book_id NOT IN (SELECT id FROM books WHERE MATCH(title) TO "
                     "'consciousness')");
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0].as_int32(), 20);
    EXPECT_EQ(r.rows[0][1].as_int32(), 4);
}

// A MATCH subquery whose predicate matches nothing must yield an empty outer
// result rather than an error.
TEST_F(Bm25SubqueryTest, MatchInsideInSubqueryNoMatches) {
    auto r = exec_ok("SELECT id FROM books WHERE id IN (SELECT id FROM books WHERE "
                     "MATCH(title) TO 'nonexistentword')");
    EXPECT_EQ(r.rows.size(), 0u);
}

// Sanity check: an IN-subquery over a table with no BM25 index must still
// produce the pre-existing NOT_FOUND error, not a false positive from the fix.
TEST_F(Bm25SubqueryTest, MatchInsideInSubqueryStillErrorsWithoutIndex) {
    exec_ok("CREATE TABLE notes (id INT PRIMARY KEY, body VARCHAR)");
    exec_ok("INSERT INTO notes VALUES (1, 'hello world')");
    auto r = engine_->execute(
        "SELECT id FROM books WHERE id IN (SELECT id FROM notes WHERE MATCH(body) TO 'hello')");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NOT_FOUND);
}

} // namespace
