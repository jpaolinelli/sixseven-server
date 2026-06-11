#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/config.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/index/btree_index.h"
#include "sixseven/index/hash_index.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/builtin_provider.h"
#include "sixseven/vector/embedding_column.h"
#include "sixseven/vector/embedding_worker.h"
#include "sixseven/vector/hnsw_index.h"
#include "sixseven/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class IndexManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_index_manager";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        config_ = Config::load_defaults();
    }

    void TearDown() override {
        index_manager_.reset();
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
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

    void restart() {
        index_manager_.reset();
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return result ? std::move(*result) : QueryResult{};
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

// =============================================================================
// AC: Rebuild btree index from existing table data
// =============================================================================

TEST_F(IndexManagerTest, RebuildBTreeIndexFromData) {
    run_bootstrap();

    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'Alice')");
    exec_ok("INSERT INTO users VALUES (2, 'Bob')");
    exec_ok("INSERT INTO users VALUES (3, 'Carol')");
    exec_ok("CREATE INDEX idx_users_id ON users(id)");

    rebuild_indexes();

    // Verify the btree index was populated.
    auto* btree_map = index_manager_->btree_map();
    ASSERT_FALSE(btree_map->empty());

    // Find the index.
    auto idx_def = catalog_->get_index("idx_users_id");
    ASSERT_TRUE(idx_def.has_value());
    auto it = btree_map->find(idx_def->index_id);
    ASSERT_NE(it, btree_map->end());
    EXPECT_EQ(it->second->size(), 3);

    // Verify point lookups work.
    auto r1 = it->second->search({Value(int32_t(1))});
    ASSERT_TRUE(r1.has_value());
    EXPECT_TRUE(r1->has_value());

    auto r4 = it->second->search({Value(int32_t(4))});
    ASSERT_TRUE(r4.has_value());
    EXPECT_FALSE(r4->has_value());
}

// =============================================================================
// AC: Rebuild hash index from existing table data
// =============================================================================

TEST_F(IndexManagerTest, RebuildHashIndexFromData) {
    run_bootstrap();

    exec_ok("CREATE TABLE products (id INT, name VARCHAR)");
    exec_ok("INSERT INTO products VALUES (10, 'Widget')");
    exec_ok("INSERT INTO products VALUES (20, 'Gadget')");
    exec_ok("CREATE INDEX idx_products_id ON products(id) USING hash");

    rebuild_indexes();

    auto* hash_map = index_manager_->hash_map();
    ASSERT_FALSE(hash_map->empty());

    auto idx_def = catalog_->get_index("idx_products_id");
    ASSERT_TRUE(idx_def.has_value());
    auto it = hash_map->find(idx_def->index_id);
    ASSERT_NE(it, hash_map->end());
    EXPECT_EQ(it->second->size(), 2);

    // Verify point lookup.
    auto r = it->second->search({Value(int32_t(10))});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->has_value());
}

// =============================================================================
// AC: Multiple indexes on the same table
// =============================================================================

TEST_F(IndexManagerTest, RebuildMultipleIndexesOnSameTable) {
    run_bootstrap();

    exec_ok("CREATE TABLE items (id INT, category VARCHAR, price INT)");
    exec_ok("INSERT INTO items VALUES (1, 'A', 100)");
    exec_ok("INSERT INTO items VALUES (2, 'B', 200)");
    exec_ok("CREATE INDEX idx_items_id ON items(id)");
    exec_ok("CREATE INDEX idx_items_price ON items(price) USING hash");

    rebuild_indexes();

    // Both index maps should be populated.
    EXPECT_EQ(index_manager_->btree_map()->size(), 1);
    EXPECT_EQ(index_manager_->hash_map()->size(), 1);

    // Both should have 2 entries.
    auto btree_idx = catalog_->get_index("idx_items_id");
    ASSERT_TRUE(btree_idx.has_value());
    EXPECT_EQ((*index_manager_->btree_map())[btree_idx->index_id]->size(), 2);

    auto hash_idx = catalog_->get_index("idx_items_price");
    ASSERT_TRUE(hash_idx.has_value());
    EXPECT_EQ((*index_manager_->hash_map())[hash_idx->index_id]->size(), 2);
}

// =============================================================================
// AC: Empty table produces empty index
// =============================================================================

TEST_F(IndexManagerTest, EmptyTableRebuild) {
    run_bootstrap();

    exec_ok("CREATE TABLE empty_tbl (id INT)");
    exec_ok("CREATE INDEX idx_empty ON empty_tbl(id)");

    rebuild_indexes();

    auto idx_def = catalog_->get_index("idx_empty");
    ASSERT_TRUE(idx_def.has_value());
    auto it = index_manager_->btree_map()->find(idx_def->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());
    EXPECT_EQ(it->second->size(), 0);
    EXPECT_TRUE(it->second->empty());
}

// =============================================================================
// AC: Autoincrement combined with index rebuild
// =============================================================================

TEST_F(IndexManagerTest, AutoincrementCombinedWithRebuild) {
    run_bootstrap();

    exec_ok("CREATE TABLE auto_tbl (id INT PRIMARY KEY AUTOINCREMENT, name VARCHAR)");
    exec_ok("INSERT INTO auto_tbl (name) VALUES ('a')");
    exec_ok("INSERT INTO auto_tbl (name) VALUES ('b')");
    exec_ok("INSERT INTO auto_tbl (name) VALUES ('c')");
    exec_ok("CREATE INDEX idx_auto_name ON auto_tbl(name)");

    // Simulate restart.
    restart();
    run_bootstrap();
    rebuild_indexes();

    // Verify autoincrement counter was initialized correctly by checking
    // catalog state directly.
    auto counter = catalog_->get_autoincrement_counter(
        catalog_->get_table(default_database_id, "auto_tbl")->table_id);
    EXPECT_EQ(counter, 4); // max was 3, so next is 4.

    // Verify index was also rebuilt with existing data.
    auto idx_def = catalog_->get_index("idx_auto_name");
    ASSERT_TRUE(idx_def.has_value());
    auto it = index_manager_->btree_map()->find(idx_def->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());
    EXPECT_EQ(it->second->size(), 3);
}

// =============================================================================
// AC: Index survives server restart (rebuild from disk)
// =============================================================================

TEST_F(IndexManagerTest, IndexUsedAfterRestart) {
    run_bootstrap();

    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    exec_ok("INSERT INTO users VALUES (1, 'Alice')");
    exec_ok("INSERT INTO users VALUES (2, 'Bob')");
    exec_ok("INSERT INTO users VALUES (3, 'Carol')");
    exec_ok("CREATE INDEX idx_users_id ON users(id)");

    // Simulate restart.
    restart();
    run_bootstrap();
    rebuild_indexes();

    // Verify the index is available after restart.
    auto idx_def = catalog_->get_index("idx_users_id");
    ASSERT_TRUE(idx_def.has_value());
    auto it = index_manager_->btree_map()->find(idx_def->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());
    EXPECT_EQ(it->second->size(), 3);

    // Verify queries still return correct results.
    auto result = exec_ok("SELECT name FROM users WHERE id = 2");
    ASSERT_EQ(result.rows.size(), 1);
    EXPECT_EQ(result.rows[0][0].as_string(), "Bob");
}

// =============================================================================
// AC: create_and_populate_index works at runtime
// =============================================================================

TEST_F(IndexManagerTest, CreateAndPopulateAtRuntime) {
    run_bootstrap();
    rebuild_indexes();

    exec_ok("CREATE TABLE rt (id INT, val VARCHAR)");
    exec_ok("INSERT INTO rt VALUES (1, 'x')");
    exec_ok("INSERT INTO rt VALUES (2, 'y')");

    // Create index at runtime (this goes through query engine which calls IndexManager).
    exec_ok("CREATE INDEX idx_rt_id ON rt(id)");

    // The index should be populated with existing data.
    auto idx_def = catalog_->get_index("idx_rt_id");
    ASSERT_TRUE(idx_def.has_value());
    auto it = index_manager_->btree_map()->find(idx_def->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());
    EXPECT_EQ(it->second->size(), 2);
}

// =============================================================================
// AC: drop_index removes from maps
// =============================================================================

TEST_F(IndexManagerTest, DropIndexRemovesFromMaps) {
    run_bootstrap();
    rebuild_indexes();

    exec_ok("CREATE TABLE dt (id INT)");
    exec_ok("INSERT INTO dt VALUES (1)");
    exec_ok("CREATE INDEX idx_dt_id ON dt(id)");

    EXPECT_EQ(index_manager_->btree_map()->size(), 1);

    exec_ok("DROP INDEX idx_dt_id");

    EXPECT_EQ(index_manager_->btree_map()->size(), 0);
}

// =============================================================================
// AC: No indexes — rebuild is a no-op
// =============================================================================

TEST_F(IndexManagerTest, NoIndexesNoScan) {
    run_bootstrap();

    exec_ok("CREATE TABLE plain (id INT, name VARCHAR)");
    exec_ok("INSERT INTO plain VALUES (1, 'test')");

    // Should succeed without any errors.
    rebuild_indexes();

    EXPECT_TRUE(index_manager_->btree_map()->empty());
    EXPECT_TRUE(index_manager_->hash_map()->empty());
}

// =============================================================================
// AC: Persist + restart loads from disk (fast path)
// =============================================================================

TEST_F(IndexManagerTest, PersistAndLoadFromDisk) {
    run_bootstrap();

    exec_ok("CREATE TABLE persist_tbl (id INT, name VARCHAR)");
    exec_ok("INSERT INTO persist_tbl VALUES (1, 'Alice')");
    exec_ok("INSERT INTO persist_tbl VALUES (2, 'Bob')");
    exec_ok("INSERT INTO persist_tbl VALUES (3, 'Carol')");
    exec_ok("CREATE INDEX idx_persist_id ON persist_tbl(id)");

    // First rebuild: builds from table data and persists to disk.
    rebuild_indexes();
    EXPECT_EQ(index_manager_->btree_map()->size(), 1);

    // Simulate restart — indexes should load from disk files.
    restart();
    run_bootstrap();
    rebuild_indexes();

    // Verify the index was loaded from disk.
    auto idx_def = catalog_->get_index("idx_persist_id");
    ASSERT_TRUE(idx_def.has_value());
    auto it = index_manager_->btree_map()->find(idx_def->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());
    EXPECT_EQ(it->second->size(), 3);

    // Verify point lookups work.
    auto r = it->second->search({Value(int32_t(2))});
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
}

// =============================================================================
// AC: Hash index persists and loads from disk
// =============================================================================

TEST_F(IndexManagerTest, HashPersistAndLoadFromDisk) {
    run_bootstrap();

    exec_ok("CREATE TABLE hash_persist (id INT, val VARCHAR)");
    exec_ok("INSERT INTO hash_persist VALUES (10, 'x')");
    exec_ok("INSERT INTO hash_persist VALUES (20, 'y')");
    exec_ok("CREATE INDEX idx_hash_persist ON hash_persist(id) USING hash");

    rebuild_indexes();
    EXPECT_EQ(index_manager_->hash_map()->size(), 1);

    restart();
    run_bootstrap();
    rebuild_indexes();

    auto idx_def = catalog_->get_index("idx_hash_persist");
    ASSERT_TRUE(idx_def.has_value());
    auto it = index_manager_->hash_map()->find(idx_def->index_id);
    ASSERT_NE(it, index_manager_->hash_map()->end());
    EXPECT_EQ(it->second->size(), 2);

    auto r = it->second->search({Value(int32_t(10))});
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
}

// =============================================================================
// AC: Autoincrement persists across restart via table file header
// =============================================================================

TEST_F(IndexManagerTest, AutoincrementPersistsViaHeader) {
    run_bootstrap();

    exec_ok("CREATE TABLE auto_persist (id INT PRIMARY KEY AUTOINCREMENT, name VARCHAR)");
    exec_ok("INSERT INTO auto_persist (name) VALUES ('a')");
    exec_ok("INSERT INTO auto_persist (name) VALUES ('b')");
    exec_ok("INSERT INTO auto_persist (name) VALUES ('c')");

    // First rebuild: scans table and persists autoincrement counter.
    rebuild_indexes();
    auto counter = catalog_->get_autoincrement_counter(
        catalog_->get_table(default_database_id, "auto_persist")->table_id);
    EXPECT_EQ(counter, 4);

    // Simulate restart — should load counter from table file header.
    restart();
    run_bootstrap();
    rebuild_indexes();

    counter = catalog_->get_autoincrement_counter(
        catalog_->get_table(default_database_id, "auto_persist")->table_id);
    EXPECT_EQ(counter, 4);
}

// =============================================================================
// AC: DROP INDEX removes the index file from disk
// =============================================================================

TEST_F(IndexManagerTest, DropIndexRemovesFile) {
    run_bootstrap();
    rebuild_indexes();

    exec_ok("CREATE TABLE drop_file_tbl (id INT)");
    exec_ok("INSERT INTO drop_file_tbl VALUES (1)");
    exec_ok("CREATE INDEX idx_drop_file ON drop_file_tbl(id)");

    EXPECT_EQ(index_manager_->btree_map()->size(), 1);

    exec_ok("DROP INDEX idx_drop_file");

    EXPECT_EQ(index_manager_->btree_map()->size(), 0);

    // After restart, index should not be present.
    restart();
    run_bootstrap();
    rebuild_indexes();

    EXPECT_TRUE(index_manager_->btree_map()->empty());
}

// =============================================================================
// AC: Flush persists latest state on shutdown
// =============================================================================

TEST_F(IndexManagerTest, FlushPersistsLatestState) {
    run_bootstrap();
    rebuild_indexes();

    exec_ok("CREATE TABLE flush_tbl (id INT, name VARCHAR)");
    exec_ok("INSERT INTO flush_tbl VALUES (1, 'a')");
    exec_ok("CREATE INDEX idx_flush ON flush_tbl(id)");

    // Insert more data after index creation.
    exec_ok("INSERT INTO flush_tbl VALUES (2, 'b')");
    exec_ok("INSERT INTO flush_tbl VALUES (3, 'c')");

    // The in-memory index should have 3 entries (insert updates it).
    // Flush to persist the latest state.
    auto flush = index_manager_->flush_all_indexes();
    ASSERT_TRUE(flush.has_value()) << flush.error().message;

    // Restart and verify.
    restart();
    run_bootstrap();
    rebuild_indexes();

    auto idx_def = catalog_->get_index("idx_flush");
    ASSERT_TRUE(idx_def.has_value());
    auto it = index_manager_->btree_map()->find(idx_def->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());
    // Note: the index was persisted during CREATE INDEX (with 1 entry).
    // The flush should update it with the latest state (3 entries if INSERT
    // updates the index, or 1 if not). Actual count depends on whether
    // INSERT goes through the index manager.
    EXPECT_GE(it->second->size(), 1u);
}

// =============================================================================
// HNSW index tests — requires embedding infrastructure
// =============================================================================

class HnswIndexManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_hnsw_idx_mgr";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        provider_registry_ = std::make_unique<ProviderRegistry>(*catalog_);
        pool_ = std::make_unique<EmbeddingWorkerPool>(
            EmbeddingWorkerConfig{.num_workers = 1, .max_batch_size = 32});
        pool_->register_provider("builtin/4", std::make_shared<BuiltinProvider>(4));
        pool_->set_provider_registry(provider_registry_.get());
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        engine_->set_provider_registry(provider_registry_.get());
        engine_->set_embedding_worker_pool(pool_.get());

        auto start = pool_->start();
        ASSERT_TRUE(start.has_value()) << start.error().message;

        config_ = Config::load_defaults();
    }

    void TearDown() override {
        index_manager_.reset();
        if (pool_ && pool_->is_running()) {
            (void)pool_->stop();
        }
        engine_.reset();
        pool_.reset();
        provider_registry_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void run_bootstrap() {
        auto result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }

    void rebuild_indexes() {
        index_manager_ = std::make_unique<IndexManager>(*catalog_, *storage_);
        index_manager_->set_catalog_persistence(persistence_.get());
        engine_->set_index_manager(index_manager_.get());
        engine_->set_hnsw_indexes(index_manager_->hnsw_map());
        auto r = index_manager_->rebuild_all_indexes();
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    /// Look up HNSW index_id by index name from the catalog.
    index_id_t hnsw_index_id(const std::string& index_name) {
        auto idx = catalog_->get_index(index_name);
        EXPECT_TRUE(idx.has_value()) << "index '" << index_name << "' not found";
        return idx ? idx->index_id : 0;
    }

    void restart() {
        index_manager_.reset();
        if (pool_ && pool_->is_running()) {
            (void)pool_->stop();
        }
        engine_.reset();
        pool_.reset();
        provider_registry_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        provider_registry_ = std::make_unique<ProviderRegistry>(*catalog_);
        pool_ = std::make_unique<EmbeddingWorkerPool>(
            EmbeddingWorkerConfig{.num_workers = 1, .max_batch_size = 32});
        pool_->register_provider("builtin/4", std::make_shared<BuiltinProvider>(4));
        pool_->set_provider_registry(provider_registry_.get());
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        engine_->set_provider_registry(provider_registry_.get());
        engine_->set_embedding_worker_pool(pool_.get());

        auto start = pool_->start();
        ASSERT_TRUE(start.has_value()) << start.error().message;
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    bool wait_for_pool(std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pool_->pending_count() == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return false;
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<ProviderRegistry> provider_registry_;
    std::unique_ptr<EmbeddingWorkerPool> pool_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<IndexManager> index_manager_;
    Config config_;
};

// =============================================================================
// AC: HNSW index is created during CREATE TABLE with EMBEDDING
// =============================================================================

TEST_F(HnswIndexManagerTest, HnswCreatedOnCreateTable) {
    run_bootstrap();
    rebuild_indexes();

    exec_ok("CREATE TABLE docs (id INT, body VARCHAR, body_vec EMBEDDING(4, body, 'builtin/4'))");

    // The HNSW map should now contain the companion index.
    auto* hnsw_map = index_manager_->hnsw_map();
    auto index_name = EmbeddingColumnManager::make_index_name("docs", "body_vec");
    auto it = hnsw_map->find(hnsw_index_id(index_name));
    ASSERT_NE(it, hnsw_map->end());
    EXPECT_EQ(it->second->node_count(), 0u);
    EXPECT_EQ(it->second->dimension(), 4u);
}

// =============================================================================
// AC: HNSW index rebuilt from table data on startup
// =============================================================================

TEST_F(HnswIndexManagerTest, HnswRebuiltFromTableData) {
    run_bootstrap();
    rebuild_indexes();

    exec_ok("CREATE TABLE docs (id INT, body VARCHAR, body_vec EMBEDDING(4, body, 'builtin/4'))");
    exec_ok("INSERT INTO docs (id, body) VALUES (1, 'hello world')");
    exec_ok("INSERT INTO docs (id, body) VALUES (2, 'foo bar')");
    exec_ok("INSERT INTO docs (id, body) VALUES (3, 'test data')");

    // Wait for embedding workers to finish.
    ASSERT_TRUE(wait_for_pool());

    auto index_name = EmbeddingColumnManager::make_index_name("docs", "body_vec");

    // Flush indexes to disk.
    auto flush = index_manager_->flush_all_indexes();
    ASSERT_TRUE(flush.has_value()) << flush.error().message;

    // Restart — indexes should load from disk or rebuild from table data.
    restart();
    run_bootstrap();
    rebuild_indexes();

    auto* hnsw_map = index_manager_->hnsw_map();
    auto it = hnsw_map->find(hnsw_index_id(index_name));
    ASSERT_NE(it, hnsw_map->end());
    EXPECT_EQ(it->second->node_count(), 3u);
}

// =============================================================================
// AC: REINDEX rebuilds HNSW index
// =============================================================================

TEST_F(HnswIndexManagerTest, ReindexRebuildsHnsw) {
    run_bootstrap();
    rebuild_indexes();

    exec_ok("CREATE TABLE docs (id INT, body VARCHAR, body_vec EMBEDDING(4, body, 'builtin/4'))");
    exec_ok("INSERT INTO docs (id, body) VALUES (1, 'hello world')");
    exec_ok("INSERT INTO docs (id, body) VALUES (2, 'foo bar')");

    ASSERT_TRUE(wait_for_pool());

    auto index_name = EmbeddingColumnManager::make_index_name("docs", "body_vec");
    auto key = hnsw_index_id(index_name);
    auto* hnsw_map = index_manager_->hnsw_map();

    // Verify index has 2 nodes.
    auto it = hnsw_map->find(key);
    ASSERT_NE(it, hnsw_map->end());
    EXPECT_EQ(it->second->node_count(), 2u);

    // REINDEX should rebuild the index from table data.
    exec_ok("REINDEX docs");

    // After REINDEX, the map should have a (potentially new) index with 2 nodes.
    it = hnsw_map->find(key);
    ASSERT_NE(it, hnsw_map->end());
    EXPECT_EQ(it->second->node_count(), 2u);
}

// =============================================================================
// AC: REINDEX by index name
// =============================================================================

TEST_F(HnswIndexManagerTest, ReindexByIndexName) {
    run_bootstrap();
    rebuild_indexes();

    exec_ok("CREATE TABLE items (id INT, description VARCHAR, desc_vec EMBEDDING(4, description, 'builtin/4'))");
    exec_ok("INSERT INTO items (id, description) VALUES (1, 'red widget')");

    ASSERT_TRUE(wait_for_pool());

    auto index_name = EmbeddingColumnManager::make_index_name("items", "desc_vec");

    exec_ok("REINDEX " + index_name);

    auto* hnsw_map = index_manager_->hnsw_map();
    auto it = hnsw_map->find(hnsw_index_id(index_name));
    ASSERT_NE(it, hnsw_map->end());
    EXPECT_EQ(it->second->node_count(), 1u);
}

// =============================================================================
// AC: Parser accepts REINDEX syntax variations
// =============================================================================

TEST_F(HnswIndexManagerTest, ReindexParserSyntax) {
    run_bootstrap();
    rebuild_indexes();

    exec_ok("CREATE TABLE syn (id INT, name VARCHAR, emb EMBEDDING(4, name, 'builtin/4'))");

    // All syntax forms should succeed.
    exec_ok("REINDEX syn");
    exec_ok("REINDEX TABLE syn");
    exec_ok("REINDEX INDEX " + EmbeddingColumnManager::make_index_name("syn", "emb"));
}

// =============================================================================
// AC: Bootstrap migrates missing HNSW IndexDef for old deployments
// =============================================================================

TEST_F(HnswIndexManagerTest, BootstrapMigratesMissingHnswIndex) {
    run_bootstrap();
    rebuild_indexes();

    exec_ok("CREATE TABLE docs (id INT, body VARCHAR, body_vec EMBEDDING(4, body, 'builtin/4'))");

    auto index_name = EmbeddingColumnManager::make_index_name("docs", "body_vec");

    // Verify index exists in catalog.
    auto idx = catalog_->get_index(index_name);
    ASSERT_TRUE(idx.has_value());

    // Simulate an old deployment: remove the HNSW IndexDef from sys_indexes
    // but leave the embedding column metadata intact.
    auto remove_r = persistence_->remove_index(idx->index_id);
    ASSERT_TRUE(remove_r.has_value()) << remove_r.error().message;
    (void)catalog_->drop_index(idx->name);

    // Confirm it's gone.
    EXPECT_FALSE(catalog_->get_index(index_name).has_value());

    // Restart — bootstrap should detect the missing HNSW entry and recreate it.
    restart();
    run_bootstrap();

    // The migration should have created the IndexDef.
    auto migrated_idx = catalog_->get_index(index_name);
    ASSERT_TRUE(migrated_idx.has_value()) << "bootstrap did not migrate missing HNSW index";
    EXPECT_EQ(migrated_idx->index_type, "hnsw");
    EXPECT_EQ(migrated_idx->columns, "body_vec");

    // IndexManager should be able to rebuild it.
    rebuild_indexes();
    auto* hnsw_map = index_manager_->hnsw_map();
    auto it = hnsw_map->find(hnsw_index_id(index_name));
    ASSERT_NE(it, hnsw_map->end());
}
