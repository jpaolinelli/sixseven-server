#include "sixseven/executor/index_manager.h"

#include <gtest/gtest.h>

#include <filesystem>

#include "query_engine_fixture.h"

using namespace sixseven;

// Verifies the IndexManager integration for BM25: CREATE INDEX ... USING bm25
// populates an in-memory inverted index, persists it to disk, and reloads it
// across a simulated restart.
class Bm25IndexManagerTest : public QueryEngineFixture {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_bm25_index_manager";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        make_stack();
    }

    void TearDown() override {
        reset_stack();
        std::filesystem::remove_all(data_dir_);
    }

    Bm25Index* find_bm25(const std::string& index_name) {
        auto def = catalog_->get_index(default_database_id, index_name);
        EXPECT_TRUE(def.has_value());
        if (!def.has_value()) {
            return nullptr;
        }
        auto* map = index_manager_->bm25_map();
        auto it = map->find(def->index_id);
        return it == map->end() ? nullptr : it->second;
    }
};

TEST_F(Bm25IndexManagerTest, CreateBm25IndexPopulatesFromTableData) {
    run_bootstrap();
    rebuild_indexes();

    exec_ok("CREATE TABLE articles (id INT, body VARCHAR)");
    exec_ok("INSERT INTO articles VALUES (1, 'the quick brown fox')");
    exec_ok("INSERT INTO articles VALUES (2, 'machine learning models')");
    exec_ok("INSERT INTO articles VALUES (3, 'quick learning techniques')");

    exec_ok("CREATE INDEX idx_body ON articles(body) USING bm25");

    auto* idx = find_bm25("idx_body");
    ASSERT_NE(idx, nullptr);
    EXPECT_EQ(idx->doc_count(), 3u);

    auto hits = idx->search("quick", 10);
    ASSERT_EQ(hits.size(), 2u); // rows 1 and 3 contain "quick".

    // "learning" stems to match rows 2 and 3.
    EXPECT_EQ(idx->search("learning", 10).size(), 2u);
}

TEST_F(Bm25IndexManagerTest, Bm25IndexSurvivesRestart) {
    run_bootstrap();
    rebuild_indexes();

    exec_ok("CREATE TABLE docs (id INT, body VARCHAR)");
    exec_ok("INSERT INTO docs VALUES (1, 'persistent inverted index')");
    exec_ok("INSERT INTO docs VALUES (2, 'transient memory only')");
    exec_ok("CREATE INDEX idx_docs ON docs(body) USING bm25");

    auto before = find_bm25("idx_docs")->search("index", 10);
    ASSERT_EQ(before.size(), 1u);

    restart_and_reload();

    auto* idx = find_bm25("idx_docs");
    ASSERT_NE(idx, nullptr) << "BM25 index should reload from disk after restart";
    EXPECT_EQ(idx->doc_count(), 2u);
    auto after = idx->search("index", 10);
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(before.front().rid, after.front().rid);
}

TEST_F(Bm25IndexManagerTest, RejectsNonStringColumn) {
    run_bootstrap();
    rebuild_indexes();

    exec_ok("CREATE TABLE nums (id INT, val INT)");
    auto r = engine_->execute("CREATE INDEX idx_bad ON nums(val) USING bm25");
    EXPECT_FALSE(r.has_value());
}

TEST_F(Bm25IndexManagerTest, DropBm25IndexRemovesIt) {
    run_bootstrap();
    rebuild_indexes();

    exec_ok("CREATE TABLE t (id INT, body VARCHAR)");
    exec_ok("INSERT INTO t VALUES (1, 'hello world')");
    exec_ok("CREATE INDEX idx_t ON t(body) USING bm25");
    ASSERT_NE(find_bm25("idx_t"), nullptr);

    exec_ok("DROP INDEX idx_t");
    auto* map = index_manager_->bm25_map();
    EXPECT_TRUE(map->empty());
}
