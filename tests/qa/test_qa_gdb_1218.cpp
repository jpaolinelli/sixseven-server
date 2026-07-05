// QA regression tests for GDB-1218: Dedup of write_native/read_native
// templates into include/sixseven/common/byte_io.h.
//
// Risk being tested: this dedup touches the byte-layout contract shared by
// on-disk WAL records (src/storage/wal_record.cpp) and on-wire replication
// messages (src/server/replication_message.cpp). Any drift here is silent
// data corruption -- these tests pin the exact byte layout produced before
// the refactor and verify both call sites still round-trip correctly.

#include "sixseven/common/byte_io.h"
#include "sixseven/server/replication_message.h"
#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <limits>
#include <vector>

using namespace sixseven;

namespace {

// Manually construct the pre-dedup expected byte layout for a WalRecord,
// independent of serialize_wal_record(), so a regression in either the
// shared header or the call site would be caught.
std::vector<uint8_t> reference_wal_bytes(const WalRecord& r) {
    std::vector<uint8_t> buf;
    auto push = [&buf](auto value) {
        std::array<uint8_t, sizeof(value)> tmp{};
        std::memcpy(tmp.data(), &value, sizeof(value));
        buf.insert(buf.end(), tmp.begin(), tmp.end());
    };

    uint32_t record_length =
        static_cast<uint32_t>(wal_record_header_size + r.data.size() + sizeof(uint32_t));
    push(record_length);
    push(r.lsn);
    push(r.txn_id);
    push(r.prev_lsn);
    push(static_cast<uint8_t>(r.type));
    push(r.table_id);
    push(r.page_id);
    push(r.slot_id);
    push(static_cast<uint32_t>(r.data.size()));
    buf.insert(buf.end(), r.data.begin(), r.data.end());
    // CRC recomputed identically by production code; we don't duplicate the
    // crc32c algorithm here, so exclude trailing 4 bytes from comparison in
    // the caller and instead assert structural bytes only.
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// AC1/AC2: WAL record round-trip is byte-identical to the pre-dedup layout.
// ---------------------------------------------------------------------------

TEST(QaGdb1218WalRoundTrip, EncodeDecodeProducesEqualStruct) {
    WalRecord record;
    record.lsn = 0x1122334455667788ULL;
    record.txn_id = 42;
    record.prev_lsn = 41;
    record.type = WalRecordType::INSERT;
    record.table_id = 7;
    record.page_id = 3;
    record.slot_id = 9;
    record.data = {0xDE, 0xAD, 0xBE, 0xEF};

    auto bytes = serialize_wal_record(record);
    auto decoded = deserialize_wal_record(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;

    EXPECT_EQ(decoded->lsn, record.lsn);
    EXPECT_EQ(decoded->txn_id, record.txn_id);
    EXPECT_EQ(decoded->prev_lsn, record.prev_lsn);
    EXPECT_EQ(decoded->type, record.type);
    EXPECT_EQ(decoded->table_id, record.table_id);
    EXPECT_EQ(decoded->page_id, record.page_id);
    EXPECT_EQ(decoded->slot_id, record.slot_id);
    EXPECT_EQ(decoded->data, record.data);
}

TEST(QaGdb1218WalRoundTrip, StructuralBytesMatchManualReferenceEncoding) {
    WalRecord record;
    record.lsn = 100;
    record.txn_id = 200;
    record.prev_lsn = 50;
    record.type = WalRecordType::UPDATE;
    record.table_id = 5;
    record.page_id = 6;
    record.slot_id = 1;
    record.data = {1, 2, 3};

    auto produced = serialize_wal_record(record);
    auto reference = reference_wal_bytes(record);

    // Reference covers everything except the trailing 4-byte CRC.
    ASSERT_GE(produced.size(), reference.size() + 4);
    for (size_t i = 0; i < reference.size(); ++i) {
        EXPECT_EQ(produced[i], reference[i]) << "byte mismatch at index " << i;
    }
}

TEST(QaGdb1218WalRoundTrip, EmptyDataPayloadRoundTrips) {
    WalRecord record;
    record.lsn = 1;
    record.txn_id = 1;
    record.type = WalRecordType::BEGIN;

    auto bytes = serialize_wal_record(record);
    EXPECT_EQ(bytes.size(), wal_record_overhead);

    auto decoded = deserialize_wal_record(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->data.empty());
}

TEST(QaGdb1218WalRoundTrip, MaxWidthFieldsRoundTrip) {
    WalRecord record;
    record.lsn = std::numeric_limits<uint64_t>::max();
    record.txn_id = std::numeric_limits<uint64_t>::max();
    record.prev_lsn = std::numeric_limits<uint64_t>::max();
    record.type = WalRecordType::EDGE_DELETE; // highest valid enum value
    record.table_id = std::numeric_limits<uint32_t>::max();
    record.page_id = std::numeric_limits<uint32_t>::max();
    record.slot_id = std::numeric_limits<uint16_t>::max();
    record.data.assign(64, 0xFF);

    auto bytes = serialize_wal_record(record);
    auto decoded = deserialize_wal_record(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(decoded->lsn, record.lsn);
    EXPECT_EQ(decoded->table_id, record.table_id);
    EXPECT_EQ(decoded->page_id, record.page_id);
    EXPECT_EQ(decoded->slot_id, record.slot_id);
    EXPECT_EQ(decoded->data, record.data);
}

TEST(QaGdb1218WalRoundTrip, CorruptedByteFailsCrcCheck) {
    WalRecord record;
    record.lsn = 5;
    record.txn_id = 6;
    record.type = WalRecordType::COMMIT;
    record.data = {9, 9, 9};

    auto bytes = serialize_wal_record(record);
    ASSERT_FALSE(bytes.empty());
    // Flip a bit in the middle of the data payload; CRC must catch it.
    bytes[bytes.size() - 5] ^= 0x01;

    auto decoded = deserialize_wal_record(bytes);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, StatusCode::IO_ERROR);
}

// ---------------------------------------------------------------------------
// AC3/AC4: Replication message round-trips for every message type.
// ---------------------------------------------------------------------------

TEST(QaGdb1218ReplicationRoundTrip, WalDataMessageRoundTrips) {
    WalDataMessage msg;
    msg.start_lsn = 10;
    msg.end_lsn = 20;
    msg.record_count = 3;
    msg.data = {1, 2, 3, 4, 5};

    auto bytes = serialize_wal_data(msg);
    auto decoded = deserialize_wal_data(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(decoded->start_lsn, msg.start_lsn);
    EXPECT_EQ(decoded->end_lsn, msg.end_lsn);
    EXPECT_EQ(decoded->record_count, msg.record_count);
    EXPECT_EQ(decoded->data, msg.data);
}

TEST(QaGdb1218ReplicationRoundTrip, WalDataMessageWithEmptyDataRoundTrips) {
    WalDataMessage msg;
    msg.start_lsn = 0;
    msg.end_lsn = 0;
    msg.record_count = 0;

    auto bytes = serialize_wal_data(msg);
    auto decoded = deserialize_wal_data(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_TRUE(decoded->data.empty());
}

TEST(QaGdb1218ReplicationRoundTrip, KeepaliveMessageRoundTripsBothReplyFlags) {
    for (bool reply : {true, false}) {
        KeepaliveMessage msg;
        msg.current_lsn = 999;
        msg.timestamp_us = 123456789;
        msg.reply_requested = reply;

        auto bytes = serialize_keepalive(msg);
        auto decoded = deserialize_keepalive(bytes);
        ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
        EXPECT_EQ(decoded->current_lsn, msg.current_lsn);
        EXPECT_EQ(decoded->timestamp_us, msg.timestamp_us);
        EXPECT_EQ(decoded->reply_requested, reply);
    }
}

TEST(QaGdb1218ReplicationRoundTrip, SegmentStartMessageRoundTrips) {
    SegmentStartMessage msg;
    msg.segment_number = std::numeric_limits<uint64_t>::max();

    auto bytes = serialize_segment_start(msg);
    auto decoded = deserialize_segment_start(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(decoded->segment_number, msg.segment_number);
}

TEST(QaGdb1218ReplicationRoundTrip, CatchupCompleteMessageRoundTrips) {
    CatchupCompleteMessage msg;
    msg.current_lsn = 42;

    auto bytes = serialize_catchup_complete(msg);
    auto decoded = deserialize_catchup_complete(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(decoded->current_lsn, msg.current_lsn);
}

TEST(QaGdb1218ReplicationRoundTrip, StandbyStatusMessageRoundTrips) {
    StandbyStatusMessage msg;
    msg.received_lsn = 1;
    msg.applied_lsn = 2;
    msg.flushed_lsn = 3;
    msg.timestamp_us = 4;

    auto bytes = serialize_standby_status(msg);
    auto decoded = deserialize_standby_status(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(decoded->received_lsn, msg.received_lsn);
    EXPECT_EQ(decoded->applied_lsn, msg.applied_lsn);
    EXPECT_EQ(decoded->flushed_lsn, msg.flushed_lsn);
    EXPECT_EQ(decoded->timestamp_us, msg.timestamp_us);
}

TEST(QaGdb1218ReplicationRoundTrip, HotStandbyFeedbackMessageRoundTrips) {
    HotStandbyFeedbackMessage msg;
    msg.xmin = 77;
    msg.timestamp_us = 88;

    auto bytes = serialize_hot_standby_feedback(msg);
    auto decoded = deserialize_hot_standby_feedback(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(decoded->xmin, msg.xmin);
    EXPECT_EQ(decoded->timestamp_us, msg.timestamp_us);
}

TEST(QaGdb1218ReplicationRoundTrip, CorruptedPayloadFailsCrcCheck) {
    KeepaliveMessage msg;
    msg.current_lsn = 1;
    msg.timestamp_us = 2;
    msg.reply_requested = true;

    auto bytes = serialize_keepalive(msg);
    ASSERT_FALSE(bytes.empty());
    bytes[replication_header_size] ^= 0xFF; // corrupt current_lsn's first byte

    auto decoded = deserialize_keepalive(bytes);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, StatusCode::IO_ERROR);
}

TEST(QaGdb1218ReplicationRoundTrip, PeekMessageTypeMatchesSerializedHeader) {
    KeepaliveMessage msg;
    msg.current_lsn = 5;
    auto bytes = serialize_keepalive(msg);

    auto type = peek_message_type(bytes);
    ASSERT_TRUE(type.has_value());
    EXPECT_EQ(*type, ReplicationMessageType::KEEPALIVE);
}

TEST(QaGdb1218ReplicationRoundTrip, MessageTotalSizeMatchesActualBufferSize) {
    StandbyStatusMessage msg;
    msg.received_lsn = 1;
    msg.applied_lsn = 2;
    msg.flushed_lsn = 3;
    msg.timestamp_us = 4;
    auto bytes = serialize_standby_status(msg);

    auto size = message_total_size(bytes);
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(*size, bytes.size());
}

// ---------------------------------------------------------------------------
// Golden byte layout parity between the two call sites: given identical
// logical field values in the shared 8-byte lsn/uint64 slots, the raw bytes
// produced by both formats' calls into write_native<uint64_t> must be
// identical (proves no per-callsite divergence crept in during the dedup).
// ---------------------------------------------------------------------------

TEST(QaGdb1218CrossFormatParity, SharedUint64FieldProducesIdenticalBytesInBothFormats) {
    constexpr uint64_t kValue = 0xCAFEBABE12345678ULL;

    // WAL: lsn is the first 8-byte field after record_length (offset 4).
    WalRecord record;
    record.lsn = kValue;
    auto wal_bytes = serialize_wal_record(record);
    std::vector<uint8_t> wal_lsn_bytes(wal_bytes.begin() + 4, wal_bytes.begin() + 12);

    // Replication: current_lsn is the first 8-byte field after the 5-byte
    // header in KeepaliveMessage.
    KeepaliveMessage keepalive;
    keepalive.current_lsn = kValue;
    auto repl_bytes = serialize_keepalive(keepalive);
    std::vector<uint8_t> repl_lsn_bytes(repl_bytes.begin() + replication_header_size,
                                       repl_bytes.begin() + replication_header_size + 8);

    EXPECT_EQ(wal_lsn_bytes, repl_lsn_bytes);

    // Both must also match a raw memcpy of the native representation --
    // i.e., no endianness normalization was introduced in either call site.
    uint8_t raw[8];
    std::memcpy(raw, &kValue, sizeof(kValue));
    for (size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(wal_lsn_bytes[i], raw[i]) << "WAL byte mismatch at " << i;
        EXPECT_EQ(repl_lsn_bytes[i], raw[i]) << "Replication byte mismatch at " << i;
    }
}
