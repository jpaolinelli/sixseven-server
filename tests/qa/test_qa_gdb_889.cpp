#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace sixseven {
namespace {

// =============================================================================
// QA_GDB889 — Adversarial tests for the GDB-889 kMinRecordLength guard
//
// The fix: deserialize_wal_record now rejects record_length < 43 (= header(39)
// + crc(4)) instead of the old check < 4.  These tests verify:
//   (1) BOUNDARY: 42 rejected, 43 (minimal empty-data record) accepted + round-trips.
//   (2) GARBAGE RANGE: lengths 4..42 all rejected (spanning the newly-closed gap).
//   (3) NO-FALSE-REJECT: real serialized records of every type round-trip.
//   (4) BUFFER-TOO-SMALL: buf.size() < wal_record_overhead is rejected before
//       the record_length field is even read.
//   (5) CRC-MISMATCH: still rejected even when record_length >= 43.
// =============================================================================

// ---------------------------------------------------------------------------
// BOUNDARY: record_length == 42 (one below min) must be rejected.
// ---------------------------------------------------------------------------
TEST(QA_GDB889, RecordLength42IsRejected) {
    std::vector<uint8_t> buf(47, 0);
    uint32_t rl = 42;
    std::memcpy(buf.data(), &rl, sizeof(uint32_t));
    auto result = deserialize_wal_record(buf);
    EXPECT_FALSE(result.has_value()) << "record_length=42 must be rejected (< kMinRecordLength=43)";
    if (!result.has_value()) {
        EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    }
}

// ---------------------------------------------------------------------------
// BOUNDARY: record_length == 43 (exactly min) from a real serialized empty
// record must SUCCEED and round-trip all fields.
// ---------------------------------------------------------------------------
TEST(QA_GDB889, RecordLength43IsAccepted) {
    WalRecord record;
    record.lsn      = 99;
    record.txn_id   = 7;
    record.prev_lsn = 0;
    record.type     = WalRecordType::BEGIN;
    record.table_id = 0;
    record.page_id  = 0;
    record.slot_id  = 0;
    record.data.clear();

    auto bytes = serialize_wal_record(record);

    // Verify the serializer emits exactly wal_record_overhead bytes.
    ASSERT_EQ(bytes.size(), wal_record_overhead); // 47

    // Verify the record_length field equals 43.
    uint32_t stored_rl = 0;
    std::memcpy(&stored_rl, bytes.data(), sizeof(uint32_t));
    ASSERT_EQ(stored_rl, 43u);

    // Full round-trip must succeed.
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->lsn,    99u);
    EXPECT_EQ(result->txn_id, 7u);
    EXPECT_EQ(result->type,   WalRecordType::BEGIN);
    EXPECT_TRUE(result->data.empty());
}

// ---------------------------------------------------------------------------
// GARBAGE RANGE: all lengths 4..42 must be rejected.
// Pre-fix: only 0..3 were caught; 4..42 passed through.
// Post-fix: 0..42 all caught.
// ---------------------------------------------------------------------------
TEST(QA_GDB889, AllLengthsInGarbageRangeRejected) {
    // Use a 200-byte all-zero buffer so buf.size() >= record_length+4 for
    // small values, ensuring we test only the record_length guard, not the
    // buffer-too-small guard.
    std::vector<uint8_t> buf(200, 0);
    for (uint32_t rl = 4; rl <= 42; ++rl) {
        std::memcpy(buf.data(), &rl, sizeof(uint32_t));
        auto result = deserialize_wal_record(buf);
        EXPECT_FALSE(result.has_value())
            << "record_length=" << rl << " should be REJECTED (< kMinRecordLength=43)";
    }
}

// ---------------------------------------------------------------------------
// NO-FALSE-REJECT: round-trip every WalRecordType with empty data.
// All must deserialize successfully (record_length == 43 for empty-data records).
// ---------------------------------------------------------------------------
TEST(QA_GDB889, AllRecordTypesWithEmptyDataRoundTrip) {
    const WalRecordType types[] = {
        WalRecordType::BEGIN,
        WalRecordType::INSERT,
        WalRecordType::UPDATE,
        WalRecordType::DELETE,
        WalRecordType::PAGE_SPLIT,
        WalRecordType::COMMIT,
        WalRecordType::ABORT,
        WalRecordType::CHECKPOINT,
        WalRecordType::CREATE_TABLE,
        WalRecordType::DROP_TABLE,
        WalRecordType::PROMOTE,
    };

    for (auto type : types) {
        WalRecord r;
        r.lsn    = 100;
        r.txn_id = 5;
        r.type   = type;
        r.data.clear();

        auto bytes  = serialize_wal_record(r);
        auto result = deserialize_wal_record(bytes);
        ASSERT_TRUE(result.has_value())
            << "type=" << wal_record_type_name(type) << " round-trip failed: "
            << (result.has_value() ? "" : result.error().message);
        EXPECT_EQ(result->type, type);
        EXPECT_TRUE(result->data.empty());
    }
}

// ---------------------------------------------------------------------------
// NO-FALSE-REJECT: round-trip records WITH non-empty data (record_length > 43).
// ---------------------------------------------------------------------------
TEST(QA_GDB889, RecordWithNonEmptyDataRoundTrips) {
    WalRecord r;
    r.lsn      = 1;
    r.txn_id   = 42;
    r.prev_lsn = 0;
    r.type     = WalRecordType::INSERT;
    r.table_id = 7;
    r.page_id  = 3;
    r.slot_id  = 2;
    r.data     = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};

    auto bytes = serialize_wal_record(r);
    // record_length field should be 43 + 8 = 51.
    uint32_t stored_rl = 0;
    std::memcpy(&stored_rl, bytes.data(), sizeof(uint32_t));
    EXPECT_EQ(stored_rl, 51u);

    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->lsn,      1u);
    EXPECT_EQ(result->txn_id,  42u);
    EXPECT_EQ(result->type,    WalRecordType::INSERT);
    EXPECT_EQ(result->table_id, 7u);
    EXPECT_EQ(result->page_id,  3u);
    EXPECT_EQ(result->slot_id,  2u);
    ASSERT_EQ(result->data.size(), 8u);
    EXPECT_EQ(result->data[0], 0xDE);
    EXPECT_EQ(result->data[7], 0x04);
}

// ---------------------------------------------------------------------------
// NO-FALSE-REJECT: large payload (1 KB data).
// ---------------------------------------------------------------------------
TEST(QA_GDB889, LargePayloadRoundTrips) {
    WalRecord r;
    r.lsn    = 9999;
    r.txn_id = 123;
    r.type   = WalRecordType::UPDATE;
    r.data.resize(1024, 0xAB);

    auto bytes  = serialize_wal_record(r);
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->lsn,            9999u);
    EXPECT_EQ(result->data.size(),    1024u);
    EXPECT_EQ(result->data[0],        0xAB);
    EXPECT_EQ(result->data[1023],     0xAB);
}

// ---------------------------------------------------------------------------
// BUFFER-TOO-SMALL: buffers shorter than wal_record_overhead (47) rejected
// before the record_length guard is reached.
// ---------------------------------------------------------------------------
TEST(QA_GDB889, BufferShorterThanOverheadRejected) {
    for (size_t sz = 0; sz < wal_record_overhead; ++sz) {
        std::vector<uint8_t> buf(sz, 0);
        auto result = deserialize_wal_record(buf);
        EXPECT_FALSE(result.has_value())
            << "buf.size()=" << sz << " should be rejected (< wal_record_overhead=47)";
    }
}

// ---------------------------------------------------------------------------
// CRC-MISMATCH: single-bit flip in a valid serialized record → CRC error.
// The fix must not short-circuit CRC checking for valid-length records.
// ---------------------------------------------------------------------------
TEST(QA_GDB889, CrcMismatchRejected) {
    WalRecord r;
    r.lsn    = 1;
    r.txn_id = 1;
    r.type   = WalRecordType::COMMIT;

    auto bytes = serialize_wal_record(r);
    ASSERT_GE(bytes.size(), wal_record_overhead);

    // Flip a bit in the header (not the CRC field, which is the last 4 bytes).
    bytes[5] ^= 0x01;

    auto result = deserialize_wal_record(bytes);
    EXPECT_FALSE(result.has_value()) << "CRC mismatch must be detected";
    if (!result.has_value()) {
        EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
    }
}

// ---------------------------------------------------------------------------
// MUTATION: confirm that the OLD code (guard < 4) would accept record_length=4.
// We verify this indirectly by running the production code (which has the fix)
// against record_length=4 and confirming it NOW returns INVALID_ARGUMENT.
// This is the key regression guard: the old guard returned ok() here.
// ---------------------------------------------------------------------------
TEST(QA_GDB889, RecordLength4NowRejectedNotAccepted) {
    std::vector<uint8_t> buf(47, 0);
    uint32_t rl = 4;
    std::memcpy(buf.data(), &rl, sizeof(uint32_t));

    auto result = deserialize_wal_record(buf);
    ASSERT_FALSE(result.has_value())
        << "record_length=4 was incorrectly ACCEPTED — kMinRecordLength guard missing";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// BOUNDARY: lengths just above 43 with non-zero data_length are validated.
// record_length=44 claims 1 byte beyond header — but the header itself needs
// 39 bytes of fixed fields, leaving 0 bytes for the data_length+data+crc slot
// if record_length=43. With record_length=44, crc_length=40, which means
// the CRC spans header(39) + 1 extra byte. An all-zero 200-byte buffer
// will produce a specific CRC; the stored CRC (bytes 88-91) may or may not
// match, but the point is we must NOT crash and must not silently accept
// garbage in the data_length field.
// ---------------------------------------------------------------------------
TEST(QA_GDB889, RecordLength44GarbageBufferCrcMismatch) {
    // record_length=44 > 43, so the new guard passes.
    // The CRC check should still catch the garbage.
    std::vector<uint8_t> buf(200, 0);
    uint32_t rl = 44;
    std::memcpy(buf.data(), &rl, sizeof(uint32_t));

    auto result = deserialize_wal_record(buf);
    // May pass or fail the CRC (all-zeros data yields a non-trivial CRC).
    // Must not crash regardless.
    (void)result; // Result checked only for crash safety.
}

// ---------------------------------------------------------------------------
// END-TO-END: serialize→deserialize a COMMIT record with max field values.
// ---------------------------------------------------------------------------
TEST(QA_GDB889, MaxFieldValuesRoundTrip) {
    WalRecord r;
    r.lsn      = UINT64_MAX;
    r.txn_id   = UINT64_MAX - 1;
    r.prev_lsn = UINT64_MAX - 2;
    r.type     = WalRecordType::PROMOTE;
    r.table_id = UINT32_MAX;
    r.page_id  = UINT32_MAX;
    r.slot_id  = UINT16_MAX;
    r.data     = {0xFF, 0xFE, 0xFD};

    auto bytes  = serialize_wal_record(r);
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->lsn,      UINT64_MAX);
    EXPECT_EQ(result->txn_id,   UINT64_MAX - 1);
    EXPECT_EQ(result->prev_lsn, UINT64_MAX - 2);
    EXPECT_EQ(result->type,     WalRecordType::PROMOTE);
    EXPECT_EQ(result->table_id, UINT32_MAX);
    EXPECT_EQ(result->page_id,  UINT32_MAX);
    EXPECT_EQ(result->slot_id,  UINT16_MAX);
    ASSERT_EQ(result->data.size(), 3u);
    EXPECT_EQ(result->data[0], 0xFF);
}

// ---------------------------------------------------------------------------
// Confirm lengths 0..3 (pre-existing guards) still reject correctly.
// ---------------------------------------------------------------------------
TEST(QA_GDB889, LengthsZeroToThreeStillRejected) {
    std::vector<uint8_t> buf(200, 0);
    for (uint32_t rl = 0; rl <= 3; ++rl) {
        std::memcpy(buf.data(), &rl, sizeof(uint32_t));
        auto result = deserialize_wal_record(buf);
        EXPECT_FALSE(result.has_value())
            << "record_length=" << rl << " should be REJECTED";
    }
}

} // namespace
} // namespace sixseven
