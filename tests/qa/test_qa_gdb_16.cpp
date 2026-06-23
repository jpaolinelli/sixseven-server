#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_record.h"
#include "sixseven/storage/wal_recovery.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "test_wal_helpers.h"

namespace sixseven {
namespace {

using test::TempWalDir;
using test::test_wal_opts;
using test::write_aborted_txn;
using test::write_committed_txn;

// =============================================================================
// QA_WalRecord — Deserialization boundary / security tests
// =============================================================================

TEST(QA_WalRecord, DeserializeRecordLengthZeroCausesUnderflow) {
    // record_length = 0 in a 47-byte buffer.
    // crc_length = 0 - 4 = SIZE_MAX (underflow).
    // This must return an error, NOT crash.
    std::vector<uint8_t> buf(47, 0);
    uint32_t record_length = 0;
    std::memcpy(buf.data(), &record_length, sizeof(uint32_t));

    auto result = deserialize_wal_record(buf);
    // A zero record_length is caught by the scan loop (treated as end of data),
    // but deserialize_wal_record itself should handle it gracefully.
    // The current code underflows crc_length — this test exposes the bug.
    EXPECT_FALSE(result.has_value());
}

TEST(QA_WalRecord, DeserializeRecordLengthOneCausesUnderflow) {
    // record_length = 1 in a 47-byte buffer.
    std::vector<uint8_t> buf(47, 0);
    uint32_t record_length = 1;
    std::memcpy(buf.data(), &record_length, sizeof(uint32_t));

    auto result = deserialize_wal_record(buf);
    EXPECT_FALSE(result.has_value());
}

TEST(QA_WalRecord, DeserializeRecordLengthTwoCausesUnderflow) {
    // record_length = 2 in a 47-byte buffer.
    std::vector<uint8_t> buf(47, 0);
    uint32_t record_length = 2;
    std::memcpy(buf.data(), &record_length, sizeof(uint32_t));

    auto result = deserialize_wal_record(buf);
    EXPECT_FALSE(result.has_value());
}

TEST(QA_WalRecord, DeserializeRecordLengthThreeCausesUnderflow) {
    // record_length = 3 in a 47-byte buffer.
    // crc_length = 3 - 4 = SIZE_MAX (underflow).
    std::vector<uint8_t> buf(47, 0);
    uint32_t record_length = 3;
    std::memcpy(buf.data(), &record_length, sizeof(uint32_t));

    auto result = deserialize_wal_record(buf);
    EXPECT_FALSE(result.has_value());
}

TEST(QA_WalRecord, DeserializeRecordLengthExactlyFour) {
    // record_length = 4 is below the minimum valid value of 43
    // (header=39 + crc=4). GDB-889: the prod fix now rejects record_length
    // values that cannot hold a complete header+CRC, so this must FAIL.
    std::vector<uint8_t> buf(47, 0);
    uint32_t record_length = 4;
    std::memcpy(buf.data(), &record_length, sizeof(uint32_t));

    auto result = deserialize_wal_record(buf);
    // record_length=4 is now correctly REJECTED (too small for header+CRC).
    EXPECT_FALSE(result.has_value());
}

TEST(QA_WalRecord, DeserializeRecordLengthSmallerThanHeaderPlusCrc) {
    // record_length = 20 — too small for the full header (39) + crc (4).
    // Should fail gracefully.
    std::vector<uint8_t> buf(47, 0);
    uint32_t record_length = 20;
    std::memcpy(buf.data(), &record_length, sizeof(uint32_t));

    auto result = deserialize_wal_record(buf);
    EXPECT_FALSE(result.has_value());
}

TEST(QA_WalRecord, DeserializeRecordLengthFortyTwoRejected) {
    // GDB-889 boundary regression: record_length=42 is one below the minimum
    // (43 = header(39) + crc(4)) and must be REJECTED.
    std::vector<uint8_t> buf(47, 0);
    uint32_t record_length = 42;
    std::memcpy(buf.data(), &record_length, sizeof(uint32_t));

    auto result = deserialize_wal_record(buf);
    EXPECT_FALSE(result.has_value());
}

TEST(QA_WalRecord, DeserializeMinimalValidRecordRoundTrips) {
    // GDB-889 boundary regression: a real serialized BEGIN record with empty
    // data must have record_length == 43 (the minimum) and must deserialize
    // correctly. Proves the fix does NOT over-correct into rejecting valid records.
    WalRecord record;
    record.lsn = 1;
    record.txn_id = 2;
    record.prev_lsn = 0;
    record.type = WalRecordType::BEGIN;
    record.table_id = 0;
    record.page_id = 0;
    record.slot_id = 0;
    record.data.clear(); // Empty — minimal record.

    auto bytes = serialize_wal_record(record);

    // The total serialized size must be wal_record_overhead (47 = 4 + 43).
    ASSERT_EQ(bytes.size(), wal_record_overhead);

    // The record_length field (first 4 bytes) must equal kMinRecordLength = 43.
    uint32_t stored_record_length = 0;
    std::memcpy(&stored_record_length, bytes.data(), sizeof(uint32_t));
    EXPECT_EQ(stored_record_length, 43u);

    // Deserialization must succeed and round-trip all fields.
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->lsn, 1u);
    EXPECT_EQ(result->txn_id, 2u);
    EXPECT_EQ(result->type, WalRecordType::BEGIN);
    EXPECT_TRUE(result->data.empty());
}

TEST(QA_WalRecord, DeserializeRecordLengthMaxUint32) {
    // record_length = UINT32_MAX — claims to need ~4GB.
    // expected_total = UINT32_MAX + 4 = overflow on 32-bit, wraps on 64-bit.
    // buf.size() < expected_total should catch this.
    std::vector<uint8_t> buf(47, 0);
    uint32_t record_length = UINT32_MAX;
    std::memcpy(buf.data(), &record_length, sizeof(uint32_t));

    auto result = deserialize_wal_record(buf);
    EXPECT_FALSE(result.has_value());
}

TEST(QA_WalRecord, DeserializeInvalidRecordType) {
    // Create a valid record, then corrupt the type field to an out-of-range value.
    WalRecord record;
    record.lsn = 1;
    record.txn_id = 1;
    record.type = WalRecordType::BEGIN;
    auto bytes = serialize_wal_record(record);

    // The type byte is at offset 4 (record_length) + 8 (lsn) + 8 (txn_id) +
    // 8 (prev_lsn) = 28.
    // Set it to 255 (invalid), then fix the CRC.
    // Actually, since the CRC will mismatch, let's just check the CRC error.
    bytes[28] = 255;
    auto result = deserialize_wal_record(bytes);
    // Should fail due to CRC mismatch (or invalid type).
    EXPECT_FALSE(result.has_value());
}

TEST(QA_WalRecord, SerializeEmptyDataRoundTrip) {
    WalRecord record;
    record.lsn = 42;
    record.txn_id = 7;
    record.type = WalRecordType::COMMIT;
    record.data.clear(); // Explicitly empty.

    auto bytes = serialize_wal_record(record);
    EXPECT_EQ(bytes.size(), wal_record_overhead);

    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->lsn, 42u);
    EXPECT_TRUE(result->data.empty());
}

TEST(QA_WalRecord, SerializeAllZeroFieldsRoundTrip) {
    WalRecord record;
    record.lsn = 0;
    record.txn_id = 0;
    record.prev_lsn = 0;
    record.type = WalRecordType::BEGIN; // = 0
    record.table_id = 0;
    record.page_id = 0;
    record.slot_id = 0;
    record.data.clear();

    auto bytes = serialize_wal_record(record);
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->lsn, 0u);
    EXPECT_EQ(result->txn_id, 0u);
}

TEST(QA_WalRecord, DeserializeExtraTrailingBytesIgnored) {
    WalRecord record;
    record.lsn = 1;
    record.txn_id = 1;
    record.type = WalRecordType::BEGIN;
    auto bytes = serialize_wal_record(record);

    // Append trailing garbage — should be ignored.
    bytes.insert(bytes.end(), 100, 0xFF);
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->lsn, 1u);
}

// =============================================================================
// QA_WalWriter — Adversarial writer tests
// =============================================================================

TEST(QA_WalWriter, AppendRecordExactlyAtSegmentSize) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.enable_group_commit = false;
    // Set segment size to exactly fit one record with overhead.
    opts.segment_size = wal_record_overhead + 10; // One record with 10 bytes data.
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    // First record should fit exactly.
    WalRecord r;
    r.type = WalRecordType::INSERT;
    r.txn_id = 1;
    r.data.resize(10, 0xAA);
    auto lsn = writer.append(r);
    ASSERT_TRUE(lsn.has_value()) << lsn.error().message;
    EXPECT_EQ(writer.current_segment_id(), 1u);

    // Second record forces rotation.
    WalRecord r2;
    r2.type = WalRecordType::INSERT;
    r2.txn_id = 1;
    r2.data.resize(10, 0xBB);
    auto lsn2 = writer.append(r2);
    ASSERT_TRUE(lsn2.has_value()) << lsn2.error().message;
    EXPECT_EQ(writer.current_segment_id(), 2u);

    ASSERT_TRUE(writer.close().has_value());
}

TEST(QA_WalWriter, AppendRecordLargerThanSegmentFails) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.enable_group_commit = false;
    opts.segment_size = 100; // Small segment.
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    // Record larger than segment size.
    WalRecord r;
    r.type = WalRecordType::INSERT;
    r.txn_id = 1;
    r.data.resize(200, 0xFF); // 200 + overhead > 100.
    auto result = writer.append(r);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);

    ASSERT_TRUE(writer.close().has_value());
}

TEST(QA_WalWriter, ReopenAfterEmptySegmentPreservesLsn) {
    TempWalDir dir;
    WalWriterOptions opts = test_wal_opts();

    // Open, write records, close.
    {
        WalWriter writer(dir.path(), opts);
        ASSERT_TRUE(writer.open().has_value());
        WalRecord r;
        r.type = WalRecordType::BEGIN;
        r.txn_id = 1;
        ASSERT_TRUE(writer.append(r).has_value());
        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    // Open, write nothing, close.
    {
        WalWriter writer(dir.path(), opts);
        ASSERT_TRUE(writer.open().has_value());
        EXPECT_EQ(writer.current_lsn(), 2u); // Resumed correctly.
        ASSERT_TRUE(writer.close().has_value());
    }

    // Open again — should still be at LSN 2.
    {
        WalWriter writer(dir.path(), opts);
        ASSERT_TRUE(writer.open().has_value());
        EXPECT_EQ(writer.current_lsn(), 2u);
        ASSERT_TRUE(writer.close().has_value());
    }
}

TEST(QA_WalWriter, SegmentRotationCallbackInvoked) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.segment_size = 128; // Tiny to force rotation.
    opts.enable_group_commit = false;
    WalWriter writer(dir.path(), opts);

    std::vector<std::pair<uint64_t, lsn_t>> rotations;
    writer.set_on_segment_rotated(
        [&](uint64_t seg_id, lsn_t last_lsn) { rotations.emplace_back(seg_id, last_lsn); });

    ASSERT_TRUE(writer.open().has_value());

    // Write enough records to force multiple rotations.
    for (int i = 0; i < 20; ++i) {
        WalRecord r;
        r.type = WalRecordType::INSERT;
        r.txn_id = 1;
        r.data = {0x01, 0x02, 0x03};
        ASSERT_TRUE(writer.append(r).has_value());
    }

    // Should have had at least one rotation.
    EXPECT_GT(rotations.size(), 0u);

    // Each rotation should have increasing segment IDs.
    for (size_t i = 1; i < rotations.size(); ++i) {
        EXPECT_GT(rotations[i].first, rotations[i - 1].first);
    }

    ASSERT_TRUE(writer.close().has_value());
}

TEST(QA_WalWriter, TruncateBeforeCurrentSegmentPreservesIt) {
    TempWalDir dir;
    WalWriterOptions opts;
    opts.segment_size = 128;
    opts.enable_group_commit = false;
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    // Write enough to create multiple segments.
    for (int i = 0; i < 30; ++i) {
        WalRecord r;
        r.type = WalRecordType::INSERT;
        r.txn_id = 1;
        r.data = {0x01, 0x02};
        ASSERT_TRUE(writer.append(r).has_value());
    }

    uint64_t current = writer.current_segment_id();

    // Truncate everything before the current segment.
    ASSERT_TRUE(writer.truncate_before(current).has_value());

    // Current segment should still exist.
    auto seg_name =
        "wal_" + std::string(6 - std::to_string(current).length(), '0') + std::to_string(current);
    EXPECT_TRUE(std::filesystem::exists(dir.path() / seg_name));

    // Can still write after truncation.
    WalRecord r;
    r.type = WalRecordType::COMMIT;
    r.txn_id = 1;
    auto lsn = writer.append(r);
    ASSERT_TRUE(lsn.has_value());

    ASSERT_TRUE(writer.close().has_value());
}

TEST(QA_WalWriter, CheckpointWithLargeActiveList) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    // Checkpoint with 1000 active transactions.
    std::vector<txn_id_t> active;
    for (uint64_t i = 1; i <= 1000; ++i) {
        active.push_back(i);
    }
    auto lsn = writer.write_checkpoint(active);
    ASSERT_TRUE(lsn.has_value());

    ASSERT_TRUE(writer.flush().has_value());
    ASSERT_TRUE(writer.close().has_value());

    // Read it back and verify.
    WalReader reader(dir.path());
    ASSERT_TRUE(reader.open().has_value());

    auto record = reader.next();
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->type, WalRecordType::CHECKPOINT);

    auto decoded = deserialize_checkpoint_data(record->data);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->size(), 1000u);
    for (uint64_t i = 0; i < 1000; ++i) {
        EXPECT_EQ((*decoded)[i], i + 1);
    }

    ASSERT_TRUE(reader.close().has_value());
}

// =============================================================================
// QA_WalReader — Adversarial reader tests
// =============================================================================

TEST(QA_WalReader, ReadWithCorruptedRecordInSegment) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    // Write two valid records.
    WalRecord r1;
    r1.type = WalRecordType::BEGIN;
    r1.txn_id = 1;
    ASSERT_TRUE(writer.append(r1).has_value());

    WalRecord r2;
    r2.type = WalRecordType::INSERT;
    r2.txn_id = 1;
    r2.data = {0x01, 0x02, 0x03};
    ASSERT_TRUE(writer.append(r2).has_value());

    ASSERT_TRUE(writer.flush().has_value());
    ASSERT_TRUE(writer.close().has_value());

    // Corrupt the second record by flipping bits in the middle of the file.
    auto seg_path = dir.path() / "wal_000001";
    auto file_size = std::filesystem::file_size(seg_path);
    std::vector<uint8_t> data(file_size);
    {
        std::ifstream ifs(seg_path, std::ios::binary);
        ifs.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(file_size));
    }

    // Corrupt bytes in the middle (second record area).
    size_t corrupt_offset = wal_record_overhead + 5; // In second record.
    if (corrupt_offset < data.size()) {
        data[corrupt_offset] ^= 0xFF;
    }
    {
        std::ofstream ofs(seg_path, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }

    // Reader should read the first record and stop at the corrupted one.
    WalReader reader(dir.path());
    ASSERT_TRUE(reader.open().has_value());

    auto first = reader.next();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->type, WalRecordType::BEGIN);

    // Second record should fail or be skipped (reader stops at invalid).
    auto second = reader.next();
    // Either successfully reads (if corruption didn't affect CRC area)
    // or returns NOT_FOUND (treated as end of segment).
    // The reader should not crash.
    if (!second.has_value()) {
        EXPECT_EQ(second.error().code, StatusCode::NOT_FOUND);
    }

    ASSERT_TRUE(reader.close().has_value());
}

TEST(QA_WalReader, ReadSegmentWithZeroBytes) {
    TempWalDir dir;

    // Create a segment file filled with zeros.
    std::filesystem::create_directories(dir.path());
    {
        std::ofstream ofs(dir.path() / "wal_000001", std::ios::binary);
        std::vector<uint8_t> zeros(1024, 0);
        ofs.write(reinterpret_cast<const char*>(zeros.data()),
                  static_cast<std::streamsize>(zeros.size()));
    }

    WalReader reader(dir.path());
    ASSERT_TRUE(reader.open().has_value());

    // Should return NOT_FOUND (zero record_length = end of data).
    auto result = reader.next();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);

    ASSERT_TRUE(reader.close().has_value());
}

TEST(QA_WalReader, ReadFromNonExistentDirectory) {
    auto non_existent = std::filesystem::temp_directory_path() / "qa_wal_nonexistent_dir";
    std::filesystem::remove_all(non_existent);

    WalReader reader(non_existent);
    ASSERT_TRUE(reader.open().has_value()); // Opens with no segments.

    auto result = reader.next();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);

    ASSERT_TRUE(reader.close().has_value());
}

TEST(QA_WalReader, ReadNotOpenFails) {
    TempWalDir dir;
    WalReader reader(dir.path());

    // next() without open should fail.
    auto result = reader.next();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_WalReader, DoubleOpenFails) {
    TempWalDir dir;
    WalReader reader(dir.path());
    ASSERT_TRUE(reader.open().has_value());

    auto result = reader.open();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);

    ASSERT_TRUE(reader.close().has_value());
}

TEST(QA_WalReader, ReadAllRecordsThenReopen) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());
        write_committed_txn(writer, 1, 10, "data");
        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    // First read.
    {
        WalReader reader(dir.path());
        ASSERT_TRUE(reader.open().has_value());
        int count = 0;
        while (reader.next().has_value()) {
            ++count;
        }
        EXPECT_EQ(count, 3); // BEGIN, INSERT, COMMIT
        ASSERT_TRUE(reader.close().has_value());
    }

    // Second read — should get the same records.
    {
        WalReader reader(dir.path());
        ASSERT_TRUE(reader.open().has_value());
        int count = 0;
        while (reader.next().has_value()) {
            ++count;
        }
        EXPECT_EQ(count, 3);
        ASSERT_TRUE(reader.close().has_value());
    }
}

// =============================================================================
// QA_WalRecovery — Adversarial recovery tests
// =============================================================================

/// Simple recovery handler for QA tests.
class QARecoveryHandler : public RecoveryHandler {
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

// txn_id == invalid_txn_id (0) is the INVALID sentinel; it is distinct from
// frozen_txn_id (~0), which is the AUTOCOMMIT marker. A data record stamped
// with the invalid sentinel is malformed -- legitimate DML always uses either
// a real txn_id or frozen_txn_id. The recovery path intentionally skips such
// records (neither redoes nor undoes them) and emits a WARN so the drop is
// observable in logs. The contrast test below (RecoverRedoesFrozenTxnIdDataRecord)
// pins that frozen_txn_id records ARE redone, so the skip is sentinel-specific.
TEST(QA_WalRecovery, RecoverSkipsInvalidTxnIdZeroDataRecord) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        // A data record stamped with the invalid sentinel (txn_id=0).
        // This is malformed: real DML never produces txn_id=0 data records.
        WalRecord insert;
        insert.type = WalRecordType::INSERT;
        insert.txn_id = invalid_txn_id; // 0 -- the invalid sentinel
        insert.table_id = 10;
        insert.data = {0x01, 0x02};
        ASSERT_TRUE(writer.append(insert).has_value());

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    QARecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // The malformed record is scanned but intentionally not redone or undone.
    // A WARN is emitted in the recovery log (observable; not a silent drop).
    EXPECT_EQ(stats->records_scanned, 1u);
    EXPECT_EQ(handler.redo_entries.size(), 0u);
    EXPECT_EQ(handler.undo_entries.size(), 0u);
}

// Contrast: a data record stamped with frozen_txn_id (~0, the AUTOCOMMIT
// marker) is VALID and must be redone. If this assertion fails, the invalid-
// sentinel skip was widened to accidentally drop frozen/autocommit records.
TEST(QA_WalRecovery, RecoverRedoesFrozenTxnIdDataRecord) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        // A data record stamped with frozen_txn_id -- normal autocommit DML.
        WalRecord insert;
        insert.type = WalRecordType::INSERT;
        insert.txn_id = frozen_txn_id; // ~0 -- the autocommit marker
        insert.table_id = 20;
        insert.data = {0xAA, 0xBB};
        ASSERT_TRUE(writer.append(insert).has_value());

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    QARecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // frozen_txn_id records are always redone (autocommit = committed).
    EXPECT_EQ(stats->records_scanned, 1u);
    ASSERT_EQ(handler.redo_entries.size(), 1u);
    EXPECT_EQ(handler.redo_entries[0].txn_id, frozen_txn_id);
    EXPECT_EQ(handler.undo_entries.size(), 0u);
}

TEST(QA_WalRecovery, RecoverWithMultipleCheckpoints) {
    // Multiple checkpoints — only the last one should matter.
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        // Txn 1: committed before first checkpoint.
        write_committed_txn(writer, 1, 10, "before-ckpt1");

        // First checkpoint.
        ASSERT_TRUE(writer.write_checkpoint({}).has_value());

        // Txn 2: committed between checkpoints.
        write_committed_txn(writer, 2, 20, "between-ckpts");

        // Second checkpoint.
        ASSERT_TRUE(writer.write_checkpoint({}).has_value());

        // Txn 3: committed after last checkpoint.
        write_committed_txn(writer, 3, 30, "after-ckpt2");

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    QARecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // Only txn 3 (after last checkpoint) should be redone.
    EXPECT_EQ(stats->committed_txns, 1u);
    EXPECT_EQ(stats->records_redone, 1u);
    ASSERT_EQ(handler.redo_entries.size(), 1u);
    EXPECT_EQ(handler.redo_entries[0].txn_id, 3u);
}

TEST(QA_WalRecovery, RecoverCheckpointWithActivesThatNeverCommit) {
    // Checkpoint records txn 5 as active. Txn 5 has data records after
    // checkpoint but never commits — simulating crash.
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        // Txn 5 starts before checkpoint.
        WalRecord begin;
        begin.type = WalRecordType::BEGIN;
        begin.txn_id = 5;
        ASSERT_TRUE(writer.append(begin).has_value());

        // Checkpoint with txn 5 active.
        ASSERT_TRUE(writer.write_checkpoint({5}).has_value());

        // Txn 5 writes more data after checkpoint.
        WalRecord insert;
        insert.type = WalRecordType::INSERT;
        insert.txn_id = 5;
        insert.table_id = 10;
        insert.data = {0xAA};
        ASSERT_TRUE(writer.append(insert).has_value());

        WalRecord update;
        update.type = WalRecordType::UPDATE;
        update.txn_id = 5;
        update.table_id = 10;
        update.data = {0xBB};
        ASSERT_TRUE(writer.append(update).has_value());

        // No COMMIT — crash.
        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    QARecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // Txn 5 was in-progress at crash, should be aborted.
    EXPECT_EQ(stats->committed_txns, 0u);
    EXPECT_EQ(stats->aborted_txns, 1u);
    EXPECT_EQ(stats->records_redone, 0u);
    EXPECT_EQ(stats->records_undone, 2u); // INSERT + UPDATE

    // Undo should be in reverse order: UPDATE first, then INSERT.
    ASSERT_EQ(handler.undo_entries.size(), 2u);
    EXPECT_EQ(handler.undo_entries[0].type, WalRecordType::UPDATE);
    EXPECT_EQ(handler.undo_entries[1].type, WalRecordType::INSERT);
}

TEST(QA_WalRecovery, RecoverManyTransactionsMixed) {
    // Stress test: 50 committed, 50 aborted, 50 in-progress.
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        for (int i = 1; i <= 50; ++i) {
            write_committed_txn(writer, static_cast<txn_id_t>(i), 10, "committed");
        }
        for (int i = 51; i <= 100; ++i) {
            write_aborted_txn(writer, static_cast<txn_id_t>(i), 20, "aborted");
        }
        for (int i = 101; i <= 150; ++i) {
            // In-progress: BEGIN + INSERT, no COMMIT/ABORT.
            WalRecord begin;
            begin.type = WalRecordType::BEGIN;
            begin.txn_id = static_cast<txn_id_t>(i);
            ASSERT_TRUE(writer.append(begin).has_value());

            WalRecord insert;
            insert.type = WalRecordType::INSERT;
            insert.txn_id = static_cast<txn_id_t>(i);
            insert.table_id = 30;
            insert.data = {0x01};
            ASSERT_TRUE(writer.append(insert).has_value());
        }

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    QARecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->committed_txns, 50u);
    EXPECT_EQ(stats->aborted_txns, 100u); // 50 aborted + 50 in-progress
    EXPECT_EQ(stats->records_redone, 50u);
    EXPECT_EQ(stats->records_undone, 100u);
    EXPECT_EQ(handler.redo_entries.size(), 50u);
    EXPECT_EQ(handler.undo_entries.size(), 100u);
}

TEST(QA_WalRecovery, RecoverCreateTableAndDropTable) {
    // CREATE_TABLE and DROP_TABLE are data records that should be redone.
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        WalRecord begin;
        begin.type = WalRecordType::BEGIN;
        begin.txn_id = 1;
        ASSERT_TRUE(writer.append(begin).has_value());

        WalRecord create;
        create.type = WalRecordType::CREATE_TABLE;
        create.txn_id = 1;
        create.table_id = 42;
        std::string name = "test_table";
        create.data.assign(name.begin(), name.end());
        ASSERT_TRUE(writer.append(create).has_value());

        WalRecord drop;
        drop.type = WalRecordType::DROP_TABLE;
        drop.txn_id = 1;
        drop.table_id = 42;
        ASSERT_TRUE(writer.append(drop).has_value());

        WalRecord commit;
        commit.type = WalRecordType::COMMIT;
        commit.txn_id = 1;
        ASSERT_TRUE(writer.append(commit).has_value());

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    QARecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->records_redone, 2u); // CREATE_TABLE + DROP_TABLE
    ASSERT_EQ(handler.redo_entries.size(), 2u);
    EXPECT_EQ(handler.redo_entries[0].type, WalRecordType::CREATE_TABLE);
    EXPECT_EQ(handler.redo_entries[1].type, WalRecordType::DROP_TABLE);
}

TEST(QA_WalRecovery, RecoverPageSplitIsDataRecord) {
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        WalRecord begin;
        begin.type = WalRecordType::BEGIN;
        begin.txn_id = 1;
        ASSERT_TRUE(writer.append(begin).has_value());

        WalRecord split;
        split.type = WalRecordType::PAGE_SPLIT;
        split.txn_id = 1;
        split.table_id = 1;
        split.page_id = 50;
        split.data = {0x01, 0x02, 0x03};
        ASSERT_TRUE(writer.append(split).has_value());

        WalRecord commit;
        commit.type = WalRecordType::COMMIT;
        commit.txn_id = 1;
        ASSERT_TRUE(writer.append(commit).has_value());

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    QARecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->records_redone, 1u);
    ASSERT_EQ(handler.redo_entries.size(), 1u);
    EXPECT_EQ(handler.redo_entries[0].type, WalRecordType::PAGE_SPLIT);
}

TEST(QA_WalRecovery, RecoverAcrossManySmallSegments) {
    // Recovery across many tiny segments (stress the reader's segment advancing).
    TempWalDir dir;
    WalWriterOptions opts = test_wal_opts();
    opts.segment_size = 64; // Very tiny — most records won't even fit.

    // Adjust: need at least wal_record_overhead (47) + some data. 64 is enough.
    {
        WalWriter writer(dir.path(), opts);
        ASSERT_TRUE(writer.open().has_value());

        // Many small committed transactions.
        for (int i = 1; i <= 10; ++i) {
            WalRecord begin;
            begin.type = WalRecordType::BEGIN;
            begin.txn_id = static_cast<txn_id_t>(i);
            ASSERT_TRUE(writer.append(begin).has_value());

            WalRecord commit;
            commit.type = WalRecordType::COMMIT;
            commit.txn_id = static_cast<txn_id_t>(i);
            ASSERT_TRUE(writer.append(commit).has_value());
        }

        ASSERT_TRUE(writer.flush().has_value());
        EXPECT_GT(writer.current_segment_id(), 5u); // Many segments.
        ASSERT_TRUE(writer.close().has_value());
    }

    QARecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->committed_txns, 10u);
    EXPECT_EQ(stats->aborted_txns, 0u);
}

TEST(QA_WalRecovery, RecoverFromEmptyDirectory) {
    // No WAL files at all (not even an empty one).
    TempWalDir dir;
    // Don't create any writer — directory exists but is empty.

    QARecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    EXPECT_EQ(stats->records_scanned, 0u);
    EXPECT_EQ(stats->committed_txns, 0u);
    EXPECT_EQ(stats->aborted_txns, 0u);
    EXPECT_EQ(stats->records_redone, 0u);
    EXPECT_EQ(stats->records_undone, 0u);
    EXPECT_TRUE(handler.redo_entries.empty());
    EXPECT_TRUE(handler.undo_entries.empty());
}

// =============================================================================
// QA_WalCheckpoint — Adversarial checkpoint data tests
// =============================================================================

TEST(QA_WalCheckpoint, SerializeDeserializeMaxTxnId) {
    std::vector<txn_id_t> txns = {UINT64_MAX, UINT64_MAX - 1, 0, 1};
    auto data = serialize_checkpoint_data(txns);
    auto result = deserialize_checkpoint_data(data);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 4u);
    EXPECT_EQ((*result)[0], UINT64_MAX);
    EXPECT_EQ((*result)[1], UINT64_MAX - 1);
    EXPECT_EQ((*result)[2], 0u);
    EXPECT_EQ((*result)[3], 1u);
}

TEST(QA_WalCheckpoint, DeserializeEmptyBuffer) {
    std::vector<uint8_t> data;
    auto result = deserialize_checkpoint_data(data);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_WalCheckpoint, DeserializeCountClaimsTooMany) {
    // Craft data claiming count=1000000 but only providing 1 txn_id.
    std::vector<uint8_t> data(sizeof(uint32_t) + sizeof(uint64_t));
    uint32_t count = 1000000;
    std::memcpy(data.data(), &count, sizeof(uint32_t));
    uint64_t txn = 42;
    std::memcpy(data.data() + sizeof(uint32_t), &txn, sizeof(uint64_t));

    auto result = deserialize_checkpoint_data(data);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// QA_WalWriter — Concurrent stress tests
// =============================================================================

TEST(QA_WalWriter, ConcurrentAppendsWithSmallSegments) {
    // Concurrent appends with very small segments to stress rotation.
    TempWalDir dir;
    WalWriterOptions opts;
    opts.segment_size = 128;
    opts.enable_group_commit = true;
    opts.flush_interval = std::chrono::milliseconds(5);
    WalWriter writer(dir.path(), opts);
    ASSERT_TRUE(writer.open().has_value());

    constexpr int num_threads = 4;
    constexpr int records_per_thread = 50;
    std::atomic<int> errors{0};
    std::atomic<int> total_appended{0};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < records_per_thread; ++i) {
                WalRecord r;
                r.type = WalRecordType::INSERT;
                r.txn_id = static_cast<txn_id_t>(t + 1);
                r.table_id = 10;
                r.data = {0x01, 0x02, 0x03};
                auto lsn = writer.append(r);
                if (!lsn.has_value()) {
                    errors.fetch_add(1);
                } else {
                    total_appended.fetch_add(1);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(errors.load(), 0);
    EXPECT_EQ(total_appended.load(), num_threads * records_per_thread);
    EXPECT_EQ(writer.current_lsn(), static_cast<lsn_t>(num_threads * records_per_thread + 1));

    // Multiple rotations should have occurred.
    EXPECT_GT(writer.current_segment_id(), 1u);

    ASSERT_TRUE(writer.close().has_value());
}

// =============================================================================
// QA_WalWriterReader — End-to-end write/read/recover integration
// =============================================================================

TEST(QA_WalWriterReader, WriteReadRecoverFullCycle) {
    // Full cycle: write committed and aborted txns, read back,
    // then recover and verify correct redo/undo.
    TempWalDir dir;
    WalWriterOptions opts = test_wal_opts();
    opts.segment_size = 256; // Force some rotations.

    {
        WalWriter writer(dir.path(), opts);
        ASSERT_TRUE(writer.open().has_value());

        // 5 committed txns.
        for (int i = 1; i <= 5; ++i) {
            write_committed_txn(
                writer, static_cast<txn_id_t>(i), static_cast<uint32_t>(i), "committed-data");
        }

        // 3 aborted txns.
        for (int i = 6; i <= 8; ++i) {
            write_aborted_txn(
                writer, static_cast<txn_id_t>(i), static_cast<uint32_t>(i), "aborted-data");
        }

        // 2 in-progress txns.
        for (int i = 9; i <= 10; ++i) {
            WalRecord begin;
            begin.type = WalRecordType::BEGIN;
            begin.txn_id = static_cast<txn_id_t>(i);
            ASSERT_TRUE(writer.append(begin).has_value());

            WalRecord insert;
            insert.type = WalRecordType::INSERT;
            insert.txn_id = static_cast<txn_id_t>(i);
            insert.table_id = static_cast<uint32_t>(i);
            insert.data = {0xFF};
            ASSERT_TRUE(writer.append(insert).has_value());
        }

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    // Read back all records.
    {
        WalReader reader(dir.path());
        ASSERT_TRUE(reader.open().has_value());

        int count = 0;
        while (reader.next().has_value()) {
            ++count;
        }

        // 5 committed: 3 records each = 15
        // 3 aborted: 3 records each = 9
        // 2 in-progress: 2 records each = 4
        // Total: 28
        EXPECT_EQ(count, 28);

        ASSERT_TRUE(reader.close().has_value());
    }

    // Recover.
    {
        QARecoveryHandler handler;
        WalRecovery recovery(dir.path(), handler);
        auto stats = recovery.recover();
        ASSERT_TRUE(stats.has_value()) << stats.error().message;

        EXPECT_EQ(stats->committed_txns, 5u);
        EXPECT_EQ(stats->aborted_txns, 5u); // 3 aborted + 2 in-progress
        EXPECT_EQ(stats->records_redone, 5u);
        EXPECT_EQ(stats->records_undone, 5u);

        EXPECT_EQ(handler.redo_entries.size(), 5u);
        EXPECT_EQ(handler.undo_entries.size(), 5u);

        // Redo should be in forward order by txn.
        for (size_t i = 0; i < handler.redo_entries.size(); ++i) {
            EXPECT_EQ(handler.redo_entries[i].txn_id, static_cast<txn_id_t>(i + 1));
        }
    }
}

TEST(QA_WalWriterReader, WriteCheckpointThenCrashRecovery) {
    // Write txns, checkpoint, write more, "crash", recover.
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        // Committed txn before checkpoint.
        write_committed_txn(writer, 1, 10, "before");

        // Checkpoint.
        ASSERT_TRUE(writer.write_checkpoint({}).has_value());

        // Start txn 2, write data, commit.
        write_committed_txn(writer, 2, 20, "after");

        // Start txn 3, write data, no commit (crash).
        WalRecord begin;
        begin.type = WalRecordType::BEGIN;
        begin.txn_id = 3;
        ASSERT_TRUE(writer.append(begin).has_value());

        WalRecord insert;
        insert.type = WalRecordType::INSERT;
        insert.txn_id = 3;
        insert.table_id = 30;
        insert.data = {0xCC};
        ASSERT_TRUE(writer.append(insert).has_value());

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    QARecoveryHandler handler;
    WalRecovery recovery(dir.path(), handler);
    auto stats = recovery.recover();
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // Only txn 2 (committed after checkpoint) should be redone.
    EXPECT_EQ(stats->committed_txns, 1u);
    EXPECT_EQ(stats->records_redone, 1u);

    // Txn 3 (in-progress) should be undone.
    EXPECT_EQ(stats->aborted_txns, 1u);
    EXPECT_EQ(stats->records_undone, 1u);

    ASSERT_EQ(handler.redo_entries.size(), 1u);
    EXPECT_EQ(handler.redo_entries[0].txn_id, 2u);

    ASSERT_EQ(handler.undo_entries.size(), 1u);
    EXPECT_EQ(handler.undo_entries[0].txn_id, 3u);
}

// =============================================================================
// QA_WalWriter — Scan recovery after partial writes
// =============================================================================

TEST(QA_WalWriter, ScanRecoveryWithTrailingGarbage) {
    // Simulate partial write: valid records followed by garbage bytes.
    TempWalDir dir;

    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        WalRecord r;
        r.type = WalRecordType::BEGIN;
        r.txn_id = 1;
        ASSERT_TRUE(writer.append(r).has_value());

        ASSERT_TRUE(writer.flush().has_value());
        ASSERT_TRUE(writer.close().has_value());
    }

    // Append garbage to the segment file.
    auto seg_path = dir.path() / "wal_000001";
    {
        std::ofstream ofs(seg_path, std::ios::binary | std::ios::app);
        std::vector<uint8_t> garbage = {0xDE,
                                        0xAD,
                                        0xBE,
                                        0xEF,
                                        0x01,
                                        0x02,
                                        0x03,
                                        0x04,
                                        0x05,
                                        0x06,
                                        0x07,
                                        0x08,
                                        0x09,
                                        0x0A,
                                        0x0B,
                                        0x0C};
        ofs.write(reinterpret_cast<const char*>(garbage.data()),
                  static_cast<std::streamsize>(garbage.size()));
    }

    // Reopen writer — scan should find the valid record and skip garbage.
    {
        WalWriter writer(dir.path(), test_wal_opts());
        ASSERT_TRUE(writer.open().has_value());

        EXPECT_EQ(writer.current_lsn(), 2u); // Resumed after the valid record.

        // Can still append.
        WalRecord r;
        r.type = WalRecordType::COMMIT;
        r.txn_id = 1;
        auto lsn = writer.append(r);
        ASSERT_TRUE(lsn.has_value());
        EXPECT_EQ(*lsn, 2u);

        ASSERT_TRUE(writer.close().has_value());
    }
}

} // namespace
} // namespace sixseven
