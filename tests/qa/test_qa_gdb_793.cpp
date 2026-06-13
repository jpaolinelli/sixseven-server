#include "sixseven/common/status.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

using namespace sixseven;

// -- GDB-793: TupleSerializer::serialize must validate value types against schema
//
// Before this fix, serialize() sized the buffer from the schema (fixed_region_size())
// but wrote bytes from the value's actual type (via write_fixed_value -> memcpy of
// value.type_id() width). A 16-byte value (DECIMAL/UUID/POINT/INTERVAL) in a 4-byte
// schema column (INT32) overwrote 12 bytes past the column slot, corrupting adjacent
// data or overflowing the buffer. A 1-byte value in an 8-byte column left 7 stale
// bytes that deserialize() then read as schema type, producing wrong values.
//
// The fix adds a per-column type check in serialize() before any buffer access.
// Mutation evidence for each test: comment out the check in serialize() and the test
// either (a) returns wrong deserialized data or (b) triggers an ASan heap-buffer-overflow.

// ---------------------------------------------------------------------------
// Oversize type into a narrower schema column must return TYPE_ERROR.
// Mutation evidence: without the check, a Decimal128 (16 bytes) written into an
// INT32 slot (4 bytes) overwrites 12 bytes past the column — adjacent slots or
// past the buffer end (ASan-detectable). With the check, we get TYPE_ERROR.
// ---------------------------------------------------------------------------
TEST(GDB793, OversizeTypeIntoSmallColumnReturnsTypeError) {
    Schema schema({{"amount", TypeId::INT32}});

    // Provide a DECIMAL value (16 bytes) for an INT32 column (4 bytes).
    std::vector<Value> values = {Value(Decimal128{42, 0})};

    auto result = TupleSerializer::serialize(values, schema);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    // Error message must identify column name and both types.
    EXPECT_NE(result.error().message.find("amount"), std::string::npos);
    EXPECT_NE(result.error().message.find("INT32"), std::string::npos);
    EXPECT_NE(result.error().message.find("DECIMAL"), std::string::npos);
}

TEST(GDB793, UuidIntoInt32ColumnReturnsTypeError) {
    Schema schema({{"id", TypeId::INT32}});

    Uuid u{};
    u.fill(0xAB);
    std::vector<Value> values = {Value(u)};

    auto result = TupleSerializer::serialize(values, schema);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_NE(result.error().message.find("id"), std::string::npos);
    EXPECT_NE(result.error().message.find("INT32"), std::string::npos);
    EXPECT_NE(result.error().message.find("UUID"), std::string::npos);
}

TEST(GDB793, PointIntoInt32ColumnReturnsTypeError) {
    Schema schema({{"coord", TypeId::INT32}});

    std::vector<Value> values = {Value(Point{1.0, 2.0})};

    auto result = TupleSerializer::serialize(values, schema);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(GDB793, IntervalIntoInt64ColumnReturnsTypeError) {
    Schema schema({{"dur", TypeId::INT64}});

    std::vector<Value> values = {Value(Interval{12, 86400000000LL})};

    auto result = TupleSerializer::serialize(values, schema);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_NE(result.error().message.find("INTERVAL"), std::string::npos);
    EXPECT_NE(result.error().message.find("INT64"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Undersize type into a wider schema column must return TYPE_ERROR.
// Mutation evidence: without the check, an INT8 (1 byte) written into an INT64
// slot (8 bytes) leaves 7 stale zero bytes; deserialize() reads them as INT64,
// returning the int8 value verbatim (accidentally correct for 0, wrong for other
// values after adjacent writes). With the check, TYPE_ERROR is returned.
// ---------------------------------------------------------------------------
TEST(GDB793, UndersizeTypeIntoWiderColumnReturnsTypeError) {
    Schema schema({{"score", TypeId::INT64}});

    // Provide an INT8 value (1 byte) for an INT64 column (8 bytes).
    std::vector<Value> values = {Value(static_cast<int8_t>(99))};

    auto result = TupleSerializer::serialize(values, schema);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_NE(result.error().message.find("score"), std::string::npos);
    EXPECT_NE(result.error().message.find("INT64"), std::string::npos);
    EXPECT_NE(result.error().message.find("INT8"), std::string::npos);
}

TEST(GDB793, Int32IntoFloat64ColumnReturnsTypeError) {
    Schema schema({{"val", TypeId::FLOAT64}});

    std::vector<Value> values = {Value(static_cast<int32_t>(7))};

    auto result = TupleSerializer::serialize(values, schema);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// ---------------------------------------------------------------------------
// NULL values must always pass the check regardless of column type.
// Mutation evidence: a NULL in any column must not trigger TYPE_ERROR.
// ---------------------------------------------------------------------------
TEST(GDB793, NullValuePassesTypeCheck) {
    Schema schema({{"id", TypeId::INT32}, {"name", TypeId::STRING}, {"score", TypeId::FLOAT64}});

    std::vector<Value> values = {Value::make_null(), Value::make_null(), Value::make_null()};

    auto result = TupleSerializer::serialize(values, schema);

    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST(GDB793, MixedNullAndMatchedValuesPass) {
    Schema schema({{"id", TypeId::INT32}, {"name", TypeId::STRING}, {"active", TypeId::BOOL}});

    std::vector<Value> values = {
        Value(static_cast<int32_t>(42)),
        Value::make_null(),
        Value(true),
    };

    auto result = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

// ---------------------------------------------------------------------------
// Matched types must round-trip correctly.
// ---------------------------------------------------------------------------
TEST(GDB793, MatchedFixedTypesRoundTrip) {
    Schema schema({
        {"a", TypeId::INT32},
        {"b", TypeId::INT64},
        {"c", TypeId::FLOAT32},
        {"d", TypeId::BOOL},
    });

    std::vector<Value> values = {
        Value(static_cast<int32_t>(123)),
        Value(static_cast<int64_t>(9876543210LL)),
        Value(3.14f),
        Value(true),
    };

    auto serialized = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(serialized.has_value()) << serialized.error().message;

    auto deserialized = TupleSerializer::deserialize(*serialized, schema);
    ASSERT_TRUE(deserialized.has_value()) << deserialized.error().message;

    ASSERT_EQ(deserialized->size(), 4u);
    EXPECT_EQ((*deserialized)[0].as_int32(), 123);
    EXPECT_EQ((*deserialized)[1].as_int64(), 9876543210LL);
    EXPECT_FLOAT_EQ((*deserialized)[2].as_float32(), 3.14f);
    EXPECT_EQ((*deserialized)[3].as_bool(), true);
}

TEST(GDB793, MatchedLargeFixedTypesRoundTrip) {
    Schema schema({
        {"dec", TypeId::DECIMAL},
        {"uuid", TypeId::UUID},
        {"pt", TypeId::POINT},
        {"iv", TypeId::INTERVAL},
    });

    Uuid u{};
    for (size_t i = 0; i < 16; ++i) {
        u[i] = static_cast<uint8_t>(i);
    }

    std::vector<Value> values = {
        Value(Decimal128{100LL, 200ULL}),
        Value(u),
        Value(Point{1.5, 2.5}),
        Value(Interval{6, 3600000000LL}),
    };

    auto serialized = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(serialized.has_value()) << serialized.error().message;

    auto deserialized = TupleSerializer::deserialize(*serialized, schema);
    ASSERT_TRUE(deserialized.has_value()) << deserialized.error().message;

    ASSERT_EQ(deserialized->size(), 4u);
    EXPECT_EQ((*deserialized)[0].as_decimal().hi, 100LL);
    EXPECT_EQ((*deserialized)[0].as_decimal().lo, 200ULL);
    EXPECT_EQ((*deserialized)[1].as_uuid(), u);
    EXPECT_DOUBLE_EQ((*deserialized)[2].as_point().x, 1.5);
    EXPECT_DOUBLE_EQ((*deserialized)[2].as_point().y, 2.5);
    EXPECT_EQ((*deserialized)[3].as_interval().months, 6LL);
    EXPECT_EQ((*deserialized)[3].as_interval().microseconds, 3600000000LL);
}

TEST(GDB793, MatchedVariableTypesRoundTrip) {
    Schema schema({
        {"name", TypeId::STRING},
        {"data", TypeId::BLOB},
        {"meta", TypeId::JSON},
    });

    Blob b = {0x01, 0x02, 0x03};
    std::vector<Value> values = {
        Value(std::string("hello")),
        Value(b),
        Value(JsonString{"{\"k\":1}"}),
    };

    auto serialized = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(serialized.has_value()) << serialized.error().message;

    auto deserialized = TupleSerializer::deserialize(*serialized, schema);
    ASSERT_TRUE(deserialized.has_value()) << deserialized.error().message;

    ASSERT_EQ(deserialized->size(), 3u);
    EXPECT_EQ((*deserialized)[0].as_string(), "hello");
    EXPECT_EQ((*deserialized)[1].as_blob(), b);
    EXPECT_EQ((*deserialized)[2].as_json().data, "{\"k\":1}");
}

// ---------------------------------------------------------------------------
// Type error must name the column index in the message for multi-column schemas.
// ---------------------------------------------------------------------------
TEST(GDB793, TypeErrorNamesColumnIndex) {
    Schema schema({
        {"ok_col", TypeId::INT32},
        {"bad_col", TypeId::INT32},
    });

    // Second column has wrong type.
    std::vector<Value> values = {
        Value(static_cast<int32_t>(1)),
        Value(static_cast<int64_t>(2)), // INT64 into INT32 slot
    };

    auto result = TupleSerializer::serialize(values, schema);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_NE(result.error().message.find("bad_col"), std::string::npos);
    // Index 1 must appear in the message.
    EXPECT_NE(result.error().message.find('1'), std::string::npos);
}