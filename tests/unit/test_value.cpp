#include "giodb/common/value.h"

#include <gtest/gtest.h>

#include <limits>

using namespace giodb;

// -- NULL values --------------------------------------------------------------

TEST(Value, DefaultIsNull) {
    Value v;
    EXPECT_TRUE(v.is_null());
}

TEST(Value, MakeNullIsNull) {
    auto v = Value::make_null();
    EXPECT_TRUE(v.is_null());
}

TEST(Value, NonNullIsNotNull) {
    Value v(int32_t{42});
    EXPECT_FALSE(v.is_null());
}

// -- Integer types ------------------------------------------------------------

TEST(Value, Int8) {
    Value v(int8_t{-128});
    EXPECT_EQ(v.type_id(), TypeId::INT8);
    EXPECT_EQ(v.as_int8(), -128);
    EXPECT_FALSE(v.is_null());
}

TEST(Value, Int16) {
    Value v(int16_t{-32000});
    EXPECT_EQ(v.type_id(), TypeId::INT16);
    EXPECT_EQ(v.as_int16(), -32000);
}

TEST(Value, Int32) {
    Value v(int32_t{2147483647});
    EXPECT_EQ(v.type_id(), TypeId::INT32);
    EXPECT_EQ(v.as_int32(), 2147483647);
}

TEST(Value, Int64) {
    Value v(int64_t{9223372036854775807LL});
    EXPECT_EQ(v.type_id(), TypeId::INT64);
    EXPECT_EQ(v.as_int64(), 9223372036854775807LL);
}

TEST(Value, Uint8) {
    Value v(uint8_t{255});
    EXPECT_EQ(v.type_id(), TypeId::UINT8);
    EXPECT_EQ(v.as_uint8(), 255);
}

TEST(Value, Uint16) {
    Value v(uint16_t{65535});
    EXPECT_EQ(v.type_id(), TypeId::UINT16);
    EXPECT_EQ(v.as_uint16(), 65535);
}

TEST(Value, Uint32) {
    Value v(uint32_t{4294967295U});
    EXPECT_EQ(v.type_id(), TypeId::UINT32);
    EXPECT_EQ(v.as_uint32(), 4294967295U);
}

TEST(Value, Uint64) {
    Value v(uint64_t{18446744073709551615ULL});
    EXPECT_EQ(v.type_id(), TypeId::UINT64);
    EXPECT_EQ(v.as_uint64(), 18446744073709551615ULL);
}

// -- Floating-point types -----------------------------------------------------

TEST(Value, Float32) {
    Value v(3.14f);
    EXPECT_EQ(v.type_id(), TypeId::FLOAT32);
    EXPECT_FLOAT_EQ(v.as_float32(), 3.14f);
}

TEST(Value, Float64) {
    Value v(2.718281828459045);
    EXPECT_EQ(v.type_id(), TypeId::FLOAT64);
    EXPECT_DOUBLE_EQ(v.as_float64(), 2.718281828459045);
}

// -- Decimal ------------------------------------------------------------------

TEST(Value, Decimal128) {
    Decimal128 d{1, 500};
    Value v(d);
    EXPECT_EQ(v.type_id(), TypeId::DECIMAL);
    EXPECT_EQ(v.as_decimal().hi, 1);
    EXPECT_EQ(v.as_decimal().lo, 500u);
}

// -- Bool ---------------------------------------------------------------------

TEST(Value, BoolTrue) {
    Value v(true);
    EXPECT_EQ(v.type_id(), TypeId::BOOL);
    EXPECT_TRUE(v.as_bool());
}

TEST(Value, BoolFalse) {
    Value v(false);
    EXPECT_EQ(v.type_id(), TypeId::BOOL);
    EXPECT_FALSE(v.as_bool());
}

// -- String -------------------------------------------------------------------

TEST(Value, String) {
    Value v(std::string{"hello world"});
    EXPECT_EQ(v.type_id(), TypeId::STRING);
    EXPECT_EQ(v.as_string(), "hello world");
}

TEST(Value, EmptyString) {
    Value v(std::string{""});
    EXPECT_EQ(v.type_id(), TypeId::STRING);
    EXPECT_EQ(v.as_string(), "");
    EXPECT_FALSE(v.is_null());
}

// -- Blob ---------------------------------------------------------------------

TEST(Value, Blob) {
    Blob b = {0x01, 0x02, 0xFF};
    Value v(b);
    EXPECT_EQ(v.type_id(), TypeId::BLOB);
    EXPECT_EQ(v.as_blob().size(), 3u);
    EXPECT_EQ(v.as_blob()[2], 0xFF);
}

TEST(Value, EmptyBlob) {
    Value v(Blob{});
    EXPECT_EQ(v.type_id(), TypeId::BLOB);
    EXPECT_TRUE(v.as_blob().empty());
    EXPECT_FALSE(v.is_null());
}

// -- Date ---------------------------------------------------------------------

TEST(Value, Date) {
    Date d{19723}; // 2023-12-31
    Value v(d);
    EXPECT_EQ(v.type_id(), TypeId::DATE);
    EXPECT_EQ(v.as_date().days_since_epoch, 19723);
}

// -- Time ---------------------------------------------------------------------

TEST(Value, Time) {
    Time t{43200000000LL}; // 12:00:00.000000
    Value v(t);
    EXPECT_EQ(v.type_id(), TypeId::TIME);
    EXPECT_EQ(v.as_time().microseconds, 43200000000LL);
}

// -- Timestamp ----------------------------------------------------------------

TEST(Value, Timestamp) {
    Timestamp ts{1703980800000000LL}; // 2023-12-31 00:00:00 UTC
    Value v(ts);
    EXPECT_EQ(v.type_id(), TypeId::TIMESTAMP);
    EXPECT_EQ(v.as_timestamp().microseconds, 1703980800000000LL);
}

// -- Interval -----------------------------------------------------------------

TEST(Value, Interval) {
    Interval i{2, 86400000000LL}; // 2 months + 1 day
    Value v(i);
    EXPECT_EQ(v.type_id(), TypeId::INTERVAL);
    EXPECT_EQ(v.as_interval().months, 2);
    EXPECT_EQ(v.as_interval().microseconds, 86400000000LL);
}

// -- Point --------------------------------------------------------------------

TEST(Value, Point) {
    Point p{-73.935242, 40.730610};
    Value v(p);
    EXPECT_EQ(v.type_id(), TypeId::POINT);
    EXPECT_DOUBLE_EQ(v.as_point().x, -73.935242);
    EXPECT_DOUBLE_EQ(v.as_point().y, 40.730610);
}

// -- JSON ---------------------------------------------------------------------

TEST(Value, Json) {
    JsonString j{R"({"key": "value"})"};
    Value v(j);
    EXPECT_EQ(v.type_id(), TypeId::JSON);
    EXPECT_EQ(v.as_json().data, R"({"key": "value"})");
}

// -- UUID ---------------------------------------------------------------------

TEST(Value, Uuid) {
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
    Value v(u);
    EXPECT_EQ(v.type_id(), TypeId::UUID);
    EXPECT_EQ(v.as_uuid()[0], 0x55);
    EXPECT_EQ(v.as_uuid()[15], 0x00);
}

// -- Embedding ----------------------------------------------------------------

TEST(Value, Embedding) {
    Embedding e = {0.1f, 0.2f, 0.3f, 0.4f};
    Value v(e);
    EXPECT_EQ(v.type_id(), TypeId::EMBEDDING);
    EXPECT_EQ(v.as_embedding().size(), 4u);
    EXPECT_FLOAT_EQ(v.as_embedding()[0], 0.1f);
}

TEST(Value, EmptyEmbedding) {
    Value v(Embedding{});
    EXPECT_EQ(v.type_id(), TypeId::EMBEDDING);
    EXPECT_TRUE(v.as_embedding().empty());
    EXPECT_FALSE(v.is_null());
}

// -- Type mismatch throws -----------------------------------------------------

TEST(Value, AccessorThrowsOnTypeMismatch) {
    Value v(int32_t{42});
    EXPECT_THROW((void)v.as_string(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_float64(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_bool(), std::bad_variant_access);
}

TEST(Value, NullAccessorThrows) {
    Value v;
    EXPECT_THROW((void)v.as_int32(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_string(), std::bad_variant_access);
}

// -- Mutable accessors --------------------------------------------------------

TEST(Value, MutableAccessor) {
    Value v(int32_t{10});
    v.as_int32() = 20;
    EXPECT_EQ(v.as_int32(), 20);
}

TEST(Value, MutableStringAccessor) {
    Value v(std::string{"hello"});
    v.as_string() += " world";
    EXPECT_EQ(v.as_string(), "hello world");
}

// -- Edge cases ---------------------------------------------------------------

TEST(Value, IntMaxValues) {
    EXPECT_EQ(Value(std::numeric_limits<int8_t>::min()).as_int8(), -128);
    EXPECT_EQ(Value(std::numeric_limits<int8_t>::max()).as_int8(), 127);
    EXPECT_EQ(Value(std::numeric_limits<int64_t>::max()).as_int64(),
              std::numeric_limits<int64_t>::max());
    EXPECT_EQ(Value(std::numeric_limits<uint64_t>::max()).as_uint64(),
              std::numeric_limits<uint64_t>::max());
}

TEST(Value, FloatSpecialValues) {
    Value inf(std::numeric_limits<double>::infinity());
    EXPECT_EQ(inf.as_float64(), std::numeric_limits<double>::infinity());

    Value neg_inf(-std::numeric_limits<double>::infinity());
    EXPECT_EQ(neg_inf.as_float64(), -std::numeric_limits<double>::infinity());

    Value nan_val(std::numeric_limits<double>::quiet_NaN());
    EXPECT_TRUE(std::isnan(nan_val.as_float64()));
}

TEST(Value, CopySemantics) {
    Value v1(std::string{"test"});
    Value v2 = v1;
    EXPECT_EQ(v2.as_string(), "test");
    EXPECT_EQ(v1.as_string(), "test"); // original unchanged
}

TEST(Value, MoveSemantics) {
    Value v1(std::string{"test"});
    Value v2 = std::move(v1);
    EXPECT_EQ(v2.as_string(), "test");
}
