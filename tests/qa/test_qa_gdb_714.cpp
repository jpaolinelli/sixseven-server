/// QA regression tests for GDB-714: Add MVCC tuple headers (xmin/xmax) to
/// TableHeap storage.
///
/// Audit foundation for C10. Acceptance criteria:
///  1. Tuples persist xmin/xmax; existing data migrated or rebuild documented.
///     -> SQL table files (StorageManager heaps) store a 24-byte
///        MvccTupleHeader per tuple; headers survive flush + reopen. There is
///        no v1 migration: the file format version was bumped to 2 and v1
///        files are rejected with rebuild guidance (docs/mvcc-tuple-format.md).
///  2. WAL records carry header changes for redo.
///     -> TableHeap WAL records carry full on-page images including headers;
///        DELETE records carry the stamped xmax; TableHeapRecoveryHandler
///        redo reconstructs identical tuples on a fresh file.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_recovery.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/table_wal.h"
#include "sixseven/txn/mvcc_tuple.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "test_qa_helpers.h"

using namespace sixseven;

namespace {

std::vector<uint8_t> qa_bytes(size_t size, uint8_t fill) {
    return std::vector<uint8_t>(size, fill);
}

} // namespace

// =============================================================================
// Fixture: full SQL pipeline over StorageManager-backed tables
// =============================================================================

class QA_GDB714 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb714";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        exec_ok("CREATE TABLE items (id INT, name VARCHAR, qty INT)");
        exec_ok("INSERT INTO items VALUES (1, 'alpha', 10)");
        exec_ok("INSERT INTO items VALUES (2, 'beta', 20)");
        exec_ok("INSERT INTO items VALUES (3, 'gamma', 30)");
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::error_code ec;
        std::filesystem::remove_all(data_dir_, ec);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value())
            << "exec failed for: " << sql
            << " :: " << (result ? std::string{} : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    /// Resolve the heap backing a SQL table.
    TableHeap* heap_for(const std::string& table_name) {
        auto schema = catalog_.get_table(default_database_id, table_name);
        EXPECT_TRUE(schema.has_value());
        if (!schema) {
            return nullptr;
        }
        auto ts = storage_->get_table_storage(schema->table_id);
        EXPECT_TRUE(ts.has_value());
        return ts ? (*ts)->heap.get() : nullptr;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::filesystem::path data_dir_;
};

// =============================================================================
// AC 1: SQL-inserted tuples carry persisted MVCC headers
// =============================================================================

TEST_F(QA_GDB714, SqlInsertedRowsCarryMvccHeaders) {
    TableHeap* heap = heap_for("items");
    ASSERT_NE(heap, nullptr);
    ASSERT_TRUE(heap->mvcc_headers());

    auto it = heap->begin();
    ASSERT_TRUE(it.has_value());

    size_t rows = 0;
    while (auto row = it->next()) {
        auto header = heap->get_tuple_header(row->first);
        ASSERT_TRUE(header.has_value()) << header.error().message;
        // Since GDB-747, autocommit DML stamps a real per-statement
        // transaction id (committed immediately), not the frozen sentinel.
        EXPECT_NE(header->xmin, invalid_txn_id);
        EXPECT_EQ(header->xmax, invalid_txn_id);
        ++rows;
    }
    EXPECT_EQ(rows, 3u);
}

TEST_F(QA_GDB714, HeadersAndDataPersistAcrossEngineRestart) {
    // Tear down the engine + storage manager. ~StorageManager flushes all
    // dirty pages and closes the table files.
    engine_.reset();
    storage_.reset();

    storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
    engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

    // Reopen the table storage over the persisted v2 file (in production this
    // happens during system bootstrap / lazy admin-command opens).
    auto schema = catalog_.get_table(default_database_id, "items");
    ASSERT_TRUE(schema.has_value());
    ASSERT_TRUE(
        storage_->open_table_storage(default_database_id, schema->table_id, *schema).has_value());

    // Data round-trips through the v2 on-disk format.
    auto qr = exec_ok("SELECT id, name, qty FROM items");
    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "alpha");
    EXPECT_EQ(qr.rows[2][2].as_int32(), 30);

    // Headers were persisted, not just the user payload.
    TableHeap* heap = heap_for("items");
    ASSERT_NE(heap, nullptr);
    auto it = heap->begin();
    ASSERT_TRUE(it.has_value());
    size_t rows = 0;
    while (auto row = it->next()) {
        auto header = heap->get_tuple_header(row->first);
        ASSERT_TRUE(header.has_value()) << header.error().message;
        // Real xmin stamps (GDB-747) persist across restart. Ids unknown to
        // the fresh TransactionManager read as committed, so the rows stay
        // visible.
        EXPECT_NE(header->xmin, invalid_txn_id);
        EXPECT_EQ(header->xmax, invalid_txn_id);
        ++rows;
    }
    EXPECT_EQ(rows, 3u);
}

TEST_F(QA_GDB714, UpdateCreatesNewVersionAndPreservesData) {
    // Since GDB-747 the UPDATE path is delete+insert: the old version gets
    // xmax (and a t_ctid chain pointer), the new version a fresh xmin.
    exec_ok("UPDATE items SET qty = 99 WHERE id = 2");

    auto qr = exec_ok("SELECT qty FROM items WHERE id = 2");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 99);

    TableHeap* heap = heap_for("items");
    ASSERT_NE(heap, nullptr);
    auto it = heap->begin();
    ASSERT_TRUE(it.has_value());
    while (auto row = it->next()) {
        // Visible versions are undeleted and carry a valid creator id.
        auto header = heap->get_tuple_header(row->first);
        ASSERT_TRUE(header.has_value()) << header.error().message;
        EXPECT_NE(header->xmin, invalid_txn_id);
        EXPECT_EQ(header->xmax, invalid_txn_id);
    }
}

TEST_F(QA_GDB714, DeleteKeepsObservableSemantics) {
    // Physical delete is intentionally preserved in this foundation: the row
    // disappears immediately from scans (the xmax stamp travels in the WAL).
    exec_ok("DELETE FROM items WHERE id = 1");
    auto qr = exec_ok("SELECT id FROM items");
    EXPECT_EQ(qr.rows.size(), 2u);
}

TEST_F(QA_GDB714, WideRowsRoundTripUnderNewLayout) {
    // Regression tripwire shared with QA_GDB711.VacuumPreservesWideRows: the
    // +24-byte header must not break wide-row round trips.
    exec_ok("CREATE TABLE wide_t (id INT, name VARCHAR)");
    exec_ok("INSERT INTO wide_t VALUES (1, 'a-thirty-character-wide-string')");
    exec_ok("INSERT INTO wide_t VALUES (2, 'another-thirty-char-wide-value')");

    auto qr = exec_ok("SELECT id, name FROM wide_t");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "a-thirty-character-wide-string");
}

// =============================================================================
// AC 1: no migration — v1 files are rejected with rebuild guidance
// =============================================================================

TEST_F(QA_GDB714, OldFormatFileRejectedWithRebuildGuidance) {
    auto path = data_dir_ / "legacy_v1.db";
    DiskManager dm;
    auto fid = dm.create_file(path);
    ASSERT_TRUE(fid.has_value());
    ASSERT_TRUE(dm.close_file(*fid).has_value());

    // Rewrite the version field to 1 (validation precedes the checksum).
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        uint32_t v1 = 1;
        f.seekp(static_cast<std::streamoff>(fh_version_offset));
        f.write(reinterpret_cast<const char*>(&v1), sizeof(uint32_t));
        ASSERT_TRUE(f.good());
    }

    auto open_result = dm.open_file(path);
    ASSERT_FALSE(open_result.has_value());
    EXPECT_EQ(open_result.error().code, StatusCode::IO_ERROR);
    EXPECT_NE(open_result.error().message.find("rebuild required"), std::string::npos)
        << open_result.error().message;
}

// =============================================================================
// AC 2: WAL records carry header changes for redo
// =============================================================================

class QA_GDB714_WalRedo : public ::testing::Test {
protected:
    static constexpr uint32_t kTableId = 99;

    void SetUp() override {
        base_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb714_wal";
        std::filesystem::remove_all(base_dir_);
        std::filesystem::create_directories(base_dir_);
        wal_dir_ = base_dir_ / "wal";

        auto fid = dm_.create_file(base_dir_ / "primary.db", false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        primary_fid_ = *fid;
        primary_bpm_ = std::make_unique<BufferPoolManager>(dm_, primary_fid_, 64);
        primary_heap_ = std::make_unique<TableHeap>(
            *primary_bpm_, dm_, primary_fid_, TableHeapOptions{.mvcc_headers = true});

        WalWriterOptions opts;
        opts.enable_group_commit = false;
        wal_ = std::make_unique<WalWriter>(wal_dir_, opts);
        ASSERT_TRUE(wal_->open().has_value());
        primary_heap_->attach_wal(wal_.get(), kTableId);
    }

    void TearDown() override {
        if (wal_) {
            (void)wal_->close();
        }
        recovered_heap_.reset();
        recovered_bpm_.reset();
        primary_heap_.reset();
        primary_bpm_.reset();
        std::error_code ec;
        std::filesystem::remove_all(base_dir_, ec);
    }

    void close_wal() {
        if (!wal_) {
            return;
        }
        ASSERT_TRUE(wal_->flush().has_value());
        ASSERT_TRUE(wal_->close().has_value());
        wal_.reset();
        primary_heap_->attach_wal(nullptr, 0);
    }

    /// Simulate a crash where data pages never reached disk: recovery replays
    /// the WAL into a brand-new empty file.
    void make_recovery_target() {
        auto fid = dm_.create_file(base_dir_ / "recovered.db", false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        recovered_fid_ = *fid;
        recovered_bpm_ = std::make_unique<BufferPoolManager>(dm_, recovered_fid_, 64);
        recovered_heap_ = std::make_unique<TableHeap>(
            *recovered_bpm_, dm_, recovered_fid_, TableHeapOptions{.mvcc_headers = true});
    }

    /// Raw on-page image of a tuple (header + user data), or empty if dead.
    std::vector<uint8_t> raw_image(BufferPoolManager& bpm, RID rid) {
        auto page_result = bpm.fetch_page(rid.page_id);
        if (!page_result) {
            return {};
        }
        auto raw = (*page_result)->get_tuple(rid.slot_id);
        (void)bpm.unpin_page(rid.page_id, false);
        return raw ? std::move(*raw) : std::vector<uint8_t>{};
    }

    std::filesystem::path base_dir_;
    std::filesystem::path wal_dir_;
    DiskManager dm_;
    FileId primary_fid_ = 0;
    FileId recovered_fid_ = 0;
    std::unique_ptr<BufferPoolManager> primary_bpm_;
    std::unique_ptr<BufferPoolManager> recovered_bpm_;
    std::unique_ptr<TableHeap> primary_heap_;
    std::unique_ptr<TableHeap> recovered_heap_;
    std::unique_ptr<WalWriter> wal_;
};

TEST_F(QA_GDB714_WalRedo, RedoReconstructsIdenticalOnPageImages) {
    std::vector<RID> rids;
    for (uint8_t i = 1; i <= 4; ++i) {
        auto rid = primary_heap_->insert_tuple(qa_bytes(64, i));
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
        rids.push_back(*rid);
    }
    ASSERT_TRUE(primary_heap_->update_tuple(rids[1], qa_bytes(80, 0x55)).has_value());
    ASSERT_TRUE(primary_heap_->delete_tuple(rids[2], /*xmax=*/frozen_txn_id).has_value());

    close_wal();
    make_recovery_target();

    TableHeapRecoveryHandler handler;
    handler.register_table(kTableId, recovered_heap_.get());
    WalRecovery recovery(wal_dir_, handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;
    EXPECT_EQ(stats->records_redone, 6u); // 4 inserts + 1 update + 1 delete.

    // Byte-identical raw images (MVCC header included) for live tuples; the
    // deleted slot stays dead.
    for (size_t i = 0; i < rids.size(); ++i) {
        auto expected = raw_image(*primary_bpm_, rids[i]);
        auto actual = raw_image(*recovered_bpm_, rids[i]);
        EXPECT_EQ(actual, expected) << "image mismatch at rid index " << i;
    }
    EXPECT_TRUE(raw_image(*recovered_bpm_, rids[2]).empty());
    EXPECT_EQ(recovered_heap_->row_count(), 3u);

    // Headers visible through the heap API after recovery.
    auto header = recovered_heap_->get_tuple_header(rids[0]);
    ASSERT_TRUE(header.has_value()) << header.error().message;
    EXPECT_EQ(header->xmin, frozen_txn_id);
    EXPECT_EQ(header->xmax, invalid_txn_id);
}

TEST_F(QA_GDB714_WalRedo, DeleteRecordCarriesStampedXmaxForRedo) {
    auto rid = primary_heap_->insert_tuple(qa_bytes(48, 0xAB), /*xmin=*/12);
    ASSERT_TRUE(rid.has_value());
    ASSERT_TRUE(primary_heap_->delete_tuple(*rid, /*xmax=*/34).has_value());
    close_wal();

    // Inspect the raw WAL stream: the DELETE record must carry the tuple
    // image with xmax stamped — the redo-able header mutation.
    WalReader reader(wal_dir_);
    ASSERT_TRUE(reader.open().has_value());
    std::vector<WalRecord> records;
    while (true) {
        auto record = reader.next();
        if (!record) {
            break;
        }
        records.push_back(std::move(*record));
    }
    ASSERT_TRUE(reader.close().has_value());

    ASSERT_EQ(records.size(), 2u);
    ASSERT_EQ(records[1].type, WalRecordType::DELETE);
    EXPECT_EQ(records[1].table_id, kTableId);
    EXPECT_EQ(records[1].page_id, rid->page_id);
    EXPECT_EQ(records[1].slot_id, rid->slot_id);

    auto payload = deserialize_table_wal_payload(records[1].data);
    ASSERT_TRUE(payload.has_value()) << payload.error().message;
    ASSERT_GE(payload->before_image.size(), mvcc_header_size);
    auto header = read_mvcc_header(payload->before_image);
    EXPECT_EQ(header.xmin, 12u);
    EXPECT_EQ(header.xmax, 34u);
    EXPECT_TRUE(payload->after_image.empty());
}

TEST_F(QA_GDB714_WalRedo, RedoIsIdempotentWhenPagesAlreadyFlushed) {
    auto r1 = primary_heap_->insert_tuple(qa_bytes(32, 0x01));
    auto r2 = primary_heap_->insert_tuple(qa_bytes(32, 0x02));
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(primary_heap_->delete_tuple(*r2).has_value());
    close_wal();

    // Pages were never lost — replay over the already-applied state.
    TableHeapRecoveryHandler handler;
    handler.register_table(kTableId, primary_heap_.get());
    WalRecovery recovery(wal_dir_, handler);
    ASSERT_TRUE(recovery.recover().has_value());
    ASSERT_TRUE(WalRecovery(wal_dir_, handler).recover().has_value());

    auto live = primary_heap_->get_tuple(*r1);
    ASSERT_TRUE(live.has_value()) << live.error().message;
    EXPECT_EQ(*live, qa_bytes(32, 0x01));
    EXPECT_FALSE(primary_heap_->get_tuple(*r2).has_value());
    EXPECT_EQ(primary_heap_->row_count(), 1u);
}
