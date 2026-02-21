#include "giodb/storage/wal.h"
#include "giodb/storage/wal_record.h"
#include "giodb/storage/wal_recovery.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace giodb {
namespace {

// -- Helpers ------------------------------------------------------------------

/// Temporary WAL directory with automatic cleanup.
class TempWalDir {
public:
    TempWalDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("wal_recovery_test_" + std::to_string(counter_++));
        std::filesystem::create_directories(path_);
    }
    ~TempWalDir() { std::filesystem::remove_all(path_); }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    static inline std::atomic<int> counter_{0};
};

/// Simple recovery handler that records redo/undo calls for verification.
class TestRecoveryHandler : public RecoveryHandler {
public:
    struct Entry {
        lsn_t lsn;
        txn_id_t txn_id;
        WalRecordType type;
        std::vector<uint8_t> data;
    };

    Result<void> redo(const WalRecord& record) override {
        redo_entries.push_back({record.lsn, record.txn_id, record.type, record.data});
        return ok();
    }

    Result<void> undo(const WalRecord& record) override {
        undo_entries.push_back({record.lsn, record.txn_id, record.type, record.data});
        return ok();
    }

    std::vector<Entry> redo_entries;
    std::vector<Entry> undo_entries;
};

/// Create writer options with group commit disabled for deterministic tests.
WalWriterOptions test_opts() {
    WalWriterOptions opts;
    opts.enable_group_commit = false;
    return opts;
}

/// Write a simple transaction to the WAL: BEGIN, INSERT, COMMIT.
void write_committed_txn(WalWriter& writer,
                         txn_id_t txn_id,
                         uint32_t table_id,
                         const std::string& data) {
    WalRecord begin;
    begin.type = WalRecordType::BEGIN;
    begin.txn_id = txn_id;
    ASSERT_TRUE(writer.append(begin).has_value());

    WalRecord insert;
    insert.type = WalRecordType::INSERT;
    insert.txn_id = txn_id;
    insert.table_id = table_id;
    insert.data.assign(data.begin(), data.end());
    ASSERT_TRUE(writer.append(insert).has_value());

    WalRecord commit;
    commit.type = WalRecordType::COMMIT;
    commit.txn_id = txn_id;
    ASSERT_TRUE(writer.append(commit).has_value());
}

/// Write an aborted transaction to the WAL: BEGIN, INSERT, ABORT.
void write_aborted_txn(WalWriter& writer,
                       txn_id_t txn_id,
                       uint32_t table_id,
                       const std::string& data) {
    WalRecord begin;
    begin.type = WalRecordType::BEGIN;
    begin.txn_id = txn_id;
    ASSERT_TRUE(writer.append(begin).has_value());

    WalRecord insert;
    insert.type = WalRecordType::INSERT;
    insert.txn_id = txn_id;
    insert.table_id = table_id;
    insert.data.assign(data.begin(), data.end());
    ASSERT_TRUE(writer.append(insert).has_value());

    WalRecord abort;
    abort.type = WalRecordType::ABORT;
    abort.txn_id = txn_id;
    ASSERT_TRUE(writer.append(abort).has_value());
}

// -- WalReader tests ----------------------------------------------------------

TEST(WalReader, ReadAllRecords) {
    TempWalDir dir;

    // Write some records.
    {
        WalWriter writer(dir.path(), test_opts());
        ASSERT_TRUE(writer.open().has_value());
        write_committed_txn(writer, 1, 10, "data1");
        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    // Read them back.
    WalReader reader(dir.path());
    ASSERT_TRUE(reader.open().has_value());

    std::vector<WalRecord> records;
    while (true) {
        auto result = reader.next();
        if (!result) {
            EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
            break;
        }
        records.push_back(std::move(*result));
    }

    ASSERT_TRUE(reader.close().has_value());

    ASSERT_EQ(records.size(), 3u); // BEGIN, INSERT, COMMIT
    EXPECT_EQ(records[0].type, WalRecordType::BEGIN);
    EXPECT_EQ(records[0].txn_id, 1u);
    EXPECT_EQ(records[1].type, WalRecordType::INSERT);
    EXPECT_EQ(records[1].table_id, 10u);
    EXPECT_EQ(records[2].type, WalRecordType::COMMIT);
}

TEST(WalReader, ReadAcrossMultipleSegments) {
    TempWalDir dir;
    WalWriterOptions opts = test_opts();
    opts.segment_size = 128; // Tiny segment to force rotation.

    int total_records = 0;
    {
        WalWriter writer(dir.path(), opts);
        ASSERT_TRUE(writer.open().has_value());
        for (int i = 0; i < 20; ++i) {
            WalRecord r;
            r.type = WalRecordType::INSERT;
            r.txn_id = 1;
            r.table_id = 10;
            r.data = {0x01, 0x02, 0x03};
            ASSERT_TRUE(writer.append(r).has_value());
            ++total_records;
        }
        ASSERT_TRUE(writer.flush().has_value());
        EXPECT_GT(writer.current_segment_id(), 1u); // Multiple segments.
        ASSERT_TRUE(writer.close().has_value());
    }

    // Read all records.
    WalReader reader(dir.path());
    ASSERT_TRUE(reader.open().has_value());

    int read_count = 0;
    while (true) {
        auto result = reader.next();
        if (!result) {
            break;
        }
        ++read_count;
        EXPECT_EQ(result->type, WalRecordType::INSERT);
    }

    ASSERT_TRUE(reader.close().has_value());
    EXPECT_EQ(read_count, total_records);
}

TEST(WalReader, ReadEmptyWalDir) {
    TempWalDir dir;
    // Create no segments.

    WalReader reader(dir.path());
    ASSERT_TRUE(reader.open().has_value());

    auto result = reader.next();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);

    ASSERT_TRUE(reader.close().has_value());
}

TEST(WalReader, ReadFromSpecificSegment) {
    TempWalDir dir;
    WalWriterOptions opts = test_opts();
    opts.segment_size = 128; // Tiny.

    {
        WalWriter writer(dir.path(), opts);
        ASSERT_TRUE(writer.open().has_value());
        for (int i = 0; i < 20; ++i) {
            WalRecord r;
            r.type = WalRecordType::INSERT;
            r.txn_id = 1;
            r.data = {0x01, 0x02};
            ASSERT_TRUE(writer.append(r).has_value());
        }
        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    // Read starting from segment 2 — should get fewer records.
    WalReader reader_all(dir.path());
    ASSERT_TRUE(reader_all.open(0).has_value());
    int all_count = 0;
    while (reader_all.next().has_value()) {
        ++all_count;
    }
    ASSERT_TRUE(reader_all.close().has_value());

    WalReader reader_partial(dir.path());
    ASSERT_TRUE(reader_partial.open(2).has_value());
    int partial_count = 0;
    while (reader_partial.next().has_value()) {
        ++partial_count;
    }
    ASSERT_TRUE(reader_partial.close().has_value());

    EXPECT_GT(all_count, partial_count);
    EXPECT_GT(partial_count, 0);
}

// -- Checkpoint helpers -------------------------------------------------------

TEST(CheckpointData, SerializeDeserializeEmpty) {
    std::vector<txn_id_t> txns;
    auto data = serialize_checkpoint_data(txns);

    auto result = deserialize_checkpoint_data(data);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

TEST(CheckpointData, SerializeDeserializeMultiple) {
    std::vector<txn_id_t> txns = {1, 42, 100, 999};
    auto data = serialize_checkpoint_data(txns);

    auto result = deserialize_checkpoint_data(data);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 4u);
    EXPECT_EQ((*result)[0], 1u);
    EXPECT_EQ((*result)[1], 42u);
    EXPECT_EQ((*result)[2], 100u);
    EXPECT_EQ((*result)[3], 999u);
}

TEST(CheckpointData, DeserializeTooSmall) {
    std::vector<uint8_t> data = {0x01}; // Too small.
    auto result = deserialize_checkpoint_data(data);
    EXPECT_FALSE(result.has_value());
}

TEST(CheckpointData, DeserializeTruncated) {
    // Say count=2, but only provide data for 1 txn.
    std::vector<uint8_t> data(sizeof(uint32_t) + sizeof(uint64_t));
    uint32_t count = 2;
    std::memcpy(data.data(), &count, sizeof(uint32_t));
    uint64_t txn = 42;
    std::memcpy(data.data() + sizeof(uint32_t), &txn, sizeof(uint64_t));

    auto result = deserialize_checkpoint_data(data);
    EXPECT_FALSE(result.has_value());
}

// -- WalWriter checkpoint tests -----------------------------------------------

TEST(WalWriter, WriteCheckpointRecord) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_opts());
    ASSERT_TRUE(writer.open().has_value());

    // Write some records.
    WalRecord r;
    r.type = WalRecordType::BEGIN;
    r.txn_id = 1;
    ASSERT_TRUE(writer.append(r).has_value());

    // Write checkpoint with one active transaction.
    auto ckpt_lsn = writer.write_checkpoint({1});
    ASSERT_TRUE(ckpt_lsn.has_value());
    EXPECT_EQ(*ckpt_lsn, 2u);

    ASSERT_TRUE(writer.flush().has_value());
    ASSERT_TRUE(writer.close().has_value());

    // Read back and verify the checkpoint record.
    WalReader reader(dir.path());
    ASSERT_TRUE(reader.open().has_value());

    auto r1 = reader.next();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->type, WalRecordType::BEGIN);

    auto r2 = reader.next();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->type, WalRecordType::CHECKPOINT);
    EXPECT_EQ(r2->lsn, 2u);

    // Decode checkpoint data.
    auto active = deserialize_checkpoint_data(r2->data);
    ASSERT_TRUE(active.has_value());
    ASSERT_EQ(active->size(), 1u);
    EXPECT_EQ((*active)[0], 1u);

    ASSERT_TRUE(reader.close().has_value());
}

TEST(WalWriter, TruncateBeforeRemovesOldSegments) {
    TempWalDir dir;
    WalWriterOptions opts = test_opts();
    opts.segment_size = 128; // Tiny.
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    // Write enough to create multiple segments.
    for (int i = 0; i < 20; ++i) {
        WalRecord r;
        r.type = WalRecordType::INSERT;
        r.txn_id = 1;
        r.data = {0x01, 0x02, 0x03};
        ASSERT_TRUE(writer.append(r).has_value());
    }

    uint64_t current_seg = writer.current_segment_id();
    EXPECT_GT(current_seg, 2u);

    // Truncate segments before segment 3.
    ASSERT_TRUE(writer.truncate_before(3).has_value());

    // Verify segments 1 and 2 are gone.
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "wal_000001"));
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "wal_000002"));

    // Segment 3 and later should still exist.
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "wal_000003"));

    ASSERT_TRUE(writer.close().has_value());
}

// -- WalRecovery tests --------------------------------------------------------

TEST(WalRecovery, RecoverCommittedTransaction) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_opts());
        ASSERT_TRUE(writer.open().has_value());
        write_committed_txn(writer, 1, 10, "committed-data");
        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    TestRecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->records_scanned, 3u); // BEGIN, INSERT, COMMIT
    EXPECT_EQ(stats->committed_txns, 1u);
    EXPECT_EQ(stats->aborted_txns, 0u);
    EXPECT_EQ(stats->records_redone, 1u); // Only INSERT is a data record.
    EXPECT_EQ(stats->records_undone, 0u);

    // Verify redo was called with the INSERT record.
    ASSERT_EQ(handler.redo_entries.size(), 1u);
    EXPECT_EQ(handler.redo_entries[0].txn_id, 1u);
    EXPECT_EQ(handler.redo_entries[0].type, WalRecordType::INSERT);

    EXPECT_TRUE(handler.undo_entries.empty());
}

TEST(WalRecovery, RecoverAbortedTransaction) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_opts());
        ASSERT_TRUE(writer.open().has_value());
        write_aborted_txn(writer, 1, 10, "aborted-data");
        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    TestRecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->committed_txns, 0u);
    EXPECT_EQ(stats->aborted_txns, 1u);
    EXPECT_EQ(stats->records_redone, 0u);
    EXPECT_EQ(stats->records_undone, 1u); // INSERT undone.

    EXPECT_TRUE(handler.redo_entries.empty());
    ASSERT_EQ(handler.undo_entries.size(), 1u);
    EXPECT_EQ(handler.undo_entries[0].txn_id, 1u);
    EXPECT_EQ(handler.undo_entries[0].type, WalRecordType::INSERT);
}

TEST(WalRecovery, RecoverInProgressTransaction) {
    // Simulates crash: BEGIN + INSERT but no COMMIT/ABORT.
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_opts());
        ASSERT_TRUE(writer.open().has_value());

        WalRecord begin;
        begin.type = WalRecordType::BEGIN;
        begin.txn_id = 1;
        ASSERT_TRUE(writer.append(begin).has_value());

        WalRecord insert;
        insert.type = WalRecordType::INSERT;
        insert.txn_id = 1;
        insert.table_id = 10;
        insert.data = {0x01, 0x02};
        ASSERT_TRUE(writer.append(insert).has_value());

        // No COMMIT or ABORT — simulates crash.
        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    TestRecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // In-progress transaction should be treated as aborted.
    EXPECT_EQ(stats->committed_txns, 0u);
    EXPECT_EQ(stats->aborted_txns, 1u);
    EXPECT_EQ(stats->records_redone, 0u);
    EXPECT_EQ(stats->records_undone, 1u);

    EXPECT_TRUE(handler.redo_entries.empty());
    ASSERT_EQ(handler.undo_entries.size(), 1u);
    EXPECT_EQ(handler.undo_entries[0].type, WalRecordType::INSERT);
}

TEST(WalRecovery, RecoverMixedTransactions) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_opts());
        ASSERT_TRUE(writer.open().has_value());

        // Txn 1: committed.
        write_committed_txn(writer, 1, 10, "committed");

        // Txn 2: aborted.
        write_aborted_txn(writer, 2, 20, "aborted");

        // Txn 3: in-progress (crash).
        WalRecord begin;
        begin.type = WalRecordType::BEGIN;
        begin.txn_id = 3;
        ASSERT_TRUE(writer.append(begin).has_value());

        WalRecord update;
        update.type = WalRecordType::UPDATE;
        update.txn_id = 3;
        update.table_id = 30;
        update.data = {0xAA, 0xBB};
        ASSERT_TRUE(writer.append(update).has_value());

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    TestRecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->committed_txns, 1u); // Txn 1
    EXPECT_EQ(stats->aborted_txns, 2u);   // Txn 2 + Txn 3
    EXPECT_EQ(stats->records_redone, 1u); // Txn 1 INSERT
    EXPECT_EQ(stats->records_undone, 2u); // Txn 2 INSERT + Txn 3 UPDATE

    // Redo: txn 1 INSERT.
    ASSERT_EQ(handler.redo_entries.size(), 1u);
    EXPECT_EQ(handler.redo_entries[0].txn_id, 1u);

    // Undo: in reverse order — txn 3 UPDATE first, then txn 2 INSERT.
    ASSERT_EQ(handler.undo_entries.size(), 2u);
    EXPECT_EQ(handler.undo_entries[0].txn_id, 3u);
    EXPECT_EQ(handler.undo_entries[0].type, WalRecordType::UPDATE);
    EXPECT_EQ(handler.undo_entries[1].txn_id, 2u);
    EXPECT_EQ(handler.undo_entries[1].type, WalRecordType::INSERT);
}

TEST(WalRecovery, RecoverMultipleCommittedTransactions) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_opts());
        ASSERT_TRUE(writer.open().has_value());
        write_committed_txn(writer, 1, 10, "data1");
        write_committed_txn(writer, 2, 20, "data2");
        write_committed_txn(writer, 3, 30, "data3");
        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    TestRecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->committed_txns, 3u);
    EXPECT_EQ(stats->aborted_txns, 0u);
    EXPECT_EQ(stats->records_redone, 3u); // 3 INSERTs.
    EXPECT_EQ(stats->records_undone, 0u);

    // Redo should be in forward order.
    ASSERT_EQ(handler.redo_entries.size(), 3u);
    EXPECT_EQ(handler.redo_entries[0].txn_id, 1u);
    EXPECT_EQ(handler.redo_entries[1].txn_id, 2u);
    EXPECT_EQ(handler.redo_entries[2].txn_id, 3u);
}

TEST(WalRecovery, RecoverWithCheckpoint) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_opts());
        ASSERT_TRUE(writer.open().has_value());

        // Txn 1: committed before checkpoint.
        write_committed_txn(writer, 1, 10, "before-checkpoint");

        // Checkpoint with no active transactions.
        ASSERT_TRUE(writer.write_checkpoint({}).has_value());

        // Txn 2: committed after checkpoint.
        write_committed_txn(writer, 2, 20, "after-checkpoint");

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    TestRecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_GT(stats->checkpoint_lsn, 0u);
    // Only txn 2 is after the checkpoint, so only it should be redone.
    // Txn 1 was committed before the checkpoint and should not be redone.
    EXPECT_EQ(stats->records_redone, 1u);
    EXPECT_EQ(stats->committed_txns, 1u);
    EXPECT_EQ(stats->aborted_txns, 0u);

    ASSERT_EQ(handler.redo_entries.size(), 1u);
    EXPECT_EQ(handler.redo_entries[0].txn_id, 2u);
}

TEST(WalRecovery, RecoverWithCheckpointAndActiveTransaction) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_opts());
        ASSERT_TRUE(writer.open().has_value());

        // Txn 1 starts before checkpoint.
        WalRecord begin;
        begin.type = WalRecordType::BEGIN;
        begin.txn_id = 1;
        ASSERT_TRUE(writer.append(begin).has_value());

        WalRecord insert_before;
        insert_before.type = WalRecordType::INSERT;
        insert_before.txn_id = 1;
        insert_before.table_id = 10;
        insert_before.data = {0x01};
        ASSERT_TRUE(writer.append(insert_before).has_value());

        // Checkpoint: txn 1 is active.
        ASSERT_TRUE(writer.write_checkpoint({1}).has_value());

        // Txn 1 writes more data after checkpoint, then commits.
        WalRecord insert_after;
        insert_after.type = WalRecordType::INSERT;
        insert_after.txn_id = 1;
        insert_after.table_id = 10;
        insert_after.data = {0x02};
        ASSERT_TRUE(writer.append(insert_after).has_value());

        WalRecord commit;
        commit.type = WalRecordType::COMMIT;
        commit.txn_id = 1;
        ASSERT_TRUE(writer.append(commit).has_value());

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    TestRecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // Txn 1 committed. The INSERT before the checkpoint is already applied,
    // so only the INSERT after the checkpoint should be redone.
    EXPECT_EQ(stats->committed_txns, 1u);
    EXPECT_EQ(stats->records_redone, 1u);
    EXPECT_EQ(stats->records_undone, 0u);

    ASSERT_EQ(handler.redo_entries.size(), 1u);
    EXPECT_EQ(handler.redo_entries[0].txn_id, 1u);
    EXPECT_EQ(handler.redo_entries[0].data.size(), 1u);
    EXPECT_EQ(handler.redo_entries[0].data[0], 0x02); // The after-checkpoint insert.
}

TEST(WalRecovery, RecoverEmptyWal) {
    TempWalDir dir;

    // Create WAL directory but write no records.
    {
        WalWriter writer(dir.path(), test_opts());
        ASSERT_TRUE(writer.open().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    TestRecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->records_scanned, 0u);
    EXPECT_EQ(stats->committed_txns, 0u);
    EXPECT_EQ(stats->aborted_txns, 0u);
    EXPECT_EQ(stats->records_redone, 0u);
    EXPECT_EQ(stats->records_undone, 0u);
}

TEST(WalRecovery, RecoverMultipleDataRecordTypes) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_opts());
        ASSERT_TRUE(writer.open().has_value());

        // Committed txn with INSERT, UPDATE, DELETE.
        WalRecord begin;
        begin.type = WalRecordType::BEGIN;
        begin.txn_id = 1;
        ASSERT_TRUE(writer.append(begin).has_value());

        WalRecord insert;
        insert.type = WalRecordType::INSERT;
        insert.txn_id = 1;
        insert.table_id = 10;
        ASSERT_TRUE(writer.append(insert).has_value());

        WalRecord update;
        update.type = WalRecordType::UPDATE;
        update.txn_id = 1;
        update.table_id = 10;
        ASSERT_TRUE(writer.append(update).has_value());

        WalRecord del;
        del.type = WalRecordType::DELETE;
        del.txn_id = 1;
        del.table_id = 10;
        ASSERT_TRUE(writer.append(del).has_value());

        WalRecord commit;
        commit.type = WalRecordType::COMMIT;
        commit.txn_id = 1;
        ASSERT_TRUE(writer.append(commit).has_value());

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    TestRecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->records_redone, 3u); // INSERT, UPDATE, DELETE

    ASSERT_EQ(handler.redo_entries.size(), 3u);
    EXPECT_EQ(handler.redo_entries[0].type, WalRecordType::INSERT);
    EXPECT_EQ(handler.redo_entries[1].type, WalRecordType::UPDATE);
    EXPECT_EQ(handler.redo_entries[2].type, WalRecordType::DELETE);
}

TEST(WalRecovery, UndoIsInReverseOrder) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_opts());
        ASSERT_TRUE(writer.open().has_value());

        // Aborted txn with multiple data records.
        WalRecord begin;
        begin.type = WalRecordType::BEGIN;
        begin.txn_id = 1;
        ASSERT_TRUE(writer.append(begin).has_value());

        WalRecord r1;
        r1.type = WalRecordType::INSERT;
        r1.txn_id = 1;
        r1.table_id = 10;
        ASSERT_TRUE(writer.append(r1).has_value());

        WalRecord r2;
        r2.type = WalRecordType::UPDATE;
        r2.txn_id = 1;
        r2.table_id = 10;
        ASSERT_TRUE(writer.append(r2).has_value());

        WalRecord r3;
        r3.type = WalRecordType::DELETE;
        r3.txn_id = 1;
        r3.table_id = 10;
        ASSERT_TRUE(writer.append(r3).has_value());

        // No COMMIT — in-progress, treated as aborted.
        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    TestRecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->records_undone, 3u);

    // Undo should be in reverse LSN order: DELETE, UPDATE, INSERT.
    ASSERT_EQ(handler.undo_entries.size(), 3u);
    EXPECT_EQ(handler.undo_entries[0].type, WalRecordType::DELETE);
    EXPECT_EQ(handler.undo_entries[1].type, WalRecordType::UPDATE);
    EXPECT_EQ(handler.undo_entries[2].type, WalRecordType::INSERT);
}

TEST(WalRecovery, RecoverAcrossMultipleSegments) {
    TempWalDir dir;
    WalWriterOptions opts = test_opts();
    opts.segment_size = 128; // Tiny to force rotations.

    {
        WalWriter writer(dir.path(), opts);
        ASSERT_TRUE(writer.open().has_value());
        write_committed_txn(writer, 1, 10, "segment-spanning-data-payload");
        write_committed_txn(writer, 2, 20, "another-segment-spanning-data");
        ASSERT_TRUE(writer.flush().has_value());
        EXPECT_GT(writer.current_segment_id(), 1u);
        ASSERT_TRUE(writer.close().has_value());
    }

    TestRecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->committed_txns, 2u);
    EXPECT_EQ(stats->records_redone, 2u); // 2 INSERTs.
}

TEST(WalRecovery, RecoverDataPreservedInCallbacks) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_opts());
        ASSERT_TRUE(writer.open().has_value());

        WalRecord begin;
        begin.type = WalRecordType::BEGIN;
        begin.txn_id = 42;
        ASSERT_TRUE(writer.append(begin).has_value());

        WalRecord insert;
        insert.type = WalRecordType::INSERT;
        insert.txn_id = 42;
        insert.table_id = 7;
        insert.page_id = 100;
        insert.slot_id = 3;
        insert.data = {0xDE, 0xAD, 0xBE, 0xEF};
        ASSERT_TRUE(writer.append(insert).has_value());

        WalRecord commit;
        commit.type = WalRecordType::COMMIT;
        commit.txn_id = 42;
        ASSERT_TRUE(writer.append(commit).has_value());

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    TestRecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    ASSERT_EQ(handler.redo_entries.size(), 1u);
    auto& entry = handler.redo_entries[0];
    EXPECT_EQ(entry.txn_id, 42u);
    EXPECT_EQ(entry.type, WalRecordType::INSERT);
    ASSERT_EQ(entry.data.size(), 4u);
    EXPECT_EQ(entry.data[0], 0xDE);
    EXPECT_EQ(entry.data[1], 0xAD);
    EXPECT_EQ(entry.data[2], 0xBE);
    EXPECT_EQ(entry.data[3], 0xEF);
}

TEST(WalRecovery, StatsMaxLsn) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_opts());
        ASSERT_TRUE(writer.open().has_value());
        write_committed_txn(writer, 1, 10, "data");
        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    TestRecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->max_lsn, 3u); // BEGIN=1, INSERT=2, COMMIT=3
}

} // namespace
} // namespace giodb
