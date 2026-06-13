/// @file test_qa_gdb_1249.cpp
/// @brief QA regression tests for GDB-1249, updated for GDB-1250.
///
/// GDB-1249 originally rejected every NEAREST+JOIN query as a silent-no-op
/// stopgap. GDB-1250 implements real pushdown: NEAREST in a WHERE with a JOIN
/// now succeeds (the vector scan is pushed to the table owning the EMBEDDING
/// column). These tests were updated to assert the post-GDB-1250 behavior:
///   * NEAREST in a WHERE with a JOIN now SUCCEEDS and returns the k nearest.
///   * NEAREST in a JOIN ON clause is still rejected (now: "ON clause").
///   * NEAREST in a non-AND position (OR / NOT) is rejected ("top-level AND").
///   * Two NEAREST conjuncts are rejected ("one NEAREST").
///   * The derived-table workaround continues to plan and execute unchanged.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/embedding_column.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_qa_helpers.h"

using namespace sixseven;

// =============================================================================
// Fixture: books(id, title, description_vec EMBEDDING), reviews(id, book_id,
// stars), readers(id, name), with edges follows(readers->readers) so the
// WITHIN TRAVERSE variant parses.
// =============================================================================

class QA_GDB1249 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1249";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        seed_fixture();
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void register_embedding(table_id_t table_id, int32_t col_id, int32_t dim) {
        EmbeddingColumnDef emb_def;
        emb_def.table_id = table_id;
        emb_def.column_id = col_id;
        emb_def.dimension = dim;
        emb_def.source_expr = "title";
        emb_def.provider = "builtin/4";
        auto reg = catalog_.register_embedding_column(emb_def);
        ASSERT_TRUE(reg.has_value()) << reg.error().message;
    }

    void seed_fixture() {
        exec_ok(
            "CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR, description_vec EMBEDDING)");
        auto books = catalog_.get_table(default_database_id, "books");
        ASSERT_TRUE(books.has_value());
        register_embedding(books->table_id, 2, 4);

        // Vectors chosen so the query [1,0,0,0] orders books 1, 2, 3, 4 by
        // increasing distance. The 2 nearest are ids 1 and 2.
        exec_ok("INSERT INTO books VALUES (1, 'alpha', [1.0, 0.0, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (2, 'beta',  [0.9, 0.1, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (3, 'gamma', [0.2, 0.8, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (4, 'delta', [0.0, 1.0, 0.0, 0.0])");

        exec_ok("CREATE TABLE reviews (id INT PRIMARY KEY, book_id INT, stars INT)");
        // One review per book so a join of the 2 nearest yields exactly 2 rows.
        exec_ok("INSERT INTO reviews VALUES (10, 1, 5)");
        exec_ok("INSERT INTO reviews VALUES (20, 2, 4)");
        exec_ok("INSERT INTO reviews VALUES (30, 3, 3)");
        exec_ok("INSERT INTO reviews VALUES (40, 4, 2)");

        exec_ok("CREATE TABLE readers (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO readers VALUES (1, 'reader-one')");
        exec_ok("CREATE EDGE TYPE follows FROM readers TO readers");
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

    // GDB-1250: NEAREST+JOIN is no longer blanket-rejected. The remaining
    // rejections carry a specific reason; assert INVALID_ARGUMENT plus a needle.
    static void expect_invalid(const Result<QueryResult>& result, const std::string& needle,
                               const std::string& sql) {
        ASSERT_FALSE(result.has_value()) << "expected rejection for: " << sql;
        EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT) << sql;
        const std::string& msg = result.error().message;
        EXPECT_NE(msg.find(needle), std::string::npos)
            << "message must contain '" << needle << "': " << msg;
    }

    // GDB-1250: NEAREST in a WHERE with a JOIN now succeeds and returns the k
    // nearest books' joined rows. Asserts the book ids on column 0.
    void expect_nearest_join_books(const std::string& sql,
                                   const std::vector<int32_t>& expected) {
        auto result = engine_->execute(sql);
        ASSERT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;
        EXPECT_EQ(sorted_first_col(*result), expected) << sql;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// AC1 (GDB-1250): NEAREST in WHERE with a JOIN now SUCCEEDS, returning the
// joined rows of the k nearest books. The 2 nearest to [1,0,0,0] are {1,2}.
// =============================================================================

TEST_F(QA_GDB1249, NearestInWhereWithSingleJoinNowSucceeds) {
    expect_nearest_join_books("SELECT b.id, r.stars FROM books b "
                              "INNER JOIN reviews r ON r.book_id = b.id "
                              "WHERE NEAREST(description_vec, 2) TO [1.0, 0.0, 0.0, 0.0]",
                              {1, 2});
}

TEST_F(QA_GDB1249, NearestAndedWithOtherPredicateNowSucceeds) {
    // r.stars > 3 keeps only book 1's review (book 2 has 4 stars > 3 too),
    // so {1,2} both survive: book 1 has 5 stars, book 2 has 4 stars.
    expect_nearest_join_books(
        "SELECT b.id FROM books b "
        "INNER JOIN reviews r ON r.book_id = b.id "
        "WHERE NEAREST(description_vec, 2) TO [1.0, 0.0, 0.0, 0.0] AND r.stars > 3",
        {1, 2});
}

// =============================================================================
// AC2 (GDB-1250): NEAREST in a JOIN ON clause is still rejected, now with a
// reason that names the ON clause specifically.
// =============================================================================

TEST_F(QA_GDB1249, NearestInJoinOnClauseRejected) {
    const std::string sql = "SELECT b.id FROM books b "
                            "INNER JOIN reviews r ON r.book_id = b.id "
                            "AND NEAREST(description_vec, 2) TO [1.0, 0.0, 0.0, 0.0]";
    expect_invalid(engine_->execute(sql), "ON clause", sql);
}

// =============================================================================
// AC3: derived-table workaround is NOT over-rejected and returns only the
// nearest books' joined reviews (exact ids asserted against fixture vectors).
// =============================================================================

TEST_F(QA_GDB1249, DerivedTableNearestWithJoinSucceeds) {
    const std::string sql = "SELECT nb.id, r.stars FROM "
                            "(SELECT id, title, _distance FROM books "
                            " WHERE NEAREST(description_vec, 2) TO [1.0, 0.0, 0.0, 0.0]) nb "
                            "JOIN reviews r ON nb.id = r.book_id";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;

    // The 2 nearest books to [1,0,0,0] are ids 1 and 2; each has one review.
    auto ids = sorted_first_col(*result);
    std::vector<int32_t> expected = {1, 2};
    EXPECT_EQ(ids, expected) << "derived-table NEAREST returned the wrong books";
}

// =============================================================================
// AC4: single-table NEAREST is unaffected by the join guard.
// =============================================================================

TEST_F(QA_GDB1249, SingleTableNearestStillWorks) {
    const std::string sql =
        "SELECT id FROM books WHERE NEAREST(description_vec, 2) TO [1.0, 0.0, 0.0, 0.0]";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto ids = sorted_first_col(*result);
    std::vector<int32_t> expected = {1, 2};
    EXPECT_EQ(ids, expected);
}

// A join query with NO nearest predicate must remain unaffected.
TEST_F(QA_GDB1249, JoinWithoutNearestUnaffected) {
    const std::string sql =
        "SELECT b.id, r.stars FROM books b INNER JOIN reviews r ON r.book_id = b.id";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 4u);
}

// =============================================================================
// Adversarial: the guard must hold across join variants, boolean nesting, and
// multiple NEAREST predicates, and must reject (not silently no-op) every WHERE
// / ON position. It must NOT over-reject derived-table workarounds.
// =============================================================================

namespace {
constexpr const char* kVec = "[1.0, 0.0, 0.0, 0.0]";
}

// --- Boolean nesting of NEAREST under a join (GDB-1250 positions) ----------

// NOT NEAREST is not a top-level AND conjunct -> rejected with that reason.
TEST_F(QA_GDB1249, NearestUnderNotWithJoinRejected) {
    const std::string sql = std::string("SELECT b.id FROM books b "
                                        "JOIN reviews r ON r.book_id = b.id "
                                        "WHERE NOT NEAREST(description_vec, 2) TO ") +
                            kVec;
    expect_invalid(engine_->execute(sql), "top-level AND", sql);
}

// NEAREST under OR is not a top-level AND conjunct -> rejected.
TEST_F(QA_GDB1249, NearestUnderOrWithJoinRejected) {
    const std::string sql = std::string("SELECT b.id FROM books b "
                                        "JOIN reviews r ON r.book_id = b.id "
                                        "WHERE NEAREST(description_vec, 2) TO ") +
                            kVec + " OR r.stars > 3";
    expect_invalid(engine_->execute(sql), "top-level AND", sql);
}

// A parenthesized single NEAREST is still a top-level conjunct -> now SUCCEEDS.
TEST_F(QA_GDB1249, NearestInParensWithJoinNowSucceeds) {
    expect_nearest_join_books(std::string("SELECT b.id FROM books b "
                                          "JOIN reviews r ON r.book_id = b.id "
                                          "WHERE (NEAREST(description_vec, 2) TO ") +
                                  kVec + ")",
                              {1, 2});
}

// Two NEAREST predicates AND-ed together must still be rejected (one per query).
TEST_F(QA_GDB1249, MultipleNearestWithJoinRejected) {
    const std::string sql = std::string("SELECT b.id FROM books b "
                                        "JOIN reviews r ON r.book_id = b.id "
                                        "WHERE NEAREST(description_vec, 2) TO ") +
                            kVec + " AND NEAREST(description_vec, 3) TO " + kVec;
    expect_invalid(engine_->execute(sql), "one NEAREST", sql);
}

// NEAREST in the ON clause AND in WHERE simultaneously: ON-clause check fires.
TEST_F(QA_GDB1249, NearestInOnAndWhereRejected) {
    const std::string sql =
        std::string("SELECT b.id FROM books b "
                    "JOIN reviews r ON r.book_id = b.id AND NEAREST(description_vec, 2) TO ") +
        kVec + " WHERE NEAREST(description_vec, 3) TO " + kVec;
    expect_invalid(engine_->execute(sql), "ON clause", sql);
}

// --- Outer / cross join variants (GDB-1250) --------------------------------

// LEFT JOIN with NEAREST on the preserved (base, books) side now SUCCEEDS.
TEST_F(QA_GDB1249, NearestWithLeftJoinOnPreservedSideNowSucceeds) {
    expect_nearest_join_books(std::string("SELECT b.id FROM books b "
                                          "LEFT JOIN reviews r ON r.book_id = b.id "
                                          "WHERE NEAREST(description_vec, 2) TO ") +
                                  kVec,
                              {1, 2});
}

// CROSS JOIN has no nullable side; NEAREST on books now SUCCEEDS. Each of the 2
// nearest books crosses with all 4 reviews -> 8 rows, book ids {1,2}.
TEST_F(QA_GDB1249, NearestWithCrossJoinNowSucceeds) {
    auto result = engine_->execute(std::string("SELECT b.id FROM books b "
                                               "CROSS JOIN reviews r "
                                               "WHERE NEAREST(description_vec, 2) TO ") +
                                   kVec);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 8u);
    EXPECT_EQ(sorted_first_col(*result), (std::vector<int32_t>{1, 1, 1, 1, 2, 2, 2, 2}));
}

// --- Not over-rejected: more complex derived-table shapes -------------------

// A derived table joined to ANOTHER derived table must still plan and return
// only the nearest rows' joined output.
TEST_F(QA_GDB1249, DerivedTableJoinedToDerivedTableSucceeds) {
    const std::string sql =
        std::string("SELECT x.id FROM "
                    "(SELECT id, _distance FROM books WHERE NEAREST(description_vec, 2) TO ") +
        kVec +
        ") x JOIN (SELECT book_id FROM reviews) y ON x.id = y.book_id";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;
    auto ids = sorted_first_col(*result);
    std::vector<int32_t> expected = {1, 2};
    EXPECT_EQ(ids, expected);
}

// A doubly-nested derived table (NEAREST two levels down) must not be rejected.
TEST_F(QA_GDB1249, DoublyNestedDerivedTableNearestSucceeds) {
    const std::string sql =
        std::string("SELECT z.id FROM "
                    "(SELECT id FROM (SELECT id, _distance FROM books WHERE "
                    "NEAREST(description_vec, 2) TO ") +
        kVec + ") inner1) z JOIN reviews r ON z.id = r.book_id";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;
    auto ids = sorted_first_col(*result);
    std::vector<int32_t> expected = {1, 2};
    EXPECT_EQ(ids, expected);
}

// A correlated/uncorrelated subquery in WHERE whose own body contains NEAREST
// (the outer join has no NEAREST of its own) must NOT be rejected: the subquery
// is planned by its own join-free plan_select and gets a real NearestScan.
TEST_F(QA_GDB1249, NearestInsideWhereSubqueryNotOverRejected) {
    const std::string sql =
        std::string("SELECT b.id FROM books b JOIN reviews r ON r.book_id = b.id "
                    "WHERE b.id IN (SELECT id FROM books b2 WHERE "
                    "NEAREST(description_vec, 2) TO ") +
        kVec + ")";
    auto result = engine_->execute(sql);
    ASSERT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;
    auto ids = sorted_first_col(*result);
    std::vector<int32_t> expected = {1, 2};
    EXPECT_EQ(ids, expected);
}
