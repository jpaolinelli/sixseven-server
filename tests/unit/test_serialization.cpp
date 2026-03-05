#include "sixseven/storage/serialization.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace sixseven;

// -- Helper: round-trip a value through serialize/deserialize ------------------

static void round_trip(const Value& original, TypeId type) {
    auto bytes = serialize(original);
    ASSERT_EQ(bytes.size(), serialized_size(original));

    auto result = deserialize(bytes, type);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // For null values, just check is_null.
    if (original.is_null()) {
        EXPECT_TRUE(result->is_null());
        return;
    }

    EXPECT_EQ(result->type_id(), type);
    EXPECT_EQ(result->data(), original.data());
}

// -- NULL ---------------------------------------------------------------------

TEST(Serialization, NullRoundTrip) {
    auto null_val = Value::make_null();
    auto bytes = serialize(null_val);
    EXPECT_EQ(bytes.size(), 1u);
    EXPECT_EQ(bytes[0], 0x00);
    EXPECT_EQ(serialized_size(null_val), 1u);

    // Deserialize with any type should return null.
    auto result = deserialize(bytes, TypeId::INT32);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_null());
}

// -- Integer types ------------------------------------------------------------

TEST(Serialization, Int8RoundTrip) {
    round_trip(Value(int8_t{-128}), TypeId::INT8);
    round_trip(Value(int8_t{0}), TypeId::INT8);
    round_trip(Value(int8_t{127}), TypeId::INT8);
}

TEST(Serialization, Int16RoundTrip) {
    round_trip(Value(int16_t{-32768}), TypeId::INT16);
    round_trip(Value(int16_t{0}), TypeId::INT16);
    round_trip(Value(int16_t{32767}), TypeId::INT16);
}

TEST(Serialization, Int32RoundTrip) {
    round_trip(Value(int32_t{-2147483648}), TypeId::INT32);
    round_trip(Value(int32_t{0}), TypeId::INT32);
    round_trip(Value(int32_t{2147483647}), TypeId::INT32);
}

TEST(Serialization, Int64RoundTrip) {
    round_trip(Value(std::numeric_limits<int64_t>::min()), TypeId::INT64);
    round_trip(Value(int64_t{0}), TypeId::INT64);
    round_trip(Value(std::numeric_limits<int64_t>::max()), TypeId::INT64);
}

TEST(Serialization, Uint8RoundTrip) {
    round_trip(Value(uint8_t{0}), TypeId::UINT8);
    round_trip(Value(uint8_t{255}), TypeId::UINT8);
}

TEST(Serialization, Uint16RoundTrip) {
    round_trip(Value(uint16_t{0}), TypeId::UINT16);
    round_trip(Value(uint16_t{65535}), TypeId::UINT16);
}

TEST(Serialization, Uint32RoundTrip) {
    round_trip(Value(uint32_t{0}), TypeId::UINT32);
    round_trip(Value(uint32_t{4294967295U}), TypeId::UINT32);
}

TEST(Serialization, Uint64RoundTrip) {
    round_trip(Value(uint64_t{0}), TypeId::UINT64);
    round_trip(Value(std::numeric_limits<uint64_t>::max()), TypeId::UINT64);
}

// -- Floating-point types -----------------------------------------------------

TEST(Serialization, Float32RoundTrip) {
    round_trip(Value(0.0f), TypeId::FLOAT32);
    round_trip(Value(3.14f), TypeId::FLOAT32);
    round_trip(Value(-1.0f), TypeId::FLOAT32);
    round_trip(Value(std::numeric_limits<float>::infinity()), TypeId::FLOAT32);
    round_trip(Value(-std::numeric_limits<float>::infinity()), TypeId::FLOAT32);
}

TEST(Serialization, Float32NaN) {
    Value nan_val(std::numeric_limits<float>::quiet_NaN());
    auto bytes = serialize(nan_val);
    auto result = deserialize(bytes, TypeId::FLOAT32);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(std::isnan(result->as_float32()));
}

TEST(Serialization, Float64RoundTrip) {
    round_trip(Value(0.0), TypeId::FLOAT64);
    round_trip(Value(2.718281828459045), TypeId::FLOAT64);
    round_trip(Value(-1.0), TypeId::FLOAT64);
    round_trip(Value(std::numeric_limits<double>::infinity()), TypeId::FLOAT64);
    round_trip(Value(-std::numeric_limits<double>::infinity()), TypeId::FLOAT64);
}

TEST(Serialization, Float64NaN) {
    Value nan_val(std::numeric_limits<double>::quiet_NaN());
    auto bytes = serialize(nan_val);
    auto result = deserialize(bytes, TypeId::FLOAT64);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(std::isnan(result->as_float64()));
}

// -- Decimal ------------------------------------------------------------------

TEST(Serialization, DecimalRoundTrip) {
    round_trip(Value(Decimal128{0, 0}), TypeId::DECIMAL);
    round_trip(Value(Decimal128{1, 500}), TypeId::DECIMAL);
    round_trip(Value(Decimal128{-1, 18446744073709551615ULL}), TypeId::DECIMAL);
}

// -- Bool ---------------------------------------------------------------------

TEST(Serialization, BoolRoundTrip) {
    round_trip(Value(true), TypeId::BOOL);
    round_trip(Value(false), TypeId::BOOL);
}

// -- String -------------------------------------------------------------------

TEST(Serialization, StringRoundTrip) {
    round_trip(Value(std::string{"hello world"}), TypeId::STRING);
    round_trip(Value(std::string{""}), TypeId::STRING);
    round_trip(Value(std::string{std::string(10000, 'x')}), TypeId::STRING);
}

TEST(Serialization, StringWithNullBytes) {
    std::string s("ab\0cd", 5);
    round_trip(Value(std::move(s)), TypeId::STRING);
}

// -- Blob ---------------------------------------------------------------------

TEST(Serialization, BlobRoundTrip) {
    round_trip(Value(Blob{0x01, 0x02, 0xFF}), TypeId::BLOB);
    round_trip(Value(Blob{}), TypeId::BLOB);
}

// -- Date ---------------------------------------------------------------------

TEST(Serialization, DateRoundTrip) {
    round_trip(Value(Date{0}), TypeId::DATE);
    round_trip(Value(Date{19723}), TypeId::DATE);
    round_trip(Value(Date{-1}), TypeId::DATE);
}

// -- Time ---------------------------------------------------------------------

TEST(Serialization, TimeRoundTrip) {
    round_trip(Value(Time{0}), TypeId::TIME);
    round_trip(Value(Time{43200000000LL}), TypeId::TIME);
    round_trip(Value(Time{86399999999LL}), TypeId::TIME);
}

// -- Timestamp ----------------------------------------------------------------

TEST(Serialization, TimestampRoundTrip) {
    round_trip(Value(Timestamp{0}), TypeId::TIMESTAMP);
    round_trip(Value(Timestamp{1703980800000000LL}), TypeId::TIMESTAMP);
    round_trip(Value(Timestamp{-1}), TypeId::TIMESTAMP);
}

// -- Interval -----------------------------------------------------------------

TEST(Serialization, IntervalRoundTrip) {
    round_trip(Value(Interval{0, 0}), TypeId::INTERVAL);
    round_trip(Value(Interval{2, 86400000000LL}), TypeId::INTERVAL);
    round_trip(Value(Interval{-12, -1}), TypeId::INTERVAL);
}

// -- Point --------------------------------------------------------------------

TEST(Serialization, PointRoundTrip) {
    round_trip(Value(Point{0.0, 0.0}), TypeId::POINT);
    round_trip(Value(Point{-73.935242, 40.730610}), TypeId::POINT);
}

// -- JSON ---------------------------------------------------------------------

TEST(Serialization, JsonRoundTrip) {
    round_trip(Value(JsonString{R"({"key": "value"})"}), TypeId::JSON);
    round_trip(Value(JsonString{""}), TypeId::JSON);
}

// -- UUID ---------------------------------------------------------------------

TEST(Serialization, UuidRoundTrip) {
    Uuid u = {0x55,
              0x0e,
              0x84,
              0x00,
              0xe2,
              0x9b,
              0x41,
              0xd4,
              0xa7,
              0x16,
              0x44,
              0x66,
              0x55,
              0x44,
              0x00,
              0x00};
    round_trip(Value(u), TypeId::UUID);

    // All zeros
    Uuid zero = {};
    round_trip(Value(zero), TypeId::UUID);

    // All 0xFF
    Uuid full;
    full.fill(0xFF);
    round_trip(Value(full), TypeId::UUID);
}

// -- Embedding ----------------------------------------------------------------

TEST(Serialization, EmbeddingRoundTrip) {
    round_trip(Value(Embedding{0.1f, 0.2f, 0.3f, 0.4f}), TypeId::EMBEDDING);
    round_trip(Value(Embedding{}), TypeId::EMBEDDING);
}

// -- Serialized size ----------------------------------------------------------

TEST(Serialization, SerializedSizeNull) {
    EXPECT_EQ(serialized_size(Value::make_null()), 1u);
}

TEST(Serialization, SerializedSizeFixed) {
    // null flag (1) + data size
    EXPECT_EQ(serialized_size(Value(int8_t{0})), 2u);
    EXPECT_EQ(serialized_size(Value(int16_t{0})), 3u);
    EXPECT_EQ(serialized_size(Value(int32_t{0})), 5u);
    EXPECT_EQ(serialized_size(Value(int64_t{0})), 9u);
    EXPECT_EQ(serialized_size(Value(uint8_t{0})), 2u);
    EXPECT_EQ(serialized_size(Value(uint16_t{0})), 3u);
    EXPECT_EQ(serialized_size(Value(uint32_t{0})), 5u);
    EXPECT_EQ(serialized_size(Value(uint64_t{0})), 9u);
    EXPECT_EQ(serialized_size(Value(0.0f)), 5u);
    EXPECT_EQ(serialized_size(Value(0.0)), 9u);
    EXPECT_EQ(serialized_size(Value(Decimal128{})), 17u);
    EXPECT_EQ(serialized_size(Value(true)), 2u);
    EXPECT_EQ(serialized_size(Value(Date{})), 5u);
    EXPECT_EQ(serialized_size(Value(Time{})), 9u);
    EXPECT_EQ(serialized_size(Value(Timestamp{})), 9u);
    EXPECT_EQ(serialized_size(Value(Interval{})), 17u);
    EXPECT_EQ(serialized_size(Value(Point{})), 17u);

    Uuid u = {};
    EXPECT_EQ(serialized_size(Value(u)), 17u);
}

TEST(Serialization, SerializedSizeVariable) {
    // null flag (1) + length prefix (4) + data
    EXPECT_EQ(serialized_size(Value(std::string{"hello"})), 1u + 4u + 5u);
    EXPECT_EQ(serialized_size(Value(std::string{""})), 1u + 4u + 0u);
    EXPECT_EQ(serialized_size(Value(Blob{1, 2, 3})), 1u + 4u + 3u);
    EXPECT_EQ(serialized_size(Value(JsonString{"{}"})), 1u + 4u + 2u);
    EXPECT_EQ(serialized_size(Value(Embedding{1.0f, 2.0f})), 1u + 4u + 8u);
}

// -- Error handling -----------------------------------------------------------

TEST(Serialization, DeserializeEmptyBuffer) {
    std::vector<uint8_t> empty;
    auto result = deserialize(empty, TypeId::INT32);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(Serialization, DeserializeInvalidNullFlag) {
    std::vector<uint8_t> bad = {0x02};
    auto result = deserialize(bad, TypeId::INT32);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(Serialization, DeserializeTruncatedData) {
    // Not enough bytes for an INT32 (need 4 after flag, only have 2).
    std::vector<uint8_t> truncated = {0x01, 0x00, 0x00};
    auto result = deserialize(truncated, TypeId::INT32);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// -- Little-endian encoding verification --------------------------------------

TEST(Serialization, LittleEndianEncoding) {
    // INT32 value 0x04030201 should be stored as bytes [01, 02, 03, 04] in LE.
    Value v(int32_t{0x04030201});
    auto bytes = serialize(v);
    ASSERT_EQ(bytes.size(), 5u);
    EXPECT_EQ(bytes[0], 0x01); // null flag
    EXPECT_EQ(bytes[1], 0x01); // least significant byte
    EXPECT_EQ(bytes[2], 0x02);
    EXPECT_EQ(bytes[3], 0x03);
    EXPECT_EQ(bytes[4], 0x04); // most significant byte
}
