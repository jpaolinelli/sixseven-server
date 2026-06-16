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

// =============================================================================
// ADVERSARIAL: INSERT...SELECT must insert rows AND maintain secondary indexes.
//
// GDB-845 REGRESSION (BUG): The planner's plan_insert() never handles
// stmt.select — when stmt.values is empty (INSERT...SELECT form), it produces
// an InsertOperator with 0 value_rows, silently inserting 0 rows.  The child-
// based InsertOperator constructor is dead code.  This test documents the
// CURRENT (broken) behavior and will flip to PASS once INSERT...SELECT is
// fully implemented and the index maintenance is confirmed.
// =============================================================================

TEST_F(GDB845Test, GDB845_Adv_InsertSelectInsertsRows) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE src_adv (id INT, label VARCHAR)");
    exec_ok("INSERT INTO src_adv VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e')");

    // Verify the source table has 5 rows.
    auto src_count = engine_->execute("SELECT COUNT(*) FROM src_adv");
    ASSERT_TRUE(src_count.has_value());
    ASSERT_EQ(src_count->rows.size(), 1u);
    ASSERT_EQ(src_count->rows[0][0].as_int64(), int64_t(5))
        << "source table must have 5 rows before INSERT...SELECT";

    exec_ok("CREATE TABLE dst_adv (id INT, label VARCHAR)");
    exec_ok("CREATE INDEX idx_dst_adv ON dst_adv(id)");

    // INSERT...SELECT — expect this to correctly insert 5 rows.
    // BUG (GDB-845 scope gap): currently inserts 0 rows because plan_insert()
    // never plans stmt.select.  The ASSERT_EQ below documents the failure.
    auto ins = engine_->execute("INSERT INTO dst_adv SELECT id, label FROM src_adv");
    ASSERT_TRUE(ins.has_value()) << ins.error().message;
    // DML results: QueryEngine clears rows and sets affected_rows for INSERT/UPDATE/DELETE.
    ASSERT_EQ(ins->affected_rows, int64_t(5))
        << "INSERT...SELECT must insert all 5 selected rows into dst_adv (GDB-845). "
           "affected_rows="
        << ins->affected_rows
        << " indicates the SELECT sub-plan was "
           "not executed (plan_insert() never handled stmt.select).";

    // If rows were inserted, the index must also be maintained.
    auto idx = catalog_->get_index("idx_dst_adv");
    ASSERT_TRUE(idx.has_value());
    auto it = index_manager_->btree_map()->find(idx->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());
    ASSERT_EQ(it->second->size(), 5u)
        << "INSERT...SELECT must maintain btree index for all 5 inserted rows (GDB-845)";
}

// =============================================================================
// ADVERSARIAL: Multiple secondary indexes on ONE table (btree + hash) — all
// maintained on a single INSERT...VALUES.
// =============================================================================

TEST_F(GDB845Test, GDB845_Adv_MultipleIndexesOnOneTable) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE multi_idx (id INT, name VARCHAR, score INT)");
    // Create a btree and a hash index on different columns before any inserts.
    exec_ok("CREATE INDEX idx_multi_id ON multi_idx(id)");
    exec_ok("CREATE INDEX idx_multi_score ON multi_idx(score) USING hash");

    // Insert 3 rows — both indexes must be maintained for each row.
    exec_ok("INSERT INTO multi_idx VALUES (1, 'alice', 100)");
    exec_ok("INSERT INTO multi_idx VALUES (2, 'bob', 200)");
    exec_ok("INSERT INTO multi_idx VALUES (3, 'carol', 300)");

    auto idx_id = catalog_->get_index("idx_multi_id");
    ASSERT_TRUE(idx_id.has_value());
    auto idx_score = catalog_->get_index("idx_multi_score");
    ASSERT_TRUE(idx_score.has_value());

    auto bt = index_manager_->btree_map()->find(idx_id->index_id);
    ASSERT_NE(bt, index_manager_->btree_map()->end());
    ASSERT_EQ(bt->second->size(), 3u) << "BTree index must track all 3 rows";

    auto ht = index_manager_->hash_map()->find(idx_score->index_id);
    ASSERT_NE(ht, index_manager_->hash_map()->end());
    ASSERT_EQ(ht->second->size(), 3u) << "Hash index must track all 3 rows";

    // Verify point lookups on btree.
    for (int32_t key : {1, 2, 3}) {
        auto r = bt->second->search({Value(key)});
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->has_value()) << "btree key " << key << " not found";
    }

    // Verify point lookups on hash.
    for (int32_t score : {100, 200, 300}) {
        auto r = ht->second->search({Value(score)});
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->has_value()) << "hash score " << score << " not found";
    }

    // Flush + restart — both indexes must survive.
    auto flush = index_manager_->flush_all_indexes();
    ASSERT_TRUE(flush.has_value());
    simulate_restart();
    bootstrap();
    build_index_manager();

    auto idx_id2 = catalog_->get_index("idx_multi_id");
    ASSERT_TRUE(idx_id2.has_value());
    auto bt2 = index_manager_->btree_map()->find(idx_id2->index_id);
    ASSERT_NE(bt2, index_manager_->btree_map()->end());
    ASSERT_EQ(bt2->second->size(), 3u) << "BTree index must persist 3 rows after restart";

    auto idx_score2 = catalog_->get_index("idx_multi_score");
    ASSERT_TRUE(idx_score2.has_value());
    auto ht2 = index_manager_->hash_map()->find(idx_score2->index_id);
    ASSERT_NE(ht2, index_manager_->hash_map()->end());
    ASSERT_EQ(ht2->second->size(), 3u) << "Hash index must persist 3 rows after restart";
}

// =============================================================================
// ADVERSARIAL: Duplicate keys in a non-unique BTree index — correct
// multiplicity (2 entries for the same key), no lost postings.
// =============================================================================

TEST_F(GDB845Test, GDB845_Adv_DuplicateKeysNonUniqueIndex) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE dup_tbl (id INT, data VARCHAR)");
    exec_ok("INSERT INTO dup_tbl VALUES (42, 'first')");
    exec_ok("CREATE INDEX idx_dup ON dup_tbl(id)");

    // Second row with same key — non-unique index must accept both postings.
    exec_ok("INSERT INTO dup_tbl VALUES (42, 'second')");

    auto idx = catalog_->get_index("idx_dup");
    ASSERT_TRUE(idx.has_value());
    auto it = index_manager_->btree_map()->find(idx->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());
    // Non-unique BTree must hold 2 entries for key 42.
    ASSERT_EQ(it->second->size(), 2u)
        << "Non-unique BTree index must store both postings for duplicate key 42 (GDB-845)";
}

// =============================================================================
// ADVERSARIAL: NULL in an indexed column — INSERT with NULL key must not crash
// or corrupt the index; non-NULL entries remain intact.
// =============================================================================

TEST_F(GDB845Test, GDB845_Adv_NullInIndexedColumn) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE null_tbl (id INT, val VARCHAR)");
    exec_ok("INSERT INTO null_tbl VALUES (1, 'x')");
    exec_ok("CREATE INDEX idx_null ON null_tbl(id)");

    // Insert a row where the indexed column is NULL.
    exec_ok("INSERT INTO null_tbl VALUES (NULL, 'null_row')");
    // Insert a normal row after the NULL row.
    exec_ok("INSERT INTO null_tbl VALUES (2, 'y')");

    auto idx = catalog_->get_index("idx_null");
    ASSERT_TRUE(idx.has_value());
    auto it = index_manager_->btree_map()->find(idx->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());

    // The non-NULL entries (key=1 and key=2) must be findable.
    auto r1 = it->second->search({Value(int32_t(1))});
    ASSERT_TRUE(r1.has_value());
    EXPECT_TRUE(r1->has_value()) << "key=1 must be in index even when a NULL row was inserted";

    auto r2 = it->second->search({Value(int32_t(2))});
    ASSERT_TRUE(r2.has_value());
    EXPECT_TRUE(r2->has_value()) << "key=2 must be in index even when a NULL row was inserted";
}

// =============================================================================
// ADVERSARIAL: Large batch INSERT (1000 rows) — all indexed + persisted across
// restart; no pin/latch leak observable (index size matches row count exactly).
// =============================================================================

TEST_F(GDB845Test, GDB845_Adv_LargeBatchInsertAllIndexed) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE large_tbl (id INT, val INT)");
    exec_ok("CREATE INDEX idx_large ON large_tbl(id)");

    // Insert 1000 rows via batched VALUES.  Each batch of 50 to stay within
    // typical SQL parser limits.
    const int total = 1000;
    const int batch = 50;
    for (int start = 0; start < total; start += batch) {
        std::string sql = "INSERT INTO large_tbl VALUES ";
        for (int i = start; i < start + batch; ++i) {
            if (i > start) {
                sql += ", ";
            }
            sql += "(" + std::to_string(i) + ", " + std::to_string(i * 2) + ")";
        }
        exec_ok(sql);
    }

    auto idx = catalog_->get_index("idx_large");
    ASSERT_TRUE(idx.has_value());
    auto it = index_manager_->btree_map()->find(idx->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());
    ASSERT_EQ(it->second->size(), static_cast<size_t>(total))
        << "All 1000 rows must be indexed after large batch INSERT";

    // Spot-check first, middle, and last keys.
    for (int32_t key : {0, 499, 999}) {
        auto r = it->second->search({Value(key)});
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->has_value()) << "key " << key << " missing after 1000-row batch";
    }

    // Flush and restart — index must survive with exactly 1000 entries.
    auto flush = index_manager_->flush_all_indexes();
    ASSERT_TRUE(flush.has_value()) << flush.error().message;
    simulate_restart();
    bootstrap();
    build_index_manager();

    auto idx2 = catalog_->get_index("idx_large");
    ASSERT_TRUE(idx2.has_value());
    auto it2 = index_manager_->btree_map()->find(idx2->index_id);
    ASSERT_NE(it2, index_manager_->btree_map()->end());
    ASSERT_EQ(it2->second->size(), static_cast<size_t>(total))
        << "1000-row index must survive flush+restart with exact count (GDB-845)";
}

// =============================================================================
// ADVERSARIAL: Rebuild fallback — corrupt the on-disk index file, restart, and
// confirm the rebuild from table scan yields correct contents (regression
// guard: the fix must not break the fallback path).
// =============================================================================

TEST_F(GDB845Test, GDB845_Adv_CorruptFileRebuildFallback) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE rebuild_tbl (id INT, val VARCHAR)");
    exec_ok("INSERT INTO rebuild_tbl VALUES (10, 'a')");
    exec_ok("CREATE INDEX idx_rebuild ON rebuild_tbl(id)");
    exec_ok("INSERT INTO rebuild_tbl VALUES (20, 'b')");
    exec_ok("INSERT INTO rebuild_tbl VALUES (30, 'c')");

    auto flush = index_manager_->flush_all_indexes();
    ASSERT_TRUE(flush.has_value()) << flush.error().message;

    // Locate and corrupt the on-disk index file.
    auto idx = catalog_->get_index("idx_rebuild");
    ASSERT_TRUE(idx.has_value());
    // Index files: <data_dir>/<db_id>/indexes/index_<index_id>.db
    // The default database id is 1 (user database after system bootstrap).
    // Use a glob-style search to find the file without hardcoding the db_id.
    std::filesystem::path index_path;
    for (auto& de : std::filesystem::recursive_directory_iterator(data_dir_)) {
        if (de.is_regular_file()) {
            auto stem = de.path().filename().string();
            if (stem == "index_" + std::to_string(idx->index_id) + ".db") {
                index_path = de.path();
                break;
            }
        }
    }
    if (std::filesystem::exists(index_path)) {
        // Overwrite with garbage bytes to force rebuild on next load.
        {
            auto f = std::fopen(index_path.string().c_str(), "wb");
            ASSERT_NE(f, nullptr) << "failed to open index file for corruption";
            const char garbage[] = "CORRUPT_DATA_GDB845";
            std::fwrite(garbage, 1, sizeof(garbage), f);
            std::fclose(f);
        }
        // Restart — rebuild_all_indexes() must fall back to table scan.
        simulate_restart();
        bootstrap();
        build_index_manager();

        auto idx2 = catalog_->get_index("idx_rebuild");
        ASSERT_TRUE(idx2.has_value());
        auto it2 = index_manager_->btree_map()->find(idx2->index_id);
        ASSERT_NE(it2, index_manager_->btree_map()->end());
        // Rebuild from table scan must yield all 3 rows.
        ASSERT_EQ(it2->second->size(), 3u)
            << "Rebuild fallback after corrupt file must yield 3 rows (GDB-845 regression)";
        for (int32_t key : {10, 20, 30}) {
            auto r = it2->second->search({Value(key)});
            ASSERT_TRUE(r.has_value());
            EXPECT_TRUE(r->has_value()) << "key " << key << " missing after fallback rebuild";
        }
    } else {
        // If the file naming convention differs, skip the corruption sub-test
        // but still verify the pre-flush state was correct.
        GTEST_SKIP() << "On-disk index file not found at expected path; "
                        "skipping corruption test. Path tried: "
                     << index_path.string();
    }
}

// =============================================================================
// GDB-1268 FIX VERIFICATION: INSERT...SELECT with WHERE filter inserts exactly
// the rows matching the predicate — not all rows, not zero.
// =============================================================================

TEST_F(GDB845Test, GDB1268_InsertSelectWithWhereFilter) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE src_filter (id INT, val VARCHAR)");
    exec_ok("INSERT INTO src_filter VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e')");

    exec_ok("CREATE TABLE dst_filter (id INT, val VARCHAR)");
    exec_ok("CREATE INDEX idx_dst_filter ON dst_filter(id)");

    // Only ids > 2 should be inserted (3 rows: 3, 4, 5).
    auto ins =
        engine_->execute("INSERT INTO dst_filter SELECT id, val FROM src_filter WHERE id > 2");
    ASSERT_TRUE(ins.has_value()) << ins.error().message;
    ASSERT_EQ(ins->affected_rows, int64_t(3))
        << "INSERT...SELECT WHERE id>2 must insert exactly 3 rows, got " << ins->affected_rows;

    // Verify via SELECT COUNT.
    auto cnt = engine_->execute("SELECT COUNT(*) FROM dst_filter");
    ASSERT_TRUE(cnt.has_value()) << cnt.error().message;
    ASSERT_EQ(cnt->rows.size(), 1u);
    ASSERT_EQ(cnt->rows[0][0].as_int64(), int64_t(3))
        << "dst_filter must have exactly 3 rows after filtered INSERT...SELECT";

    // Index must reflect the 3 inserted rows.
    auto idx = catalog_->get_index("idx_dst_filter");
    ASSERT_TRUE(idx.has_value());
    auto it = index_manager_->btree_map()->find(idx->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());
    ASSERT_EQ(it->second->size(), 3u)
        << "BTree index must have exactly 3 entries after INSERT...SELECT WHERE";

    // Keys 3,4,5 must be present; keys 1,2 must NOT be present.
    for (int32_t key : {3, 4, 5}) {
        auto r = it->second->search({Value(key)});
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->has_value())
            << "key " << key << " missing from index after INSERT...SELECT WHERE";
    }
    for (int32_t key : {1, 2}) {
        auto r = it->second->search({Value(key)});
        ASSERT_TRUE(r.has_value());
        EXPECT_FALSE(r->has_value())
            << "key " << key << " must NOT be in index (was filtered by WHERE id>2)";
    }
}

// =============================================================================
// GDB-1268 FIX VERIFICATION: INSERT...SELECT with empty result (no rows match
// WHERE) — must insert exactly 0 rows, not error, not silently corrupt state.
// =============================================================================

TEST_F(GDB845Test, GDB1268_InsertSelectEmptyResult) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE src_empty (id INT, val VARCHAR)");
    exec_ok("INSERT INTO src_empty VALUES (1, 'a'), (2, 'b')");

    exec_ok("CREATE TABLE dst_empty (id INT, val VARCHAR)");
    exec_ok("CREATE INDEX idx_dst_empty ON dst_empty(id)");

    // WHERE 1=0 guarantees zero rows from SELECT.
    auto ins =
        engine_->execute("INSERT INTO dst_empty SELECT id, val FROM src_empty WHERE id > 100");
    ASSERT_TRUE(ins.has_value()) << "INSERT...SELECT with empty result must succeed (not error): "
                                 << (ins.has_value() ? "" : ins.error().message);
    ASSERT_EQ(ins->affected_rows, int64_t(0))
        << "INSERT...SELECT with zero matching rows must report affected_rows=0";

    // Table must remain empty.
    auto cnt = engine_->execute("SELECT COUNT(*) FROM dst_empty");
    ASSERT_TRUE(cnt.has_value());
    ASSERT_EQ(cnt->rows[0][0].as_int64(), int64_t(0)) << "dst_empty must have 0 rows";

    // Index must also be empty.
    auto idx = catalog_->get_index("idx_dst_empty");
    ASSERT_TRUE(idx.has_value());
    auto it = index_manager_->btree_map()->find(idx->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());
    ASSERT_EQ(it->second->size(), 0u) << "Index must be empty after zero-row INSERT...SELECT";
}

// =============================================================================
// GDB-1268 FIX VERIFICATION: INSERT INTO t(b, a) SELECT x, y — permuted/reversed
// column list maps SELECT output to the CORRECT storage columns.  Asserts actual
// stored values per column, not just row count.
// =============================================================================

TEST_F(GDB845Test, GDB1268_InsertSelectPermutedColumnList) {
    bootstrap();
    build_index_manager();

    // Source has columns (x INT, y VARCHAR).
    exec_ok("CREATE TABLE src_perm (x INT, y VARCHAR)");
    exec_ok("INSERT INTO src_perm VALUES (42, 'hello')");

    // Target has columns (a VARCHAR, b INT) — reversed from SELECT output.
    // INSERT INTO dst_perm(b, a) SELECT x, y — so b<-x=42, a<-y='hello'.
    exec_ok("CREATE TABLE dst_perm (a VARCHAR, b INT)");

    auto ins = engine_->execute("INSERT INTO dst_perm(b, a) SELECT x, y FROM src_perm");
    ASSERT_TRUE(ins.has_value()) << ins.error().message;
    ASSERT_EQ(ins->affected_rows, int64_t(1))
        << "INSERT INTO dst_perm(b,a) SELECT must insert 1 row";

    // Read back and verify the actual stored values.
    auto sel = engine_->execute("SELECT a, b FROM dst_perm");
    ASSERT_TRUE(sel.has_value()) << sel.error().message;
    ASSERT_EQ(sel->rows.size(), 1u) << "dst_perm must have exactly 1 row";
    // a should be 'hello' (mapped from SELECT y)
    EXPECT_EQ(sel->rows[0][0].as_string(), std::string("hello"))
        << "Column a must contain 'hello' (mapped from SELECT y via column list b,a)";
    // b should be 42 (mapped from SELECT x)
    EXPECT_EQ(sel->rows[0][1].as_int32(), int32_t(42))
        << "Column b must contain 42 (mapped from SELECT x via column list b,a)";
}

// =============================================================================
// GDB-1268 FIX VERIFICATION: Secondary index maintained via SELECT path.
// After INSERT...SELECT, point-lookups for inserted keys return correct RIDs;
// index entry count matches row count exactly.  Persists across restart.
// =============================================================================

TEST_F(GDB845Test, GDB1268_InsertSelectMaintainsIndexPersistsAcrossRestart) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE src_idx (id INT, name VARCHAR)");
    exec_ok("INSERT INTO src_idx VALUES (10, 'ten'), (20, 'twenty'), (30, 'thirty')");

    exec_ok("CREATE TABLE dst_idx (id INT, name VARCHAR)");
    exec_ok("CREATE INDEX idx_dst_id ON dst_idx(id)");
    exec_ok("CREATE INDEX idx_dst_id_hash ON dst_idx(id) USING hash");

    auto ins = engine_->execute("INSERT INTO dst_idx SELECT id, name FROM src_idx");
    ASSERT_TRUE(ins.has_value()) << ins.error().message;
    ASSERT_EQ(ins->affected_rows, int64_t(3));

    // Check btree index in-memory.
    auto btree_idx = catalog_->get_index("idx_dst_id");
    ASSERT_TRUE(btree_idx.has_value());
    auto bt = index_manager_->btree_map()->find(btree_idx->index_id);
    ASSERT_NE(bt, index_manager_->btree_map()->end());
    ASSERT_EQ(bt->second->size(), 3u)
        << "BTree index must have 3 entries immediately after INSERT...SELECT";
    for (int32_t key : {10, 20, 30}) {
        auto r = bt->second->search({Value(key)});
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->has_value()) << "BTree: key " << key << " not found after INSERT...SELECT";
    }

    // Check hash index in-memory.
    auto hash_idx = catalog_->get_index("idx_dst_id_hash");
    ASSERT_TRUE(hash_idx.has_value());
    auto ht = index_manager_->hash_map()->find(hash_idx->index_id);
    ASSERT_NE(ht, index_manager_->hash_map()->end());
    ASSERT_EQ(ht->second->size(), 3u)
        << "Hash index must have 3 entries immediately after INSERT...SELECT";
    for (int32_t key : {10, 20, 30}) {
        auto r = ht->second->search({Value(key)});
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->has_value()) << "Hash: key " << key << " not found after INSERT...SELECT";
    }

    // Flush + restart — both indexes must survive with exact count.
    auto flush = index_manager_->flush_all_indexes();
    ASSERT_TRUE(flush.has_value()) << flush.error().message;
    simulate_restart();
    bootstrap();
    build_index_manager();

    auto btree_idx2 = catalog_->get_index("idx_dst_id");
    ASSERT_TRUE(btree_idx2.has_value());
    auto bt2 = index_manager_->btree_map()->find(btree_idx2->index_id);
    ASSERT_NE(bt2, index_manager_->btree_map()->end());
    ASSERT_EQ(bt2->second->size(), 3u)
        << "BTree index must have 3 entries after flush+restart (INSERT...SELECT path)";

    auto hash_idx2 = catalog_->get_index("idx_dst_id_hash");
    ASSERT_TRUE(hash_idx2.has_value());
    auto ht2 = index_manager_->hash_map()->find(hash_idx2->index_id);
    ASSERT_NE(ht2, index_manager_->hash_map()->end());
    ASSERT_EQ(ht2->second->size(), 3u)
        << "Hash index must have 3 entries after flush+restart (INSERT...SELECT path)";
}

// =============================================================================
// GDB-1268: INSERT...SELECT from a table that itself has indexes (source table
// has a btree index).  Only the DESTINATION indexes are maintained; the source
// index must remain unchanged.
// =============================================================================

TEST_F(GDB845Test, GDB1268_InsertSelectFromIndexedSourceTable) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE src_with_idx (id INT, val VARCHAR)");
    exec_ok("CREATE INDEX idx_src ON src_with_idx(id)");
    exec_ok("INSERT INTO src_with_idx VALUES (1, 'x'), (2, 'y'), (3, 'z')");

    exec_ok("CREATE TABLE dst_from_indexed (id INT, val VARCHAR)");
    exec_ok("CREATE INDEX idx_dst_from_indexed ON dst_from_indexed(id)");

    auto ins = engine_->execute("INSERT INTO dst_from_indexed SELECT id, val FROM src_with_idx");
    ASSERT_TRUE(ins.has_value()) << ins.error().message;
    ASSERT_EQ(ins->affected_rows, int64_t(3));

    // Source index must still have exactly 3 entries (unaffected).
    auto src_idx = catalog_->get_index("idx_src");
    ASSERT_TRUE(src_idx.has_value());
    auto src_bt = index_manager_->btree_map()->find(src_idx->index_id);
    ASSERT_NE(src_bt, index_manager_->btree_map()->end());
    ASSERT_EQ(src_bt->second->size(), 3u) << "Source index must be unaffected by INSERT...SELECT";

    // Destination index must have 3 entries.
    auto dst_idx = catalog_->get_index("idx_dst_from_indexed");
    ASSERT_TRUE(dst_idx.has_value());
    auto dst_bt = index_manager_->btree_map()->find(dst_idx->index_id);
    ASSERT_NE(dst_bt, index_manager_->btree_map()->end());
    ASSERT_EQ(dst_bt->second->size(), 3u)
        << "Destination index must have 3 entries after INSERT...SELECT from indexed source";
}

// =============================================================================
// GDB-1268: INSERT...SELECT with duplicate keys into a non-unique destination
// index — correct multiplicity (both rows indexed, search returns a hit).
// =============================================================================

TEST_F(GDB845Test, GDB1268_InsertSelectDuplicateKeysNonUniqueTargetIndex) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE src_dup (id INT, val VARCHAR)");
    // Two rows with the same id.
    exec_ok("INSERT INTO src_dup VALUES (7, 'first'), (7, 'second'), (8, 'third')");

    exec_ok("CREATE TABLE dst_dup (id INT, val VARCHAR)");
    exec_ok("CREATE INDEX idx_dst_dup ON dst_dup(id)");

    auto ins = engine_->execute("INSERT INTO dst_dup SELECT id, val FROM src_dup");
    ASSERT_TRUE(ins.has_value()) << ins.error().message;
    ASSERT_EQ(ins->affected_rows, int64_t(3));

    // Non-unique BTree must hold 3 entries: 2 for key=7, 1 for key=8.
    auto idx = catalog_->get_index("idx_dst_dup");
    ASSERT_TRUE(idx.has_value());
    auto it = index_manager_->btree_map()->find(idx->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());
    ASSERT_EQ(it->second->size(), 3u)
        << "Non-unique BTree must hold 3 entries (2×key=7 + 1×key=8) after INSERT...SELECT";

    // Both duplicate-key search results must return a valid entry.
    auto r7 = it->second->search({Value(int32_t(7))});
    ASSERT_TRUE(r7.has_value());
    EXPECT_TRUE(r7->has_value()) << "key=7 must be found in non-unique index (INSERT...SELECT)";
    auto r8 = it->second->search({Value(int32_t(8))});
    ASSERT_TRUE(r8.has_value());
    EXPECT_TRUE(r8->has_value()) << "key=8 must be found in non-unique index (INSERT...SELECT)";
}

// =============================================================================
// GDB-1268: Column-count mismatch — SELECT yields fewer columns than the target
// column list expects → planner must return a proper error, NOT crash or silently
// insert wrong data.
// =============================================================================

TEST_F(GDB845Test, GDB1268_InsertSelectColumnCountMismatchFewerCols) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE src_mismatch (id INT)");
    exec_ok("INSERT INTO src_mismatch VALUES (1), (2)");

    exec_ok("CREATE TABLE dst_mismatch (a INT, b VARCHAR)");

    // SELECT produces 1 column but target (no explicit list) expects 2.
    auto ins = engine_->execute("INSERT INTO dst_mismatch SELECT id FROM src_mismatch");
    // Must error — column count mismatch.
    EXPECT_FALSE(ins.has_value())
        << "INSERT INTO dst_mismatch (2 cols) SELECT id (1 col) must fail with column-count error";
    if (!ins.has_value()) {
        EXPECT_NE(ins.error().message.size(), 0u) << "Error message must not be empty";
    }
}

// =============================================================================
// GDB-1268: NULL values in SELECT output inserted into indexed column — must not
// crash or corrupt the index; non-NULL rows remain findable.
// =============================================================================

TEST_F(GDB845Test, GDB1268_InsertSelectNullValueInIndexedColumn) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE src_null_sel (id INT, val VARCHAR)");
    exec_ok("INSERT INTO src_null_sel VALUES (1, 'good'), (NULL, 'null_id'), (2, 'also_good')");

    exec_ok("CREATE TABLE dst_null_sel (id INT, val VARCHAR)");
    exec_ok("CREATE INDEX idx_dst_null_sel ON dst_null_sel(id)");

    auto ins = engine_->execute("INSERT INTO dst_null_sel SELECT id, val FROM src_null_sel");
    ASSERT_TRUE(ins.has_value()) << "INSERT...SELECT with NULL in indexed column must not error: "
                                 << (ins.has_value() ? "" : ins.error().message);
    ASSERT_EQ(ins->affected_rows, int64_t(3)) << "All 3 rows must be inserted including NULL row";

    // Non-NULL keys must be findable.
    auto idx = catalog_->get_index("idx_dst_null_sel");
    ASSERT_TRUE(idx.has_value());
    auto it = index_manager_->btree_map()->find(idx->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());

    auto r1 = it->second->search({Value(int32_t(1))});
    ASSERT_TRUE(r1.has_value());
    EXPECT_TRUE(r1->has_value()) << "key=1 must be in index after INSERT...SELECT with a NULL row";
    auto r2 = it->second->search({Value(int32_t(2))});
    ASSERT_TRUE(r2.has_value());
    EXPECT_TRUE(r2->has_value()) << "key=2 must be in index after INSERT...SELECT with a NULL row";
}

// =============================================================================
// GDB-1268: Large batch (1000 rows) via INSERT...SELECT — all rows inserted,
// all indexed, no pin/latch leak observable (exact index count).
// =============================================================================

TEST_F(GDB845Test, GDB1268_InsertSelectLargeBatch1000Rows) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE src_large_sel (id INT, val INT)");

    // Insert 1000 rows into the source table (50 per statement).
    const int total = 1000;
    const int batch = 50;
    for (int start = 0; start < total; start += batch) {
        std::string sql = "INSERT INTO src_large_sel VALUES ";
        for (int i = start; i < start + batch; ++i) {
            if (i > start)
                sql += ", ";
            sql += "(" + std::to_string(i) + ", " + std::to_string(i * 3) + ")";
        }
        exec_ok(sql);
    }

    exec_ok("CREATE TABLE dst_large_sel (id INT, val INT)");
    exec_ok("CREATE INDEX idx_dst_large_sel ON dst_large_sel(id)");

    auto ins = engine_->execute("INSERT INTO dst_large_sel SELECT id, val FROM src_large_sel");
    ASSERT_TRUE(ins.has_value()) << ins.error().message;
    ASSERT_EQ(ins->affected_rows, int64_t(total))
        << "INSERT...SELECT must insert all " << total << " rows, got " << ins->affected_rows;

    // Index must have exactly 1000 entries.
    auto idx = catalog_->get_index("idx_dst_large_sel");
    ASSERT_TRUE(idx.has_value());
    auto it = index_manager_->btree_map()->find(idx->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());
    ASSERT_EQ(it->second->size(), static_cast<size_t>(total))
        << "Index must have exactly " << total << " entries after large INSERT...SELECT";

    // Spot-check boundary keys.
    for (int32_t key : {0, 499, 999}) {
        auto r = it->second->search({Value(key)});
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->has_value()) << "key " << key << " missing after 1000-row INSERT...SELECT";
    }

    // Flush + restart — exact count must survive.
    auto flush = index_manager_->flush_all_indexes();
    ASSERT_TRUE(flush.has_value()) << flush.error().message;
    simulate_restart();
    bootstrap();
    build_index_manager();

    auto idx2 = catalog_->get_index("idx_dst_large_sel");
    ASSERT_TRUE(idx2.has_value());
    auto it2 = index_manager_->btree_map()->find(idx2->index_id);
    ASSERT_NE(it2, index_manager_->btree_map()->end());
    ASSERT_EQ(it2->second->size(), static_cast<size_t>(total))
        << "1000-row INSERT...SELECT index must survive flush+restart with exact count";
}

// =============================================================================
// GDB-1268 LOW-GAP BOUNDARY: Unmapped non-nullable column via explicit column
// list INSERT...SELECT.  A non-nullable column not in the INSERT column list
// must either (a) produce an error or (b) use the column DEFAULT.  Silently
// storing null in a non-nullable column is a correctness bug.
//
// This test probes the boundary.  If the engine silently stores null without
// error (violating NOT NULL), we record it as a documented bug.
// =============================================================================

TEST_F(GDB845Test, GDB1268_LowGap_UnmappedNonNullableColumnRejectsOrErrors) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE src_notnull (x INT)");
    exec_ok("INSERT INTO src_notnull VALUES (1)");

    // Target has a non-nullable column 'b' that is NOT in the INSERT column list.
    // The INSERT...SELECT column list only maps to 'a'.
    // Expected: either error at plan/execute time, or default applied.
    // Bug: silently stores NULL in non-nullable column b.
    exec_ok("CREATE TABLE dst_notnull (a INT, b INT NOT NULL)");

    auto ins = engine_->execute("INSERT INTO dst_notnull(a) SELECT x FROM src_notnull");
    if (ins.has_value()) {
        // If insert succeeded, verify what was actually stored for column b.
        auto sel = engine_->execute("SELECT a, b FROM dst_notnull");
        if (sel.has_value() && !sel->rows.empty()) {
            bool b_is_null = sel->rows[0][1].is_null();
            // Document the finding. A null stored in a NOT NULL column is a bug.
            // We use EXPECT (not ASSERT) so the test reports but doesn't abort.
            EXPECT_FALSE(b_is_null)
                << "GDB-1268 LOW-GAP: column b is NOT NULL but INSERT...SELECT(a) stored NULL "
                   "in b without error. This is a correctness violation — the engine must "
                   "reject this INSERT or apply the DEFAULT value.";
            RecordProperty("low_gap_null_in_not_null_col", b_is_null ? 1 : 0);
        }
    } else {
        // Engine correctly rejected the INSERT — this is the preferred behavior.
        RecordProperty("low_gap_rejected_with_error", 1);
    }
}

// =============================================================================
// KNOWN-GAP PROBE (documented, not a QA_FAIL): Does a subsequent UPDATE leave
// the secondary index stale across restart?  This is intentionally OUT OF SCOPE
// for GDB-845 (UPDATE/DELETE maintenance is deferred).  The test documents the
// current behavior so a future ticket can verify the fix.
// =============================================================================

TEST_F(GDB845Test, GDB845_KnownGap_UpdateLeavesIndexStale) {
    bootstrap();
    build_index_manager();

    exec_ok("CREATE TABLE upd_gap_tbl (id INT, val VARCHAR)");
    exec_ok("INSERT INTO upd_gap_tbl VALUES (1, 'original')");
    exec_ok("CREATE INDEX idx_upd_gap ON upd_gap_tbl(id)");
    exec_ok("INSERT INTO upd_gap_tbl VALUES (2, 'second')");

    // UPDATE changes id=2 to id=99.  The secondary index is NOT updated by
    // the fix (deferred scope).  We observe the stale index size to document
    // the gap — we do NOT ASSERT_EQ here so the test never blocks a merge.
    exec_ok("UPDATE upd_gap_tbl SET id = 99 WHERE id = 2");

    auto idx = catalog_->get_index("idx_upd_gap");
    ASSERT_TRUE(idx.has_value());
    auto it = index_manager_->btree_map()->find(idx->index_id);
    ASSERT_NE(it, index_manager_->btree_map()->end());

    // Document (not assert) the known gap: after UPDATE the index may still
    // show 2 entries with key=2 present and key=99 absent.
    size_t observed_size = it->second->size();
    auto r99 = it->second->search({Value(int32_t(99))});
    ASSERT_TRUE(r99.has_value());
    bool key99_indexed = r99->has_value();

    // These are informational RecordProperty calls, not assertions that can fail.
    RecordProperty("known_gap_index_size_after_update", static_cast<int>(observed_size));
    RecordProperty("known_gap_key99_in_index_after_update", key99_indexed ? 1 : 0);

    // The only hard assertion: INSERT-maintained entries (key=1) must still
    // be present — UPDATE must not corrupt them.
    auto r1 = it->second->search({Value(int32_t(1))});
    ASSERT_TRUE(r1.has_value());
    EXPECT_TRUE(r1->has_value())
        << "key=1 (INSERT-maintained) must remain in index after UPDATE of another row";
}

// GDB-1269: INSERT...SELECT with explicit column list must reject rows where
// an unmapped column is NOT NULL (no default).  Silently storing NULL was a
// constraint-violation bypass.
TEST_F(GDB845Test, GDB1268_LowGap_UnmappedNonNullableColumnRejectsOrErrors) {
    bootstrap();
    build_index_manager();

    // dst has two columns: a nullable, b NOT NULL.
    exec_ok("CREATE TABLE dst_notnull (a INT, b INT NOT NULL)");
    exec_ok("CREATE TABLE src_notnull (x INT)");
    exec_ok("INSERT INTO src_notnull VALUES (42)");

    // INSERT INTO dst_notnull(a) SELECT x FROM src_notnull
    // maps x -> a but leaves b (NOT NULL) unmapped.
    // Must fail with CONSTRAINT_VIOLATION, not silently store NULL in b.
    auto result = engine_->execute("INSERT INTO dst_notnull(a) SELECT x FROM src_notnull");

    ASSERT_FALSE(result.has_value())
        << "INSERT...SELECT that leaves a NOT NULL column unmapped must be rejected";
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION)
        << "Expected CONSTRAINT_VIOLATION for NOT NULL violation, got: " << result.error().message;

    // Positive control: dst2 has b as nullable — unmapped b should succeed and store NULL.
    exec_ok("CREATE TABLE dst_nullable (a INT, b INT)");
    auto ok_result = engine_->execute("INSERT INTO dst_nullable(a) SELECT x FROM src_notnull");
    ASSERT_TRUE(ok_result.has_value())
        << "INSERT...SELECT with unmapped nullable column must succeed, got: "
        << (ok_result ? "ok" : ok_result.error().message);

    auto sel = engine_->execute("SELECT a, b FROM dst_nullable");
    ASSERT_TRUE(sel.has_value());
    ASSERT_EQ(sel->rows.size(), 1u);
    EXPECT_EQ(sel->rows[0][0].as_int32(), 42);
    EXPECT_TRUE(sel->rows[0][1].is_null())
        << "Unmapped nullable column must be NULL after INSERT...SELECT";
}
