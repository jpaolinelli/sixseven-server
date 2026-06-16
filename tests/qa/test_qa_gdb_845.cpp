// GDB-845: FlushPersistsLatestState cannot fail even if flush_all_indexes
// persists nothing.
//
// Root cause: INSERT did not maintain secondary BTree/Hash indexes, so the
// in-memory index only ever reflected rows present at CREATE INDEX time.
// flush_all_indexes() persisted this stale snapshot, and rebuild_all_indexes()
// on restart loaded the stale file — silently losing post-creation inserts.
// The original unit test used EXPECT_GE(size, 1) which passed whether flush
// wrote 1 entry (stale) or 3 entries (correct).
//
// Fix: InsertOperator now calls maintain_secondary_indexes() for every
// inserted row, keeping BTree/Hash indexes in sync at all times.
//
// These QA tests verify the end-to-end durability contract:
//   1. INSERT maintains the live in-memory index immediately.
//   2. flush_all_indexes() persists the up-to-date state.
//   3. After restart, the loaded index contains all rows (not just CREATE INDEX rows).
//   4. A broken flush (no-op) would leave size==1, which ASSERT_EQ(size,3) catches.

#include "sixseven/catalog/catalog.h"
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

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "../unit/test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Shared fixture
// =============================================================================

class GDB845Test : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb845";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        init_stack();
    }

    void TearDown() override {
        destroy_stack();
        std::filesystem::remove_all(data_dir_);
    }

    void init_stack() {
        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        config_ = Config::load_defaults();
    }

    void destroy_stack() {
        index_manager_.reset();
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
    }

    void bootstrap() {
        auto r = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    void build_index_manager() {
        index_manager_ = std::make_unique<IndexManager>(*catalog_, *storage_);
        index_manager_->set_catalog_persistence(persistence_.get());
        auto r = index_manager_->rebuild_all_indexes();
        ASSERT_TRUE(r.has_value()) << r.error().message;
        engine_->set_index_manager(index_manager_.get());
    }

    void simulate_restart() {
        destroy_stack();
        init_stack();
    }

    void exec_ok(const std::string& sql) {
        auto r = engine_->execute(sql);
        ASSERT_TRUE(r.has_value()) << "SQL failed: " << sql << " — " << r.error().message;
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
// AC1: INSERT maintains the in-memory BTree index immediately.
// Regression: before the fix, INSERT did not touch btree_indexes_, so the
// live index only had 1 entry (the CREATE INDEX snapshot).
// =============================================================================

TEST_F(GDB845Test, GDB845_InsertMaintainsBtreeIndexInMemory) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE t_btree (id INT, val VARCHAR)");
    exec_ok("INSERT INTO t_btree VALUES (10, 'ten')");
    exec_ok("CREATE INDEX idx_btree ON t_btree(id)");

    // At this point the in-memory index has 1 entry (only the row present
    // during CREATE INDEX).
    exec_ok("INSERT INTO t_btree VALUES (20, 'twenty')");
    exec_ok("INSERT INTO t_btree VALUES (30, 'thirty')");

    auto idx = catalog_->get_index("idx_btree");
    ASSERT_TRUE(idx.has_value());
    auto it = index_manager_->btree_map()->find(idx->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());

    // Must be 3: INSERT must maintain the index.
    ASSERT_EQ(it->second->size(), 3u) << "INSERT must maintain the BTree index in memory (GDB-845)";

    // Every inserted key must be searchable.
    for (int32_t key : {10, 20, 30}) {
        auto r = it->second->search({Value(key)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
        EXPECT_TRUE(r->has_value()) << "key " << key << " not found in index";
    }
    // Key not inserted must not be found.
    auto r_miss = it->second->search({Value(int32_t(99))});
    ASSERT_TRUE(r_miss.has_value());
    EXPECT_FALSE(r_miss->has_value());
}

// =============================================================================
// AC2: flush_all_indexes() persists the post-INSERT state; after restart the
// loaded index has all 3 entries, not just the CREATE INDEX snapshot.
// Regression: before the fix, the flushed file had 1 entry; the loaded count
// after restart was 1, but EXPECT_GE(size,1) masked it.
// =============================================================================

TEST_F(GDB845Test, GDB845_FlushPersistsPostInsertState) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE t_flush (id INT, name VARCHAR)");
    exec_ok("INSERT INTO t_flush VALUES (1, 'a')");
    exec_ok("CREATE INDEX idx_flush ON t_flush(id)");

    // Two rows inserted after CREATE INDEX — the index must track them.
    exec_ok("INSERT INTO t_flush VALUES (2, 'b')");
    exec_ok("INSERT INTO t_flush VALUES (3, 'c')");

    // Flush the 3-entry index to disk.
    auto flush = index_manager_->flush_all_indexes();
    ASSERT_TRUE(flush.has_value()) << flush.error().message;

    // Restart.  rebuild_all_indexes() loads from disk when the file is valid.
    // If flush had been a no-op, the file would carry 1 entry → test fails.
    simulate_restart();
    bootstrap();
    build_index_manager();

    auto idx = catalog_->get_index("idx_flush");
    ASSERT_TRUE(idx.has_value());
    auto it = index_manager_->btree_map()->find(idx->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());

    // Exact count: flush must have persisted all 3 entries.
    ASSERT_EQ(it->second->size(), 3u)
        << "flush_all_indexes() must persist the post-INSERT state (3 rows); "
           "if size==1, flush wrote only the CREATE INDEX snapshot (GDB-845)";

    // All three keys must be present and return valid RIDs.
    for (int32_t key : {1, 2, 3}) {
        auto r = it->second->search({Value(key)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
        EXPECT_TRUE(r->has_value())
            << "key " << key << " missing from persisted index after restart";
    }

    // A key that was never inserted must not appear.
    auto r_miss = it->second->search({Value(int32_t(4))});
    ASSERT_TRUE(r_miss.has_value());
    EXPECT_FALSE(r_miss->has_value()) << "phantom key 4 found in index";
}

// =============================================================================
// AC3: INSERT maintains the in-memory Hash index immediately.
// =============================================================================

TEST_F(GDB845Test, GDB845_InsertMaintainsHashIndexInMemory) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE t_hash (id INT, val VARCHAR)");
    exec_ok("INSERT INTO t_hash VALUES (100, 'x')");
    exec_ok("CREATE INDEX idx_hash ON t_hash(id) USING hash");

    exec_ok("INSERT INTO t_hash VALUES (200, 'y')");
    exec_ok("INSERT INTO t_hash VALUES (300, 'z')");

    auto idx = catalog_->get_index("idx_hash");
    ASSERT_TRUE(idx.has_value());
    auto it = index_manager_->hash_map()->find(idx->index_id);
    ASSERT_NE(it, index_manager_->hash_map()->end());

    ASSERT_EQ(it->second->size(), 3u) << "INSERT must maintain the Hash index in memory (GDB-845)";

    for (int32_t key : {100, 200, 300}) {
        auto r = it->second->search({Value(key)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
        EXPECT_TRUE(r->has_value()) << "key " << key << " not found in hash index";
    }
}

// =============================================================================
// AC4: flush_all_indexes() persists the post-INSERT Hash index state.
// =============================================================================

TEST_F(GDB845Test, GDB845_FlushPersistsPostInsertHashState) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE t_hash_flush (id INT, name VARCHAR)");
    exec_ok("INSERT INTO t_hash_flush VALUES (10, 'x')");
    exec_ok("CREATE INDEX idx_hash_flush ON t_hash_flush(id) USING hash");

    exec_ok("INSERT INTO t_hash_flush VALUES (20, 'y')");
    exec_ok("INSERT INTO t_hash_flush VALUES (30, 'z')");

    auto flush = index_manager_->flush_all_indexes();
    ASSERT_TRUE(flush.has_value()) << flush.error().message;

    simulate_restart();
    bootstrap();
    build_index_manager();

    auto idx = catalog_->get_index("idx_hash_flush");
    ASSERT_TRUE(idx.has_value());
    auto it = index_manager_->hash_map()->find(idx->index_id);
    ASSERT_NE(it, index_manager_->hash_map()->end());

    ASSERT_EQ(it->second->size(), 3u)
        << "flush_all_indexes() must persist the post-INSERT hash state (3 rows)";

    for (int32_t key : {10, 20, 30}) {
        auto r = it->second->search({Value(key)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
        EXPECT_TRUE(r->has_value())
            << "key " << key << " missing from persisted hash index after restart";
    }
}
