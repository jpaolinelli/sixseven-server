/// @file test_qa_gdb_1250.cpp
/// @brief QA regression tests for GDB-1250: NEAREST(...) (including WITHIN
/// TRAVERSE graph scoping) pushdown through JOINs. The planner pushes the
/// vector scan down to the table that owns the searched EMBEDDING column so the
/// join consumes exactly the k nearest rows and the synthetic `_distance`
/// column lands in the join's combined output schema.
///
/// These tests map each acceptance criterion to a concrete assertion against a
/// fixture whose vectors make the nearest order unambiguous. Adversarial probes
/// for owner-resolution ambiguity, self-joins, and scope/limit interactions
/// live in test_qa_gdb_1250_adversarial.cpp (gdb-745 convention).

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

namespace {

// Cosine distance (NEAREST default) of query [1,0,0,0] against the books:
//   id 1 [1.0,0.0,0.0,0.0] -> 0.0   (nearest)
//   id 2 [0.9,0.1,0.0,0.0] -> ~0.006
//   id 3 [0.0,1.0,0.0,0.0] -> 1.0
//   id 4 [0.0,0.0,1.0,0.0] -> 1.0
//   id 5 [0.0,0.0,0.0,1.0] -> 1.0
// k=2 -> {1,2}; k=3 -> {1,2,3}.
class QA_GDB1250 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1250";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);
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

    static void assert_invalid(const Result<QueryResult>& result, const std::string& needle) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT) << result.error().message;
        EXPECT_NE(result.error().message.find(needle), std::string::npos)
            << "actual: " << result.error().message;
    }

    static std::vector<int32_t> sorted_int_col(const QueryResult& qr, size_t col) {
        std::vector<int32_t> ids;
        for (const auto& row : qr.rows) {
            ids.push_back(row[col].as_int32());
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    void register_embedding(table_id_t table_id, int32_t col_id, int32_t dim) {
        EmbeddingColumnDef emb;
        emb.table_id = table_id;
        emb.column_id = col_id;
        emb.dimension = dim;
        emb.source_expr = "title";
        emb.provider = "builtin/4";
        auto reg = catalog_.register_embedding_column(emb);
        ASSERT_TRUE(reg.has_value()) << reg.error().message;
    }

    void seed() {
        exec_ok(
            "CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR, description_vec EMBEDDING)");
        auto books = catalog_.get_table(default_database_id, "books");
        ASSERT_TRUE(books.has_value());
        register_embedding(books->table_id, 2, 4);

        exec_ok("INSERT INTO books VALUES (1, 'Alpha',  [1.0, 0.0, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (2, 'Apex',   [0.9, 0.1, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (3, 'Gamma',  [0.0, 1.0, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (4, 'Delta',  [0.0, 0.0, 1.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (5, 'Epsilon',[0.0, 0.0, 0.0, 1.0])");

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

constexpr const char* kVec = "[1.0, 0.0, 0.0, 0.0]";

// AC1: demo query returns reviews of exactly the k nearest books.
TEST_F(QA_GDB1250, DemoQueryReturnsReviewsOfKNearestBooks) {
    auto qr = run_ok(std::string("SELECT b.id, r.id FROM books b "
                                 "INNER JOIN reviews r ON r.book_id = b.id "
                                 "WHERE NEAREST(b.description_vec, 2) TO ") +
                     kVec);
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(sorted_int_col(qr, 0), (std::vector<int32_t>{1, 2}));
    EXPECT_EQ(sorted_int_col(qr, 1), (std::vector<int32_t>{10, 20}));
}

// AC1: _distance resolves and ORDER BY _distance ASC ranks the joined output.
TEST_F(QA_GDB1250, DistanceResolvesAndOrdersThroughJoin) {
    auto qr = run_ok(std::string("SELECT b.id, r.stars, _distance FROM books b "
                                 "INNER JOIN reviews r ON r.book_id = b.id "
                                 "WHERE NEAREST(b.description_vec, 3) TO ") +
                     kVec + " ORDER BY _distance ASC");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[1][0].as_int32(), 2);
    EXPECT_EQ(qr.rows[2][0].as_int32(), 3);
    EXPECT_NEAR(qr.rows[0][2].as_float64(), 0.0, 1e-4);
}

// AC: vector table as the joined (right) source with aliases.
TEST_F(QA_GDB1250, VectorTableAsRightSourceWithAliases) {
    auto qr = run_ok(std::string("SELECT b.id, r.id FROM reviews r "
                                 "INNER JOIN books b ON r.book_id = b.id "
                                 "WHERE NEAREST(b.description_vec, 2) TO ") +
                     kVec);
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(sorted_int_col(qr, 0), (std::vector<int32_t>{1, 2}));
    EXPECT_EQ(sorted_int_col(qr, 1), (std::vector<int32_t>{10, 20}));
}

// AC: filtered-kNN consistency — sibling predicate on the vector table is
// applied BEFORE top-k and matches the single-table result.
TEST_F(QA_GDB1250, FilteredKnnMatchesSingleTable) {
    auto single = run_ok(std::string("SELECT id FROM books "
                                     "WHERE NEAREST(description_vec, 2) TO ") +
                         kVec + " AND title <> 'Alpha'");
    EXPECT_EQ(sorted_int_col(single, 0), (std::vector<int32_t>{2, 3}));

    auto joined = run_ok(std::string("SELECT b.id FROM books b "
                                     "INNER JOIN reviews r ON r.book_id = b.id "
                                     "WHERE NEAREST(b.description_vec, 2) TO ") +
                         kVec + " AND b.title <> 'Alpha'");
    EXPECT_EQ(sorted_int_col(joined, 0), (std::vector<int32_t>{2, 3}));
}

// AC: other-table predicate filters reviews without re-ranking; count may drop.
TEST_F(QA_GDB1250, OtherTablePredicateFiltersWithoutReranking) {
    auto qr = run_ok(std::string("SELECT b.id FROM books b "
                                 "INNER JOIN reviews r ON r.book_id = b.id "
                                 "WHERE NEAREST(b.description_vec, 2) TO ") +
                     kVec + " AND r.stars >= 4");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
}

// AC: NEAREST on the preserved (left) side of a LEFT JOIN works.
TEST_F(QA_GDB1250, NearestOnPreservedLeftSideWorks) {
    auto qr = run_ok(std::string("SELECT b.id FROM books b "
                                 "LEFT JOIN reviews r ON r.book_id = b.id "
                                 "WHERE NEAREST(b.description_vec, 2) TO ") +
                     kVec);
    EXPECT_EQ(sorted_int_col(qr, 0), (std::vector<int32_t>{1, 2}));
}

// AC: NEAREST on the nullable side of a LEFT JOIN -> clean error.
TEST_F(QA_GDB1250, NearestOnNullableSideRejected) {
    assert_invalid(engine_->execute(std::string("SELECT b.id FROM reviews r "
                                                 "LEFT JOIN books b ON r.book_id = b.id "
                                                 "WHERE NEAREST(b.description_vec, 2) TO ") +
                                    kVec),
                   "nullable side");
}

// AC: two NEAREST conjuncts -> clean error.
TEST_F(QA_GDB1250, TwoNearestConjunctsRejected) {
    assert_invalid(engine_->execute(std::string("SELECT b.id FROM books b "
                                                "INNER JOIN reviews r ON r.book_id = b.id "
                                                "WHERE NEAREST(b.description_vec, 2) TO ") +
                                    kVec + " AND NEAREST(b.description_vec, 3) TO " + kVec),
                   "one NEAREST");
}

// AC: NEAREST + MATCH rejection still holds.
TEST_F(QA_GDB1250, NearestPlusMatchRejected) {
    auto result = engine_->execute(std::string("SELECT b.id FROM books b "
                                               "INNER JOIN reviews r ON r.book_id = b.id "
                                               "WHERE NEAREST(b.description_vec, 2) TO ") +
                                   kVec + " AND MATCH(b.title) TO 'alpha'");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT) << result.error().message;
}

// AC: EXPLAIN shows a Nearest Scan node feeding the join.
TEST_F(QA_GDB1250, ExplainShowsNearestScanFeedingJoin) {
    auto qr = run_ok(std::string("EXPLAIN SELECT b.id FROM books b "
                                 "INNER JOIN reviews r ON r.book_id = b.id "
                                 "WHERE NEAREST(b.description_vec, 2) TO ") +
                     kVec);
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

// AC: WITHIN TRAVERSE restricts the k-set to the traversal scope before join.
TEST_F(QA_GDB1250, WithinTraverseScopesBeforeJoin) {
    exec_ok("CREATE EDGE TYPE similar_to FROM books TO books");
    exec_ok("LINK books(1) TO books(2) VIA similar_to");
    exec_ok("LINK books(2) TO books(3) VIA similar_to");

    auto qr = run_ok(std::string("SELECT b.id FROM books b "
                                 "INNER JOIN reviews r ON r.book_id = b.id "
                                 "WHERE NEAREST(b.description_vec, 5) TO ") +
                     kVec + " WITHIN TRAVERSE similar_to FROM books(1) DIRECTION OUT MAX_DEPTH 2");
    EXPECT_EQ(sorted_int_col(qr, 0), (std::vector<int32_t>{1, 2, 3}));
}

// AC: GDB-1249 derived-table workaround still plans and executes.
TEST_F(QA_GDB1250, DerivedTableWorkaroundStillWorks) {
    auto qr = run_ok(std::string("SELECT nb.id FROM "
                                 "(SELECT id, _distance FROM books WHERE NEAREST(description_vec, 2) TO ") +
                     kVec + ") nb JOIN reviews r ON nb.id = r.book_id");
    EXPECT_EQ(sorted_int_col(qr, 0), (std::vector<int32_t>{1, 2}));
}

} // namespace
