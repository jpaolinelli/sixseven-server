/// Unit tests for GDB-747: DML stamps xmin/xmax from real transactions and
/// UPDATE creates new tuple versions instead of overwriting in place.
///
/// Covers:
///  - GDB-753: Insert/Update/Delete operators stamp the statement transaction
///    (explicit BEGIN transaction or implicit autocommit transaction).
///  - GDB-755: UPDATE inserts a new version (fresh xmin, new RID) and marks
///    the old version deleted (xmax + t_ctid chain) — no in-place overwrite.
///  - GDB-761: version visibility after COMMIT / ROLLBACK.
///  - TableHeap::mark_deleted / visibility filtering primitives.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/txn/txn_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Fixture: engine pipeline with heap-level header inspection
// =============================================================================

class MvccDmlTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_mvcc_dml";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value())
            << "exec failed for: " << sql
            << " :: " << (result ? std::string{} : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    TableHeap& users_heap() {
        auto schema = catalog_.get_table(default_database_id, "users");
        EXPECT_TRUE(schema.has_value());
        auto ts = storage_->get_table_storage(schema->table_id);
        EXPECT_TRUE(ts.has_value());
        return *(*ts)->heap;
    }

    /// Collect MVCC headers for every physical version on page 1 (slots that
    /// still exist, visible or not — get_tuple_header does not filter).
    std::vector<std::pair<RID, MvccTupleHeader>> physical_versions() {
        std::vector<std::pair<RID, MvccTupleHeader>> out;
        auto& heap = users_heap();
        for (uint16_t slot = 0; slot < 16; ++slot) {
            RID rid{1, slot};
            auto header = heap.get_tuple_header(rid);
            if (header) {
                out.emplace_back(rid, *header);
            }
        }
        return out;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// GDB-755: UPDATE creates a new version (no in-place overwrite)
// =============================================================================

TEST_F(MvccDmlTest, UpdateCreatesNewVersionWithChainedHeaders) {
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("UPDATE users SET name = 'alicia' WHERE id = 1");

    auto versions = physical_versions();
    ASSERT_EQ(versions.size(), 2u) << "UPDATE must keep the old version and add a new one";

    const auto& [old_rid, old_hdr] = versions[0];
    const auto& [new_rid, new_hdr] = versions[1];

    // Old version: deleted by a real transaction, chained to the new version.
    EXPECT_NE(old_hdr.xmax, invalid_txn_id);
    EXPECT_NE(old_hdr.xmax, frozen_txn_id) << "xmax must be a real txn id, not frozen";
    ASSERT_TRUE(old_hdr.has_next_version());
    EXPECT_EQ(old_hdr.t_ctid, new_rid);
    EXPECT_NE(old_rid, new_rid) << "in-place overwrite detected: same RID for both versions";

    // New version: fresh xmin from the same transaction, not deleted.
    EXPECT_EQ(new_hdr.xmin, old_hdr.xmax);
    EXPECT_EQ(new_hdr.xmax, invalid_txn_id);
    EXPECT_FALSE(new_hdr.has_next_version());

    // Only the new version is visible to a query.
    auto qr = exec_ok("SELECT name FROM users WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alicia");
}

TEST_F(MvccDmlTest, UpdateOfMultipleRowsDoesNotReprocessNewVersions) {
    exec_ok("INSERT INTO users VALUES (1, 'a')");
    exec_ok("INSERT INTO users VALUES (2, 'b')");
    exec_ok("INSERT INTO users VALUES (3, 'c')");

    // Halloween protection: each row updated exactly once even though the
    // new versions land later in the same heap scan range.
    auto qr = exec_ok("UPDATE users SET id = id + 10");
    EXPECT_EQ(qr.affected_rows, 3);

    auto rows = exec_ok("SELECT id FROM users ORDER BY id");
    ASSERT_EQ(rows.rows.size(), 3u);
    EXPECT_EQ(rows.rows[0][0].as_int32(), 11);
    EXPECT_EQ(rows.rows[1][0].as_int32(), 12);
    EXPECT_EQ(rows.rows[2][0].as_int32(), 13);
}

// =============================================================================
// GDB-753 / GDB-761: abort semantics
// =============================================================================

TEST_F(MvccDmlTest, AbortedInsertIsInvisible) {
    exec_ok("BEGIN");
    exec_ok("INSERT INTO users VALUES (1, 'ghost')");
    exec_ok("ROLLBACK");

    auto qr = exec_ok("SELECT * FROM users");
    EXPECT_TRUE(qr.rows.empty()) << "row inserted by aborted txn must be invisible";

    auto count = exec_ok("SELECT COUNT(*) FROM users");
    ASSERT_EQ(count.rows.size(), 1u);
    EXPECT_EQ(count.rows[0][0].as_int64(), 0);
}

TEST_F(MvccDmlTest, AbortedDeleteLeavesRowVisible) {
    exec_ok("INSERT INTO users VALUES (1, 'alice')");

    exec_ok("BEGIN");
    auto del = exec_ok("DELETE FROM users WHERE id = 1");
    EXPECT_EQ(del.affected_rows, 1);
    // Inside the deleting transaction the row is already gone.
    EXPECT_TRUE(exec_ok("SELECT * FROM users").rows.empty());
    exec_ok("ROLLBACK");

    auto qr = exec_ok("SELECT name FROM users WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u) << "aborted DELETE must leave the row undeleted";
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");

    auto count = exec_ok("SELECT COUNT(*) FROM users");
    ASSERT_EQ(count.rows.size(), 1u);
    EXPECT_EQ(count.rows[0][0].as_int64(), 1);
}

TEST_F(MvccDmlTest, AbortedUpdateKeepsOldVersionHidesNewVersion) {
    exec_ok("INSERT INTO users VALUES (1, 'alice')");

    exec_ok("BEGIN");
    exec_ok("UPDATE users SET name = 'mallory' WHERE id = 1");
    exec_ok("ROLLBACK");

    auto qr = exec_ok("SELECT name FROM users WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice")
        << "aborted UPDATE must restore visibility of the old version";

    auto count = exec_ok("SELECT COUNT(*) FROM users");
    ASSERT_EQ(count.rows.size(), 1u);
    EXPECT_EQ(count.rows[0][0].as_int64(), 1);
}

// =============================================================================
// Committed paths still work
// =============================================================================

TEST_F(MvccDmlTest, CommittedExplicitTransactionPersistsChanges) {
    exec_ok("BEGIN");
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    exec_ok("INSERT INTO users VALUES (2, 'bob')");
    exec_ok("DELETE FROM users WHERE id = 2");
    exec_ok("COMMIT");

    auto qr = exec_ok("SELECT name FROM users");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alice");
}

TEST_F(MvccDmlTest, AutocommitDmlStillWorks) {
    auto ins = exec_ok("INSERT INTO users VALUES (1, 'alice')");
    EXPECT_EQ(ins.affected_rows, 1);

    auto upd = exec_ok("UPDATE users SET name = 'alicia' WHERE id = 1");
    EXPECT_EQ(upd.affected_rows, 1);
    auto qr = exec_ok("SELECT name FROM users WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "alicia");

    auto del = exec_ok("DELETE FROM users WHERE id = 1");
    EXPECT_EQ(del.affected_rows, 1);
    EXPECT_TRUE(exec_ok("SELECT * FROM users").rows.empty());
}

TEST_F(MvccDmlTest, InsertStampsRealTxnIdAsXmin) {
    exec_ok("INSERT INTO users VALUES (1, 'alice')");
    auto versions = physical_versions();
    ASSERT_EQ(versions.size(), 1u);
    EXPECT_NE(versions[0].second.xmin, invalid_txn_id);
    EXPECT_NE(versions[0].second.xmin, frozen_txn_id)
        << "autocommit INSERT must stamp a real (implicit) transaction id";
    EXPECT_EQ(versions[0].second.xmax, invalid_txn_id);
}

TEST_F(MvccDmlTest, BeginCommitRollbackReturnUtilityTags) {
    EXPECT_EQ(exec_ok("BEGIN").message, "BEGIN");
    EXPECT_EQ(exec_ok("COMMIT").message, "COMMIT");
    EXPECT_EQ(exec_ok("ROLLBACK").message, "ROLLBACK");
}

// =============================================================================
// TableHeap-level primitives (mark_deleted + visibility filtering)
// =============================================================================

class TableHeapMarkDeletedTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_mark_deleted";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        auto fid = dm_.create_file(data_dir_ / "heap.db", false, true);
        ASSERT_TRUE(fid.has_value());
        file_id_ = *fid;
        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 16);
        heap_ = std::make_unique<TableHeap>(
            *bpm_, dm_, file_id_, TableHeapOptions{.mvcc_headers = true});
    }

    void TearDown() override {
        heap_.reset();
        bpm_.reset();
        (void)dm_.close_file(file_id_);
        std::filesystem::remove_all(data_dir_);
    }

    DiskManager dm_;
    FileId file_id_ = 0;
    std::filesystem::path data_dir_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<TableHeap> heap_;
};

TEST_F(TableHeapMarkDeletedTest, MarkDeletedHidesTupleButKeepsSlot) {
    std::vector<uint8_t> data{1, 2, 3, 4};
    auto rid = heap_->insert_tuple(data);
    ASSERT_TRUE(rid.has_value());
    EXPECT_EQ(heap_->row_count(), 1u);

    ASSERT_TRUE(heap_->mark_deleted(*rid, frozen_txn_id).has_value());
    EXPECT_EQ(heap_->row_count(), 0u);

    // get_tuple filters the logically deleted version.
    auto tuple = heap_->get_tuple(*rid);
    ASSERT_FALSE(tuple.has_value());
    EXPECT_EQ(tuple.error().code, StatusCode::NOT_FOUND);

    // The physical slot survives with xmax stamped.
    auto header = heap_->get_tuple_header(*rid);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->xmax, frozen_txn_id);

    // Scans skip the deleted version.
    auto it = heap_->begin();
    ASSERT_TRUE(it.has_value());
    EXPECT_FALSE(it->next().has_value());
}

TEST_F(TableHeapMarkDeletedTest, MarkDeletedStampsVersionChainPointer) {
    std::vector<uint8_t> v1{1, 1};
    std::vector<uint8_t> v2{2, 2};
    auto old_rid = heap_->insert_tuple(v1);
    ASSERT_TRUE(old_rid.has_value());
    auto new_rid = heap_->insert_tuple(v2);
    ASSERT_TRUE(new_rid.has_value());

    ASSERT_TRUE(heap_->mark_deleted(*old_rid, frozen_txn_id, *new_rid).has_value());

    auto header = heap_->get_tuple_header(*old_rid);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->xmax, frozen_txn_id);
    ASSERT_TRUE(header->has_next_version());
    EXPECT_EQ(header->t_ctid, *new_rid);
}

TEST_F(TableHeapMarkDeletedTest, VisibilityHonorsTransactionManagerStatus) {
    TransactionManager mgr;
    heap_->attach_txn_manager(&mgr);

    auto txn = mgr.begin();
    ASSERT_TRUE(txn.has_value());
    txn_id_t xid = (*txn)->txn_id;

    std::vector<uint8_t> data{9, 9};
    auto rid = heap_->insert_tuple(data, xid);
    ASSERT_TRUE(rid.has_value());

    // Active creator: visible.
    EXPECT_TRUE(heap_->get_tuple(*rid).has_value());

    // Aborted creator: invisible.
    ASSERT_TRUE(mgr.abort(xid).has_value());
    auto tuple = heap_->get_tuple(*rid);
    ASSERT_FALSE(tuple.has_value());
    EXPECT_EQ(tuple.error().code, StatusCode::NOT_FOUND);

    // Aborted deleter: row stays visible.
    auto txn2 = mgr.begin();
    ASSERT_TRUE(txn2.has_value());
    auto rid2 = heap_->insert_tuple(data, frozen_txn_id);
    ASSERT_TRUE(rid2.has_value());
    ASSERT_TRUE(heap_->mark_deleted(*rid2, (*txn2)->txn_id).has_value());
    EXPECT_FALSE(heap_->get_tuple(*rid2).has_value());
    ASSERT_TRUE(mgr.abort((*txn2)->txn_id).has_value());
    EXPECT_TRUE(heap_->get_tuple(*rid2).has_value());

    heap_->attach_txn_manager(nullptr);
}

TEST_F(TableHeapMarkDeletedTest, AdjustRowCountClampsAtZero) {
    std::vector<uint8_t> data{5};
    ASSERT_TRUE(heap_->insert_tuple(data).has_value());
    EXPECT_EQ(heap_->row_count(), 1u);

    heap_->adjust_row_count(2);
    EXPECT_EQ(heap_->row_count(), 3u);
    heap_->adjust_row_count(-5);
    EXPECT_EQ(heap_->row_count(), 0u);
}

TEST_F(TableHeapMarkDeletedTest, MarkDeletedRejectsNonMvccHeap) {
    auto fid = dm_.create_file(data_dir_ / "raw.db", false, true);
    ASSERT_TRUE(fid.has_value());
    BufferPoolManager bpm(dm_, *fid, 16);
    TableHeap raw(bpm, dm_, *fid);

    std::vector<uint8_t> data{1};
    auto rid = raw.insert_tuple(data);
    ASSERT_TRUE(rid.has_value());

    auto result = raw.mark_deleted(*rid, frozen_txn_id);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_IMPLEMENTED);
    (void)dm_.close_file(*fid);
}
