/// @file tests/qa/test_qa_gdb_604.cpp
/// QA adversarial tests for GDB-604: Type casting FLOAT64 to INT32 fails.
///
/// The fix adds explicit_cast() which supports float→integer truncation and
/// numeric narrowing. These tests exercise boundary values, overflow, special
/// floats, unsigned targets, and cross-type combinations.

#include "sixseven/common/coercion.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>

namespace sixseven {
namespace {

// =============================================================================
// AC-1: FLOAT64 → INT32 truncates toward zero (basic ticket scenario)
// =============================================================================

TEST(QA_GDB604, Float64ToInt32_PositiveFraction) {
    auto r = explicit_cast(Value(9.99), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->type_id(), TypeId::INT32);
    EXPECT_EQ(r->as_int32(), 9);
}

TEST(QA_GDB604, Float64ToInt32_NegativeFraction) {
    auto r = explicit_cast(Value(-9.99), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), -9);
}

TEST(QA_GDB604, Float64ToInt32_ExactInteger) {
    auto r = explicit_cast(Value(42.0), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), 42);
}

TEST(QA_GDB604, Float64ToInt32_Zero) {
    auto r = explicit_cast(Value(0.0), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), 0);
}

TEST(QA_GDB604, Float64ToInt32_NegativeZero) {
    auto r = explicit_cast(Value(-0.0), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), 0);
}

// =============================================================================
// Boundary: INT32 range limits
// =============================================================================

TEST(QA_GDB604, Float64ToInt32_MaxInt32) {
    // INT32_MAX = 2147483647. As a double this is exact.
    auto r = explicit_cast(Value(2147483647.0), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), 2147483647);
}

TEST(QA_GDB604, Float64ToInt32_MinInt32) {
    // INT32_MIN = -2147483648
    auto r = explicit_cast(Value(-2147483648.0), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), -2147483648);
}

// Overflow: values outside INT32 range. PostgreSQL returns "integer out of range".
// Current implementation silently wraps — this test documents the behavior.
TEST(QA_GDB604, Float64ToInt32_OverflowAboveMax) {
    // 3e9 > INT32_MAX (2.1e9). Should ideally return an error.
    auto r = explicit_cast(Value(3e9), TypeId::INT32);
    // PostgreSQL: ERROR: integer out of range
    // We expect an error for overflow, but if the implementation silently wraps,
    // this test will capture that behavior.
    if (r.has_value()) {
        // Silent wrap — this is a potential bug. The value won't be 3000000000.
        ADD_FAILURE() << "Expected error for overflow, got value: " << r->as_int32()
                      << " (silent truncation of 3e9 to INT32)";
    } else {
        EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
    }
}

TEST(QA_GDB604, Float64ToInt32_OverflowBelowMin) {
    // -3e9 < INT32_MIN (-2.1e9). Should ideally return an error.
    auto r = explicit_cast(Value(-3e9), TypeId::INT32);
    if (r.has_value()) {
        ADD_FAILURE() << "Expected error for underflow, got value: " << r->as_int32()
                      << " (silent truncation of -3e9 to INT32)";
    } else {
        EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
    }
}

// =============================================================================
// Special floats: NaN, +Inf, -Inf
// =============================================================================

TEST(QA_GDB604, Float64NaN_ToInt32_ReturnsError) {
    auto r = explicit_cast(Value(std::numeric_limits<double>::quiet_NaN()), TypeId::INT32);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB604, Float64PosInf_ToInt32_ReturnsError) {
    auto r = explicit_cast(Value(std::numeric_limits<double>::infinity()), TypeId::INT32);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB604, Float64NegInf_ToInt32_ReturnsError) {
    auto r = explicit_cast(Value(-std::numeric_limits<double>::infinity()), TypeId::INT32);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB604, Float64NaN_ErrorMessageContainsNaN) {
    auto r = explicit_cast(Value(std::numeric_limits<double>::quiet_NaN()), TypeId::INT64);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("NaN"), std::string::npos)
        << "Error message should mention NaN: " << r.error().message;
}

TEST(QA_GDB604, Float64PosInf_ErrorMessageContainsInfinity) {
    auto r = explicit_cast(Value(std::numeric_limits<double>::infinity()), TypeId::INT64);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("infinity"), std::string::npos)
        << "Error message should mention infinity: " << r.error().message;
}

TEST(QA_GDB604, Float64NegInf_ErrorMessageContainsNegInfinity) {
    auto r = explicit_cast(Value(-std::numeric_limits<double>::infinity()), TypeId::INT64);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("-infinity"), std::string::npos)
        << "Error message should mention -infinity: " << r.error().message;
}

// =============================================================================
// Float → other integer types
// =============================================================================

TEST(QA_GDB604, Float64ToInt8) {
    auto r = explicit_cast(Value(127.9), TypeId::INT8);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int8(), 127);
}

TEST(QA_GDB604, Float64ToInt8_Overflow) {
    // 200 > INT8_MAX (127)
    auto r = explicit_cast(Value(200.0), TypeId::INT8);
    if (r.has_value()) {
        // Silent truncation — bug
        ADD_FAILURE() << "Expected error for INT8 overflow, got: "
                      << static_cast<int>(r->as_int8());
    } else {
        EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
    }
}

TEST(QA_GDB604, Float64ToInt16) {
    auto r = explicit_cast(Value(32767.4), TypeId::INT16);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int16(), 32767);
}

TEST(QA_GDB604, Float64ToInt16_Overflow) {
    // 40000 > INT16_MAX (32767)
    auto r = explicit_cast(Value(40000.0), TypeId::INT16);
    if (r.has_value()) {
        ADD_FAILURE() << "Expected error for INT16 overflow, got: " << r->as_int16();
    } else {
        EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
    }
}

TEST(QA_GDB604, Float64ToInt64) {
    auto r = explicit_cast(Value(1e15), TypeId::INT64);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int64(), 1000000000000000LL);
}

TEST(QA_GDB604, Float64ToInt64_VeryLarge) {
    // 1e20 > INT64_MAX (~9.2e18). static_cast<int64_t>(1e20) is UB.
    // Should return an error.
    auto r = explicit_cast(Value(1e20), TypeId::INT64);
    if (r.has_value()) {
        ADD_FAILURE() << "Expected error for INT64 overflow from 1e20, got: " << r->as_int64();
    } else {
        EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
    }
}

// =============================================================================
// Float → unsigned integer types
// =============================================================================

TEST(QA_GDB604, Float64ToUint32_Positive) {
    auto r = explicit_cast(Value(42.7), TypeId::UINT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_uint32(), 42u);
}

TEST(QA_GDB604, Float64ToUint32_Negative) {
    // Negative float → unsigned should be an error (PostgreSQL errors).
    auto r = explicit_cast(Value(-1.5), TypeId::UINT32);
    if (r.has_value()) {
        ADD_FAILURE() << "Expected error for negative float→UINT32, got: " << r->as_uint32();
    } else {
        EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
    }
}

TEST(QA_GDB604, Float64ToUint8_Overflow) {
    // 300 > UINT8_MAX (255)
    auto r = explicit_cast(Value(300.0), TypeId::UINT8);
    if (r.has_value()) {
        ADD_FAILURE() << "Expected error for UINT8 overflow, got: "
                      << static_cast<int>(r->as_uint8());
    } else {
        EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
    }
}

// =============================================================================
// Float32 → integer types
// =============================================================================

TEST(QA_GDB604, Float32ToInt32) {
    auto r = explicit_cast(Value(42.7f), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), 42);
}

TEST(QA_GDB604, Float32ToInt64) {
    auto r = explicit_cast(Value(1000.9f), TypeId::INT64);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int64(), 1000);
}

// =============================================================================
// Integer → integer narrowing
// =============================================================================

TEST(QA_GDB604, Int64ToInt32_InRange) {
    auto r = explicit_cast(Value(int64_t{100}), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), 100);
}

TEST(QA_GDB604, Int64ToInt32_Overflow) {
    // INT64 value exceeding INT32_MAX
    auto r = explicit_cast(Value(int64_t{3000000000LL}), TypeId::INT32);
    if (r.has_value()) {
        ADD_FAILURE() << "Expected error for INT64→INT32 overflow, got: " << r->as_int32();
    } else {
        EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
    }
}

TEST(QA_GDB604, Int64ToInt8_Overflow) {
    auto r = explicit_cast(Value(int64_t{256}), TypeId::INT8);
    if (r.has_value()) {
        ADD_FAILURE() << "Expected error for INT64→INT8 overflow, got: "
                      << static_cast<int>(r->as_int8());
    } else {
        EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
    }
}

// =============================================================================
// Float → float narrowing
// =============================================================================

TEST(QA_GDB604, Float64ToFloat32) {
    auto r = explicit_cast(Value(3.14), TypeId::FLOAT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->type_id(), TypeId::FLOAT32);
    EXPECT_NEAR(r->as_float32(), 3.14f, 1e-6f);
}

TEST(QA_GDB604, Float64ToFloat32_VeryLarge) {
    // DBL_MAX → float is infinity
    auto r = explicit_cast(Value(std::numeric_limits<double>::max()), TypeId::FLOAT32);
    if (r.has_value()) {
        // Result should be infinity or an error
        EXPECT_TRUE(std::isinf(r->as_float32()))
            << "Expected infinity for DBL_MAX→FLOAT32, got: " << r->as_float32();
    }
}

// =============================================================================
// Null preservation
// =============================================================================

TEST(QA_GDB604, NullToInt32) {
    auto r = explicit_cast(Value::make_null(), TypeId::INT32);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->is_null());
}

TEST(QA_GDB604, NullToFloat64) {
    auto r = explicit_cast(Value::make_null(), TypeId::FLOAT64);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->is_null());
}

// =============================================================================
// Identity cast
// =============================================================================

TEST(QA_GDB604, Float64ToFloat64_Identity) {
    auto r = explicit_cast(Value(3.14), TypeId::FLOAT64);
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(r->as_float64(), 3.14);
}

TEST(QA_GDB604, Int32ToInt32_Identity) {
    auto r = explicit_cast(Value(int32_t{42}), TypeId::INT32);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->as_int32(), 42);
}

// =============================================================================
// Widening still works through explicit_cast
// =============================================================================

TEST(QA_GDB604, Int32ToInt64_Widening) {
    auto r = explicit_cast(Value(int32_t{42}), TypeId::INT64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->as_int64(), 42);
}

TEST(QA_GDB604, Int32ToFloat64_Widening) {
    auto r = explicit_cast(Value(int32_t{42}), TypeId::FLOAT64);
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(r->as_float64(), 42.0);
}

// =============================================================================
// Incompatible types — should always error
// =============================================================================

TEST(QA_GDB604, StringToInt32_Error) {
    auto r = explicit_cast(Value(std::string{"hello"}), TypeId::INT32);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB604, StringToFloat64_Error) {
    auto r = explicit_cast(Value(std::string{"3.14"}), TypeId::FLOAT64);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

// =============================================================================
// Tiny fractions near zero
// =============================================================================

TEST(QA_GDB604, Float64TinyPositive_ToInt32) {
    auto r = explicit_cast(Value(0.0001), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), 0);
}

TEST(QA_GDB604, Float64TinyNegative_ToInt32) {
    auto r = explicit_cast(Value(-0.0001), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), 0);
}

// =============================================================================
// Subnormal double
// =============================================================================

TEST(QA_GDB604, Float64Subnormal_ToInt32) {
    auto r = explicit_cast(Value(std::numeric_limits<double>::denorm_min()), TypeId::INT32);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int32(), 0);
}

// =============================================================================
// Large float that fits in INT64 but not INT32
// =============================================================================

TEST(QA_GDB604, Float64FitsInt64ButNotInt32) {
    // 5e9 fits in INT64, but not INT32
    auto r64 = explicit_cast(Value(5e9), TypeId::INT64);
    ASSERT_TRUE(r64.has_value()) << r64.error().message;
    EXPECT_EQ(r64->as_int64(), 5000000000LL);

    auto r32 = explicit_cast(Value(5e9), TypeId::INT32);
    if (r32.has_value()) {
        ADD_FAILURE() << "5e9 does not fit in INT32, expected error, got: " << r32->as_int32();
    }
}

} // namespace
} // namespace sixseven
