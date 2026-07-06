// GDB-1250: NEAREST(...) (including WITHIN TRAVERSE graph scoping) pushdown
// through JOINs. The planner pushes the vector scan down to the table owning
// the searched EMBEDDING column; that NearestScan becomes the owning side's
// source, so the join consumes exactly the k nearest rows and the synthetic
// `_distance` column lands in the combined output schema.
//
// Fixtures use literal vectors so the nearest order is unambiguous and exact
// ids/order can be asserted. Cosine distance (the NEAREST default) of the query
// vector [1,0,0,0] against the seeded books:
//   id 1 [1.0,0.0,0.0,0.0]  -> 0.0    (nearest)
//   id 2 [0.9,0.1,0.0,0.0]  -> ~0.006
//   id 3 [0.0,1.0,0.0,0.0]  -> 1.0
//   id 4 [0.0,0.0,1.0,0.0]  -> 1.0
//   id 5 [0.0,0.0,0.0,1.0]  -> 1.0
// so the k=2 set is {1,2}, k=3 set is {1,2,3}, ranked 1 < 2 < 3.

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

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

class NearestJoinTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_nearest_join";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        seed();
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        ASSERT_TRUE(result.has_value()) << sql << ": " << result.error().message;
    }

    QueryResult run_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    static void assert_error(const Result<QueryResult>& result, const std::string& needle) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT) << result.error().message;
        EXPECT_NE(result.error().message.find(needle), std::string::npos) << result.error().message;
    }

    // Collect a single INT column across all rows as int32, sorted ascending.
    static std::vector<int32_t> sorted_int_col(const QueryResult& qr, size_t col) {
        std::vector<int32_t> ids;
        for (const auto& row : qr.rows) {
            ids.push_back(row[col].as_int32());
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    void seed() {
        exec_ok(
            "CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR, description_vec EMBEDDING)");
        auto books = catalog_.get_table(default_database_id, "books");
        ASSERT_TRUE(books.has_value());
        EmbeddingColumnDef emb;
        emb.table_id = books->table_id;
        emb.column_id = 2;
        emb.dimension = 4;
        emb.source_expr = "title";
        emb.provider = "builtin/4";
        ASSERT_TRUE(catalog_.register_embedding_column(emb).has_value());

        exec_ok("INSERT INTO books VALUES (1, 'Alpha',  [1.0, 0.0, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (2, 'Apex',   [0.9, 0.1, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (3, 'Gamma',  [0.0, 1.0, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (4, 'Delta',  [0.0, 0.0, 1.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (5, 'Epsilon',[0.0, 0.0, 0.0, 1.0])");

        // One review per book (book_id matches the book id), with varied stars.
        exec_ok("CREATE TABLE reviews (id INT PRIMARY KEY, book_id INT, stars INT)");
        exec_ok("INSERT INTO reviews VALUES (10, 1, 5)");
        exec_ok("INSERT INTO reviews VALUES (20, 2, 3)");
        exec_ok("INSERT INTO reviews VALUES (30, 3, 4)");
        exec_ok("INSERT INTO reviews VALUES (40, 4, 5)");
        exec_ok("INSERT INTO reviews VALUES (50, 5, 2)");
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// AC1: the hybrid demo query succeeds and returns reviews of exactly the k
// nearest books. With one review per book, k=2 yields books {1,2}.
TEST_F(NearestJoinTest, DemoQueryReturnsReviewsOfKNearestBooks) {
    auto qr = run_ok("SELECT b.id, r.id "
                     "FROM books b INNER JOIN reviews r ON r.book_id = b.id "
                     "WHERE NEAREST(b.description_vec, 2) TO [1.0, 0.0, 0.0, 0.0]");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(sorted_int_col(qr, 0), (std::vector<int32_t>{1, 2}));
    EXPECT_EQ(sorted_int_col(qr, 1), (std::vector<int32_t>{10, 20}));
}

// AC1: _distance resolves through the join and ORDER BY _distance ASC ranks
// the joined output (book 1 is exactly at distance 0, then book 2).
TEST_F(NearestJoinTest, DistanceColumnResolvesAndOrdersThroughJoin) {
    auto qr = run_ok("SELECT b.id, r.stars, _distance "
                     "FROM books b INNER JOIN reviews r ON r.book_id = b.id "
                     "WHERE NEAREST(b.description_vec, 3) TO [1.0, 0.0, 0.0, 0.0] "
                     "ORDER BY _distance ASC");
    ASSERT_EQ(qr.rows.size(), 3u);
    // Ranked order: book 1 (dist 0) < book 2 < book 3.
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 2);
    EXPECT_EQ(qr.rows[2][0].as_int32(), 3);
    // Distances are non-decreasing and the first is ~0.
    EXPECT_NEAR(qr.rows[0][2].as_float64(), 0.0, 1e-4);
    EXPECT_LE(qr.rows[0][2].as_float64(), qr.rows[1][2].as_float64());
    EXPECT_LE(qr.rows[1][2].as_float64(), qr.rows[2][2].as_float64());
}

// AC: vector table as the joined (right) source with aliases — exercises alias
// propagation so that qualified `b.id` resolves against the join's combined
// schema despite `reviews` also having an `id` column.
TEST_F(NearestJoinTest, VectorTableAsRightSourceWithAliases) {
    auto qr = run_ok("SELECT b.id, r.id "
                     "FROM reviews r INNER JOIN books b ON r.book_id = b.id "
                     "WHERE NEAREST(b.description_vec, 2) TO [1.0, 0.0, 0.0, 0.0]");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(sorted_int_col(qr, 0), (std::vector<int32_t>{1, 2}));
    EXPECT_EQ(sorted_int_col(qr, 1), (std::vector<int32_t>{10, 20}));
}

// AC: filtered-kNN consistency — a sibling predicate on the vector table is
// applied BEFORE top-k. `title LIKE 'A%'` removes book 3+ from the candidate
// set, so NEAREST(..., 2) over {Alpha, Apex} still yields books {1,2}, matching
// the single-table query.
TEST_F(NearestJoinTest, FilteredKnnAppliesSiblingPredicateBeforeTopK) {
    auto single = run_ok("SELECT id FROM books "
                         "WHERE NEAREST(description_vec, 2) TO [1.0, 0.0, 0.0, 0.0] "
                         "AND title LIKE 'A%'");
    EXPECT_EQ(sorted_int_col(single, 0), (std::vector<int32_t>{1, 2}));

    auto joined = run_ok("SELECT b.id "
                         "FROM books b INNER JOIN reviews r ON r.book_id = b.id "
                         "WHERE NEAREST(b.description_vec, 2) TO [1.0, 0.0, 0.0, 0.0] "
                         "AND b.title LIKE 'A%'");
    EXPECT_EQ(sorted_int_col(joined, 0), (std::vector<int32_t>{1, 2}));
}

// AC: filtered-kNN intersects the sibling predicate with the fixed top-k
// window rather than backfilling past it (GDB-1229). The 2 nearest books to
// the query vector are {1 (Alpha), 2 (Apex)}; `title <> 'Alpha'` excludes
// book 1 from that fixed window, leaving only book 2 — NOT {2, 3}, since
// book 3 (Gamma) is the 3rd nearest and outside the top-2 window.
TEST_F(NearestJoinTest, FilteredKnnChangesResultSetBeforeTopK) {
    auto joined = run_ok("SELECT b.id "
                         "FROM books b INNER JOIN reviews r ON r.book_id = b.id "
                         "WHERE NEAREST(b.description_vec, 2) TO [1.0, 0.0, 0.0, 0.0] "
                         "AND b.title <> 'Alpha'");
    EXPECT_EQ(sorted_int_col(joined, 0), (std::vector<int32_t>{2}));
}

// AC: other-table predicate filters the joined table without re-ranking the
// vector side; the book count may drop below k. NEAREST picks books {1,2}; the
// review for book 2 has 3 stars, so `r.stars >= 4` keeps only book 1.
TEST_F(NearestJoinTest, OtherTablePredicateFiltersWithoutReranking) {
    auto qr = run_ok("SELECT b.id "
                     "FROM books b INNER JOIN reviews r ON r.book_id = b.id "
                     "WHERE NEAREST(b.description_vec, 2) TO [1.0, 0.0, 0.0, 0.0] "
                     "AND r.stars >= 4");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
}

// AC: NEAREST on the preserved (left) side of a LEFT JOIN works.
TEST_F(NearestJoinTest, NearestOnPreservedLeftSideOfLeftJoin) {
    auto qr = run_ok("SELECT b.id "
                     "FROM books b LEFT JOIN reviews r ON r.book_id = b.id "
                     "WHERE NEAREST(b.description_vec, 2) TO [1.0, 0.0, 0.0, 0.0]");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(sorted_int_col(qr, 0), (std::vector<int32_t>{1, 2}));
}

// AC: NEAREST on the nullable side of a LEFT JOIN returns a clean error.
TEST_F(NearestJoinTest, NearestOnNullableSideOfLeftJoinRejected) {
    assert_error(engine_->execute("SELECT b.id "
                                  "FROM reviews r LEFT JOIN books b ON r.book_id = b.id "
                                  "WHERE NEAREST(b.description_vec, 2) TO [1.0, 0.0, 0.0, 0.0]"),
                 "nullable side");
}

// AC: two NEAREST conjuncts in one WHERE return a clean error.
TEST_F(NearestJoinTest, TwoNearestConjunctsRejected) {
    assert_error(engine_->execute("SELECT b.id "
                                  "FROM books b INNER JOIN reviews r ON r.book_id = b.id "
                                  "WHERE NEAREST(b.description_vec, 2) TO [1.0, 0.0, 0.0, 0.0] "
                                  "AND NEAREST(b.description_vec, 3) TO [0.0, 1.0, 0.0, 0.0]"),
                 "one NEAREST");
}

// AC: NEAREST inside a JOIN ON clause returns a clean error.
TEST_F(NearestJoinTest, NearestInJoinOnClauseRejected) {
    assert_error(engine_->execute("SELECT b.id "
                                  "FROM books b INNER JOIN reviews r "
                                  "ON r.book_id = b.id AND NEAREST(b.description_vec, 2) TO "
                                  "[1.0, 0.0, 0.0, 0.0]"),
                 "ON clause");
}

// AC: EXPLAIN shows a Nearest Scan node feeding the join.
TEST_F(NearestJoinTest, ExplainShowsNearestScanFeedingJoin) {
    auto qr = run_ok("EXPLAIN SELECT b.id "
                     "FROM books b INNER JOIN reviews r ON r.book_id = b.id "
                     "WHERE NEAREST(b.description_vec, 2) TO [1.0, 0.0, 0.0, 0.0]");
    std::string plan;
    for (const auto& row : qr.rows) {
        plan += row[0].as_string();
        plan += '\n';
    }
    EXPECT_NE(plan.find("Nearest Scan"), std::string::npos) << plan;
    bool has_join = plan.find("Hash Join") != std::string::npos ||
                    plan.find("Nested Loop") != std::string::npos;
    EXPECT_TRUE(has_join) << plan;
}

// AC: WITHIN TRAVERSE restricts the k-set to the traversal scope before joining.
// Edges similar_to: 1 -> 2 -> 3. Scope from book 1 (OUT, depth 2) is {1,2,3}.
// k=5 exceeds the scope, so the nearest set is exactly the 3 reachable books;
// joined with their reviews that is 3 rows for books {1,2,3}.
TEST_F(NearestJoinTest, WithinTraverseScopesBeforeJoin) {
    exec_ok("CREATE EDGE TYPE similar_to FROM books TO books");
    exec_ok("LINK books(1) TO books(2) VIA similar_to");
    exec_ok("LINK books(2) TO books(3) VIA similar_to");

    auto qr = run_ok("SELECT b.id "
                     "FROM books b INNER JOIN reviews r ON r.book_id = b.id "
                     "WHERE NEAREST(b.description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                     "WITHIN TRAVERSE similar_to FROM books(1) DIRECTION OUT MAX_DEPTH 2");
    EXPECT_EQ(sorted_int_col(qr, 0), (std::vector<int32_t>{1, 2, 3}));
    // Book 4 and 5 are out of the traversal scope.
    for (const auto& row : qr.rows) {
        EXPECT_NE(row[0].as_int32(), 4);
        EXPECT_NE(row[0].as_int32(), 5);
    }
}

// Regression guard: a JOIN query with no NEAREST is completely unaffected.
TEST_F(NearestJoinTest, PlainJoinWithoutNearestUnaffected) {
    auto qr = run_ok("SELECT b.id "
                     "FROM books b INNER JOIN reviews r ON r.book_id = b.id "
                     "WHERE r.stars >= 4");
    // Reviews with >= 4 stars: books 1, 3, 4.
    EXPECT_EQ(sorted_int_col(qr, 0), (std::vector<int32_t>{1, 3, 4}));
}

} // namespace
