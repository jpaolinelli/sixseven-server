// QA adversarial tests for GDB-873
// Bug: coerce(UINT64, DECIMAL) sign-flip for values above INT64_MAX.
// Fix: special-case UINT64 before to_int64() round-trip.
//
// Test goals:
//   1. Mutation check: BoundaryAtInt64Max would fail on pre-fix code.
//   2. Exhaustive boundary: 0, INT64_MAX-1, INT64_MAX, INT64_MAX+1 (==2^63), UINT64_MAX.
//   3. Round-trip / compare() path for coerced UINT64 DECIMAL values.
//   4. No regression on INT8/16/32/64, UINT8/16/32 and negative signed ints.
//   5. Duplicate test-name check is structural (done by inspection).

#include "sixseven/common/coercion.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Suite: QA_GDB873_Uint64ToDecimal
// Adversarial boundary values for UINT64 -> DECIMAL coercion.
// ---------------------------------------------------------------------------

// AC verification: small UINT64 value (same as dev test, kept as baseline).
TEST(QA_GDB873_Uint64ToDecimal, ZeroIsPositive) {
    auto result = coerce(Value(static_cast<uint64_t>(0)), TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto d = result->as_decimal();
    // Zero must be hi==0, lo==0, NOT hi==-1.
    EXPECT_EQ(d.hi, 0);
    EXPECT_EQ(d.lo, static_cast<uint64_t>(0));
}

TEST(QA_GDB873_Uint64ToDecimal, OneIsPositive) {
    auto result = coerce(Value(static_cast<uint64_t>(1)), TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto d = result->as_decimal();
    EXPECT_EQ(d.hi, 0);
    EXPECT_EQ(d.lo, static_cast<uint64_t>(1));
}

TEST(QA_GDB873_Uint64ToDecimal, Int64MaxMinusOneIsPositive) {
    // INT64_MAX - 1 = 2^63 - 2; still fits in signed path, must be hi==0.
    constexpr uint64_t v = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - 1;
    auto result = coerce(Value(v), TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto d = result->as_decimal();
    EXPECT_EQ(d.hi, 0);
    EXPECT_EQ(d.lo, v);
}

TEST(QA_GDB873_Uint64ToDecimal, ExactlyInt64MaxIsPositive) {
    // INT64_MAX = 2^63 - 1 as uint64; must be hi==0.
    constexpr uint64_t v = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    auto result = coerce(Value(v), TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto d = result->as_decimal();
    EXPECT_EQ(d.hi, 0);
    EXPECT_EQ(d.lo, v);
}

// MUTATION CHECK: This is the canary. On pre-fix code to_int64() wraps 2^63
// to -2^63 (negative), yielding hi==-1. The fix returns hi==0.
TEST(QA_GDB873_Uint64ToDecimal, FirstUnsignedOnlyValue_BoundaryAtInt64MaxPlus1) {
    // 2^63 is INT64_MAX + 1 and the first UINT64 value that breaks to_int64().
    constexpr uint64_t two63 = static_cast<uint64_t>(1) << 63;
    auto result = coerce(Value(two63), TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto d = result->as_decimal();
    // Must be POSITIVE (hi==0). Pre-fix: hi==-1 (sign-flip bug).
    EXPECT_EQ(d.hi, 0) << "MUTATION FAILED: hi==-1 means the to_int64 sign-flip bug is present";
    EXPECT_EQ(d.lo, two63);
}

TEST(QA_GDB873_Uint64ToDecimal, UpperMidValue) {
    // 3 * 2^62 — well above INT64_MAX, must be hi==0.
    constexpr uint64_t v = (static_cast<uint64_t>(3) << 62);
    auto result = coerce(Value(v), TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto d = result->as_decimal();
    EXPECT_EQ(d.hi, 0);
    EXPECT_EQ(d.lo, v);
}

TEST(QA_GDB873_Uint64ToDecimal, MaxUint64IsPositive) {
    // UINT64_MAX = 2^64 - 1; must be hi==0, lo==UINT64_MAX.
    constexpr uint64_t max_u64 = std::numeric_limits<uint64_t>::max();
    auto result = coerce(Value(max_u64), TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto d = result->as_decimal();
    EXPECT_EQ(d.hi, 0);
    EXPECT_EQ(d.lo, max_u64);
}

// ---------------------------------------------------------------------------
// Suite: QA_GDB873_NullHandling
// NULL passthrough must still work after the UINT64 special-case.
// ---------------------------------------------------------------------------

TEST(QA_GDB873_NullHandling, NullUint64CoerceToDecimal) {
    Value null_val = Value::make_null();
    auto result = coerce(null_val, TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_null());
}

// ---------------------------------------------------------------------------
// Suite: QA_GDB873_SignedPathRegression
// Ensure the pre-existing INT8/16/32/64, UINT8/16/32 signed paths are untouched.
// ---------------------------------------------------------------------------

TEST(QA_GDB873_SignedPathRegression, NegativeInt8StillHiMinusOne) {
    auto result = coerce(Value(static_cast<int8_t>(-1)), TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto d = result->as_decimal();
    EXPECT_EQ(d.hi, -1);
    EXPECT_EQ(d.lo, static_cast<uint64_t>(-1));
}

TEST(QA_GDB873_SignedPathRegression, NegativeInt16StillHiMinusOne) {
    auto result = coerce(Value(static_cast<int16_t>(-100)), TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto d = result->as_decimal();
    EXPECT_EQ(d.hi, -1);
    EXPECT_EQ(d.lo, static_cast<uint64_t>(-100LL));
}

TEST(QA_GDB873_SignedPathRegression, NegativeInt32StillHiMinusOne) {
    auto result = coerce(Value(static_cast<int32_t>(-999999)), TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto d = result->as_decimal();
    EXPECT_EQ(d.hi, -1);
    EXPECT_EQ(d.lo, static_cast<uint64_t>(-999999LL));
}

TEST(QA_GDB873_SignedPathRegression, NegativeInt64StillHiMinusOne) {
    auto result = coerce(Value(static_cast<int64_t>(-1)), TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto d = result->as_decimal();
    EXPECT_EQ(d.hi, -1);
    EXPECT_EQ(d.lo, static_cast<uint64_t>(-1LL));
}

TEST(QA_GDB873_SignedPathRegression, Int64MinStillHiMinusOne) {
    // INT64_MIN is the most-negative signed 64-bit value.
    constexpr int64_t min_i64 = std::numeric_limits<int64_t>::min();
    auto result = coerce(Value(min_i64), TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto d = result->as_decimal();
    EXPECT_EQ(d.hi, -1);
    EXPECT_EQ(d.lo, static_cast<uint64_t>(min_i64));
}

TEST(QA_GDB873_SignedPathRegression, PositiveInt8StillHiZero) {
    auto result = coerce(Value(static_cast<int8_t>(127)), TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto d = result->as_decimal();
    EXPECT_EQ(d.hi, 0);
    EXPECT_EQ(d.lo, static_cast<uint64_t>(127));
}

TEST(QA_GDB873_SignedPathRegression, Uint8MaxStillHiZero) {
    auto result = coerce(Value(static_cast<uint8_t>(255)), TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto d = result->as_decimal();
    EXPECT_EQ(d.hi, 0);
    EXPECT_EQ(d.lo, static_cast<uint64_t>(255));
}

TEST(QA_GDB873_SignedPathRegression, Uint16MaxStillHiZero) {
    auto result = coerce(Value(static_cast<uint16_t>(65535)), TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto d = result->as_decimal();
    EXPECT_EQ(d.hi, 0);
    EXPECT_EQ(d.lo, static_cast<uint64_t>(65535));
}

TEST(QA_GDB873_SignedPathRegression, Uint32MaxStillHiZero) {
    constexpr uint32_t max_u32 = std::numeric_limits<uint32_t>::max();
    auto result = coerce(Value(max_u32), TypeId::DECIMAL);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto d = result->as_decimal();
    EXPECT_EQ(d.hi, 0);
    EXPECT_EQ(d.lo, static_cast<uint64_t>(max_u32));
}

// ---------------------------------------------------------------------------
// Suite: QA_GDB873_CompareRoundTrip
// Coerced UINT64 DECIMAL values must compare correctly via compare().
// Note: compare() routes DECIMAL cross-type through to_double(), so we verify
// same-type DECIMAL comparison using compare_same_type path (compare two DECIMALs).
// ---------------------------------------------------------------------------

TEST(QA_GDB873_CompareRoundTrip, TwoUint64DecimalsOrderCorrectly) {
    // Coerce two UINT64 values and compare the resulting DECIMALs.
    constexpr uint64_t two63 = static_cast<uint64_t>(1) << 63;
    constexpr uint64_t two63_plus1 = two63 + 1;

    auto a = coerce(Value(two63), TypeId::DECIMAL);
    auto b = coerce(Value(two63_plus1), TypeId::DECIMAL);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());

    // a < b
    auto cmp = compare(*a, *b);
    ASSERT_TRUE(cmp.has_value()) << cmp.error().message;
    EXPECT_EQ(*cmp, std::strong_ordering::less);

    // b > a
    cmp = compare(*b, *a);
    ASSERT_TRUE(cmp.has_value());
    EXPECT_EQ(*cmp, std::strong_ordering::greater);

    // a == a
    cmp = compare(*a, *a);
    ASSERT_TRUE(cmp.has_value());
    EXPECT_EQ(*cmp, std::strong_ordering::equal);
}

TEST(QA_GDB873_CompareRoundTrip, ZeroDecimalLessThanTwo63Decimal) {
    auto zero_dec = coerce(Value(static_cast<uint64_t>(0)), TypeId::DECIMAL);
    auto two63_dec = coerce(Value(static_cast<uint64_t>(1) << 63), TypeId::DECIMAL);
    ASSERT_TRUE(zero_dec.has_value());
    ASSERT_TRUE(two63_dec.has_value());

    // 0 < 2^63 in DECIMAL space.
    auto cmp = compare(*zero_dec, *two63_dec);
    ASSERT_TRUE(cmp.has_value()) << cmp.error().message;
    EXPECT_EQ(*cmp, std::strong_ordering::less)
        << "Pre-fix bug: 2^63 would be negative so 0 > 2^63 (wrong ordering)";
}

TEST(QA_GDB873_CompareRoundTrip, NegativeDecimalLessThanUint64Decimal) {
    // A genuine negative INT64 decimal must compare less than any UINT64 decimal.
    auto neg_dec = coerce(Value(static_cast<int64_t>(-1)), TypeId::DECIMAL);
    auto pos_dec = coerce(Value(static_cast<uint64_t>(1) << 63), TypeId::DECIMAL);
    ASSERT_TRUE(neg_dec.has_value());
    ASSERT_TRUE(pos_dec.has_value());

    auto cmp = compare(*neg_dec, *pos_dec);
    ASSERT_TRUE(cmp.has_value()) << cmp.error().message;
    EXPECT_EQ(*cmp, std::strong_ordering::less);
}

// ---------------------------------------------------------------------------
// Suite: QA_GDB873_DoubleConversionNote
// The compare() cross-type DECIMAL path goes through to_double(), which loses
// precision for UINT64 values > 2^53. This is a known limitation, not a new bug.
// These tests document the behavior so regressions are caught.
// ---------------------------------------------------------------------------

TEST(QA_GDB873_DoubleConversionNote, LargeUint64CrossTypeCompareViaDouble) {
    // compare(UINT64, DECIMAL) routes through to_double for both sides.
    // For UINT64_MAX vs DECIMAL(lo=UINT64_MAX, hi=0) this must be equal
    // (both convert to the same double, even though the double is imprecise).
    constexpr uint64_t max_u64 = std::numeric_limits<uint64_t>::max();
    auto dec = coerce(Value(max_u64), TypeId::DECIMAL);
    ASSERT_TRUE(dec.has_value());

    // Cross-type: UINT64 vs DECIMAL — both become to_double() in compare().
    auto cmp = compare(Value(max_u64), *dec);
    ASSERT_TRUE(cmp.has_value()) << cmp.error().message;
    // Both sides to_double() yield the same imprecise double, so equal.
    EXPECT_EQ(*cmp, std::strong_ordering::equal);
}
