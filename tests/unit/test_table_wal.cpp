/// Unit tests for table-heap WAL payloads and the TableHeapRecoveryHandler
/// (GDB-714 / GDB-736): tuple mutations logged by TableHeap carry full
/// on-page images (MVCC header included) so redo reconstructs identical
/// pages, and xmax stamping on delete is redo-able.

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
#include <memory>
#include <vector>

using namespace sixseven;

namespace {

std::vector<uint8_t> make_bytes(size_t size, uint8_t fill) {
    return std::vector<uint8_t>(size, fill);
}

WalWriterOptions test_wal_opts() {
    WalWriterOptions opts;
    opts.enable_group_commit = false; // Deterministic flushes in tests.
    return opts;
}

} // namespace

// =============================================================================
// Payload serialization
// =============================================================================

TEST(TableWalPayload, RoundtripInsertShape) {
    auto after = make_bytes(40, 0xAB);
    auto buf = serialize_table_wal_payload({}, after);

    auto payload = deserialize_table_wal_payload(buf);
    ASSERT_TRUE(payload.has_value()) << payload.error().message;
    EXPECT_TRUE(payload->before_image.empty());
    EXPECT_EQ(payload->after_image, after);
}

TEST(TableWalPayload, RoundtripUpdateShape) {
    auto before = make_bytes(32, 0x11);
    auto after = make_bytes(64, 0x22);
    auto buf = serialize_table_wal_payload(before, after);

    auto payload = deserialize_table_wal_payload(buf);
    ASSERT_TRUE(payload.has_value()) << payload.error().message;
    EXPECT_EQ(payload->before_image, before);
    EXPECT_EQ(payload->after_image, after);
}

TEST(TableWalPayload, RoundtripDeleteShape) {
    auto before = make_bytes(28, 0x33);
    auto buf = serialize_table_wal_payload(before, {});

    auto payload = deserialize_table_wal_payload(buf);
    ASSERT_TRUE(payload.has_value()) << payload.error().message;
    EXPECT_EQ(payload->before_image, before);
    EXPECT_TRUE(payload->after_image.empty());
}

TEST(TableWalPayload, RoundtripStructOverload) {
    TableWalPayload in;
    in.before_image = make_bytes(8, 0x44);
    in.after_image = make_bytes(16, 0x55);
    auto buf = serialize_table_wal_payload(in);

    auto out = deserialize_table_wal_payload(buf);
    ASSERT_TRUE(out.has_value()) << out.error().message;
    EXPECT_EQ(out->before_image, in.before_image);
    EXPECT_EQ(out->after_image, in.after_image);
}

TEST(TableWalPayload, EmptyBothImages) {
    auto buf = serialize_table_wal_payload(std::span<const uint8_t>{}, std::span<const uint8_t>{});
    EXPECT_EQ(buf.size(), 2 * sizeof(uint32_t));

    auto payload = deserialize_table_wal_payload(buf);
    ASSERT_TRUE(payload.has_value()) << payload.error().message;
    EXPECT_TRUE(payload->before_image.empty());
    EXPECT_TRUE(payload->after_image.empty());
}

TEST(TableWalPayload, TruncatedLengthFieldRejected) {
    std::vector<uint8_t> buf = {0x01, 0x02};
    auto payload = deserialize_table_wal_payload(buf);
    ASSERT_FALSE(payload.has_value());
    EXPECT_EQ(payload.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(TableWalPayload, TruncatedImageBytesRejected) {
    auto buf = serialize_table_wal_payload(make_bytes(32, 0x66), make_bytes(8, 0x77));
    buf.resize(buf.size() - 4); // Chop off part of the after-image.
    auto payload = deserialize_table_wal_payload(buf);
    ASSERT_FALSE(payload.has_value());
    EXPECT_EQ(payload.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(TableWalPayload, TrailingBytesRejected) {
    auto buf = serialize_table_wal_payload(make_bytes(4, 0x88), make_bytes(4, 0x99));
    buf.push_back(0xFF);
    auto payload = deserialize_table_wal_payload(buf);
    ASSERT_FALSE(payload.has_value());
    EXPECT_EQ(payload.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// Recovery fixture: MVCC heap with attached WAL + a second "recovered" heap
// =============================================================================

class TableWalRecoveryTest : public ::testing::Test {
protected:
    static constexpr uint32_t kTableId = 7;

    void SetUp() override {
        base_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_table_wal";
        std::filesystem::remove_all(base_dir_);
        std::filesystem::create_directories(base_dir_);
        wal_dir_ = base_dir_ / "wal";

        auto fid = dm_.create_file(base_dir_ / "primary.db", false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        primary_fid_ = *fid;
        primary_bpm_ = std::make_unique<BufferPoolManager>(dm_, primary_fid_, 64);
        primary_heap_ = std::make_unique<TableHeap>(
            *primary_bpm_, dm_, primary_fid_, TableHeapOptions{.mvcc_headers = true});

        wal_ = std::make_unique<WalWriter>(wal_dir_, test_wal_opts());
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

    /// Flush and close the WAL, detaching it from the primary heap so the
    /// heap never touches a destroyed writer.
    void close_wal() {
        if (!wal_) {
            return;
        }
        ASSERT_TRUE(wal_->flush().has_value());
        ASSERT_TRUE(wal_->close().has_value());
        wal_.reset();
        primary_heap_->attach_wal(nullptr, 0);
    }

    /// Flush + close the WAL ("crash") and create a fresh, empty heap that
    /// recovery will replay into.
    void crash_and_make_recovery_target() {
        close_wal();

        auto fid = dm_.create_file(base_dir_ / "recovered.db", false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        recovered_fid_ = *fid;
        recovered_bpm_ = std::make_unique<BufferPoolManager>(dm_, recovered_fid_, 64);
        recovered_heap_ = std::make_unique<TableHeap>(
            *recovered_bpm_, dm_, recovered_fid_, TableHeapOptions{.mvcc_headers = true});
    }

    /// Run full WAL recovery into the given heap.
    RecoveryStats recover_into(TableHeap& heap) {
        TableHeapRecoveryHandler handler;
        handler.register_table(kTableId, &heap);
        WalRecovery recovery(wal_dir_, handler);
        auto stats = recovery.recover();
        EXPECT_TRUE(stats.has_value()) << (stats ? "" : stats.error().message);
        return stats ? *stats : RecoveryStats{};
    }

    /// Read all WAL records (analysis-free), e.g. to inspect payloads.
    /// Closes the writer first so the reader never races an open segment.
    std::vector<WalRecord> read_wal_records() {
        close_wal();
        std::vector<WalRecord> records;
        WalReader reader(wal_dir_);
        EXPECT_TRUE(reader.open().has_value());
        while (true) {
            auto record = reader.next();
            if (!record) {
                break;
            }
            records.push_back(std::move(*record));
        }
        (void)reader.close();
        return records;
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

// =============================================================================
// Redo: full crash-recovery loop (frozen autocommit records)
// =============================================================================

TEST_F(TableWalRecoveryTest, RedoRecreatesInsertedTuplesWithHeaders) {
    std::vector<RID> rids;
    for (uint8_t i = 1; i <= 3; ++i) {
        auto rid = primary_heap_->insert_tuple(make_bytes(50, i));
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
        rids.push_back(*rid);
    }

    crash_and_make_recovery_target();
    auto stats = recover_into(*recovered_heap_);
    EXPECT_EQ(stats.records_redone, 3u);

    for (uint8_t i = 1; i <= 3; ++i) {
        auto data = recovered_heap_->get_tuple(rids[i - 1u]);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        EXPECT_EQ(*data, make_bytes(50, i));

        auto header = recovered_heap_->get_tuple_header(rids[i - 1u]);
        ASSERT_TRUE(header.has_value()) << header.error().message;
        EXPECT_EQ(header->xmin, frozen_txn_id);
        EXPECT_EQ(header->xmax, invalid_txn_id);
    }
    EXPECT_EQ(recovered_heap_->row_count(), 3u);
}

TEST_F(TableWalRecoveryTest, RedoReplaysDelete) {
    auto r1 = primary_heap_->insert_tuple(make_bytes(40, 0x01));
    auto r2 = primary_heap_->insert_tuple(make_bytes(40, 0x02));
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(primary_heap_->delete_tuple(*r1, /*xmax=*/frozen_txn_id).has_value());

    crash_and_make_recovery_target();
    recover_into(*recovered_heap_);

    auto deleted = recovered_heap_->get_tuple(*r1);
    EXPECT_FALSE(deleted.has_value());

    auto live = recovered_heap_->get_tuple(*r2);
    ASSERT_TRUE(live.has_value()) << live.error().message;
    EXPECT_EQ(*live, make_bytes(40, 0x02));
    EXPECT_EQ(recovered_heap_->row_count(), 1u);
}

TEST_F(TableWalRecoveryTest, RedoReplaysUpdatePreservingHeader) {
    auto rid = primary_heap_->insert_tuple(make_bytes(30, 0xAA));
    ASSERT_TRUE(rid.has_value());
    ASSERT_TRUE(primary_heap_->update_tuple(*rid, make_bytes(60, 0xBB)).has_value());

    crash_and_make_recovery_target();
    recover_into(*recovered_heap_);

    auto data = recovered_heap_->get_tuple(*rid);
    ASSERT_TRUE(data.has_value()) << data.error().message;
    EXPECT_EQ(*data, make_bytes(60, 0xBB));

    auto header = recovered_heap_->get_tuple_header(*rid);
    ASSERT_TRUE(header.has_value()) << header.error().message;
    EXPECT_EQ(header->xmin, frozen_txn_id);
    EXPECT_EQ(header->xmax, invalid_txn_id);
}

TEST_F(TableWalRecoveryTest, RedoIsIdempotentOnAlreadyAppliedState) {
    auto r1 = primary_heap_->insert_tuple(make_bytes(20, 0x0A));
    auto r2 = primary_heap_->insert_tuple(make_bytes(20, 0x0B));
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(primary_heap_->delete_tuple(*r2).has_value());

    close_wal();

    // Replay onto the primary heap itself: pages already reflect every
    // record. State must be unchanged.
    recover_into(*primary_heap_);
    recover_into(*primary_heap_);

    auto live = primary_heap_->get_tuple(*r1);
    ASSERT_TRUE(live.has_value()) << live.error().message;
    EXPECT_EQ(*live, make_bytes(20, 0x0A));
    EXPECT_FALSE(primary_heap_->get_tuple(*r2).has_value());
    EXPECT_EQ(primary_heap_->row_count(), 1u);
}

TEST_F(TableWalRecoveryTest, ExplicitTxnRecordsRedoneOnlyWithCommit) {
    // Records stamped with a real txn id are only redone if the WAL also
    // contains a COMMIT for that transaction (ARIES semantics).
    constexpr txn_id_t kTxn = 42;
    auto rid = primary_heap_->insert_tuple(make_bytes(25, 0xCC), /*xmin=*/kTxn);
    ASSERT_TRUE(rid.has_value());

    WalRecord commit;
    commit.type = WalRecordType::COMMIT;
    commit.txn_id = kTxn;
    ASSERT_TRUE(wal_->append(commit).has_value());

    crash_and_make_recovery_target();
    auto stats = recover_into(*recovered_heap_);
    EXPECT_EQ(stats.records_redone, 1u);

    auto header = recovered_heap_->get_tuple_header(*rid);
    ASSERT_TRUE(header.has_value()) << header.error().message;
    EXPECT_EQ(header->xmin, kTxn);
}

TEST_F(TableWalRecoveryTest, UncommittedExplicitTxnIsUndoneNotRedone) {
    constexpr txn_id_t kTxn = 43;
    auto rid = primary_heap_->insert_tuple(make_bytes(25, 0xCD), /*xmin=*/kTxn);
    ASSERT_TRUE(rid.has_value());

    crash_and_make_recovery_target();
    auto stats = recover_into(*recovered_heap_);
    EXPECT_EQ(stats.records_redone, 0u);
    EXPECT_EQ(stats.records_undone, 1u);
    EXPECT_FALSE(recovered_heap_->get_tuple(*rid).has_value());
    EXPECT_EQ(recovered_heap_->row_count(), 0u);
}

// =============================================================================
// WAL record payload contents (AC2: records carry header changes)
// =============================================================================

TEST_F(TableWalRecoveryTest, InsertRecordCarriesFullImageWithHeader) {
    auto rid = primary_heap_->insert_tuple(make_bytes(32, 0xEE), /*xmin=*/77);
    ASSERT_TRUE(rid.has_value());
    ASSERT_TRUE(wal_->flush().has_value());

    auto records = read_wal_records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].type, WalRecordType::INSERT);
    EXPECT_EQ(records[0].table_id, kTableId);
    EXPECT_EQ(records[0].page_id, rid->page_id);
    EXPECT_EQ(records[0].slot_id, rid->slot_id);

    auto payload = deserialize_table_wal_payload(records[0].data);
    ASSERT_TRUE(payload.has_value()) << payload.error().message;
    EXPECT_TRUE(payload->before_image.empty());
    ASSERT_EQ(payload->after_image.size(), mvcc_header_size + 32);

    auto header = read_mvcc_header(payload->after_image);
    EXPECT_EQ(header.xmin, 77u);
    EXPECT_EQ(header.xmax, invalid_txn_id);
}

TEST_F(TableWalRecoveryTest, DeleteRecordCarriesStampedXmax) {
    auto rid = primary_heap_->insert_tuple(make_bytes(32, 0xEF), /*xmin=*/77);
    ASSERT_TRUE(rid.has_value());
    ASSERT_TRUE(primary_heap_->delete_tuple(*rid, /*xmax=*/99).has_value());
    ASSERT_TRUE(wal_->flush().has_value());

    auto records = read_wal_records();
    ASSERT_EQ(records.size(), 2u);
    ASSERT_EQ(records[1].type, WalRecordType::DELETE);
    EXPECT_EQ(records[1].txn_id, 99u);

    auto payload = deserialize_table_wal_payload(records[1].data);
    ASSERT_TRUE(payload.has_value()) << payload.error().message;
    EXPECT_TRUE(payload->after_image.empty());
    ASSERT_GE(payload->before_image.size(), mvcc_header_size);

    auto header = read_mvcc_header(payload->before_image);
    EXPECT_EQ(header.xmin, 77u);
    EXPECT_EQ(header.xmax, 99u); // The redo-able header mutation.
}

TEST_F(TableWalRecoveryTest, UpdateRecordCarriesBeforeAndAfterImages) {
    auto rid = primary_heap_->insert_tuple(make_bytes(16, 0x10), /*xmin=*/5);
    ASSERT_TRUE(rid.has_value());
    ASSERT_TRUE(primary_heap_->update_tuple(*rid, make_bytes(24, 0x20)).has_value());
    ASSERT_TRUE(wal_->flush().has_value());

    auto records = read_wal_records();
    ASSERT_EQ(records.size(), 2u);
    ASSERT_EQ(records[1].type, WalRecordType::UPDATE);

    auto payload = deserialize_table_wal_payload(records[1].data);
    ASSERT_TRUE(payload.has_value()) << payload.error().message;
    ASSERT_EQ(payload->before_image.size(), mvcc_header_size + 16);
    ASSERT_EQ(payload->after_image.size(), mvcc_header_size + 24);
    // Both images carry the preserved header.
    EXPECT_EQ(read_mvcc_header(payload->before_image).xmin, 5u);
    EXPECT_EQ(read_mvcc_header(payload->after_image).xmin, 5u);
}

// =============================================================================
// Undo
// =============================================================================

TEST_F(TableWalRecoveryTest, UndoDeleteRestoresTupleWithXmaxCleared) {
    auto rid = primary_heap_->insert_tuple(make_bytes(32, 0x31), /*xmin=*/7);
    ASSERT_TRUE(rid.has_value());
    ASSERT_TRUE(primary_heap_->delete_tuple(*rid, /*xmax=*/88).has_value());
    ASSERT_TRUE(wal_->flush().has_value());

    auto records = read_wal_records();
    ASSERT_EQ(records.size(), 2u);

    TableHeapRecoveryHandler handler;
    handler.register_table(kTableId, primary_heap_.get());
    ASSERT_TRUE(handler.undo(records[1]).has_value());

    auto data = primary_heap_->get_tuple(*rid);
    ASSERT_TRUE(data.has_value()) << data.error().message;
    EXPECT_EQ(*data, make_bytes(32, 0x31));

    auto header = primary_heap_->get_tuple_header(*rid);
    ASSERT_TRUE(header.has_value()) << header.error().message;
    EXPECT_EQ(header->xmin, 7u);
    EXPECT_EQ(header->xmax, invalid_txn_id); // Stamp rolled back.
    EXPECT_EQ(primary_heap_->row_count(), 1u);
}

TEST_F(TableWalRecoveryTest, UndoInsertRemovesTuple) {
    auto rid = primary_heap_->insert_tuple(make_bytes(32, 0x41));
    ASSERT_TRUE(rid.has_value());
    ASSERT_TRUE(wal_->flush().has_value());

    auto records = read_wal_records();
    ASSERT_EQ(records.size(), 1u);

    TableHeapRecoveryHandler handler;
    handler.register_table(kTableId, primary_heap_.get());
    ASSERT_TRUE(handler.undo(records[0]).has_value());

    EXPECT_FALSE(primary_heap_->get_tuple(*rid).has_value());
    EXPECT_EQ(primary_heap_->row_count(), 0u);

    // Undo is idempotent.
    ASSERT_TRUE(handler.undo(records[0]).has_value());
    EXPECT_EQ(primary_heap_->row_count(), 0u);
}

TEST_F(TableWalRecoveryTest, UndoUpdateRestoresBeforeImage) {
    auto rid = primary_heap_->insert_tuple(make_bytes(16, 0x51), /*xmin=*/3);
    ASSERT_TRUE(rid.has_value());
    ASSERT_TRUE(primary_heap_->update_tuple(*rid, make_bytes(48, 0x52)).has_value());
    ASSERT_TRUE(wal_->flush().has_value());

    auto records = read_wal_records();
    ASSERT_EQ(records.size(), 2u);

    TableHeapRecoveryHandler handler;
    handler.register_table(kTableId, primary_heap_.get());
    ASSERT_TRUE(handler.undo(records[1]).has_value());

    auto data = primary_heap_->get_tuple(*rid);
    ASSERT_TRUE(data.has_value()) << data.error().message;
    EXPECT_EQ(*data, make_bytes(16, 0x51));
    EXPECT_EQ(primary_heap_->get_tuple_header(*rid)->xmin, 3u);
}

// =============================================================================
// Handler routing
// =============================================================================

TEST_F(TableWalRecoveryTest, UnregisteredTableIdIsSkipped) {
    auto rid = primary_heap_->insert_tuple(make_bytes(8, 0x61));
    ASSERT_TRUE(rid.has_value());
    ASSERT_TRUE(wal_->flush().has_value());

    auto records = read_wal_records();
    ASSERT_EQ(records.size(), 1u);

    TableHeapRecoveryHandler handler; // Nothing registered.
    EXPECT_TRUE(handler.redo(records[0]).has_value());
    EXPECT_TRUE(handler.undo(records[0]).has_value());
}

TEST_F(TableWalRecoveryTest, RedoWithMissingAfterImageRejected) {
    TableHeapRecoveryHandler handler;
    handler.register_table(kTableId, primary_heap_.get());

    WalRecord bad;
    bad.type = WalRecordType::INSERT;
    bad.table_id = kTableId;
    bad.page_id = 1;
    bad.slot_id = 0;
    bad.data = serialize_table_wal_payload(std::span<const uint8_t>{}, std::span<const uint8_t>{});
    auto result = handler.redo(bad);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}
