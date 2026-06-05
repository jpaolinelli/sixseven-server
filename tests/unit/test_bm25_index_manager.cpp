#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
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

// Verifies the IndexManager integration for BM25: CREATE INDEX ... USING bm25
// populates an in-memory inverted index, persists it to disk, and reloads it
// across a simulated restart.
class Bm25IndexManagerTest : public ::testing::Test {
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
        auto r = index_manager_->rebuild_all_indexes();
        ASSERT_TRUE(r.has_value()) << r.error().message;
        engine_->set_index_manager(index_manager_.get());
    }

    void restart_and_reload() {
        // Flush indexes, tear down, rebuild the stack, reload the persisted
        // catalog (bootstrap), then reload indexes from disk.
        ASSERT_TRUE(index_manager_->flush_all_indexes().has_value());
        reset_stack();
        make_stack();
        run_bootstrap();
        rebuild_indexes();
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return std::move(*result);
    }

    Bm25Index* find_bm25(const std::string& index_name) {
        auto def = catalog_->get_index(index_name);
        EXPECT_TRUE(def.has_value());
        auto* map = index_manager_->bm25_map();
        auto it = map->find(def->index_id);
        return it == map->end() ? nullptr : it->second;
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
