/// @file test_qa_gdb_12.cpp
/// @brief Adversarial QA tests for GDB-12: Type System
///
/// Covers: TypeId edge cases, Value NULL/type-id collision, signed-to-unsigned
/// coercion (BUG), DECIMAL cross-type comparison (BUG), boundary values,
/// serialization error paths, and stress scenarios.

#include "giodb/common/coercion.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/storage/serialization.h"

#include <gtest/gtest.h>

#include <cmath>
#include <compare>
#include <limits>

using namespace giodb;

// ============================================================================
// QA_TypeId — TypeId enum and helper function edge cases
// ============================================================================

TEST(QA_TypeId, AllTwentyTwoTypesPresent) {
    // Enumerate all 22 TypeIds and verify type_name returns a non-empty string.
    std::vector<TypeId> all_types = {
        TypeId::INT8,    TypeId::INT16,     TypeId::INT32,    TypeId::INT64,   TypeId::UINT8,
        TypeId::UINT16,  TypeId::UINT32,    TypeId::UINT64,   TypeId::FLOAT32, TypeId::FLOAT64,
        TypeId::DECIMAL, TypeId::BOOL,      TypeId::STRING,   TypeId::BLOB,    TypeId::DATE,
        TypeId::TIME,    TypeId::TIMESTAMP, TypeId::INTERVAL, TypeId::POINT,   TypeId::JSON,
        TypeId::UUID,    TypeId::EMBEDDING,
    };
    EXPECT_EQ(all_types.size(), 22u);
    for (auto t : all_types) {
        EXPECT_FALSE(type_name(t).empty())
            << "type_name returned empty for type index " << static_cast<int>(t);
    }
}

TEST(QA_TypeId, FixedSizeConsistencyWithAlignment) {
    // For every fixed-size type, the reported alignment should not exceed the
    // fixed size.
    std::vector<TypeId> all_types = {
        TypeId::INT8,
        TypeId::INT16,
        TypeId::INT32,
        TypeId::INT64,
        TypeId::UINT8,
        TypeId::UINT16,
        TypeId::UINT32,
        TypeId::UINT64,
        TypeId::FLOAT32,
        TypeId::FLOAT64,
        TypeId::DECIMAL,
        TypeId::BOOL,
        TypeId::DATE,
        TypeId::TIME,
        TypeId::TIMESTAMP,
        TypeId::INTERVAL,
        TypeId::POINT,
        TypeId::UUID,
    };
    for (auto t : all_types) {
        auto sz = fixed_size(t);
        ASSERT_TRUE(sz.has_value()) << "Expected fixed-size for " << type_name(t);
        EXPECT_GE(*sz, alignment(t)) << "Alignment exceeds size for " << type_name(t);
    }
}

TEST(QA_TypeId, VariableLengthTypesHaveNoFixedSize) {
    // The four variable-length types must all return nullopt from fixed_size.
    EXPECT_FALSE(fixed_size(TypeId::STRING).has_value());
    EXPECT_FALSE(fixed_size(TypeId::BLOB).has_value());
    EXPECT_FALSE(fixed_size(TypeId::JSON).has_value());
    EXPECT_FALSE(fixed_size(TypeId::EMBEDDING).has_value());
}

TEST(QA_TypeId, IsComparableExcludesOnlyBlobAndEmbedding) {
    // Only BLOB and EMBEDDING are not comparable.
    EXPECT_FALSE(is_comparable(TypeId::BLOB));
    EXPECT_FALSE(is_comparable(TypeId::EMBEDDING));

    // All other 20 types must be comparable.
    std::vector<TypeId> comparable_types = {
        TypeId::INT8,      TypeId::INT16,    TypeId::INT32,  TypeId::INT64,   TypeId::UINT8,
        TypeId::UINT16,    TypeId::UINT32,   TypeId::UINT64, TypeId::FLOAT32, TypeId::FLOAT64,
        TypeId::DECIMAL,   TypeId::BOOL,     TypeId::STRING, TypeId::DATE,    TypeId::TIME,
        TypeId::TIMESTAMP, TypeId::INTERVAL, TypeId::POINT,  TypeId::JSON,    TypeId::UUID,
    };
    for (auto t : comparable_types) {
        EXPECT_TRUE(is_comparable(t)) << type_name(t) << " should be comparable";
    }
}

// ============================================================================
// QA_Value — NULL / type_id collision and accessor edge cases
// ============================================================================

TEST(QA_Value, NullTypeIdReturnsInt8ButIsNullIsTrue) {
    // Documented: type_id() returns INT8 for NULL. Verify this is detectable.
    Value null_val;
    EXPECT_TRUE(null_val.is_null());
    // The type_id() returns INT8 as a placeholder — callers MUST check is_null().
    // This test ensures callers cannot rely on type_id() alone to detect NULL.
    EXPECT_EQ(null_val.type_id(), TypeId::INT8);

    Value real_int8(int8_t{42});
    EXPECT_FALSE(real_int8.is_null());
    EXPECT_EQ(real_int8.type_id(), TypeId::INT8);

    // Both have the same type_id! Only is_null() distinguishes them.
    EXPECT_EQ(null_val.type_id(), real_int8.type_id());
}

TEST(QA_Value, NullThrowsOnAllTypedAccessors) {
    Value v;
    EXPECT_THROW((void)v.as_int8(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_int16(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_int32(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_int64(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_uint8(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_uint16(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_uint32(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_uint64(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_float32(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_float64(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_decimal(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_bool(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_string(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_blob(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_date(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_time(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_timestamp(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_interval(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_point(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_json(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_uuid(), std::bad_variant_access);
    EXPECT_THROW((void)v.as_embedding(), std::bad_variant_access);
}

TEST(QA_Value, TryAsReturnsErrorForAllTypeMismatches) {
    // Verify try_as_* returns TYPE_ERROR for a mismatched type, not a crash.
    Value v(int32_t{0});
    EXPECT_FALSE(v.try_as_string().has_value());
    EXPECT_FALSE(v.try_as_float64().has_value());
    EXPECT_FALSE(v.try_as_bool().has_value());
    EXPECT_FALSE(v.try_as_blob().has_value());
    EXPECT_FALSE(v.try_as_uuid().has_value());
    EXPECT_FALSE(v.try_as_embedding().has_value());
}

TEST(QA_Value, BoolVsInt8AreDistinct) {
    // bool and int8_t are distinct types in the variant — accessing one as the
    // other must throw.
    Value bool_val(true);
    EXPECT_EQ(bool_val.type_id(), TypeId::BOOL);
    EXPECT_THROW((void)bool_val.as_int8(), std::bad_variant_access);

    Value int8_val(int8_t{1});
    EXPECT_EQ(int8_val.type_id(), TypeId::INT8);
    EXPECT_THROW((void)int8_val.as_bool(), std::bad_variant_access);
}

TEST(QA_Value, MaxAndMinBoundaryValues) {
    // Verify correct storage and retrieval at numeric extremes.
    EXPECT_EQ(Value(std::numeric_limits<int8_t>::min()).as_int8(), -128);
    EXPECT_EQ(Value(std::numeric_limits<int8_t>::max()).as_int8(), 127);
    EXPECT_EQ(Value(std::numeric_limits<uint8_t>::max()).as_uint8(), 255u);
    EXPECT_EQ(Value(std::numeric_limits<int16_t>::min()).as_int16(), -32768);
    EXPECT_EQ(Value(std::numeric_limits<int16_t>::max()).as_int16(), 32767);
    EXPECT_EQ(Value(std::numeric_limits<uint16_t>::max()).as_uint16(), 65535u);
    EXPECT_EQ(Value(std::numeric_limits<int32_t>::min()).as_int32(), -2147483648);
    EXPECT_EQ(Value(std::numeric_limits<int32_t>::max()).as_int32(), 2147483647);
    EXPECT_EQ(Value(std::numeric_limits<uint32_t>::max()).as_uint32(), 4294967295u);
    EXPECT_EQ(Value(std::numeric_limits<int64_t>::min()).as_int64(),
              std::numeric_limits<int64_t>::min());
    EXPECT_EQ(Value(std::numeric_limits<int64_t>::max()).as_int64(),
              std::numeric_limits<int64_t>::max());
    EXPECT_EQ(Value(std::numeric_limits<uint64_t>::max()).as_uint64(),
              std::numeric_limits<uint64_t>::max());
}

// ============================================================================
// QA_Coercion — Adversarial coercion tests
// ============================================================================

// ADVERSARIAL: Signed-to-unsigned coercion must NOT be allowed.
// The spec says "Integers: smaller to larger (INT8→INT16→INT32→INT64)" and
// "Unsigned to signed of equal or larger size" — there is NO signed→unsigned
// path. The interleaved rank ordering (INT8=1, UINT8=2) incorrectly allows
// signed types to coerce to higher-ranked unsigned types.
TEST(QA_Coercion, SignedToUnsignedSameWidthNotAllowed) {
    // INT8 (-128..127) does not fit into UINT8 (0..255) for negative values.
    EXPECT_FALSE(can_coerce(TypeId::INT8, TypeId::UINT8))
        << "BUG: INT8 should NOT coerce to UINT8 (negative values cannot be represented)";

    EXPECT_FALSE(can_coerce(TypeId::INT16, TypeId::UINT16))
        << "BUG: INT16 should NOT coerce to UINT16";

    EXPECT_FALSE(can_coerce(TypeId::INT32, TypeId::UINT32))
        << "BUG: INT32 should NOT coerce to UINT32";

    EXPECT_FALSE(can_coerce(TypeId::INT64, TypeId::UINT64))
        << "BUG: INT64 should NOT coerce to UINT64";
}

TEST(QA_Coercion, SignedToLargerUnsignedNotAllowed) {
    // A signed integer should not be coercible to a larger unsigned type either,
    // because negative values have no valid representation.
    EXPECT_FALSE(can_coerce(TypeId::INT8, TypeId::UINT16))
        << "BUG: INT8 should NOT coerce to UINT16";

    EXPECT_FALSE(can_coerce(TypeId::INT8, TypeId::UINT32))
        << "BUG: INT8 should NOT coerce to UINT32";

    EXPECT_FALSE(can_coerce(TypeId::INT8, TypeId::UINT64))
        << "BUG: INT8 should NOT coerce to UINT64";

    EXPECT_FALSE(can_coerce(TypeId::INT32, TypeId::UINT64))
        << "BUG: INT32 should NOT coerce to UINT64";
}

TEST(QA_Coercion, CoerceNegativeSignedToUnsignedReturnsError) {
    // Coercing a negative signed value to any unsigned type must fail.
    auto r = coerce(Value(int8_t{-1}), TypeId::UINT8);
    EXPECT_FALSE(r.has_value()) << "BUG: coerce(INT8{-1}, UINT8) should return an error";

    r = coerce(Value(int32_t{-1}), TypeId::UINT32);
    EXPECT_FALSE(r.has_value()) << "BUG: coerce(INT32{-1}, UINT32) should return an error";

    r = coerce(Value(int64_t{-1}), TypeId::UINT64);
    EXPECT_FALSE(r.has_value()) << "BUG: coerce(INT64{-1}, UINT64) should return an error";
}

TEST(QA_Coercion, UnsignedToSignedNarrowing) {
    // UINT16 cannot coerce to INT8 or INT16 (too narrow or same size).
    EXPECT_FALSE(can_coerce(TypeId::UINT16, TypeId::INT8));
    EXPECT_FALSE(can_coerce(TypeId::UINT16, TypeId::INT16));
    EXPECT_TRUE(can_coerce(TypeId::UINT16, TypeId::INT32)); // INT32 is wider
    EXPECT_TRUE(can_coerce(TypeId::UINT16, TypeId::INT64)); // INT64 is wider
}

TEST(QA_Coercion, UINT64CannotCoerceToAnySigned) {
    // UINT64 can hold values > INT64_MAX, so it cannot coerce to any signed type.
    EXPECT_FALSE(can_coerce(TypeId::UINT64, TypeId::INT64));
    EXPECT_FALSE(can_coerce(TypeId::UINT64, TypeId::INT32));
    EXPECT_FALSE(can_coerce(TypeId::UINT64, TypeId::INT8));
}

TEST(QA_Coercion, CoerceAllValidIntegerWidenings) {
    // Verify the happy path — all legitimate widening coercions work.
    auto r = coerce(Value(int8_t{-128}), TypeId::INT64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->as_int64(), -128);

    r = coerce(Value(uint8_t{200}), TypeId::INT16);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->as_int16(), 200);

    r = coerce(Value(uint8_t{200}), TypeId::INT32);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->as_int32(), 200);

    r = coerce(Value(uint32_t{4000000000u}), TypeId::INT64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->as_int64(), 4000000000LL);
}

TEST(QA_Coercion, CoerceIntMaxToLargerSignedType) {
    // INT32_MAX should coerce correctly to INT64.
    auto r = coerce(Value(std::numeric_limits<int32_t>::max()), TypeId::INT64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->as_int64(), static_cast<int64_t>(std::numeric_limits<int32_t>::max()));
}

TEST(QA_Coercion, CoerceNullPreservesNullForAllTargets) {
    // NULL coerced to any type must remain NULL.
    std::vector<TypeId> targets = {
        TypeId::INT8, TypeId::INT64, TypeId::FLOAT64, TypeId::DECIMAL, TypeId::STRING};
    for (auto t : targets) {
        auto r = coerce(Value::make_null(), t);
        ASSERT_TRUE(r.has_value()) << "coerce(NULL, " << type_name(t) << ") should succeed";
        EXPECT_TRUE(r->is_null()) << "coerce(NULL, " << type_name(t) << ") should remain NULL";
    }
}

// ============================================================================
// QA_Compare — Adversarial comparison tests
// ============================================================================

TEST(QA_Compare, DecimalCrossTypeComparisonWithInt) {
    // compare(INT32{5}, DECIMAL{0, 5}) — DECIMAL represents the integer 5.
    // Both sides equal 5, so the result should be equal.
    // BUG: to_double() has no DECIMAL case and returns 0.0, causing this to
    // report INT32{5} > DECIMAL{0,5}.
    auto r = compare(Value(int32_t{5}), Value(Decimal128{0, 5ULL}));
    ASSERT_TRUE(r.has_value()) << "BUG: compare(INT32{5}, DECIMAL{0,5}) should not return an error";
    EXPECT_EQ(*r, std::strong_ordering::equal)
        << "BUG: compare(INT32{5}, DECIMAL{0,5}) should be equal";
}

TEST(QA_Compare, DecimalCrossTypeComparisonGreater) {
    // compare(INT32{10}, DECIMAL{0, 5}) — INT32 is greater.
    auto r = compare(Value(int32_t{10}), Value(Decimal128{0, 5ULL}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::greater)
        << "BUG: compare(INT32{10}, DECIMAL{0,5}) should be greater";
}

TEST(QA_Compare, DecimalCrossTypeComparisonLess) {
    // compare(INT32{3}, DECIMAL{0, 5}) — INT32 is less.
    auto r = compare(Value(int32_t{3}), Value(Decimal128{0, 5ULL}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less)
        << "BUG: compare(INT32{3}, DECIMAL{0,5}) should be less";
}

TEST(QA_Compare, DecimalSameTypeZeroValues) {
    // Both sides are DECIMAL{0,0} — must be equal.
    auto r = compare(Value(Decimal128{0, 0}), Value(Decimal128{0, 0}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::equal);
}

TEST(QA_Compare, DecimalSameTypeNegativeValues) {
    // DECIMAL{-1, 0} represents a large negative number.
    // DECIMAL{-1, UINT64_MAX} represents -1 (in 128-bit 2's complement).
    // -2^64 < -1, so DECIMAL{-1,0} < DECIMAL{-1,UINT64_MAX}.
    auto r = compare(Value(Decimal128{-1, 0ULL}),
                     Value(Decimal128{-1, std::numeric_limits<uint64_t>::max()}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);
}

TEST(QA_Compare, NullVsNullIsEqual) {
    auto r = compare(Value::make_null(), Value::make_null());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::equal);
}

TEST(QA_Compare, NullSortsBeforeAllNonNullTypes) {
    // NULL must sort before every non-NULL type.
    std::vector<Value> non_nulls = {
        Value(int8_t{0}),
        Value(int64_t{0}),
        Value(uint64_t{0}),
        Value(0.0f),
        Value(0.0),
        Value(false),
        Value(std::string{""}),
        Value(Date{0}),
        Value(Time{0}),
        Value(Timestamp{0}),
        Value(Decimal128{0, 0}),
        Value(Interval{0, 0}),
        Value(Point{0.0, 0.0}),
    };
    for (const auto& v : non_nulls) {
        auto r = compare(Value::make_null(), v);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(*r, std::strong_ordering::less)
            << "NULL should be less than non-null " << type_name(v.type_id());

        r = compare(v, Value::make_null());
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(*r, std::strong_ordering::greater)
            << "Non-null " << type_name(v.type_id()) << " should be greater than NULL";
    }
}

TEST(QA_Compare, BlobCompareShouldReturnError) {
    // BLOB is not comparable — compare must return an error, not crash.
    auto r = compare(Value(Blob{0x01}), Value(Blob{0x02}));
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_Compare, EmbeddingCompareShouldReturnError) {
    auto r = compare(Value(Embedding{0.5f}), Value(Embedding{1.0f}));
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_Compare, IncompatibleTypesStringVsInt) {
    auto r = compare(Value(std::string{"42"}), Value(int32_t{42}));
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_Compare, IncompatibleTypesDateVsInt) {
    auto r = compare(Value(Date{100}), Value(int32_t{100}));
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_Compare, SignedUnsignedBoundary_Int64MaxVsUint64) {
    // INT64_MAX (9223372036854775807) vs UINT64 value of INT64_MAX+1.
    uint64_t u = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ULL;
    auto r = compare(Value(std::numeric_limits<int64_t>::max()), Value(u));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);
}

TEST(QA_Compare, UINT64MaxVsInt64Zero) {
    // UINT64_MAX (18446744073709551615) vs INT64{0}.
    auto r = compare(Value(std::numeric_limits<uint64_t>::max()), Value(int64_t{0}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::greater);
}

TEST(QA_Compare, NegativeInt64VsUint64Zero) {
    // INT64{-1} vs UINT64{0}: negative signed is always less.
    auto r = compare(Value(int64_t{-1}), Value(uint64_t{0}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);
}

TEST(QA_Compare, FloatNaNVsInfinity) {
    auto nan = std::numeric_limits<double>::quiet_NaN();
    auto inf = std::numeric_limits<double>::infinity();

    // NaN > infinity (per implementation: NaN sorts last).
    auto r = compare(Value(nan), Value(inf));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::greater);

    // NaN > -infinity.
    r = compare(Value(nan), Value(-inf));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::greater);
}

TEST(QA_Compare, PointWithNaNCoordinates) {
    // A Point with NaN x — NaN sorts greater than normal x values.
    Point nan_x{std::numeric_limits<double>::quiet_NaN(), 0.0};
    Point normal{0.0, 0.0};
    auto r = compare(Value(nan_x), Value(normal));
    ASSERT_TRUE(r.has_value());
    // NaN x is greater than 0.0 x.
    EXPECT_EQ(*r, std::strong_ordering::greater);
}

TEST(QA_Compare, IntervalLexicographicOrdering) {
    // Interval{2, 0} (2 months) vs Interval{1, 86400000000} (1 month + 1 day).
    // months component takes priority: 2 > 1 so first interval is greater.
    auto r = compare(Value(Interval{2, 0}), Value(Interval{1, 86400000000LL}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::greater);
}

TEST(QA_Compare, UuidLexicographicOrdering) {
    // UUID differing only in last byte.
    Uuid a = {0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0x00};
    Uuid b = {0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF,
              0xFF};
    auto r = compare(Value(a), Value(b));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);
}

TEST(QA_Compare, StringEmptyVsNonEmpty) {
    auto r = compare(Value(std::string{""}), Value(std::string{"a"}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);
}

TEST(QA_Compare, BoolOrdering) {
    auto r = compare(Value(false), Value(true));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);

    r = compare(Value(true), Value(false));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::greater);

    r = compare(Value(true), Value(true));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::equal);
}

// ============================================================================
// QA_Serialization — Adversarial serialization edge cases
// ============================================================================

TEST(QA_Serialization, SerializedSizeMatchesActualBufferSize) {
    // For every Value, serialized_size() must exactly match serialize().size().
    std::vector<Value> values = {
        Value::make_null(),
        Value(int8_t{-128}),
        Value(int64_t{std::numeric_limits<int64_t>::max()}),
        Value(uint64_t{std::numeric_limits<uint64_t>::max()}),
        Value(0.0f),
        Value(std::numeric_limits<double>::quiet_NaN()),
        Value(std::numeric_limits<double>::infinity()),
        Value(std::string{""}),
        Value(std::string(1024, 'A')),
        Value(Blob{0x00, 0xFF, 0x00}),
        Value(Decimal128{-1, std::numeric_limits<uint64_t>::max()}),
        Value(Date{-1}),
        Value(Timestamp{std::numeric_limits<int64_t>::min()}),
        Value(Interval{-12, -86400000000LL}),
        Value(Point{-180.0, -90.0}),
        Value(JsonString{""}),
        Value(JsonString{R"([1,2,3])"}),
        Value(Embedding{}),
        Value(Embedding{1.0f, 2.0f, 3.0f}),
    };
    for (const auto& v : values) {
        auto bytes = serialize(v);
        EXPECT_EQ(bytes.size(), serialized_size(v))
            << "Size mismatch for type "
            << (v.is_null() ? "NULL" : std::string(type_name(v.type_id())));
    }
}

TEST(QA_Serialization, RoundTripAllBoundaryIntegers) {
    auto rt = [](const Value& v, TypeId t) {
        auto bytes = serialize(v);
        auto result = deserialize(bytes, t);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->data(), v.data());
    };

    rt(Value(std::numeric_limits<int8_t>::min()), TypeId::INT8);
    rt(Value(std::numeric_limits<int8_t>::max()), TypeId::INT8);
    rt(Value(std::numeric_limits<int16_t>::min()), TypeId::INT16);
    rt(Value(std::numeric_limits<int16_t>::max()), TypeId::INT16);
    rt(Value(std::numeric_limits<int32_t>::min()), TypeId::INT32);
    rt(Value(std::numeric_limits<int32_t>::max()), TypeId::INT32);
    rt(Value(std::numeric_limits<int64_t>::min()), TypeId::INT64);
    rt(Value(std::numeric_limits<int64_t>::max()), TypeId::INT64);
    rt(Value(std::numeric_limits<uint8_t>::max()), TypeId::UINT8);
    rt(Value(std::numeric_limits<uint16_t>::max()), TypeId::UINT16);
    rt(Value(std::numeric_limits<uint32_t>::max()), TypeId::UINT32);
    rt(Value(std::numeric_limits<uint64_t>::max()), TypeId::UINT64);
}

TEST(QA_Serialization, RoundTripNegativeDateAndTimestamp) {
    auto bytes = serialize(Value(Date{-365})); // 1969-01-01
    auto result = deserialize(bytes, TypeId::DATE);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->as_date().days_since_epoch, -365);

    bytes = serialize(Value(Timestamp{std::numeric_limits<int64_t>::min()}));
    result = deserialize(bytes, TypeId::TIMESTAMP);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->as_timestamp().microseconds, std::numeric_limits<int64_t>::min());
}

TEST(QA_Serialization, RoundTripStringWithNullAndSpecialBytes) {
    // String containing embedded null bytes, DEL, and high bytes.
    std::string s;
    s.push_back('\0');
    s.push_back('\x01');
    s.push_back('\x7F');
    s.push_back('\xFF');
    s.push_back('\0');

    auto bytes = serialize(Value(s));
    auto result = deserialize(bytes, TypeId::STRING);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->as_string(), s);
    EXPECT_EQ(result->as_string().size(), 5u);
}

TEST(QA_Serialization, RoundTripLargeEmbedding) {
    // 1024-element embedding.
    Embedding e(1024);
    for (size_t i = 0; i < e.size(); ++i) {
        e[i] = static_cast<float>(i) * 0.001f;
    }
    Value v(e);
    auto bytes = serialize(v);
    EXPECT_EQ(bytes.size(), serialized_size(v));

    auto result = deserialize(bytes, TypeId::EMBEDDING);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->as_embedding().size(), 1024u);
    for (size_t i = 0; i < 1024u; ++i) {
        EXPECT_FLOAT_EQ(result->as_embedding()[i], e[i]);
    }
}

TEST(QA_Serialization, RoundTripUuidAllBytes) {
    // UUID with all distinct bytes, ensure all 16 bytes survive round-trip.
    Uuid u;
    for (size_t i = 0; i < 16; ++i) {
        u[i] = static_cast<uint8_t>(i * 17); // 0, 17, 34, ..., 255
    }
    auto bytes = serialize(Value(u));
    auto result = deserialize(bytes, TypeId::UUID);
    ASSERT_TRUE(result.has_value());
    for (size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(result->as_uuid()[i], u[i]) << "UUID byte " << i << " mismatch";
    }
}

TEST(QA_Serialization, DeserializeEmptyBufferReturnsError) {
    std::vector<uint8_t> empty;
    auto r = deserialize(empty, TypeId::INT32);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_Serialization, DeserializeInvalidNullFlagReturnsError) {
    for (uint8_t flag : {0x02u, 0xFFu, 0x80u}) {
        std::vector<uint8_t> bad = {flag, 0x00, 0x00, 0x00, 0x00};
        auto r = deserialize(bad, TypeId::INT32);
        EXPECT_FALSE(r.has_value())
            << "null flag 0x" << std::hex << static_cast<int>(flag) << " should be rejected";
        EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    }
}

TEST(QA_Serialization, DeserializeTruncatedFixedTypes) {
    // Provide only the null flag with insufficient payload bytes.
    std::vector<std::pair<TypeId, size_t>> fixed_types = {
        {TypeId::INT8, 1},
        {TypeId::INT16, 2},
        {TypeId::INT32, 4},
        {TypeId::INT64, 8},
        {TypeId::DECIMAL, 16},
        {TypeId::UUID, 16},
        {TypeId::POINT, 16},
    };
    for (auto [t, sz] : fixed_types) {
        // Give only sz-1 bytes after the null flag.
        std::vector<uint8_t> truncated(sz, 0x00);
        truncated[0] = 0x01; // null flag = not null
        // truncated has 1 (flag) + sz-1 (partial payload) bytes
        auto r = deserialize(truncated, t);
        EXPECT_FALSE(r.has_value())
            << "Truncated buffer for " << type_name(t) << " should return error";
        EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    }
}

TEST(QA_Serialization, DeserializeCraftedStringLengthOverflow) {
    // Buffer with a length field claiming 1 billion bytes but only 5 actual bytes.
    // Deserialize should detect the mismatch and return an error.
    std::vector<uint8_t> crafted = {
        0x01, // not null
        0x00,
        0xCA,
        0x9A,
        0x3B, // len = 0x3B9ACA00 = 1,000,000,000
        0x41,
        0x42,
        0x43, // only 3 bytes of "ABC"
    };
    auto r = deserialize(crafted, TypeId::STRING);
    EXPECT_FALSE(r.has_value()) << "Crafted oversized length should be rejected by deserialize";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_Serialization, DeserializeCraftedEmbeddingCountOverflow) {
    // Buffer claims 1,000,000 floats but only provides 4 bytes of payload.
    std::vector<uint8_t> crafted = {
        0x01, // not null
        0x40,
        0x42,
        0x0F,
        0x00, // count = 1,000,000
        0x00,
        0x00,
        0x80,
        0x3F, // only 1 float (4 bytes)
    };
    auto r = deserialize(crafted, TypeId::EMBEDDING);
    EXPECT_FALSE(r.has_value()) << "Embedding with crafted oversized count should be rejected";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_Serialization, NullDeserializesWithAnyTypeId) {
    // A null-encoded buffer (single 0x00 byte) should deserialize to NULL
    // regardless of the requested TypeId.
    std::vector<uint8_t> null_buf = {0x00};
    std::vector<TypeId> types = {
        TypeId::INT8, TypeId::INT64, TypeId::STRING, TypeId::DECIMAL, TypeId::EMBEDDING};
    for (auto t : types) {
        auto r = deserialize(null_buf, t);
        ASSERT_TRUE(r.has_value()) << "NULL deserialization failed for " << type_name(t);
        EXPECT_TRUE(r->is_null()) << "Expected NULL for type " << type_name(t);
    }
}

TEST(QA_Serialization, LittleEndianEncodingInt64) {
    // INT64 value 0x0102030405060708 should encode LSB first.
    Value v(int64_t{0x0102030405060708LL});
    auto bytes = serialize(v);
    ASSERT_EQ(bytes.size(), 9u); // 1 flag + 8 data
    EXPECT_EQ(bytes[0], 0x01);   // null flag
    EXPECT_EQ(bytes[1], 0x08);   // LSB
    EXPECT_EQ(bytes[2], 0x07);
    EXPECT_EQ(bytes[3], 0x06);
    EXPECT_EQ(bytes[4], 0x05);
    EXPECT_EQ(bytes[5], 0x04);
    EXPECT_EQ(bytes[6], 0x03);
    EXPECT_EQ(bytes[7], 0x02);
    EXPECT_EQ(bytes[8], 0x01); // MSB
}

TEST(QA_Serialization, LittleEndianEncodingUUID) {
    // UUID bytes must be stored verbatim (no endian conversion — it is a byte
    // array, not an integer).
    Uuid u = {0x00,
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
              0x0C,
              0x0D,
              0x0E,
              0x0F};
    auto bytes = serialize(Value(u));
    ASSERT_EQ(bytes.size(), 17u); // 1 flag + 16 data
    for (size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(bytes[i + 1], u[i]) << "UUID byte " << i << " not stored verbatim";
    }
}

TEST(QA_Serialization, BoolEncodingIsZeroOrOne) {
    auto t_bytes = serialize(Value(true));
    ASSERT_EQ(t_bytes.size(), 2u);
    EXPECT_EQ(t_bytes[1], 0x01);

    auto f_bytes = serialize(Value(false));
    ASSERT_EQ(f_bytes.size(), 2u);
    EXPECT_EQ(f_bytes[1], 0x00);
}

TEST(QA_Serialization, DecimalRoundTripNegativeValue) {
    // DECIMAL{-1, UINT64_MAX} represents -1 in 128-bit 2's complement.
    Decimal128 neg_one{-1, std::numeric_limits<uint64_t>::max()};
    auto bytes = serialize(Value(neg_one));
    auto result = deserialize(bytes, TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->as_decimal().hi, -1);
    EXPECT_EQ(result->as_decimal().lo, std::numeric_limits<uint64_t>::max());
}
