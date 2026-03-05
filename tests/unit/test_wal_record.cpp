#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace sixseven;

// =============================================================================
// Serialization Helpers
// =============================================================================

/// Build a basic WAL record with the given type and optional data.
static WalRecord make_record(WalRecordType type,
                             lsn_t lsn = 1,
                             txn_id_t txn_id = 100,
                             std::vector<uint8_t> data = {}) {
    WalRecord r;
    r.lsn = lsn;
    r.txn_id = txn_id;
    r.prev_lsn = invalid_lsn;
    r.type = type;
    r.table_id = 0;
    r.page_id = 0;
    r.slot_id = 0;
    r.data = std::move(data);
    return r;
}

/// Verify that two WalRecords have identical field values.
static void expect_records_equal(const WalRecord& a, const WalRecord& b) {
    EXPECT_EQ(a.lsn, b.lsn);
    EXPECT_EQ(a.txn_id, b.txn_id);
    EXPECT_EQ(a.prev_lsn, b.prev_lsn);
    EXPECT_EQ(a.type, b.type);
    EXPECT_EQ(a.table_id, b.table_id);
    EXPECT_EQ(a.page_id, b.page_id);
    EXPECT_EQ(a.slot_id, b.slot_id);
    ASSERT_EQ(a.data.size(), b.data.size());
    EXPECT_TRUE(std::equal(a.data.begin(), a.data.end(), b.data.begin()));
}

// =============================================================================
// Round-Trip Serialization Tests
// =============================================================================

TEST(WalRecord, SerializeDeserializeBegin) {
    auto record = make_record(WalRecordType::BEGIN, 1, 42);
    auto bytes = serialize_wal_record(record);
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value());
    expect_records_equal(record, *result);
}

TEST(WalRecord, SerializeDeserializeCommit) {
    auto record = make_record(WalRecordType::COMMIT, 10, 42);
    record.prev_lsn = 1;
    auto bytes = serialize_wal_record(record);
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value());
    expect_records_equal(record, *result);
}

TEST(WalRecord, SerializeDeserializeAbort) {
    auto record = make_record(WalRecordType::ABORT, 15, 42);
    record.prev_lsn = 10;
    auto bytes = serialize_wal_record(record);
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value());
    expect_records_equal(record, *result);
}

TEST(WalRecord, SerializeDeserializeInsert) {
    auto tuple_data = std::vector<uint8_t>(100, 0xAB);
    auto record = make_record(WalRecordType::INSERT, 5, 42, tuple_data);
    record.table_id = 1;
    record.page_id = 10;
    record.slot_id = 3;
    record.prev_lsn = 1;

    auto bytes = serialize_wal_record(record);
    EXPECT_EQ(bytes.size(), wal_record_overhead + 100);

    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value());
    expect_records_equal(record, *result);
}

TEST(WalRecord, SerializeDeserializeUpdate) {
    // UPDATE carries before-image + after-image as data.
    std::vector<uint8_t> data;
    // before-image: 50 bytes of 0x11
    data.insert(data.end(), 50, 0x11);
    // after-image: 50 bytes of 0x22
    data.insert(data.end(), 50, 0x22);

    auto record = make_record(WalRecordType::UPDATE, 7, 42, data);
    record.table_id = 2;
    record.page_id = 20;
    record.slot_id = 5;
    record.prev_lsn = 5;

    auto bytes = serialize_wal_record(record);
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value());
    expect_records_equal(record, *result);

    // Verify the data payload is intact.
    const auto& d = result->data;
    ASSERT_EQ(d.size(), 100u);
    EXPECT_EQ(d[0], 0x11);
    EXPECT_EQ(d[49], 0x11);
    EXPECT_EQ(d[50], 0x22);
    EXPECT_EQ(d[99], 0x22);
}

TEST(WalRecord, SerializeDeserializeDelete) {
    // DELETE carries before-image as data.
    auto before_image = std::vector<uint8_t>(80, 0xDE);
    auto record = make_record(WalRecordType::DELETE, 9, 42, before_image);
    record.table_id = 1;
    record.page_id = 10;
    record.slot_id = 3;
    record.prev_lsn = 7;

    auto bytes = serialize_wal_record(record);
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value());
    expect_records_equal(record, *result);
}

TEST(WalRecord, SerializeDeserializePageSplit) {
    // PAGE_SPLIT might carry split point data.
    auto split_data = std::vector<uint8_t>(200, 0xCC);
    auto record = make_record(WalRecordType::PAGE_SPLIT, 20, 42, split_data);
    record.table_id = 3;
    record.page_id = 50;

    auto bytes = serialize_wal_record(record);
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value());
    expect_records_equal(record, *result);
}

TEST(WalRecord, SerializeDeserializeCheckpoint) {
    // CHECKPOINT might carry a list of active transaction IDs.
    std::vector<uint8_t> active_txns;
    // Encode 3 txn_ids as raw bytes.
    for (uint64_t txn : {100ULL, 200ULL, 300ULL}) {
        auto* p = reinterpret_cast<const uint8_t*>(&txn);
        active_txns.insert(active_txns.end(), p, p + sizeof(uint64_t));
    }

    auto record = make_record(WalRecordType::CHECKPOINT, 50, 0, active_txns);
    auto bytes = serialize_wal_record(record);
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value());
    expect_records_equal(record, *result);
}

TEST(WalRecord, SerializeDeserializeCreateTable) {
    // CREATE_TABLE might carry the table name.
    std::string name = "users";
    auto data = std::vector<uint8_t>(name.begin(), name.end());
    auto record = make_record(WalRecordType::CREATE_TABLE, 2, 42, data);
    record.table_id = 10;

    auto bytes = serialize_wal_record(record);
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value());
    expect_records_equal(record, *result);

    // Verify table name in data payload.
    std::string recovered(result->data.begin(), result->data.end());
    EXPECT_EQ(recovered, "users");
}

TEST(WalRecord, SerializeDeserializeDropTable) {
    auto record = make_record(WalRecordType::DROP_TABLE, 30, 42);
    record.table_id = 10;

    auto bytes = serialize_wal_record(record);
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value());
    expect_records_equal(record, *result);
}

// =============================================================================
// Size and Format Tests
// =============================================================================

TEST(WalRecord, EmptyDataRecordHasCorrectSize) {
    auto record = make_record(WalRecordType::BEGIN);
    auto bytes = serialize_wal_record(record);
    EXPECT_EQ(bytes.size(), wal_record_overhead);
    EXPECT_EQ(bytes.size(), serialized_wal_record_size(record));
}

TEST(WalRecord, RecordWithDataHasCorrectSize) {
    auto data = std::vector<uint8_t>(256, 0xFF);
    auto record = make_record(WalRecordType::INSERT, 1, 1, data);
    auto bytes = serialize_wal_record(record);
    EXPECT_EQ(bytes.size(), wal_record_overhead + 256);
    EXPECT_EQ(bytes.size(), serialized_wal_record_size(record));
}

TEST(WalRecord, LengthPrefixIsCorrect) {
    auto data = std::vector<uint8_t>(100, 0xAA);
    auto record = make_record(WalRecordType::INSERT, 1, 1, data);
    auto bytes = serialize_wal_record(record);

    // First 4 bytes = record_length = total_size - 4.
    uint32_t record_length = 0;
    std::memcpy(&record_length, bytes.data(), sizeof(uint32_t));
    EXPECT_EQ(record_length, static_cast<uint32_t>(bytes.size() - sizeof(uint32_t)));
}

// =============================================================================
// Field Preservation Tests
// =============================================================================

TEST(WalRecord, AllFieldsPreserved) {
    WalRecord record;
    record.lsn = 123456789;
    record.txn_id = 987667671;
    record.prev_lsn = 123456788;
    record.type = WalRecordType::UPDATE;
    record.table_id = 42;
    record.page_id = 1000;
    record.slot_id = 65535;
    record.data = {0x01, 0x02, 0x03, 0x04, 0x05};

    auto bytes = serialize_wal_record(record);
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value());
    expect_records_equal(record, *result);
}

TEST(WalRecord, MaxSlotIdPreserved) {
    WalRecord record;
    record.lsn = 1;
    record.txn_id = 1;
    record.type = WalRecordType::INSERT;
    record.slot_id = 65535; // max uint16_t

    auto bytes = serialize_wal_record(record);
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->slot_id, 65535);
}

TEST(WalRecord, LargeLsnPreserved) {
    WalRecord record;
    record.lsn = UINT64_MAX;
    record.txn_id = UINT64_MAX;
    record.prev_lsn = UINT64_MAX - 1;
    record.type = WalRecordType::COMMIT;

    auto bytes = serialize_wal_record(record);
    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->lsn, UINT64_MAX);
    EXPECT_EQ(result->txn_id, UINT64_MAX);
    EXPECT_EQ(result->prev_lsn, UINT64_MAX - 1);
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST(WalRecord, DeserializeEmptyBufferFails) {
    auto result = deserialize_wal_record({});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(WalRecord, DeserializeTruncatedBufferFails) {
    auto record = make_record(WalRecordType::INSERT, 1, 1, {0x01, 0x02, 0x03});
    auto bytes = serialize_wal_record(record);

    // Truncate the buffer.
    auto truncated = std::vector<uint8_t>(bytes.begin(), bytes.begin() + 20);
    auto result = deserialize_wal_record(truncated);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(WalRecord, DeserializeCorruptCrcFails) {
    auto record = make_record(WalRecordType::COMMIT, 1, 42);
    auto bytes = serialize_wal_record(record);

    // Corrupt the CRC (last 4 bytes).
    bytes[bytes.size() - 1] ^= 0xFF;

    auto result = deserialize_wal_record(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(WalRecord, DeserializeCorruptDataFails) {
    auto data = std::vector<uint8_t>(50, 0xAA);
    auto record = make_record(WalRecordType::INSERT, 1, 42, data);
    auto bytes = serialize_wal_record(record);

    // Corrupt a data byte in the middle of the record.
    bytes[bytes.size() / 2] ^= 0xFF;

    auto result = deserialize_wal_record(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST(WalRecord, DeserializeBufferTooSmallForDeclaredLength) {
    auto record = make_record(WalRecordType::INSERT, 1, 1, std::vector<uint8_t>(200, 0xBB));
    auto bytes = serialize_wal_record(record);

    // Give a buffer that has the length prefix claiming a large size,
    // but truncate the actual data.
    auto partial = std::vector<uint8_t>(bytes.begin(), bytes.begin() + 60);
    auto result = deserialize_wal_record(partial);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// WalRecordType Name Tests
// =============================================================================

TEST(WalRecord, RecordTypeNames) {
    EXPECT_STREQ(wal_record_type_name(WalRecordType::BEGIN), "BEGIN");
    EXPECT_STREQ(wal_record_type_name(WalRecordType::INSERT), "INSERT");
    EXPECT_STREQ(wal_record_type_name(WalRecordType::UPDATE), "UPDATE");
    EXPECT_STREQ(wal_record_type_name(WalRecordType::DELETE), "DELETE");
    EXPECT_STREQ(wal_record_type_name(WalRecordType::PAGE_SPLIT), "PAGE_SPLIT");
    EXPECT_STREQ(wal_record_type_name(WalRecordType::COMMIT), "COMMIT");
    EXPECT_STREQ(wal_record_type_name(WalRecordType::ABORT), "ABORT");
    EXPECT_STREQ(wal_record_type_name(WalRecordType::CHECKPOINT), "CHECKPOINT");
    EXPECT_STREQ(wal_record_type_name(WalRecordType::CREATE_TABLE), "CREATE_TABLE");
    EXPECT_STREQ(wal_record_type_name(WalRecordType::DROP_TABLE), "DROP_TABLE");
}

// =============================================================================
// Large Data Tests
// =============================================================================

TEST(WalRecord, LargeDataPayload) {
    // 64KB data payload.
    auto data = std::vector<uint8_t>(65536, 0x42);
    auto record = make_record(WalRecordType::INSERT, 1, 1, data);
    record.table_id = 5;
    record.page_id = 100;
    record.slot_id = 0;

    auto bytes = serialize_wal_record(record);
    EXPECT_EQ(bytes.size(), wal_record_overhead + 65536);

    auto result = deserialize_wal_record(bytes);
    ASSERT_TRUE(result.has_value());
    expect_records_equal(record, *result);
}

// =============================================================================
// Consecutive Record Deserialization
// =============================================================================

TEST(WalRecord, MultipleRecordsInSequence) {
    // Simulate reading multiple consecutive records from a buffer.
    std::vector<uint8_t> combined;

    WalRecord r1 = make_record(WalRecordType::BEGIN, 1, 100);
    WalRecord r2 = make_record(WalRecordType::INSERT, 2, 100, {0x01, 0x02, 0x03});
    r2.table_id = 1;
    r2.page_id = 5;
    r2.slot_id = 0;
    r2.prev_lsn = 1;
    WalRecord r3 = make_record(WalRecordType::COMMIT, 3, 100);
    r3.prev_lsn = 2;

    auto b1 = serialize_wal_record(r1);
    auto b2 = serialize_wal_record(r2);
    auto b3 = serialize_wal_record(r3);

    combined.insert(combined.end(), b1.begin(), b1.end());
    combined.insert(combined.end(), b2.begin(), b2.end());
    combined.insert(combined.end(), b3.begin(), b3.end());

    // Deserialize each record by reading the length prefix to advance.
    size_t offset = 0;

    // Record 1.
    auto result1 = deserialize_wal_record({combined.data() + offset, combined.size() - offset});
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(result1->type, WalRecordType::BEGIN);
    offset += serialized_wal_record_size(*result1);

    // Record 2.
    auto result2 = deserialize_wal_record({combined.data() + offset, combined.size() - offset});
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2->type, WalRecordType::INSERT);
    EXPECT_EQ(result2->data.size(), 3u);
    offset += serialized_wal_record_size(*result2);

    // Record 3.
    auto result3 = deserialize_wal_record({combined.data() + offset, combined.size() - offset});
    ASSERT_TRUE(result3.has_value());
    EXPECT_EQ(result3->type, WalRecordType::COMMIT);
    EXPECT_EQ(result3->prev_lsn, 2u);
}
