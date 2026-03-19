#include "sixseven/common/coercion.h"
#include "sixseven/common/uuid.h"

#include <gtest/gtest.h>

#include <compare>

using namespace sixseven;

// -- can_coerce ---------------------------------------------------------------

TEST(Coercion, IdentityCoercion) {
    EXPECT_TRUE(can_coerce(TypeId::INT32, TypeId::INT32));
    EXPECT_TRUE(can_coerce(TypeId::STRING, TypeId::STRING));
    EXPECT_TRUE(can_coerce(TypeId::FLOAT64, TypeId::FLOAT64));
}

TEST(Coercion, IntegerWidening) {
    EXPECT_TRUE(can_coerce(TypeId::INT8, TypeId::INT16));
    EXPECT_TRUE(can_coerce(TypeId::INT8, TypeId::INT32));
    EXPECT_TRUE(can_coerce(TypeId::INT8, TypeId::INT64));
    EXPECT_TRUE(can_coerce(TypeId::INT16, TypeId::INT32));
    EXPECT_TRUE(can_coerce(TypeId::INT32, TypeId::INT64));

    // Cannot narrow
    EXPECT_FALSE(can_coerce(TypeId::INT64, TypeId::INT32));
    EXPECT_FALSE(can_coerce(TypeId::INT32, TypeId::INT16));
}

TEST(Coercion, UnsignedWidening) {
    EXPECT_TRUE(can_coerce(TypeId::UINT8, TypeId::UINT16));
    EXPECT_TRUE(can_coerce(TypeId::UINT16, TypeId::UINT32));
    EXPECT_TRUE(can_coerce(TypeId::UINT32, TypeId::UINT64));
}

TEST(Coercion, UnsignedToSignedPromotion) {
    EXPECT_TRUE(can_coerce(TypeId::UINT8, TypeId::INT16));
    EXPECT_TRUE(can_coerce(TypeId::UINT16, TypeId::INT32));
    EXPECT_TRUE(can_coerce(TypeId::UINT32, TypeId::INT64));
}

TEST(Coercion, IntegerToFloat) {
    EXPECT_TRUE(can_coerce(TypeId::INT8, TypeId::FLOAT32));
    EXPECT_TRUE(can_coerce(TypeId::INT32, TypeId::FLOAT64));
    EXPECT_TRUE(can_coerce(TypeId::UINT64, TypeId::FLOAT64));
}

TEST(Coercion, NumericToDecimal) {
    EXPECT_TRUE(can_coerce(TypeId::INT32, TypeId::DECIMAL));
    EXPECT_TRUE(can_coerce(TypeId::FLOAT64, TypeId::DECIMAL));
    EXPECT_TRUE(can_coerce(TypeId::UINT64, TypeId::DECIMAL));
}

TEST(Coercion, FloatToInteger) {
    // Narrowing: float -> integer not allowed implicitly
    EXPECT_FALSE(can_coerce(TypeId::FLOAT64, TypeId::INT32));
    EXPECT_FALSE(can_coerce(TypeId::FLOAT32, TypeId::INT64));
}

TEST(Coercion, IncompatibleTypes) {
    EXPECT_FALSE(can_coerce(TypeId::STRING, TypeId::INT32));
    EXPECT_FALSE(can_coerce(TypeId::BOOL, TypeId::INT32));
    EXPECT_FALSE(can_coerce(TypeId::DATE, TypeId::INT32));
    EXPECT_FALSE(can_coerce(TypeId::BLOB, TypeId::STRING));
}

TEST(Coercion, StringToUuid) {
    EXPECT_TRUE(can_coerce(TypeId::STRING, TypeId::UUID));
    // UUID→STRING is not supported.
    EXPECT_FALSE(can_coerce(TypeId::UUID, TypeId::STRING));
}

// -- coerce -------------------------------------------------------------------

TEST(Coercion, CoerceInt8ToInt32) {
    Value v(int8_t{42});
    auto result = coerce(v, TypeId::INT32);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type_id(), TypeId::INT32);
    EXPECT_EQ(result->as_int32(), 42);
}

TEST(Coercion, CoerceInt32ToFloat64) {
    Value v(int32_t{1000});
    auto result = coerce(v, TypeId::FLOAT64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type_id(), TypeId::FLOAT64);
    EXPECT_DOUBLE_EQ(result->as_float64(), 1000.0);
}

TEST(Coercion, CoerceInt32ToInt64) {
    Value v(int32_t{-500});
    auto result = coerce(v, TypeId::INT64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->as_int64(), -500);
}

TEST(Coercion, CoerceUint8ToInt16) {
    Value v(uint8_t{200});
    auto result = coerce(v, TypeId::INT16);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->as_int16(), 200);
}

TEST(Coercion, CoerceNullPreservesNull) {
    Value v;
    auto result = coerce(v, TypeId::INT64);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_null());
}

TEST(Coercion, CoerceIdentityReturnsCopy) {
    Value v(int32_t{42});
    auto result = coerce(v, TypeId::INT32);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->as_int32(), 42);
}

TEST(Coercion, CoerceIncompatibleReturnsError) {
    Value v(std::string{"hello"});
    auto result = coerce(v, TypeId::INT32);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(Coercion, CoerceIntToDecimal) {
    Value v(int32_t{42});
    auto result = coerce(v, TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type_id(), TypeId::DECIMAL);
    EXPECT_EQ(result->as_decimal().lo, 42u);
}

TEST(Coercion, CoerceFloat32ToFloat64) {
    Value v(3.14f);
    auto result = coerce(v, TypeId::FLOAT64);
    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(result->as_float64(), 3.14, 0.001);
}

TEST(Coercion, CoerceStringToUuid) {
    Value v(std::string{"d1458b55-f0bf-44d4-b191-e52f1ef1f60a"});
    auto result = coerce(v, TypeId::UUID);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->type_id(), TypeId::UUID);

    Uuid expected = {0xd1,
                     0x45,
                     0x8b,
                     0x55,
                     0xf0,
                     0xbf,
                     0x44,
                     0xd4,
                     0xb1,
                     0x91,
                     0xe5,
                     0x2f,
                     0x1e,
                     0xf1,
                     0xf6,
                     0x0a};
    EXPECT_EQ(result->as_uuid(), expected);
}

TEST(Coercion, CoerceStringToUuidInvalid) {
    Value v(std::string{"not-a-uuid"});
    auto result = coerce(v, TypeId::UUID);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(Coercion, CoerceNullToUuid) {
    Value v;
    auto result = coerce(v, TypeId::UUID);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_null());
}

TEST(Coercion, FitStringToUuid) {
    Value v(std::string{"550e8400-e29b-41d4-a716-446655440000"});
    auto result = fit_to_storage(v, TypeId::UUID);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->type_id(), TypeId::UUID);

    Uuid expected = {0x55,
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
    EXPECT_EQ(result->as_uuid(), expected);
}

// -- compare ------------------------------------------------------------------

TEST(Compare, SameTypeIntegers) {
    auto r = compare(Value(int32_t{10}), Value(int32_t{20}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);

    r = compare(Value(int32_t{20}), Value(int32_t{10}));
    EXPECT_EQ(*r, std::strong_ordering::greater);

    r = compare(Value(int32_t{10}), Value(int32_t{10}));
    EXPECT_EQ(*r, std::strong_ordering::equal);
}

TEST(Compare, SameTypeStrings) {
    auto r = compare(Value(std::string{"abc"}), Value(std::string{"def"}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);

    r = compare(Value(std::string{"def"}), Value(std::string{"abc"}));
    EXPECT_EQ(*r, std::strong_ordering::greater);

    r = compare(Value(std::string{"same"}), Value(std::string{"same"}));
    EXPECT_EQ(*r, std::strong_ordering::equal);
}

TEST(Compare, SameTypeFloats) {
    auto r = compare(Value(1.5), Value(2.5));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);
}

TEST(Compare, SameTypeBool) {
    auto r = compare(Value(false), Value(true));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);
}

TEST(Compare, SameTypeDate) {
    auto r = compare(Value(Date{100}), Value(Date{200}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);
}

TEST(Compare, SameTypeTimestamp) {
    auto r = compare(Value(Timestamp{1000}), Value(Timestamp{2000}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);
}

TEST(Compare, SameTypeUuid) {
    Uuid a = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    Uuid b = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
    auto r = compare(Value(a), Value(b));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);
}

TEST(Compare, CrossTypeIntegerPromotion) {
    // INT32 vs INT64: promote INT32 to INT64
    auto r = compare(Value(int32_t{100}), Value(int64_t{200}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);

    r = compare(Value(int32_t{200}), Value(int64_t{100}));
    EXPECT_EQ(*r, std::strong_ordering::greater);

    r = compare(Value(int32_t{100}), Value(int64_t{100}));
    EXPECT_EQ(*r, std::strong_ordering::equal);
}

TEST(Compare, CrossTypeIntFloat) {
    auto r = compare(Value(int32_t{10}), Value(10.5));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);
}

TEST(Compare, NullComparisons) {
    // NULL == NULL
    auto r = compare(Value::make_null(), Value::make_null());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::equal);

    // NULL < non-NULL
    r = compare(Value::make_null(), Value(int32_t{42}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);

    // non-NULL > NULL
    r = compare(Value(int32_t{42}), Value::make_null());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::greater);
}

TEST(Compare, IncomparableTypesReturnError) {
    // BLOB is not comparable
    auto r = compare(Value(Blob{1, 2}), Value(Blob{3, 4}));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);

    // EMBEDDING is not comparable
    r = compare(Value(Embedding{0.1f}), Value(Embedding{0.2f}));
    ASSERT_FALSE(r.has_value());
}

TEST(Compare, IncompatibleTypesReturnError) {
    // STRING vs INT32: no coercion path
    auto r = compare(Value(std::string{"hello"}), Value(int32_t{42}));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(Compare, CrossTypeSignedUnsigned) {
    // Must fix: compare(int64_t{-1}, uint64_t{1}) should return less, not greater.
    auto r = compare(Value(int64_t{-1}), Value(uint64_t{1}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);

    // Negative signed vs any unsigned is always less.
    r = compare(Value(int32_t{-100}), Value(uint32_t{0}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);

    // Unsigned vs negative signed is always greater.
    r = compare(Value(uint64_t{1}), Value(int64_t{-1}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::greater);

    // Both non-negative: normal comparison.
    r = compare(Value(int64_t{100}), Value(uint64_t{200}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);

    r = compare(Value(int32_t{100}), Value(uint32_t{100}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::equal);

    // Large uint64_t vs small int64_t — correct unsigned comparison.
    r = compare(Value(uint64_t{18446744073709551615ULL}), Value(int64_t{0}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::greater);

    // INT8 negative vs UINT8.
    r = compare(Value(int8_t{-1}), Value(uint8_t{0}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::less);
}

TEST(Compare, FloatNaN) {
    auto nan = std::numeric_limits<double>::quiet_NaN();
    auto r = compare(Value(nan), Value(1.0));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, std::strong_ordering::greater);

    r = compare(Value(1.0), Value(nan));
    EXPECT_EQ(*r, std::strong_ordering::less);

    r = compare(Value(nan), Value(nan));
    EXPECT_EQ(*r, std::strong_ordering::equal);
}

// -- explicit_cast ------------------------------------------------------------

TEST(ExplicitCast, Float64ToInt32Truncates) {
    auto r = explicit_cast(Value(9.99), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->type_id(), TypeId::INT32);
    EXPECT_EQ(r->as_int32(), 9);
}

TEST(ExplicitCast, Float64NegativeToInt32Truncates) {
    auto r = explicit_cast(Value(-3.7), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), -3);
}

TEST(ExplicitCast, Float64ZeroToInt32) {
    auto r = explicit_cast(Value(0.0), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), 0);
}

TEST(ExplicitCast, Float64ToInt64) {
    auto r = explicit_cast(Value(1000000.9), TypeId::INT64);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int64(), 1000000);
}

TEST(ExplicitCast, Float32ToInt32) {
    auto r = explicit_cast(Value(42.7f), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), 42);
}

TEST(ExplicitCast, Float64ToInt16) {
    auto r = explicit_cast(Value(127.9), TypeId::INT16);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int16(), 127);
}

TEST(ExplicitCast, Float64NaNReturnsError) {
    auto nan = std::numeric_limits<double>::quiet_NaN();
    auto r = explicit_cast(Value(nan), TypeId::INT32);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(ExplicitCast, Float64InfinityReturnsError) {
    auto inf = std::numeric_limits<double>::infinity();
    auto r = explicit_cast(Value(inf), TypeId::INT32);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(ExplicitCast, NullPreservesNull) {
    auto r = explicit_cast(Value::make_null(), TypeId::INT32);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->is_null());
}

TEST(ExplicitCast, IdentityReturnsCopy) {
    auto r = explicit_cast(Value(42.0), TypeId::FLOAT64);
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(r->as_float64(), 42.0);
}

TEST(ExplicitCast, WideningStillWorks) {
    auto r = explicit_cast(Value(int32_t{42}), TypeId::INT64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->as_int64(), 42);
}

TEST(ExplicitCast, IntegerNarrowing) {
    auto r = explicit_cast(Value(int64_t{100}), TypeId::INT32);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->as_int32(), 100);
}

TEST(ExplicitCast, IncompatibleReturnsError) {
    auto r = explicit_cast(Value(std::string{"hello"}), TypeId::INT32);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}
