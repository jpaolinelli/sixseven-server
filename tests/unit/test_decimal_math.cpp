#include "sixseven/common/decimal_math.h"

#include <gtest/gtest.h>

// Helper: build a Decimal128 from a small signed integer (fits in lo).
static sixseven::Decimal128 d128(int64_t v) {
    if (v >= 0) {
        return sixseven::Decimal128{0, static_cast<uint64_t>(v)};
    }
    return sixseven::Decimal128{-1, static_cast<uint64_t>(v)};
}

namespace sixseven {

// ---------------------------------------------------------------------------
// Primitive tests
// ---------------------------------------------------------------------------

TEST(DecimalMath, Dec128IsZero) {
    EXPECT_TRUE(dec128_is_zero(Decimal128{0, 0}));
    EXPECT_FALSE(dec128_is_zero(d128(1)));
    EXPECT_FALSE(dec128_is_zero(d128(-1)));
}

TEST(DecimalMath, Dec128Sign) {
    EXPECT_EQ(dec128_sign(Decimal128{0, 0}), 0);
    EXPECT_EQ(dec128_sign(d128(5)), 1);
    EXPECT_EQ(dec128_sign(d128(-5)), -1);
    // Large positive (hi != 0).
    EXPECT_EQ(dec128_sign(Decimal128{1, 0}), 1);
    // Negative with hi.
    EXPECT_EQ(dec128_sign(Decimal128{-1, 0xFFFFFFFFFFFFFFFFULL}), -1);
}

TEST(DecimalMath, Dec128AddBasic) {
    // 10 + 20 = 30
    auto r = dec128_add(d128(10), d128(20));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->hi, 0);
    EXPECT_EQ(r->lo, 30ULL);
}

TEST(DecimalMath, Dec128AddNegative) {
    // -10 + 20 = 10
    auto r = dec128_add(d128(-10), d128(20));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, d128(10));
}

TEST(DecimalMath, Dec128AddCarryIntoHi) {
    // UINT64_MAX + 1 should carry into hi.
    Decimal128 a{0, 0xFFFFFFFFFFFFFFFFULL};
    Decimal128 b{0, 1ULL};
    auto r = dec128_add(a, b);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->hi, 1);
    EXPECT_EQ(r->lo, 0ULL);
}

TEST(DecimalMath, Dec128AddOverflow) {
    // INT64_MAX * 2^64 + UINT64_MAX + 1 overflows.
    Decimal128 max_pos{INT64_MAX, 0xFFFFFFFFFFFFFFFFULL};
    auto r = dec128_add(max_pos, d128(1));
    EXPECT_FALSE(r.has_value());
}

TEST(DecimalMath, Dec128SubBasic) {
    auto r = dec128_sub(d128(30), d128(10));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, d128(20));
}

TEST(DecimalMath, Dec128SubNegativeResult) {
    auto r = dec128_sub(d128(10), d128(30));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, d128(-20));
}

TEST(DecimalMath, Dec128MulBasic) {
    // 3 * 7 = 21
    auto r = dec128_mul(d128(3), d128(7));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, d128(21));
}

TEST(DecimalMath, Dec128MulNegativePositive) {
    // -3 * 7 = -21
    auto r = dec128_mul(d128(-3), d128(7));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, d128(-21));
}

TEST(DecimalMath, Dec128MulZero) {
    auto r = dec128_mul(d128(12345678), d128(0));
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(dec128_is_zero(*r));
}

TEST(DecimalMath, Dec128MulPow10Basic) {
    // 1 * 10^2 = 100
    auto r = dec128_mul_pow10(d128(1), 2);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, d128(100));
}

TEST(DecimalMath, Dec128MulPow10Zero) {
    auto r = dec128_mul_pow10(d128(42), 0);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, d128(42));
}

TEST(DecimalMath, Dec128DivRoundHalfAwayFromZero) {
    // 10 / 3 = 3.333... -> rounds to 3 (half-away: remainder=1, 2*1 < 3).
    auto r = dec128_div_round(d128(10), d128(3));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, d128(3));

    // 7 / 2 = 3.5 -> rounds to 4 (half-away-from-zero).
    auto r2 = dec128_div_round(d128(7), d128(2));
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, d128(4));

    // -7 / 2 = -3.5 -> rounds to -4 (half-away-from-zero).
    auto r3 = dec128_div_round(d128(-7), d128(2));
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(*r3, d128(-4));
}

TEST(DecimalMath, Dec128DivByZero) {
    auto r = dec128_div_round(d128(10), d128(0));
    EXPECT_FALSE(r.has_value());
}

TEST(DecimalMath, Dec128ModBasic) {
    // 10 % 3 = 1
    auto r = dec128_mod(d128(10), d128(3));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, d128(1));

    // -10 % 3 = -1 (sign follows dividend)
    auto r2 = dec128_mod(d128(-10), d128(3));
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, d128(-1));
}

// ---------------------------------------------------------------------------
// decimal_add: 0.1 + 0.2 = 0.30 exactly at scale 2
// This is the canonical test that FAILS under the old double routing because
// 0.1 + 0.2 != 0.3 in IEEE 754. With the coefficient representation and
// scale-aligned integer add, coeff(0.1)=10 + coeff(0.2)=20 = 30 exactly.
// ---------------------------------------------------------------------------

TEST(DecimalMath, AddOnePointOneAndZeroPointTwo) {
    // 0.1 at scale 2 -> coefficient 10; 0.2 at scale 2 -> coefficient 20.
    // Expected: coefficient 30, scale 2 (i.e. 0.30).
    auto r = decimal_add(d128(10), 2, d128(20), 2);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->coeff, d128(30));
    EXPECT_EQ(r->scale, 2);
}

TEST(DecimalMath, AddDifferentScales) {
    // 1.5 (scale 1, coeff 15) + 0.25 (scale 2, coeff 25) = 1.75 (scale 2, coeff 175).
    auto r = decimal_add(d128(15), 1, d128(25), 2);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->coeff, d128(175));
    EXPECT_EQ(r->scale, 2);
}

TEST(DecimalMath, SubBasic) {
    // 1.00 (coeff 100, scale 2) - 0.30 (coeff 30, scale 2) = 0.70 (coeff 70).
    auto r = decimal_sub(d128(100), 2, d128(30), 2);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->coeff, d128(70));
    EXPECT_EQ(r->scale, 2);
}

// ---------------------------------------------------------------------------
// decimal_mul: result scale = s1 + s2
// Under old double routing 0.1 * 0.3 could be 0.030000000000000002 not 0.030.
// With coefficient multiply: coeff(0.1)=1 (scale1=1) * coeff(0.3)=3 (scale1=1)
// = coeff 3, scale 2 -> exactly 0.03.
// ---------------------------------------------------------------------------

TEST(DecimalMath, MulResultScale) {
    // 0.1 (coeff 1, scale 1) * 0.3 (coeff 3, scale 1) = 0.03 (coeff 3, scale 2).
    auto r = decimal_mul(d128(1), 1, d128(3), 1);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->coeff, d128(3));
    EXPECT_EQ(r->scale, 2); // s1+s2 = 1+1
}

TEST(DecimalMath, MulLargerCoefficients) {
    // 3.14 (coeff 314, scale 2) * 2.00 (coeff 200, scale 2) = 6.2800 (coeff 62800, scale 4).
    auto r = decimal_mul(d128(314), 2, d128(200), 2);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->coeff, d128(62800));
    EXPECT_EQ(r->scale, 4);
}

// ---------------------------------------------------------------------------
// decimal_div: result scale = max(s1+6, s1) = s1+6, round-half-away
// Under old double routing 1.0/3.0 would produce a float approximation.
// With exact arithmetic: coeff=1 (scale1=0) / coeff=3 (scale2=0) -> scale=6,
// numerator = 1 * 10^(6+0) = 1000000, quotient = 333333 with remainder 1,
// 2*1 < 3 -> no round up -> 0.333333 (coeff 333333 scale 6).
// ---------------------------------------------------------------------------

TEST(DecimalMath, DivResultScale) {
    // 1 (coeff 1, scale 0) / 3 (coeff 3, scale 0) -> scale 6.
    // num = 1 * 10^(6+0) = 1000000; 1000000/3 = 333333 rem 1; 2*1 < 3 -> no round.
    auto r = decimal_div(d128(1), 0, d128(3), 0);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->coeff, d128(333333));
    EXPECT_EQ(r->scale, 6);
}

TEST(DecimalMath, DivHalfAwayRounding) {
    // 1 (scale 0) / 2 (scale 0) -> scale 6.
    // num = 1 * 10^6 = 1000000; 1000000/2 = 500000 exactly -> 0.500000.
    auto r = decimal_div(d128(1), 0, d128(2), 0);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->coeff, d128(500000));
    EXPECT_EQ(r->scale, 6);
}

TEST(DecimalMath, DivByZeroError) {
    auto r = decimal_div(d128(10), 2, d128(0), 2);
    EXPECT_FALSE(r.has_value());
}

// ---------------------------------------------------------------------------
// Overflow -> TYPE_ERROR
// Under old double routing, overflow would silently produce +/-infinity or
// max double; here it must return a TYPE_ERROR.
// ---------------------------------------------------------------------------

TEST(DecimalMath, OverflowAddReturnsError) {
    // Two large positive values summing beyond INT128 range.
    Decimal128 big{INT64_MAX, 0xFFFFFFFFFFFFFFFFULL};
    auto r = decimal_add(big, 0, d128(1), 0);
    EXPECT_FALSE(r.has_value());
    if (!r.has_value()) {
        EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
    }
}

TEST(DecimalMath, OverflowMulReturnsError) {
    // Multiplying two large values that exceed 128 bits.
    Decimal128 large{0, 0xFFFFFFFFFFFFFFFFULL}; // 2^64 - 1
    // Use the primitive (dec128_mul) which operates on raw coefficients.
    auto r = dec128_mul(large, large);
    EXPECT_FALSE(r.has_value());
    if (!r.has_value()) {
        EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
    }
}

// ---------------------------------------------------------------------------
// decimal_compare: scale-aware
// ---------------------------------------------------------------------------

TEST(DecimalMath, CompareEqualDifferentScale) {
    // 1.0 (coeff 10, scale 1) == 1.00 (coeff 100, scale 2).
    auto r = decimal_compare(d128(10), 1, d128(100), 2);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 0);
}

TEST(DecimalMath, CompareLess) {
    // 0.1 < 0.2
    auto r = decimal_compare(d128(10), 2, d128(20), 2);
    ASSERT_TRUE(r.has_value());
    EXPECT_LT(*r, 0);
}

TEST(DecimalMath, CompareGreater) {
    // 0.3 > 0.2
    auto r = decimal_compare(d128(30), 2, d128(20), 2);
    ASSERT_TRUE(r.has_value());
    EXPECT_GT(*r, 0);
}

// ---------------------------------------------------------------------------
// decimal_to_double (utility)
// ---------------------------------------------------------------------------

TEST(DecimalMath, ToDouble) {
    // coeff 30, scale 2 -> 0.30.
    double v = decimal_to_double(d128(30), 2);
    EXPECT_NEAR(v, 0.30, 1e-10);
}

} // namespace sixseven
