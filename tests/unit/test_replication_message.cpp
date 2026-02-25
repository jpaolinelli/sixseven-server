#include "giodb/server/replication_message.h"
#include "giodb/storage/wal_record.h"

#include <gtest/gtest.h>

using namespace giodb;

// -- Round-trip tests ---------------------------------------------------------

TEST(ReplicationMessage, WalDataRoundTrip) {
    WalDataMessage msg;
    msg.start_lsn = 100;
    msg.end_lsn = 200;
    msg.record_count = 3;
    msg.data = {0x01, 0x02, 0x03, 0x04, 0x05};

    auto buf = serialize_wal_data(msg);
    ASSERT_FALSE(buf.empty());

    auto result = deserialize_wal_data(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->start_lsn, 100u);
    EXPECT_EQ(result->end_lsn, 200u);
    EXPECT_EQ(result->record_count, 3u);
    EXPECT_EQ(result->data.size(), 5u);
    EXPECT_EQ(result->data, msg.data);
}

TEST(ReplicationMessage, WalDataEmptyPayload) {
    WalDataMessage msg;
    msg.start_lsn = 1;
    msg.end_lsn = 1;
    msg.record_count = 0;

    auto buf = serialize_wal_data(msg);
    auto result = deserialize_wal_data(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->record_count, 0u);
    EXPECT_TRUE(result->data.empty());
}

TEST(ReplicationMessage, WalDataWithSerializedRecords) {
    // Serialize actual WAL records and put them in a WalData message.
    WalRecord rec;
    rec.lsn = 42;
    rec.txn_id = 7;
    rec.type = WalRecordType::INSERT;
    rec.table_id = 1;
    rec.data = {0xAA, 0xBB};
    auto serialized_rec = serialize_wal_record(rec);

    WalDataMessage msg;
    msg.start_lsn = 42;
    msg.end_lsn = 42;
    msg.record_count = 1;
    msg.data = serialized_rec;

    auto buf = serialize_wal_data(msg);
    auto result = deserialize_wal_data(buf);
    ASSERT_TRUE(result.has_value());

    // Verify we can deserialize the embedded WAL record.
    auto inner = deserialize_wal_record(result->data);
    ASSERT_TRUE(inner.has_value());
    EXPECT_EQ(inner->lsn, 42u);
    EXPECT_EQ(inner->txn_id, 7u);
    EXPECT_EQ(inner->type, WalRecordType::INSERT);
}

TEST(ReplicationMessage, KeepaliveRoundTrip) {
    KeepaliveMessage msg;
    msg.current_lsn = 12345;
    msg.timestamp_us = 1700000000000000ULL;
    msg.reply_requested = true;

    auto buf = serialize_keepalive(msg);
    auto result = deserialize_keepalive(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->current_lsn, 12345u);
    EXPECT_EQ(result->timestamp_us, 1700000000000000ULL);
    EXPECT_TRUE(result->reply_requested);
}

TEST(ReplicationMessage, KeepaliveNoReply) {
    KeepaliveMessage msg;
    msg.current_lsn = 99;
    msg.timestamp_us = 42;
    msg.reply_requested = false;

    auto buf = serialize_keepalive(msg);
    auto result = deserialize_keepalive(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->reply_requested);
}

TEST(ReplicationMessage, SegmentStartRoundTrip) {
    SegmentStartMessage msg;
    msg.segment_number = 42;

    auto buf = serialize_segment_start(msg);
    auto result = deserialize_segment_start(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->segment_number, 42u);
}

TEST(ReplicationMessage, CatchupCompleteRoundTrip) {
    CatchupCompleteMessage msg;
    msg.current_lsn = 9999;

    auto buf = serialize_catchup_complete(msg);
    auto result = deserialize_catchup_complete(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->current_lsn, 9999u);
}

TEST(ReplicationMessage, StandbyStatusRoundTrip) {
    StandbyStatusMessage msg;
    msg.received_lsn = 100;
    msg.applied_lsn = 90;
    msg.flushed_lsn = 80;
    msg.timestamp_us = 1234567890ULL;

    auto buf = serialize_standby_status(msg);
    auto result = deserialize_standby_status(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->received_lsn, 100u);
    EXPECT_EQ(result->applied_lsn, 90u);
    EXPECT_EQ(result->flushed_lsn, 80u);
    EXPECT_EQ(result->timestamp_us, 1234567890ULL);
}

TEST(ReplicationMessage, HotStandbyFeedbackRoundTrip) {
    HotStandbyFeedbackMessage msg;
    msg.xmin = 55;
    msg.timestamp_us = 9876676710ULL;

    auto buf = serialize_hot_standby_feedback(msg);
    auto result = deserialize_hot_standby_feedback(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->xmin, 55u);
    EXPECT_EQ(result->timestamp_us, 9876676710ULL);
}

// -- CRC corruption tests -----------------------------------------------------

TEST(ReplicationMessage, KeepaliveCrcCorruption) {
    KeepaliveMessage msg;
    msg.current_lsn = 100;
    msg.timestamp_us = 42;
    msg.reply_requested = true;

    auto buf = serialize_keepalive(msg);
    // Corrupt a byte in the payload.
    buf[replication_header_size + 2] ^= 0xFF;

    auto result = deserialize_keepalive(buf);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(ReplicationMessage, WalDataCrcCorruption) {
    WalDataMessage msg;
    msg.start_lsn = 1;
    msg.end_lsn = 1;
    msg.record_count = 0;

    auto buf = serialize_wal_data(msg);
    // Corrupt CRC bytes at the end.
    buf.back() ^= 0xFF;

    auto result = deserialize_wal_data(buf);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(ReplicationMessage, StandbyStatusCrcCorruption) {
    StandbyStatusMessage msg;
    msg.received_lsn = 10;
    msg.applied_lsn = 8;
    msg.flushed_lsn = 6;
    msg.timestamp_us = 99;

    auto buf = serialize_standby_status(msg);
    buf[replication_header_size] ^= 0x01;

    auto result = deserialize_standby_status(buf);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

// -- Buffer-too-small tests ---------------------------------------------------

TEST(ReplicationMessage, EmptyBufferPeek) {
    std::span<const uint8_t> empty;
    auto result = peek_message_type(empty);
    ASSERT_FALSE(result.has_value());
}

TEST(ReplicationMessage, BufferTooSmallForHeader) {
    std::vector<uint8_t> buf = {0x01}; // type byte only, no payload_length
    auto result = message_total_size(buf);
    ASSERT_FALSE(result.has_value());
}

TEST(ReplicationMessage, BufferTooSmallForPayload) {
    KeepaliveMessage msg;
    msg.current_lsn = 1;
    auto buf = serialize_keepalive(msg);
    // Truncate the buffer.
    buf.resize(replication_header_size + 2);

    auto result = deserialize_keepalive(buf);
    ASSERT_FALSE(result.has_value());
}

TEST(ReplicationMessage, WrongMessageType) {
    KeepaliveMessage msg;
    msg.current_lsn = 1;
    auto buf = serialize_keepalive(msg);

    // Try to deserialize as StandbyStatus.
    auto result = deserialize_standby_status(buf);
    ASSERT_FALSE(result.has_value());
}

// -- Peek and total size ------------------------------------------------------

TEST(ReplicationMessage, PeekMessageType) {
    auto buf = serialize_keepalive({});
    auto type = peek_message_type(buf);
    ASSERT_TRUE(type.has_value());
    EXPECT_EQ(*type, ReplicationMessageType::KEEPALIVE);
}

TEST(ReplicationMessage, PeekInvalidType) {
    std::vector<uint8_t> buf = {0xFF};
    auto result = peek_message_type(buf);
    ASSERT_FALSE(result.has_value());
}

TEST(ReplicationMessage, MessageTotalSize) {
    auto buf = serialize_keepalive({});
    auto size = message_total_size(buf);
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(*size, buf.size());
}

TEST(ReplicationMessage, MessageTotalSizeWalData) {
    WalDataMessage msg;
    msg.start_lsn = 1;
    msg.end_lsn = 10;
    msg.record_count = 2;
    msg.data = {0x01, 0x02, 0x03};

    auto buf = serialize_wal_data(msg);
    auto size = message_total_size(buf);
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(*size, buf.size());
}

// -- Type name ----------------------------------------------------------------

TEST(ReplicationMessage, TypeNames) {
    EXPECT_STREQ(replication_message_type_name(ReplicationMessageType::WAL_DATA), "WAL_DATA");
    EXPECT_STREQ(replication_message_type_name(ReplicationMessageType::KEEPALIVE), "KEEPALIVE");
    EXPECT_STREQ(replication_message_type_name(ReplicationMessageType::SEGMENT_START),
                 "SEGMENT_START");
    EXPECT_STREQ(replication_message_type_name(ReplicationMessageType::CATCHUP_COMPLETE),
                 "CATCHUP_COMPLETE");
    EXPECT_STREQ(replication_message_type_name(ReplicationMessageType::STANDBY_STATUS),
                 "STANDBY_STATUS");
    EXPECT_STREQ(replication_message_type_name(ReplicationMessageType::HOT_STANDBY_FEEDBACK),
                 "HOT_STANDBY_FEEDBACK");
}
