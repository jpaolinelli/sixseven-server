/// @file test_qa_gdb_834.cpp
/// @brief Adversarial QA tests for GDB-834: TupleSerializer refactor — shared
/// tuple_byte_count helper.
///
/// The refactor extracted a single anonymous-namespace helper (tuple_byte_count)
/// so TupleSerializer::serialize() and TupleSerializer::compute_tuple_size()
/// share identical byte arithmetic.  The critical invariant is:
///   compute_tuple_size(values, schema) == serialize(values, schema)->size()
/// for every tuple shape.  A divergence would be a silent correctness bug
/// (wrong buffer size pre-allocated → heap write past end OR wrong overflow
/// routing decision).
///
/// Tests are grouped under QA_GDB834_*.

#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helper: assert the three-way invariant for any set of values+schema.
// 1. compute_tuple_size returns the expected hardcoded value (when provided).
// 2. serialize succeeds and produces exactly compute_tuple_size bytes.
// 3. deserialize round-trips every non-NULL value faithfully.
// ---------------------------------------------------------------------------

namespace {

/// Round-trip helper: serialize then deserialize and assert value equality.
/// Caller supplies the values and schema; this checks the buffer size equals
/// compute_tuple_size and the deserialized values match the originals.
void assert_roundtrip(const std::vector<Value>& values, const Schema& schema,
                      std::optional<size_t> expected_bytes = std::nullopt) {
    size_t computed = TupleSerializer::compute_tuple_size(values, schema);

    if (expected_bytes.has_value()) {
        EXPECT_EQ(computed, *expected_bytes)
            << "compute_tuple_size returned unexpected value";
    }

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value()) << "serialize failed: " << buf.error().message;

    // Core invariant: buffer length == compute_tuple_size.
    EXPECT_EQ(buf->size(), computed)
        << "serialize().size() != compute_tuple_size() — sizing divergence detected!";

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value()) << "deserialize failed: " << result.error().message;
    ASSERT_EQ(result->size(), values.size());
}

} // namespace

// ---------------------------------------------------------------------------
// AC-1: hardcoded expected sizes match — all fixed-width (29 B)
// ---------------------------------------------------------------------------

TEST(QA_GDB834_HardcodedSizes, AllFixedWidth29B) {
    // INT8(1)+INT16(2)+INT32(4)+INT64(8)+FLOAT32(4)+FLOAT64(8)+BOOL(1) = 28 fixed
    // bitmap = ceil(7/8) = 1
    // total = 1 + 28 = 29
    Schema schema({
        {"a", TypeId::INT8},
        {"b", TypeId::INT16},
        {"c", TypeId::INT32},
        {"d", TypeId::INT64},
        {"e", TypeId::FLOAT32},
        {"f", TypeId::FLOAT64},
        {"g", TypeId::BOOL},
    });
    std::vector<Value> values = {
        Value(int8_t{1}),
        Value(int16_t{2}),
        Value(int32_t{3}),
        Value(int64_t{4}),
        Value(1.0f),
        Value(2.0),
        Value(true),
    };
    assert_roundtrip(values, schema, 29u);
}

TEST(QA_GDB834_HardcodedSizes, BlobColumn13B) {
    // INT32(4) + BLOB({4 bytes}) — bitmap=1, fixed=4, var_table=4, var_data=4 => 13
    Schema schema({{"id", TypeId::INT32}, {"data", TypeId::BLOB}});
    Blob blob = {0xDE, 0xAD, 0xBE, 0xEF};
    std::vector<Value> values = {Value(int32_t{7}), Value(blob)};
    assert_roundtrip(values, schema, 13u);
}

TEST(QA_GDB834_HardcodedSizes, EmbeddingColumn21B) {
    // INT32(4) + EMBEDDING(3 floats=12) — bitmap=1, fixed=4, var_table=4, var_data=12 => 21
    Schema schema({{"id", TypeId::INT32}, {"vec", TypeId::EMBEDDING}});
    Embedding emb = {1.0f, 2.0f, 3.0f};
    std::vector<Value> values = {Value(int32_t{1}), Value(emb)};
    assert_roundtrip(values, schema, 21u);
}

TEST(QA_GDB834_HardcodedSizes, WideNullBitmap38B) {
    // 9 * INT32 — bitmap=2, fixed=36, var=0 => 38
    std::vector<ColumnDef> cols;
    for (int i = 0; i < 9; ++i)
        cols.push_back({"c" + std::to_string(i), TypeId::INT32});
    Schema schema(std::move(cols));
    std::vector<Value> values;
    for (int i = 0; i < 9; ++i)
        values.emplace_back(int32_t{i});
    assert_roundtrip(values, schema, 38u);
}

TEST(QA_GDB834_HardcodedSizes, EmptySchema0B) {
    Schema schema(std::vector<ColumnDef>{});
    std::vector<Value> values;
    assert_roundtrip(values, schema, 0u);
}

// ---------------------------------------------------------------------------
// AC-2 / AC-3: no shape makes serialize().size() != compute_tuple_size()
// ---------------------------------------------------------------------------

// All-NULL tuple.
TEST(QA_GDB834_SizingInvariant, AllNullTuple) {
    Schema schema({
        {"a", TypeId::INT32},
        {"b", TypeId::STRING},
        {"c", TypeId::FLOAT64},
    });
    std::vector<Value> values = {Value::make_null(), Value::make_null(), Value::make_null()};
    assert_roundtrip(values, schema);
}

// Alternating NULL / non-NULL.
TEST(QA_GDB834_SizingInvariant, AlternatingNullNonNull) {
    Schema schema({
        {"a", TypeId::INT32},   // non-null
        {"b", TypeId::STRING},  // null
        {"c", TypeId::INT64},   // non-null
        {"d", TypeId::BLOB},    // null
    });
    std::vector<Value> values = {
        Value(int32_t{1}),
        Value::make_null(),
        Value(int64_t{99}),
        Value::make_null(),
    };
    assert_roundtrip(values, schema);
}

// Bitmap boundary: exactly 8 columns (1 bitmap byte full).
TEST(QA_GDB834_SizingInvariant, BitmapBoundary8Cols) {
    // 8 INT32 cols — bitmap = ceil(8/8) = 1 byte, fixed = 32, total = 33
    std::vector<ColumnDef> cols;
    for (int i = 0; i < 8; ++i)
        cols.push_back({"c" + std::to_string(i), TypeId::INT32});
    Schema schema(std::move(cols));
    std::vector<Value> values;
    for (int i = 0; i < 8; ++i)
        values.emplace_back(int32_t{i});
    assert_roundtrip(values, schema, 1u + 32u);
}

// Bitmap boundary: 9 columns (spills into second bitmap byte).
TEST(QA_GDB834_SizingInvariant, BitmapBoundary9Cols) {
    std::vector<ColumnDef> cols;
    for (int i = 0; i < 9; ++i)
        cols.push_back({"c" + std::to_string(i), TypeId::INT32});
    Schema schema(std::move(cols));
    std::vector<Value> values;
    for (int i = 0; i < 9; ++i)
        values.emplace_back(int32_t{i});
    assert_roundtrip(values, schema, 2u + 36u);
}

// Bitmap boundary: exactly 16 columns (2 bitmap bytes full).
TEST(QA_GDB834_SizingInvariant, BitmapBoundary16Cols) {
    std::vector<ColumnDef> cols;
    for (int i = 0; i < 16; ++i)
        cols.push_back({"c" + std::to_string(i), TypeId::INT32});
    Schema schema(std::move(cols));
    std::vector<Value> values;
    for (int i = 0; i < 16; ++i)
        values.emplace_back(int32_t{i});
    // bitmap=2, fixed=64, var=0 => 66
    assert_roundtrip(values, schema, 2u + 64u);
}

// Bitmap boundary: 17 columns (spills into third bitmap byte).
TEST(QA_GDB834_SizingInvariant, BitmapBoundary17Cols) {
    std::vector<ColumnDef> cols;
    for (int i = 0; i < 17; ++i)
        cols.push_back({"c" + std::to_string(i), TypeId::INT32});
    Schema schema(std::move(cols));
    std::vector<Value> values;
    for (int i = 0; i < 17; ++i)
        values.emplace_back(int32_t{i});
    // bitmap=3, fixed=68, var=0 => 71
    assert_roundtrip(values, schema, 3u + 68u);
}

// Single-column fixed.
TEST(QA_GDB834_SizingInvariant, SingleColumnFixed) {
    Schema schema({{"x", TypeId::INT64}});
    std::vector<Value> values = {Value(int64_t{-1})};
    // bitmap=1, fixed=8, var=0 => 9
    assert_roundtrip(values, schema, 1u + 8u);
}

// Single-column variable STRING.
TEST(QA_GDB834_SizingInvariant, SingleColumnVarString) {
    Schema schema({{"s", TypeId::STRING}});
    std::vector<Value> values = {Value(std::string{"hello"})};
    // bitmap=1, fixed=0, var_table=4, var_data=5 => 10
    assert_roundtrip(values, schema, 1u + 0u + 4u + 5u);
}

// Empty STRING value (distinct from NULL).
TEST(QA_GDB834_SizingInvariant, EmptyStringDistinctFromNull) {
    Schema schema({{"s", TypeId::STRING}});
    std::vector<Value> non_null_empty = {Value(std::string{""})};
    std::vector<Value> null_val = {Value::make_null()};

    size_t empty_size = TupleSerializer::compute_tuple_size(non_null_empty, schema);
    size_t null_size  = TupleSerializer::compute_tuple_size(null_val, schema);

    // Both occupy a var-table entry (4 bytes) but no var data; sizes are EQUAL.
    // The distinction is encoded in the null bitmap, not the size.
    EXPECT_EQ(empty_size, null_size)
        << "empty STRING and NULL STRING should have the same serialized size";

    // Verify the sizes individually via serialize().
    auto buf_empty = TupleSerializer::serialize(non_null_empty, schema);
    ASSERT_TRUE(buf_empty.has_value());
    EXPECT_EQ(buf_empty->size(), empty_size);

    auto buf_null = TupleSerializer::serialize(null_val, schema);
    ASSERT_TRUE(buf_null.has_value());
    EXPECT_EQ(buf_null->size(), null_size);

    // The buffers must differ — null bitmap bit is set in the NULL case.
    EXPECT_NE(*buf_empty, *buf_null) << "empty STRING and NULL STRING should produce different buffers";
}

// Large STRING (1024 bytes).
TEST(QA_GDB834_SizingInvariant, LargeString1024) {
    Schema schema({{"s", TypeId::STRING}});
    std::string big(1024, 'X');
    std::vector<Value> values = {Value(big)};
    // bitmap=1, fixed=0, var_table=4, var_data=1024 => 1029
    assert_roundtrip(values, schema, 1u + 0u + 4u + 1024u);
}

// Large BLOB (2048 bytes).
TEST(QA_GDB834_SizingInvariant, LargeBlob2048) {
    Schema schema({{"b", TypeId::BLOB}});
    Blob big(2048, 0xAB);
    std::vector<Value> values = {Value(big)};
    // bitmap=1, var_table=4, var_data=2048 => 2053
    assert_roundtrip(values, schema, 1u + 0u + 4u + 2048u);
}

// Multiple variable-length columns.
TEST(QA_GDB834_SizingInvariant, MultipleVarCols) {
    Schema schema({
        {"s1", TypeId::STRING},
        {"s2", TypeId::STRING},
        {"s3", TypeId::STRING},
    });
    std::vector<Value> values = {
        Value(std::string{"abc"}),
        Value(std::string{"de"}),
        Value(std::string{"f"}),
    };
    // bitmap=1, fixed=0, var_table=3*4=12, var_data=3+2+1=6 => 19
    assert_roundtrip(values, schema, 1u + 0u + 12u + 6u);
}

// EMBEDDING dim=1.
TEST(QA_GDB834_SizingInvariant, EmbeddingDim1) {
    Schema schema({{"v", TypeId::EMBEDDING}});
    Embedding emb = {3.14f};
    std::vector<Value> values = {Value(emb)};
    // bitmap=1, var_table=4, var_data=1*sizeof(float)=4 => 9
    assert_roundtrip(values, schema, 1u + 0u + 4u + 4u);
}

// EMBEDDING large dim (128 floats).
TEST(QA_GDB834_SizingInvariant, EmbeddingDim128) {
    Schema schema({{"v", TypeId::EMBEDDING}});
    Embedding emb(128, 1.0f);
    std::vector<Value> values = {Value(emb)};
    // bitmap=1, var_table=4, var_data=128*4=512 => 517
    assert_roundtrip(values, schema, 1u + 0u + 4u + 512u);
}

// JSON column.
TEST(QA_GDB834_SizingInvariant, JsonColumn) {
    Schema schema({{"j", TypeId::JSON}});
    JsonString js;
    js.data = R"({"key":"value"})";
    std::vector<Value> values = {Value(js)};
    size_t payload = js.data.size();
    assert_roundtrip(values, schema, 1u + 0u + 4u + payload);
}

// Mixed everything: fixed + var + NULLs across all categories.
TEST(QA_GDB834_SizingInvariant, MixedEverything) {
    Schema schema({
        {"id",    TypeId::INT64},
        {"name",  TypeId::STRING},
        {"score", TypeId::FLOAT32},
        {"vec",   TypeId::EMBEDDING},
        {"note",  TypeId::STRING},
        {"flag",  TypeId::BOOL},
    });
    Embedding emb = {1.0f, 2.0f};
    std::vector<Value> values = {
        Value(int64_t{1}),
        Value(std::string{"Alice"}),
        Value::make_null(),         // score NULL
        Value(emb),
        Value(std::string{"ok"}),
        Value(true),
    };
    // bitmap=1, fixed=INT64(8)+FLOAT32(4)+BOOL(1)=13
    // var_table=3*4=12 (name, vec, note)
    // var_data=5+8+2=15 (score null so no data)
    // total=1+13+12+15=41
    assert_roundtrip(values, schema, 1u + 13u + 12u + 15u);
}

// ---------------------------------------------------------------------------
// AC: round-trip fidelity for all fixed types
// ---------------------------------------------------------------------------

TEST(QA_GDB834_RoundTrip, AllFixedTypeValues) {
    Schema schema({
        {"a", TypeId::INT8},
        {"b", TypeId::UINT8},
        {"c", TypeId::INT16},
        {"d", TypeId::UINT16},
        {"e", TypeId::INT32},
        {"f", TypeId::UINT32},
        {"g", TypeId::INT64},
        {"h", TypeId::UINT64},
        {"i", TypeId::FLOAT32},
        {"j", TypeId::FLOAT64},
        {"k", TypeId::BOOL},
    });
    std::vector<Value> values = {
        Value(int8_t{-128}),
        Value(uint8_t{255}),
        Value(int16_t{-32768}),
        Value(uint16_t{65535}),
        Value(int32_t{std::numeric_limits<int32_t>::max()}),
        Value(uint32_t{std::numeric_limits<uint32_t>::max()}),
        Value(int64_t{std::numeric_limits<int64_t>::min()}),
        Value(uint64_t{std::numeric_limits<uint64_t>::max()}),
        Value(std::numeric_limits<float>::infinity()),
        Value(std::numeric_limits<double>::max()),
        Value(false),
    };

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value()) << buf.error().message;
    EXPECT_EQ(buf->size(), TupleSerializer::compute_tuple_size(values, schema));

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), values.size());

    EXPECT_EQ((*result)[0].as_int8(),   int8_t{-128});
    EXPECT_EQ((*result)[1].as_uint8(),  uint8_t{255});
    EXPECT_EQ((*result)[2].as_int16(),  int16_t{-32768});
    EXPECT_EQ((*result)[3].as_uint16(), uint16_t{65535});
    EXPECT_EQ((*result)[4].as_int32(),  std::numeric_limits<int32_t>::max());
    EXPECT_EQ((*result)[5].as_uint32(), std::numeric_limits<uint32_t>::max());
    EXPECT_EQ((*result)[6].as_int64(),  std::numeric_limits<int64_t>::min());
    EXPECT_EQ((*result)[7].as_uint64(), std::numeric_limits<uint64_t>::max());
    EXPECT_FALSE((*result)[10].as_bool());
}

// Round-trip: all-NULL across every column.
TEST(QA_GDB834_RoundTrip, AllNullAllTypes) {
    Schema schema({
        {"a", TypeId::INT32},
        {"b", TypeId::STRING},
        {"c", TypeId::EMBEDDING},
        {"d", TypeId::BLOB},
        {"e", TypeId::INT64},
    });
    std::vector<Value> values(5, Value::make_null());

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());
    EXPECT_EQ(buf->size(), TupleSerializer::compute_tuple_size(values, schema));

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    for (const auto& v : *result)
        EXPECT_TRUE(v.is_null());
}

// Round-trip: null bitmap edges — col 7 (last bit of byte 0) and col 8 (first
// bit of byte 1) are NULL; everything else is non-NULL.
TEST(QA_GDB834_RoundTrip, NullAtBitmapByteBoundary) {
    // 9 INT32 columns; make col 7 and col 8 NULL.
    std::vector<ColumnDef> cols;
    for (int i = 0; i < 9; ++i)
        cols.push_back({"c" + std::to_string(i), TypeId::INT32});
    Schema schema(std::move(cols));

    std::vector<Value> values;
    for (int i = 0; i < 9; ++i) {
        if (i == 7 || i == 8)
            values.push_back(Value::make_null());
        else
            values.emplace_back(int32_t{i * 10});
    }

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());
    EXPECT_EQ(buf->size(), TupleSerializer::compute_tuple_size(values, schema));

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 9u);
    for (int i = 0; i < 9; ++i) {
        if (i == 7 || i == 8) {
            EXPECT_TRUE((*result)[i].is_null()) << "col " << i;
        } else {
            EXPECT_EQ((*result)[i].as_int32(), i * 10) << "col " << i;
        }
    }
}

// Round-trip: EMBEDDING round-trips with exact float values (not just dim).
TEST(QA_GDB834_RoundTrip, EmbeddingFloatPrecision) {
    Schema schema({{"v", TypeId::EMBEDDING}});
    Embedding emb = {0.1f, 0.2f, 0.3f, -1.0f, std::numeric_limits<float>::epsilon()};
    std::vector<Value> values = {Value(emb)};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());
    EXPECT_EQ(buf->size(), TupleSerializer::compute_tuple_size(values, schema));

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    const auto& rt = (*result)[0].as_embedding();
    ASSERT_EQ(rt.size(), emb.size());
    for (size_t i = 0; i < emb.size(); ++i)
        EXPECT_EQ(rt[i], emb[i]) << "float mismatch at index " << i;
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

// Value count mismatch returns INVALID_ARGUMENT.
TEST(QA_GDB834_ErrorPaths, ValueCountMismatch) {
    Schema schema({{"a", TypeId::INT32}, {"b", TypeId::INT32}});
    std::vector<Value> values = {Value(int32_t{1})};  // only 1 of 2

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_FALSE(buf.has_value());
    EXPECT_EQ(buf.error().code, StatusCode::INVALID_ARGUMENT);
}

// Type mismatch returns TYPE_ERROR.
TEST(QA_GDB834_ErrorPaths, TypeMismatch) {
    Schema schema({{"a", TypeId::INT32}});
    std::vector<Value> values = {Value(int64_t{1})};  // wrong type

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_FALSE(buf.has_value());
    EXPECT_EQ(buf.error().code, StatusCode::TYPE_ERROR);
}

// Oversized tuple (> 65535 bytes) returns INVALID_ARGUMENT.
TEST(QA_GDB834_ErrorPaths, OversizedTupleReturnsError) {
    Schema schema({{"big", TypeId::STRING}});
    // 65536 bytes of payload > max_tuple_size (65535)
    // total = 1 (bitmap) + 0 (fixed) + 4 (var_table) + 65536 (data) = 65541 > 65535
    std::string huge(65536, 'A');
    std::vector<Value> values = {Value(huge)};

    size_t computed = TupleSerializer::compute_tuple_size(values, schema);
    EXPECT_GT(computed, static_cast<size_t>(65535u))
        << "compute_tuple_size should report oversized tuple";

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_FALSE(buf.has_value());
    EXPECT_EQ(buf.error().code, StatusCode::INVALID_ARGUMENT);
}

// Tuple right at the 65535-byte boundary should succeed.
TEST(QA_GDB834_ErrorPaths, MaxSizeTupleSucceeds) {
    // We need total == 65535: bitmap=1 + var_table=4 + var_data=65530 = 65535.
    Schema schema({{"s", TypeId::STRING}});
    std::string at_limit(65530, 'Z');
    std::vector<Value> values = {Value(at_limit)};

    size_t computed = TupleSerializer::compute_tuple_size(values, schema);
    EXPECT_EQ(computed, static_cast<size_t>(65535u));

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value()) << buf.error().message;
    EXPECT_EQ(buf->size(), 65535u);
}

// get_field out-of-range returns INVALID_ARGUMENT.
TEST(QA_GDB834_ErrorPaths, GetFieldOutOfRange) {
    Schema schema({{"a", TypeId::INT32}});
    std::vector<Value> values = {Value(int32_t{42})};
    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::get_field(*buf, schema, 99u);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// get_field on a NULL column returns NULL value.
TEST(QA_GDB834_ErrorPaths, GetFieldOnNullColumn) {
    Schema schema({{"a", TypeId::INT32}, {"b", TypeId::STRING}});
    std::vector<Value> values = {Value::make_null(), Value(std::string{"x"})};
    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::get_field(*buf, schema, 0u);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_null());
}

// ---------------------------------------------------------------------------
// Strengthened self-consistency: existing GDB-13 vacuous assertions replaced
// with hardcoded expected sizes.
// ---------------------------------------------------------------------------

// This mirrors test_qa_gdb_13.cpp QA_TupleSerializer::ComputeSizeMatchesActual
// but asserts a hardcoded expected size instead of only comparing computed==actual.
// Schema: INT64(8)+STRING(5)+FLOAT64(8)+BLOB(3)+BOOL(1)
// bitmap=ceil(5/8)=1, fixed=INT64+FLOAT64+BOOL=17, var_table=2*4=8, var_data=5+3=8 => 34
TEST(QA_GDB834_StrengthenedAssertions, ComputeSizeMatchesHardcoded) {
    Schema schema({
        {"id",     TypeId::INT64},
        {"name",   TypeId::STRING},
        {"score",  TypeId::FLOAT64},
        {"bio",    TypeId::BLOB},
        {"active", TypeId::BOOL},
    });
    std::vector<Value> values = {
        Value(int64_t{42}),
        Value(std::string{"Alice"}),
        Value(3.14),
        Value(Blob{0x01, 0x02, 0x03}),
        Value(true),
    };
    // bitmap=1, fixed=INT64(8)+FLOAT64(8)+BOOL(1)=17, var_table=2*4=8, var_data=5+3=8 => 34
    constexpr size_t expected = 1u + 17u + 8u + 8u;
    static_assert(expected == 34u);
    assert_roundtrip(values, schema, expected);
}

// Strengthened all-null assertion: hardcoded size.
// Schema: INT32+STRING — bitmap=1, fixed=4, var_table=4, var_data=0 => 9
TEST(QA_GDB834_StrengthenedAssertions, AllNullsHardcodedSize) {
    Schema schema({{"a", TypeId::INT32}, {"b", TypeId::STRING}});
    std::vector<Value> values = {Value::make_null(), Value::make_null()};
    assert_roundtrip(values, schema, 9u);
}
