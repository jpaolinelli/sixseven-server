#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace sixseven;

// -- Schema tests -------------------------------------------------------------

TEST(Schema, Construction) {
    Schema schema({
        {"id", TypeId::INT32},
        {"name", TypeId::STRING},
        {"age", TypeId::UINT8},
    });

    EXPECT_EQ(schema.column_count(), 3u);
    EXPECT_EQ(schema.fixed_column_count(), 2u);
    EXPECT_EQ(schema.variable_column_count(), 1u);
    EXPECT_EQ(schema.column(0).name, "id");
    EXPECT_EQ(schema.column(0).type, TypeId::INT32);
    EXPECT_EQ(schema.column(1).name, "name");
    EXPECT_EQ(schema.column(1).type, TypeId::STRING);
    EXPECT_EQ(schema.column(2).name, "age");
    EXPECT_EQ(schema.column(2).type, TypeId::UINT8);
}

TEST(Schema, NullBitmapSize) {
    // 1-8 columns -> 1 byte
    Schema s1({{"a", TypeId::INT32}});
    EXPECT_EQ(s1.null_bitmap_size(), 1u);

    Schema s8({{"a", TypeId::INT32},
               {"b", TypeId::INT32},
               {"c", TypeId::INT32},
               {"d", TypeId::INT32},
               {"e", TypeId::INT32},
               {"f", TypeId::INT32},
               {"g", TypeId::INT32},
               {"h", TypeId::INT32}});
    EXPECT_EQ(s8.null_bitmap_size(), 1u);

    // 9 columns -> 2 bytes
    Schema s9({{"a", TypeId::INT32},
               {"b", TypeId::INT32},
               {"c", TypeId::INT32},
               {"d", TypeId::INT32},
               {"e", TypeId::INT32},
               {"f", TypeId::INT32},
               {"g", TypeId::INT32},
               {"h", TypeId::INT32},
               {"i", TypeId::INT32}});
    EXPECT_EQ(s9.null_bitmap_size(), 2u);
}

TEST(Schema, FixedFieldOffsets) {
    Schema schema({
        {"a", TypeId::INT32},  // offset 0, size 4
        {"b", TypeId::INT64},  // offset 4, size 8
        {"c", TypeId::UINT8},  // offset 12, size 1
        {"d", TypeId::STRING}, // variable
    });

    auto ao = schema.fixed_field_offset(0);
    ASSERT_TRUE(ao.has_value());
    EXPECT_EQ(*ao, 0u);

    auto bo = schema.fixed_field_offset(1);
    ASSERT_TRUE(bo.has_value());
    EXPECT_EQ(*bo, 4u);

    auto co = schema.fixed_field_offset(2);
    ASSERT_TRUE(co.has_value());
    EXPECT_EQ(*co, 12u);

    // Variable-length column has no fixed offset.
    auto d_off = schema.fixed_field_offset(3);
    EXPECT_FALSE(d_off.has_value());

    EXPECT_EQ(schema.fixed_region_size(), 13u); // 4 + 8 + 1
}

TEST(Schema, VarFieldIndex) {
    Schema schema({
        {"a", TypeId::INT32},
        {"b", TypeId::STRING}, // var index 0
        {"c", TypeId::INT64},
        {"d", TypeId::BLOB}, // var index 1
        {"e", TypeId::JSON}, // var index 2
    });

    EXPECT_FALSE(schema.var_field_index(0).has_value()); // fixed
    EXPECT_EQ(*schema.var_field_index(1), 0u);
    EXPECT_FALSE(schema.var_field_index(2).has_value()); // fixed
    EXPECT_EQ(*schema.var_field_index(3), 1u);
    EXPECT_EQ(*schema.var_field_index(4), 2u);

    EXPECT_EQ(schema.variable_column_count(), 3u);
}

// -- Serialize / Deserialize round-trip tests ---------------------------------

TEST(TupleSerializer, FixedOnlyRoundTrip) {
    Schema schema({
        {"id", TypeId::INT32},
        {"age", TypeId::UINT8},
        {"score", TypeId::FLOAT64},
    });

    std::vector<Value> values = {Value(int32_t{42}), Value(uint8_t{25}), Value(3.14)};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 3u);

    EXPECT_EQ((*result)[0].as_int32(), 42);
    EXPECT_EQ((*result)[1].as_uint8(), 25);
    EXPECT_DOUBLE_EQ((*result)[2].as_float64(), 3.14);
}

TEST(TupleSerializer, VariableOnlyRoundTrip) {
    Schema schema({
        {"name", TypeId::STRING},
        {"data", TypeId::BLOB},
    });

    std::vector<Value> values = {Value(std::string{"hello world"}), Value(Blob{0xDE, 0xAD})};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2u);

    EXPECT_EQ((*result)[0].as_string(), "hello world");
    EXPECT_EQ((*result)[1].as_blob(), (Blob{0xDE, 0xAD}));
}

TEST(TupleSerializer, MixedFixedAndVariable) {
    Schema schema({
        {"id", TypeId::INT32},
        {"name", TypeId::STRING},
        {"active", TypeId::BOOL},
        {"bio", TypeId::STRING},
    });

    std::vector<Value> values = {Value(int32_t{100}),
                                 Value(std::string{"Alice"}),
                                 Value(true),
                                 Value(std::string{"Software engineer"})};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 4u);

    EXPECT_EQ((*result)[0].as_int32(), 100);
    EXPECT_EQ((*result)[1].as_string(), "Alice");
    EXPECT_EQ((*result)[2].as_bool(), true);
    EXPECT_EQ((*result)[3].as_string(), "Software engineer");
}

TEST(TupleSerializer, NullFields) {
    Schema schema({
        {"id", TypeId::INT32},
        {"name", TypeId::STRING},
        {"age", TypeId::UINT8},
    });

    std::vector<Value> values = {Value(int32_t{1}), Value::make_null(), Value::make_null()};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 3u);

    EXPECT_EQ((*result)[0].as_int32(), 1);
    EXPECT_TRUE((*result)[1].is_null());
    EXPECT_TRUE((*result)[2].is_null());
}

TEST(TupleSerializer, AllNulls) {
    Schema schema({
        {"a", TypeId::INT32},
        {"b", TypeId::STRING},
        {"c", TypeId::FLOAT64},
    });

    std::vector<Value> values = {Value::make_null(), Value::make_null(), Value::make_null()};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 3u);

    for (size_t i = 0; i < 3; ++i) {
        EXPECT_TRUE((*result)[i].is_null()) << "column " << i << " should be null";
    }
}

TEST(TupleSerializer, EmptyString) {
    Schema schema({{"name", TypeId::STRING}});
    std::vector<Value> values = {Value(std::string{""})};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ((*result)[0].as_string(), "");
}

TEST(TupleSerializer, ValueCountMismatch) {
    Schema schema({{"a", TypeId::INT32}, {"b", TypeId::STRING}});
    std::vector<Value> values = {Value(int32_t{1})};

    auto result = TupleSerializer::serialize(values, schema);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// -- Single field access tests ------------------------------------------------

TEST(TupleSerializer, GetFieldFixed) {
    Schema schema({
        {"id", TypeId::INT32},
        {"score", TypeId::FLOAT64},
        {"flag", TypeId::BOOL},
    });

    std::vector<Value> values = {Value(int32_t{42}), Value(99.5), Value(true)};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto f0 = TupleSerializer::get_field(*buf, schema, 0);
    ASSERT_TRUE(f0.has_value());
    EXPECT_EQ(f0->as_int32(), 42);

    auto f1 = TupleSerializer::get_field(*buf, schema, 1);
    ASSERT_TRUE(f1.has_value());
    EXPECT_DOUBLE_EQ(f1->as_float64(), 99.5);

    auto f2 = TupleSerializer::get_field(*buf, schema, 2);
    ASSERT_TRUE(f2.has_value());
    EXPECT_EQ(f2->as_bool(), true);
}

TEST(TupleSerializer, GetFieldVariable) {
    Schema schema({
        {"id", TypeId::INT32},
        {"name", TypeId::STRING},
        {"bio", TypeId::STRING},
    });

    std::vector<Value> values = {
        Value(int32_t{1}), Value(std::string{"Alice"}), Value(std::string{"Engineer"})};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto f1 = TupleSerializer::get_field(*buf, schema, 1);
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f1->as_string(), "Alice");

    auto f2 = TupleSerializer::get_field(*buf, schema, 2);
    ASSERT_TRUE(f2.has_value());
    EXPECT_EQ(f2->as_string(), "Engineer");
}

TEST(TupleSerializer, GetFieldNull) {
    Schema schema({
        {"id", TypeId::INT32},
        {"name", TypeId::STRING},
    });

    std::vector<Value> values = {Value::make_null(), Value::make_null()};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto f0 = TupleSerializer::get_field(*buf, schema, 0);
    ASSERT_TRUE(f0.has_value());
    EXPECT_TRUE(f0->is_null());

    auto f1 = TupleSerializer::get_field(*buf, schema, 1);
    ASSERT_TRUE(f1.has_value());
    EXPECT_TRUE(f1->is_null());
}

TEST(TupleSerializer, GetFieldOutOfRange) {
    Schema schema({{"id", TypeId::INT32}});
    std::vector<Value> values = {Value(int32_t{1})};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto f = TupleSerializer::get_field(*buf, schema, 5);
    ASSERT_FALSE(f.has_value());
    EXPECT_EQ(f.error().code, StatusCode::INVALID_ARGUMENT);
}

// -- All fixed-size types round-trip ------------------------------------------

TEST(TupleSerializer, AllFixedTypesRoundTrip) {
    Schema schema({
        {"a", TypeId::INT8},
        {"b", TypeId::INT16},
        {"c", TypeId::INT32},
        {"d", TypeId::INT64},
        {"e", TypeId::UINT8},
        {"f", TypeId::UINT16},
        {"g", TypeId::UINT32},
        {"h", TypeId::UINT64},
        {"i", TypeId::FLOAT32},
        {"j", TypeId::FLOAT64},
        {"k", TypeId::DECIMAL},
        {"l", TypeId::BOOL},
        {"m", TypeId::DATE},
        {"n", TypeId::TIME},
        {"o", TypeId::TIMESTAMP},
        {"p", TypeId::INTERVAL},
        {"q", TypeId::POINT},
        {"r", TypeId::UUID},
    });

    Uuid test_uuid = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

    std::vector<Value> values = {
        Value(int8_t{-42}),
        Value(int16_t{-1000}),
        Value(int32_t{100000}),
        Value(int64_t{-9999999999LL}),
        Value(uint8_t{255}),
        Value(uint16_t{60000}),
        Value(uint32_t{4000000000u}),
        Value(uint64_t{18446744073709551615ULL}),
        Value(3.14f),
        Value(2.718281828),
        Value(Decimal128{-1, 42}),
        Value(true),
        Value(Date{19000}),
        Value(Time{43200000000LL}),
        Value(Timestamp{1700000000000000LL}),
        Value(Interval{6, 86400000000LL}),
        Value(Point{-122.4194, 37.7749}),
        Value(test_uuid),
    };

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 18u);

    EXPECT_EQ((*result)[0].as_int8(), -42);
    EXPECT_EQ((*result)[1].as_int16(), -1000);
    EXPECT_EQ((*result)[2].as_int32(), 100000);
    EXPECT_EQ((*result)[3].as_int64(), -9999999999LL);
    EXPECT_EQ((*result)[4].as_uint8(), 255);
    EXPECT_EQ((*result)[5].as_uint16(), 60000);
    EXPECT_EQ((*result)[6].as_uint32(), 4000000000u);
    EXPECT_EQ((*result)[7].as_uint64(), 18446744073709551615ULL);
    EXPECT_FLOAT_EQ((*result)[8].as_float32(), 3.14f);
    EXPECT_DOUBLE_EQ((*result)[9].as_float64(), 2.718281828);
    EXPECT_EQ((*result)[10].as_decimal().hi, -1);
    EXPECT_EQ((*result)[10].as_decimal().lo, 42u);
    EXPECT_EQ((*result)[11].as_bool(), true);
    EXPECT_EQ((*result)[12].as_date().days_since_epoch, 19000);
    EXPECT_EQ((*result)[13].as_time().microseconds, 43200000000LL);
    EXPECT_EQ((*result)[14].as_timestamp().microseconds, 1700000000000000LL);
    EXPECT_EQ((*result)[15].as_interval().months, 6);
    EXPECT_EQ((*result)[15].as_interval().microseconds, 86400000000LL);
    EXPECT_DOUBLE_EQ((*result)[16].as_point().x, -122.4194);
    EXPECT_DOUBLE_EQ((*result)[16].as_point().y, 37.7749);
    EXPECT_EQ((*result)[17].as_uuid(), test_uuid);
}

// -- Variable-length types round-trip -----------------------------------------

TEST(TupleSerializer, AllVariableTypesRoundTrip) {
    Schema schema({
        {"s", TypeId::STRING},
        {"b", TypeId::BLOB},
        {"j", TypeId::JSON},
        {"e", TypeId::EMBEDDING},
    });

    std::vector<Value> values = {
        Value(std::string{"hello world"}),
        Value(Blob{0x01, 0x02, 0x03, 0x04}),
        Value(JsonString{R"({"key":"value"})"}),
        Value(Embedding{1.0f, 2.0f, 3.0f}),
    };

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 4u);

    EXPECT_EQ((*result)[0].as_string(), "hello world");
    EXPECT_EQ((*result)[1].as_blob(), (Blob{0x01, 0x02, 0x03, 0x04}));
    EXPECT_EQ((*result)[2].as_json().data, R"({"key":"value"})");
    ASSERT_EQ((*result)[3].as_embedding().size(), 3u);
    EXPECT_FLOAT_EQ((*result)[3].as_embedding()[0], 1.0f);
    EXPECT_FLOAT_EQ((*result)[3].as_embedding()[1], 2.0f);
    EXPECT_FLOAT_EQ((*result)[3].as_embedding()[2], 3.0f);
}

// -- Tuple size computation ---------------------------------------------------

TEST(TupleSerializer, ComputeTupleSize) {
    Schema schema({
        {"id", TypeId::INT32},
        {"name", TypeId::STRING},
        {"active", TypeId::BOOL},
    });

    std::vector<Value> values = {Value(int32_t{1}), Value(std::string{"Alice"}), Value(true)};

    size_t expected_size = 1 +       // null bitmap (3 cols -> 1 byte)
                           (4 + 1) + // fixed fields: INT32(4) + BOOL(1)
                           4 +       // var offset table: 1 var col * 4 bytes
                           5;        // var data: "Alice" = 5 bytes
    EXPECT_EQ(expected_size, 15u);

    size_t computed = TupleSerializer::compute_tuple_size(values, schema);
    EXPECT_EQ(computed, expected_size);
}

TEST(TupleSerializer, ComputeTupleSizeWithNulls) {
    Schema schema({
        {"id", TypeId::INT32},
        {"name", TypeId::STRING},
    });

    std::vector<Value> values = {Value::make_null(), Value::make_null()};

    size_t expected_size = 1 + // null bitmap
                           4 + // fixed region (INT32 still occupies space, even if null)
                           4;  // var offset table: 1 var col * 4 bytes (offset=0, length=0)
    EXPECT_EQ(expected_size, 9u);

    size_t computed = TupleSerializer::compute_tuple_size(values, schema);
    EXPECT_EQ(computed, expected_size);
}

// -- NullBitmap more than 8 columns -------------------------------------------

TEST(TupleSerializer, MoreThan8Columns) {
    Schema schema({
        {"c0", TypeId::INT32},
        {"c1", TypeId::INT32},
        {"c2", TypeId::INT32},
        {"c3", TypeId::INT32},
        {"c4", TypeId::INT32},
        {"c5", TypeId::INT32},
        {"c6", TypeId::INT32},
        {"c7", TypeId::INT32},
        {"c8", TypeId::INT32},
        {"c9", TypeId::STRING},
    });

    std::vector<Value> values;
    for (int i = 0; i < 9; ++i) {
        values.push_back(Value(int32_t{i * 10}));
    }
    values.push_back(Value(std::string{"tenth"}));

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 10u);

    for (int i = 0; i < 9; ++i) {
        EXPECT_EQ((*result)[i].as_int32(), i * 10) << "column " << i;
    }
    EXPECT_EQ((*result)[9].as_string(), "tenth");
}

TEST(TupleSerializer, NullsSpanMultipleBitmapBytes) {
    Schema schema({
        {"c0", TypeId::INT32},
        {"c1", TypeId::INT32},
        {"c2", TypeId::INT32},
        {"c3", TypeId::INT32},
        {"c4", TypeId::INT32},
        {"c5", TypeId::INT32},
        {"c6", TypeId::INT32},
        {"c7", TypeId::INT32},
        {"c8", TypeId::INT32},
        {"c9", TypeId::INT32},
    });

    std::vector<Value> values;
    for (int i = 0; i < 10; ++i) {
        // Make every other column null.
        if (i % 2 == 0) {
            values.push_back(Value(int32_t{i}));
        } else {
            values.push_back(Value::make_null());
        }
    }

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 10u);

    for (int i = 0; i < 10; ++i) {
        if (i % 2 == 0) {
            EXPECT_EQ((*result)[i].as_int32(), i) << "column " << i;
        } else {
            EXPECT_TRUE((*result)[i].is_null()) << "column " << i;
        }
    }
}

// -- GetField consistency with full deserialization ----------------------------

TEST(TupleSerializer, GetFieldConsistentWithDeserialize) {
    Schema schema({
        {"id", TypeId::INT64},
        {"name", TypeId::STRING},
        {"score", TypeId::FLOAT32},
        {"bio", TypeId::JSON},
        {"active", TypeId::BOOL},
    });

    std::vector<Value> values = {Value(int64_t{9999}),
                                 Value(std::string{"Bob"}),
                                 Value(95.5f),
                                 Value(JsonString{R"({"role":"dev"})"}),
                                 Value(false)};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto full = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(full.has_value());

    // Verify get_field returns same values as full deserialization.
    for (size_t i = 0; i < 5; ++i) {
        auto field = TupleSerializer::get_field(*buf, schema, i);
        ASSERT_TRUE(field.has_value()) << "get_field failed at column " << i;

        if ((*full)[i].is_null()) {
            EXPECT_TRUE(field->is_null()) << "column " << i;
        } else {
            EXPECT_EQ(field->type_id(), (*full)[i].type_id()) << "column " << i;
        }
    }

    // Spot-check values.
    auto f0 = TupleSerializer::get_field(*buf, schema, 0);
    EXPECT_EQ(f0->as_int64(), 9999);

    auto f1 = TupleSerializer::get_field(*buf, schema, 1);
    EXPECT_EQ(f1->as_string(), "Bob");

    auto f2 = TupleSerializer::get_field(*buf, schema, 2);
    EXPECT_FLOAT_EQ(f2->as_float32(), 95.5f);

    auto f3 = TupleSerializer::get_field(*buf, schema, 3);
    EXPECT_EQ(f3->as_json().data, R"({"role":"dev"})");

    auto f4 = TupleSerializer::get_field(*buf, schema, 4);
    EXPECT_EQ(f4->as_bool(), false);
}

// -- Truncated data detection -------------------------------------------------

TEST(TupleSerializer, DeserializeTruncatedData) {
    Schema schema({{"id", TypeId::INT32}, {"name", TypeId::STRING}});
    std::vector<Value> values = {Value(int32_t{1}), Value(std::string{"hello"})};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    // Truncate the buffer.
    std::vector<uint8_t> truncated(buf->begin(), buf->begin() + 2);
    auto result = TupleSerializer::deserialize(truncated, schema);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// -- Single column schema -----------------------------------------------------

TEST(TupleSerializer, SingleColumnInt) {
    Schema schema({{"id", TypeId::INT32}});
    std::vector<Value> values = {Value(int32_t{42})};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ((*result)[0].as_int32(), 42);
}

TEST(TupleSerializer, SingleColumnString) {
    Schema schema({{"name", TypeId::STRING}});
    std::vector<Value> values = {Value(std::string{"test"})};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ((*result)[0].as_string(), "test");
}

// -- Large string test --------------------------------------------------------

TEST(TupleSerializer, LargeString) {
    Schema schema({
        {"id", TypeId::INT32},
        {"data", TypeId::STRING},
    });

    std::string large(5000, 'x');
    std::vector<Value> values = {Value(int32_t{1}), Value(large)};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2u);

    EXPECT_EQ((*result)[0].as_int32(), 1);
    EXPECT_EQ((*result)[1].as_string(), large);
}

// -- String with null bytes ---------------------------------------------------

TEST(TupleSerializer, StringWithNullBytes) {
    Schema schema({{"data", TypeId::STRING}});
    std::string data("hello\0world", 11);
    std::vector<Value> values = {Value(data)};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ((*result)[0].as_string().size(), 11u);
    EXPECT_EQ((*result)[0].as_string(), data);
}
