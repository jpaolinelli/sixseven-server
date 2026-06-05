#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "test_catalog_helpers.h"

using namespace sixseven;

// End-to-end SQL coverage for the MATCH(col) TO 'q' full-text predicate.
class Bm25QueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_bm25_query";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        make_stack();
        run_bootstrap();
        rebuild_indexes();
    }

    void TearDown() override {
        reset_stack();
        std::filesystem::remove_all(data_dir_);
    }

    void make_stack() {
        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        config_ = Config::load_defaults();
    }

    void reset_stack() {
        index_manager_.reset();
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
    }

    void run_bootstrap() {
        auto result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }

    void rebuild_indexes() {
        index_manager_ = std::make_unique<IndexManager>(*catalog_, *storage_);
        index_manager_->set_catalog_persistence(persistence_.get());
        ASSERT_TRUE(index_manager_->rebuild_all_indexes().has_value());
        engine_->set_index_manager(index_manager_.get());
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void seed_articles() {
        exec_ok("CREATE TABLE articles (id INT, body VARCHAR)");
        exec_ok("INSERT INTO articles VALUES (1, 'the quick brown fox jumps')");
        exec_ok("INSERT INTO articles VALUES (2, 'quick quick brown bear runs')");
        exec_ok("INSERT INTO articles VALUES (3, 'a slow green turtle')");
        exec_ok("INSERT INTO articles VALUES (4, 'machine learning models learn')");
        exec_ok("CREATE INDEX idx_body ON articles(body) USING bm25");
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<IndexManager> index_manager_;
    Config config_;
};

TEST_F(Bm25QueryTest, MatchReturnsOnlyMatchingRows) {
    seed_articles();
    auto r = exec_ok("SELECT id FROM articles WHERE MATCH(body) TO 'quick'");
    // Rows 1 and 2 contain "quick"; rows 3 and 4 do not.
    ASSERT_EQ(r.rows.size(), 2u);
    std::vector<int32_t> ids;
    for (auto& row : r.rows) {
        ids.push_back(row[0].as_int32());
    }
    EXPECT_NE(std::find(ids.begin(), ids.end(), 1), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), 2), ids.end());
}

TEST_F(Bm25QueryTest, SelectScoreAndOrderByRelevance) {
    seed_articles();
    auto r = exec_ok(
        "SELECT id, _score FROM articles WHERE MATCH(body) TO 'quick' ORDER BY _score DESC");
    ASSERT_EQ(r.rows.size(), 2u);
    // Row 2 has two "quick" occurrences -> higher score -> ranked first.
    EXPECT_EQ(r.rows[0][0].as_int32(), 2);
    EXPECT_EQ(r.rows[1][0].as_int32(), 1);
    // _score column is present and positive.
    ASSERT_EQ(r.column_names.size(), 2u);
    EXPECT_EQ(r.column_names[1], "_score");
    EXPECT_GT(r.rows[0][1].as_float64(), 0.0);
    EXPECT_GE(r.rows[0][1].as_float64(), r.rows[1][1].as_float64());
}

TEST_F(Bm25QueryTest, MatchWithResidualAndPredicate) {
    seed_articles();
    // MATCH(...) AND id > 1 should drop row 1 even though it matches "quick".
    auto r = exec_ok("SELECT id FROM articles WHERE MATCH(body) TO 'quick' AND id > 1");
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0].as_int32(), 2);
}

TEST_F(Bm25QueryTest, MatchWithLimit) {
    seed_articles();
    auto r = exec_ok("SELECT id FROM articles WHERE MATCH(body) TO 'quick brown' ORDER BY "
                     "_score DESC LIMIT 1");
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0].as_int32(), 2);
}

TEST_F(Bm25QueryTest, StemmedQueryMatches) {
    seed_articles();
    // "learning" stems to match row 4 ("learning"/"learn").
    auto r = exec_ok("SELECT id FROM articles WHERE MATCH(body) TO 'learn'");
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0].as_int32(), 4);
}

TEST_F(Bm25QueryTest, ErrorWhenNoBm25Index) {
    exec_ok("CREATE TABLE notes (id INT, body VARCHAR)");
    exec_ok("INSERT INTO notes VALUES (1, 'hello world')");
    // No BM25 index created on notes.body.
    auto r = engine_->execute("SELECT id FROM notes WHERE MATCH(body) TO 'hello'");
    EXPECT_FALSE(r.has_value());
}

TEST_F(Bm25QueryTest, NoMatchesReturnsEmpty) {
    seed_articles();
    auto r = exec_ok("SELECT id FROM articles WHERE MATCH(body) TO 'nonexistentword'");
    EXPECT_EQ(r.rows.size(), 0u);
}

// --- DML maintenance -------------------------------------------------------

TEST_F(Bm25QueryTest, InsertAfterIndexIsSearchable) {
    seed_articles();
    // A row inserted AFTER the index was created must be searchable immediately.
    exec_ok("INSERT INTO articles VALUES (5, 'a brand new quick article')");
    auto r = exec_ok("SELECT id FROM articles WHERE MATCH(body) TO 'quick'");
    // Rows 1, 2, and now 5 contain "quick".
    ASSERT_EQ(r.rows.size(), 3u);
    std::vector<int32_t> ids;
    for (auto& row : r.rows) {
        ids.push_back(row[0].as_int32());
    }
    EXPECT_NE(std::find(ids.begin(), ids.end(), 5), ids.end());
}

TEST_F(Bm25QueryTest, DeleteRemovesFromResults) {
    seed_articles();
    exec_ok("DELETE FROM articles WHERE id = 2");
    auto r = exec_ok("SELECT id FROM articles WHERE MATCH(body) TO 'quick'");
    // Only row 1 still contains "quick" after deleting row 2.
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0].as_int32(), 1);
}

TEST_F(Bm25QueryTest, UpdateReindexesText) {
    seed_articles();
    // Row 3 ("a slow green turtle") gains the term "quick".
    exec_ok("UPDATE articles SET body = 'now a quick turtle' WHERE id = 3");
    auto r = exec_ok("SELECT id FROM articles WHERE MATCH(body) TO 'quick'");
    // Rows 1, 2, and now 3 match.
    ASSERT_EQ(r.rows.size(), 3u);
    std::vector<int32_t> ids;
    for (auto& row : r.rows) {
        ids.push_back(row[0].as_int32());
    }
    EXPECT_NE(std::find(ids.begin(), ids.end(), 3), ids.end());

    // The old term "slow" should no longer match row 3.
    auto r2 = exec_ok("SELECT id FROM articles WHERE MATCH(body) TO 'slow'");
    EXPECT_EQ(r2.rows.size(), 0u);
}

// Smoke-test the exact query shapes used in docs/demo-queries.sql against a
// demo-style schema (TEXT columns, template descriptions), so the published
// demo material is known-good.
TEST_F(Bm25QueryTest, DemoQueriesExecute) {
    exec_ok("CREATE TABLE books (id INT PRIMARY KEY, title TEXT, genre TEXT, "
            "published_year INT, pages INT, rating DOUBLE, description TEXT)");
    exec_ok("INSERT INTO books VALUES (1, 'A Book', 'Mystery', 2001, 300, 4.1, "
            "'A political intrigue story following an unlikely detective confronting the "
            "nature of power.')");
    exec_ok("INSERT INTO books VALUES (2, 'B Book', 'Thriller', 2010, 280, 3.8, "
            "'A survival against all odds story set on a remote island.')");
    exec_ok("INSERT INTO books VALUES (3, 'C Book', 'Fantasy', 2015, 500, 4.6, "
            "'A betrayal and redemption story in a mythical kingdom.')");

    exec_ok("CREATE INDEX idx_books_desc ON books(description) USING bm25");

    exec_ok("SELECT id, title, genre FROM books WHERE MATCH(description) TO 'survival island'");
    exec_ok("SELECT title, genre, _score FROM books "
            "WHERE MATCH(description) TO 'political intrigue power' "
            "ORDER BY _score DESC LIMIT 10");
    exec_ok("SELECT title, rating, _score FROM books "
            "WHERE MATCH(description) TO 'detective' AND genre = 'Mystery' "
            "ORDER BY _score DESC LIMIT 10");

    // Stemming: the plural 'kingdoms' must match the description containing "kingdom".
    auto stem = exec_ok("SELECT title, _score FROM books "
                        "WHERE MATCH(description) TO 'kingdoms' ORDER BY _score DESC LIMIT 5");
    ASSERT_EQ(stem.rows.size(), 1u);

    // Insert maintenance: a new row is immediately searchable.
    exec_ok("INSERT INTO books (id, title, genre, published_year, pages, rating, description) "
            "VALUES (99001, 'Test Title', 'Thriller', 2024, 320, 4.2, "
            "'A gripping submarine thriller about survival beneath the ice.')");
    auto fresh = exec_ok("SELECT id, title, _score FROM books "
                         "WHERE MATCH(description) TO 'submarine survival' "
                         "ORDER BY _score DESC LIMIT 5");
    bool found_new = false;
    for (auto& row : fresh.rows) {
        if (row[0].as_int32() == 99001) {
            found_new = true;
        }
    }
    EXPECT_TRUE(found_new);

    // EXPLAIN over a BM25 query should plan without error.
    exec_ok("EXPLAIN SELECT title, _score FROM books "
            "WHERE MATCH(description) TO 'political intrigue' ORDER BY _score DESC LIMIT 5");
}

TEST_F(Bm25QueryTest, MaintenancePersistsAcrossRestart) {
    seed_articles();
    exec_ok("INSERT INTO articles VALUES (5, 'another quick note')");
    exec_ok("DELETE FROM articles WHERE id = 1");

    // Flush, tear down, and reload from disk.
    ASSERT_TRUE(index_manager_->flush_all_indexes().has_value());
    reset_stack();
    make_stack();
    run_bootstrap();
    rebuild_indexes();

    auto r = exec_ok("SELECT id FROM articles WHERE MATCH(body) TO 'quick'");
    // Rows 2 and 5 contain "quick" (row 1 deleted).
    ASSERT_EQ(r.rows.size(), 2u);
    std::vector<int32_t> ids;
    for (auto& row : r.rows) {
        ids.push_back(row[0].as_int32());
    }
    EXPECT_NE(std::find(ids.begin(), ids.end(), 2), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), 5), ids.end());
    EXPECT_EQ(std::find(ids.begin(), ids.end(), 1), ids.end());
}
