// GDB-1252: MATCH(...) BM25 full-text search combined with a JOIN.
//
// Mirror of GDB-1249's NEAREST+JOIN stopgap: any query combining a BM25
// MATCH(...) predicate with a JOIN previously silently ignored the full-text
// search (no relevance ranking, no _score, no error) and returned the full
// join output, because try_plan_bm25_scan only runs in the joinless
// WHERE-pushdown path, and a residual MatchExpr evaluates to TRUE by
// construction. This stopgap rejects the combination with INVALID_ARGUMENT
// naming MATCH, JOIN, and the derived-table workaround, while leaving the
// derived-table workaround itself (MATCH inside a subquery, joined at the
// outer level) fully functional.

#include "sixseven/common/status.h"
#include "sixseven/common/value.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "query_engine_fixture.h"

using namespace sixseven;

namespace {

class MatchJoinRejectTest : public QueryEngineFixture {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_match_join_reject";
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

    static void assert_rejected(const Result<QueryResult>& result, const std::string& needle) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
        const std::string& m = result.error().message;
        EXPECT_NE(m.find("MATCH"), std::string::npos) << m;
        EXPECT_NE(m.find(needle), std::string::npos) << m;
    }
};

// MATCH in a WHERE clause with a JOIN must be rejected (single join).
TEST_F(MatchJoinRejectTest, MatchInWhereWithJoinRejected) {
    assert_rejected(engine_->execute("SELECT b.title, r.stars FROM books b INNER JOIN reviews r ON "
                                     "r.book_id = b.id WHERE MATCH(b.title) TO 'consciousness'"),
                    "JOIN");
}

// MATCH in a WHERE clause with multiple joins must be rejected.
TEST_F(MatchJoinRejectTest, MatchInWhereWithMultipleJoinsRejected) {
    assert_rejected(engine_->execute("SELECT b.title FROM books b "
                                     "INNER JOIN reviews r ON r.book_id = b.id "
                                     "INNER JOIN reviews r2 ON r2.book_id = b.id "
                                     "WHERE MATCH(b.title) TO 'consciousness'"),
                    "JOIN");
}

// MATCH AND-ed with another predicate, in a query with a JOIN, is still
// rejected (the residual-TRUE bug applies regardless of sibling conjuncts).
TEST_F(MatchJoinRejectTest, MatchAndedWithOtherPredicateWithJoinRejected) {
    assert_rejected(
        engine_->execute("SELECT b.title FROM books b INNER JOIN reviews r ON r.book_id = b.id "
                         "WHERE MATCH(b.title) TO 'consciousness' AND r.stars > 2"),
        "JOIN");
}

// The error message names the derived-table workaround explicitly.
TEST_F(MatchJoinRejectTest, ErrorNamesDerivedTableWorkaround) {
    auto result =
        engine_->execute("SELECT b.title FROM books b INNER JOIN reviews r ON r.book_id = b.id "
                         "WHERE MATCH(b.title) TO 'consciousness'");
    ASSERT_FALSE(result.has_value());
    const std::string& m = result.error().message;
    EXPECT_NE(m.find("derived table"), std::string::npos) << m;
}

// MATCH inside a JOIN ON clause is rejected with the same error class.
TEST_F(MatchJoinRejectTest, MatchInJoinOnClauseRejected) {
    assert_rejected(
        engine_->execute("SELECT b.title FROM books b INNER JOIN reviews r ON r.book_id = b.id "
                         "AND MATCH(b.title) TO 'consciousness'"),
        "ON clause");
}

// The derived-table workaround must NOT be over-rejected: MATCH lives inside
// the derived table's own (joinless) plan_select, so it plans as a real BM25
// scan there; the outer query's own WHERE/ON exprs contain no MATCH.
TEST_F(MatchJoinRejectTest, DerivedTableWorkaroundSucceeds) {
    auto result =
        engine_->execute("SELECT nb.id, r.stars FROM "
                         "(SELECT id, _score FROM books WHERE MATCH(title) TO 'consciousness') nb "
                         "JOIN reviews r ON nb.id = r.book_id");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    std::vector<int32_t> ids;
    for (const auto& row : result->rows) {
        ASSERT_FALSE(row.empty());
        ids.push_back(row[0].as_int32());
    }
    std::sort(ids.begin(), ids.end());
    // Only books 1 and 3 contain "consciousness"; book 2 must be excluded.
    EXPECT_EQ(ids, (std::vector<int32_t>{1, 3}));
}

// Single-table MATCH + _score regressions must remain intact (no JOIN at all).
TEST_F(MatchJoinRejectTest, SingleTableMatchUnaffected) {
    auto result = engine_->execute("SELECT id, _score FROM books WHERE MATCH(title) TO "
                                   "'consciousness' ORDER BY _score DESC");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 2u);
}

} // namespace
