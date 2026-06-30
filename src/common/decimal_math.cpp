#include "sixseven/common/decimal_math.h"

#include "sixseven/common/status.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace sixseven {

// ---------------------------------------------------------------------------
// Internal 128-bit signed integer representation
//
// We treat Decimal128{int64_t hi, uint64_t lo} as a two's-complement signed
// 128-bit integer where:
//   value = (hi * 2^64) + lo    (lo is unsigned, contributes positively)
//
// The minimum value is {INT64_MIN, 0} and the maximum is {INT64_MAX, UINT64_MAX}.
// ---------------------------------------------------------------------------

namespace {

// Construct a Decimal128 from a native int64 (scale-0).
Decimal128 from_int64(int64_t v) {
    if (v >= 0) {
        return Decimal128{0, static_cast<uint64_t>(v)};
    }
    // Negative: hi = -1 (all ones), lo = two's-complement low 64 bits.
    return Decimal128{-1, static_cast<uint64_t>(v)};
}

// Return true if the Decimal128 represents the signed 128-bit minimum
// (i.e., {INT64_MIN, 0}), which has no positive counterpart.
bool is_min128(Decimal128 a) {
    return a.hi == INT64_MIN && a.lo == 0;
}

// Compare two Decimal128 as signed 128-bit integers.
// Returns -1, 0, or +1.
int cmp128(Decimal128 a, Decimal128 b) {
    if (a.hi != b.hi) {
        return (a.hi < b.hi) ? -1 : 1;
    }
    if (a.lo != b.lo) {
        return (a.lo < b.lo) ? -1 : 1;
    }
    return 0;
}

// Negate a signed 128-bit integer. Returns error if value is INT128_MIN.
Result<Decimal128> negate128(Decimal128 a) {
    if (is_min128(a)) {
        return make_error(StatusCode::TYPE_ERROR,
                          "DECIMAL overflow: cannot negate minimum 128-bit value");
    }
    // Two's complement negation: flip all bits, add 1.
    uint64_t lo = ~a.lo + 1u;
    int64_t hi = ~a.hi;
    if (lo == 0u) {
        // Carry: increment hi.
        if (hi == INT64_MAX) {
            return make_error(StatusCode::TYPE_ERROR, "DECIMAL overflow during negation");
        }
        hi += 1;
    }
    return ok(Decimal128{hi, lo});
}

// Absolute value (as Decimal128). Returns error on INT128_MIN.
Result<Decimal128> abs128(Decimal128 a) {
    if (a.hi < 0) {
        return negate128(a);
    }
    return ok(a);
}

// Low table: 10^0 .. 10^18 as uint64_t (all fit).
static const uint64_t pow10_table_lo[] = {
    1ULL,
    10ULL,
    100ULL,
    1000ULL,
    10000ULL,
    100000ULL,
    1000000ULL,
    10000000ULL,
    100000000ULL,
    1000000000ULL,
    10000000000ULL,
    100000000000ULL,
    1000000000000ULL,
    10000000000000ULL,
    100000000000000ULL,
    1000000000000000ULL,
    10000000000000000ULL,
    100000000000000000ULL,
    1000000000000000000ULL, // 10^18
};

// Multiply two unsigned 64-bit values, returning 128-bit result as {hi, lo}.
void umul64(uint64_t a, uint64_t b, uint64_t& hi_out, uint64_t& lo_out) {
    // Split each into 32-bit halves.
    uint64_t a_lo = a & 0xFFFFFFFFu;
    uint64_t a_hi = a >> 32u;
    uint64_t b_lo = b & 0xFFFFFFFFu;
    uint64_t b_hi = b >> 32u;

    uint64_t p0 = a_lo * b_lo;
    uint64_t p1 = a_lo * b_hi;
    uint64_t p2 = a_hi * b_lo;
    uint64_t p3 = a_hi * b_hi;

    uint64_t mid = (p0 >> 32u) + (p1 & 0xFFFFFFFFu) + (p2 & 0xFFFFFFFFu);
    hi_out = p3 + (p1 >> 32u) + (p2 >> 32u) + (mid >> 32u);
    lo_out = (mid << 32u) | (p0 & 0xFFFFFFFFu);
}

} // namespace

// ---------------------------------------------------------------------------
// dec128_is_zero / dec128_sign
// ---------------------------------------------------------------------------

bool dec128_is_zero(Decimal128 a) {
    return a.hi == 0 && a.lo == 0;
}

int dec128_sign(Decimal128 a) {
    if (a.hi < 0) {
        return -1;
    }
    if (a.hi > 0 || a.lo != 0) {
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// dec128_add
// ---------------------------------------------------------------------------

Result<Decimal128> dec128_add(Decimal128 a, Decimal128 b) {
    uint64_t lo = a.lo + b.lo;
    uint64_t carry = (lo < a.lo) ? 1u : 0u;

    // Perform signed hi + hi + carry, detecting overflow.
    // Overflow occurs when adding two same-sign values produces opposite sign.
    int64_t hi_raw =
        static_cast<int64_t>(static_cast<uint64_t>(a.hi) + static_cast<uint64_t>(b.hi) + carry);
    // Overflow check: if a.hi and b.hi have the same sign but hi_raw differs.
    bool a_pos = (a.hi >= 0);
    bool b_pos = (b.hi >= 0);
    bool r_pos = (hi_raw >= 0);
    if (a_pos == b_pos && a_pos != r_pos) {
        return make_error(StatusCode::TYPE_ERROR, "DECIMAL arithmetic overflow (addition)");
    }

    return ok(Decimal128{hi_raw, lo});
}

// ---------------------------------------------------------------------------
// dec128_sub
// ---------------------------------------------------------------------------

Result<Decimal128> dec128_sub(Decimal128 a, Decimal128 b) {
    // a - b = a + (-b). Special case: negating INT128_MIN is impossible.
    auto nb = negate128(b);
    if (!nb) {
        // b is INT128_MIN; -b would be INT128_MAX+1 which overflows.
        // a - INT128_MIN overflows for any finite a (since a >= INT128_MIN).
        return make_error(StatusCode::TYPE_ERROR, "DECIMAL arithmetic overflow (subtraction)");
    }
    return dec128_add(a, *nb);
}

// ---------------------------------------------------------------------------
// dec128_mul
// ---------------------------------------------------------------------------

Result<Decimal128> dec128_mul(Decimal128 a, Decimal128 b) {
    if (dec128_is_zero(a) || dec128_is_zero(b)) {
        return ok(Decimal128{0, 0});
    }

    // Determine result sign.
    int sign_a = dec128_sign(a);
    int sign_b = dec128_sign(b);
    int sign_r = sign_a * sign_b;

    // Work with absolute values.
    auto abs_a = abs128(a);
    auto abs_b = abs128(b);
    if (!abs_a || !abs_b) {
        return make_error(StatusCode::TYPE_ERROR, "DECIMAL overflow in multiplication");
    }

    // If either hi is non-zero the product would exceed 128 bits.
    if (abs_a->hi != 0 || abs_b->hi != 0) {
        return make_error(StatusCode::TYPE_ERROR,
                          "DECIMAL arithmetic overflow (multiplication exceeds 128 bits)");
    }

    // Both abs values fit in 64 bits. Perform 64x64->128 unsigned multiply.
    uint64_t r_hi = 0;
    uint64_t r_lo = 0;
    umul64(abs_a->lo, abs_b->lo, r_hi, r_lo);

    // The result must fit in a *signed* 128-bit integer.
    // Max positive value: hi==INT64_MAX, lo==UINT64_MAX.
    // Max negative abs:   hi==INT64_MAX+1 unsigned, lo==0 (i.e., INT128_MIN abs).
    if (sign_r >= 0) {
        // Positive result: hi must fit in int64_t (non-negative).
        if (r_hi > static_cast<uint64_t>(INT64_MAX)) {
            return make_error(StatusCode::TYPE_ERROR,
                              "DECIMAL arithmetic overflow (multiplication)");
        }
        return ok(Decimal128{static_cast<int64_t>(r_hi), r_lo});
    } else {
        // Negative result: abs must be <= INT128_MIN_ABS = {INT64_MAX+1u, 0}.
        // INT128_MIN == {INT64_MIN, 0} which has abs == {INT64_MAX+1 as uint, 0}.
        uint64_t min_abs_hi = static_cast<uint64_t>(INT64_MAX) + 1u;
        if (r_hi > min_abs_hi || (r_hi == min_abs_hi && r_lo != 0)) {
            return make_error(StatusCode::TYPE_ERROR,
                              "DECIMAL arithmetic overflow (multiplication)");
        }
        // Negate the unsigned result using two's complement.
        uint64_t neg_lo = ~r_lo + 1u;
        int64_t neg_hi = static_cast<int64_t>(~r_hi);
        if (neg_lo == 0u) {
            neg_hi += 1;
        }
        return ok(Decimal128{neg_hi, neg_lo});
    }
}

// ---------------------------------------------------------------------------
// dec128_mul_pow10
// ---------------------------------------------------------------------------

Result<Decimal128> dec128_mul_pow10(Decimal128 a, int32_t n) {
    if (n < 0) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "dec128_mul_pow10: negative exponent not supported");
    }
    if (n == 0 || dec128_is_zero(a)) {
        return ok(a);
    }

    // Multiply by 10 one step at a time using the table for small n,
    // or iteratively for large n. Each step uses dec128_mul.
    Decimal128 result = a;
    int32_t remaining = n;

    while (remaining > 0) {
        int32_t step = (remaining <= 18) ? remaining : 18;
        Decimal128 factor = from_int64(static_cast<int64_t>(pow10_table_lo[step]));
        auto r = dec128_mul(result, factor);
        if (!r) {
            return r;
        }
        result = *r;
        remaining -= step;
    }
    return ok(result);
}

// ---------------------------------------------------------------------------
// dec128_div_round (round-half-away-from-zero)
// ---------------------------------------------------------------------------

Result<Decimal128> dec128_div_round(Decimal128 a, Decimal128 b) {
    if (dec128_is_zero(b)) {
        return make_error(StatusCode::INVALID_ARGUMENT, "DECIMAL division by zero");
    }
    if (dec128_is_zero(a)) {
        return ok(Decimal128{0, 0});
    }

    // Signs.
    int sign_a = dec128_sign(a);
    int sign_b = dec128_sign(b);
    int sign_r = sign_a * sign_b;

    auto abs_a = abs128(a);
    auto abs_b = abs128(b);
    if (!abs_a || !abs_b) {
        // INT128_MIN: extremely rare case; if b == INT128_MIN and a != b the
        // quotient is 0 or +-1 depending on magnitudes.
        // For simplicity, return 0 or +-1.
        if (is_min128(b)) {
            // |b| > any |a| unless a == b: quotient is 0.
            if (is_min128(a)) {
                return ok(from_int64(1)); // a/b == 1 (same value)
            }
            return ok(Decimal128{0, 0});
        }
        // a == INT128_MIN, b != INT128_MIN
        // We'll approximate: INT128_MIN / b.
        // Use the fact that INT128_MIN = -INT128_MAX - 1.
        // This edge case almost never occurs in practice; return error.
        return make_error(StatusCode::TYPE_ERROR,
                          "DECIMAL arithmetic overflow (division of minimum value)");
    }

    // Both |a| and |b| fit in unsigned 128-bit with non-negative hi.
    // Perform unsigned 128/128 division iteratively.
    // Since abs_a->hi and abs_b->hi must both be 0 for values that fit in
    // 63-bit magnitude (normal range), handle both cases.
    if (abs_b->hi != 0) {
        // |b| > 2^63; |a| < 2^127 but typically |a|/|b| < 1 -> quotient 0.
        // (This is very unusual for typical DECIMAL scales.)
        // Exact: if |a| < |b|, quotient = 0; else quotient = 1 (only when |a| >= |b|).
        int c = (abs_a->hi != abs_b->hi)
                    ? ((abs_a->hi < abs_b->hi) ? -1 : 1)
                    : ((abs_a->lo < abs_b->lo) ? -1 : (abs_a->lo == abs_b->lo ? 0 : 1));
        if (c < 0) {
            // |a| < |b|: quotient 0; check rounding: 2*|a| vs |b|.
            auto two_a = dec128_mul_pow10(*abs_a, 0);
            (void)two_a; // just need to compare 2*rem vs |b|
            // Simpler: 2*|a| >= |b| -> round to 1.
            // 2*|a|: shift by 1 means *2, not *10 -- use dec128_mul with factor 2.
            Decimal128 factor2 = from_int64(2);
            auto two_abs_a = dec128_mul(*abs_a, factor2);
            if (!two_abs_a) {
                return ok(Decimal128{0, 0}); // overflow -> quotient 0 is fine
            }
            int c2 =
                (two_abs_a->hi != abs_b->hi)
                    ? ((two_abs_a->hi < abs_b->hi) ? -1 : 1)
                    : ((two_abs_a->lo < abs_b->lo) ? -1 : (two_abs_a->lo == abs_b->lo ? 0 : 1));
            if (c2 >= 0) {
                // 2*|a| >= |b| -> round away from zero to 1.
                return ok(from_int64(sign_r));
            }
            return ok(Decimal128{0, 0});
        }
        // |a| >= |b|: quotient is 1.
        return ok(from_int64(sign_r));
    }

    if (abs_a->hi != 0 || abs_b->hi != 0) {
        // General 128/128 case is complex; abs_b->hi == 0 handled below.
        // For abs_b->hi==0 we only need 128/64 division.
        // This branch handles the remaining case where abs_b->hi != 0 was
        // already caught above. So here abs_b->hi == 0.
        (void)0;
    }

    // Now abs_b->hi == 0, so divisor fits in 64 bits.
    uint64_t divisor = abs_b->lo;

    // Perform 128-bit / 64-bit unsigned division.
    // Result = abs_a / divisor, remainder = abs_a % divisor.
    // Use long division over two 64-bit halves.
    uint64_t q_hi = 0;
    uint64_t q_lo = 0;
    uint64_t rem = 0;

    // Divide high 64 bits first.
    if (abs_a->hi != 0) {
        // abs_a->hi is non-negative (we took abs128).
        uint64_t a_hi = static_cast<uint64_t>(abs_a->hi);
        q_hi = a_hi / divisor;
        rem = a_hi % divisor;
    }

    // Now divide (rem * 2^64 + abs_a->lo) by divisor.
    // Use __uint128_t if available (MSVC doesn't have it, use manual approach).
    // Manual: two-step with 64-bit chunks.
    // Step 1: divide (rem << 32 | (abs_a->lo >> 32)) by divisor.
    uint64_t tmp_hi = (rem << 32u) | (abs_a->lo >> 32u);
    uint64_t q_mid = tmp_hi / divisor;
    uint64_t rem2 = tmp_hi % divisor;

    // Step 2: divide (rem2 << 32 | (abs_a->lo & 0xFFFFFFFF)) by divisor.
    uint64_t tmp_lo = (rem2 << 32u) | (abs_a->lo & 0xFFFFFFFFu);
    uint64_t q_low = tmp_lo / divisor;
    uint64_t remainder = tmp_lo % divisor;

    // Combine quotient parts.
    q_lo = (q_mid << 32u) | q_low;

    // Round half away from zero: if 2*remainder >= divisor, round up.
    bool round_up =
        (remainder * 2u >= divisor) || (remainder * 2u < remainder); // overflow means >= divisor
    if (round_up) {
        q_lo += 1u;
        if (q_lo == 0u) {
            q_hi += 1u;
        }
    }

    // q_hi must fit in signed int64_t (non-negative).
    if (q_hi > static_cast<uint64_t>(INT64_MAX)) {
        return make_error(StatusCode::TYPE_ERROR,
                          "DECIMAL arithmetic overflow (quotient exceeds 128 bits)");
    }

    Decimal128 pos_result{static_cast<int64_t>(q_hi), q_lo};
    if (sign_r >= 0) {
        return ok(pos_result);
    }
    return negate128(pos_result);
}

// ---------------------------------------------------------------------------
// dec128_mod
// ---------------------------------------------------------------------------

Result<Decimal128> dec128_mod(Decimal128 a, Decimal128 b) {
    if (dec128_is_zero(b)) {
        return make_error(StatusCode::INVALID_ARGUMENT, "DECIMAL modulo by zero");
    }
    if (dec128_is_zero(a)) {
        return ok(Decimal128{0, 0});
    }

    // quotient = trunc(a / b) (NOT round-half-away; for mod we need exact trunc).
    // remainder = a - quotient * b
    // Sign follows dividend (SQL standard).
    //
    // Compute quotient using the same machinery but with truncation.
    // Since dec128_div_round rounds, we compute the floor/trunc by:
    //   q = dec128_div_round(a, b), then verify r = a - q*b is in [0, |b|).
    // Actually simpler: reuse the same long-division code but without rounding.

    int sign_a = dec128_sign(a);
    int sign_b = dec128_sign(b);
    int sign_r_div = sign_a * sign_b;

    auto abs_a = abs128(a);
    auto abs_b = abs128(b);
    if (!abs_a || !abs_b) {
        return make_error(StatusCode::TYPE_ERROR, "DECIMAL overflow in modulo (extreme value)");
    }

    if (abs_b->hi != 0) {
        // |b| > |a| (since |a| < 2^127) -> quotient 0, remainder = a.
        return ok(a);
    }

    uint64_t divisor = abs_b->lo;
    uint64_t rem = 0;

    if (abs_a->hi != 0) {
        uint64_t a_hi = static_cast<uint64_t>(abs_a->hi);
        (void)(a_hi / divisor);
        rem = a_hi % divisor;
    }

    uint64_t tmp_hi = (rem << 32u) | (abs_a->lo >> 32u);
    uint64_t rem2 = tmp_hi % divisor;

    uint64_t tmp_lo = (rem2 << 32u) | (abs_a->lo & 0xFFFFFFFFu);
    uint64_t remainder = tmp_lo % divisor;

    (void)sign_r_div;

    // remainder is |a mod b|; apply dividend sign.
    if (remainder == 0) {
        return ok(Decimal128{0, 0});
    }
    Decimal128 pos_rem = from_int64(static_cast<int64_t>(remainder));
    if (sign_a >= 0) {
        return ok(pos_rem);
    }
    return negate128(pos_rem);
}

// ---------------------------------------------------------------------------
// decimal_add
// ---------------------------------------------------------------------------

Result<DecimalResult> decimal_add(Decimal128 c1, int32_t s1, Decimal128 c2, int32_t s2) {
    int32_t rs = (s1 >= s2) ? s1 : s2;

    // Align the smaller-scale operand by multiplying its coefficient by 10^diff.
    Decimal128 a = c1;
    Decimal128 b = c2;
    if (s1 < s2) {
        auto r = dec128_mul_pow10(c1, s2 - s1);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        a = *r;
    } else if (s2 < s1) {
        auto r = dec128_mul_pow10(c2, s1 - s2);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        b = *r;
    }

    auto sum = dec128_add(a, b);
    if (!sum) {
        return make_error(sum.error().code, sum.error().message);
    }
    return ok(DecimalResult{*sum, rs});
}

// ---------------------------------------------------------------------------
// decimal_sub
// ---------------------------------------------------------------------------

Result<DecimalResult> decimal_sub(Decimal128 c1, int32_t s1, Decimal128 c2, int32_t s2) {
    int32_t rs = (s1 >= s2) ? s1 : s2;

    Decimal128 a = c1;
    Decimal128 b = c2;
    if (s1 < s2) {
        auto r = dec128_mul_pow10(c1, s2 - s1);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        a = *r;
    } else if (s2 < s1) {
        auto r = dec128_mul_pow10(c2, s1 - s2);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        b = *r;
    }

    auto diff = dec128_sub(a, b);
    if (!diff) {
        return make_error(diff.error().code, diff.error().message);
    }
    return ok(DecimalResult{*diff, rs});
}

// ---------------------------------------------------------------------------
// decimal_mul
// ---------------------------------------------------------------------------

Result<DecimalResult> decimal_mul(Decimal128 c1, int32_t s1, Decimal128 c2, int32_t s2) {
    int32_t rs = s1 + s2;
    auto prod = dec128_mul(c1, c2);
    if (!prod) {
        return make_error(prod.error().code, prod.error().message);
    }
    return ok(DecimalResult{*prod, rs});
}

// ---------------------------------------------------------------------------
// decimal_div
// ---------------------------------------------------------------------------

Result<DecimalResult> decimal_div(Decimal128 c1, int32_t s1, Decimal128 c2, int32_t s2) {
    if (dec128_is_zero(c2)) {
        return make_error(StatusCode::INVALID_ARGUMENT, "DECIMAL division by zero");
    }

    // Result scale = max(s1+6, s1) = s1+6 (since 6 >= 0).
    int32_t rs = s1 + 6;

    // Numerator = c1 * 10^(rs - s1 + s2) to preserve scale.
    // rs - s1 + s2 = 6 + s2.
    int32_t extra = 6 + s2;
    auto num = dec128_mul_pow10(c1, extra);
    if (!num) {
        return make_error(num.error().code, num.error().message);
    }

    auto quot = dec128_div_round(*num, c2);
    if (!quot) {
        return make_error(quot.error().code, quot.error().message);
    }
    return ok(DecimalResult{*quot, rs});
}

// ---------------------------------------------------------------------------
// decimal_mod
// ---------------------------------------------------------------------------

Result<DecimalResult> decimal_mod(Decimal128 c1, int32_t s1, Decimal128 c2, int32_t s2) {
    if (dec128_is_zero(c2)) {
        return make_error(StatusCode::INVALID_ARGUMENT, "DECIMAL modulo by zero");
    }

    int32_t rs = (s1 >= s2) ? s1 : s2;

    // Align scales like add/sub.
    Decimal128 a = c1;
    Decimal128 b = c2;
    if (s1 < s2) {
        auto r = dec128_mul_pow10(c1, s2 - s1);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        a = *r;
    } else if (s2 < s1) {
        auto r = dec128_mul_pow10(c2, s1 - s2);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        b = *r;
    }

    auto rem = dec128_mod(a, b);
    if (!rem) {
        return make_error(rem.error().code, rem.error().message);
    }
    return ok(DecimalResult{*rem, rs});
}

// ---------------------------------------------------------------------------
// decimal_compare
// ---------------------------------------------------------------------------

Result<int> decimal_compare(Decimal128 c1, int32_t s1, Decimal128 c2, int32_t s2) {
    // Align scales exactly as add/sub.
    Decimal128 a = c1;
    Decimal128 b = c2;
    if (s1 < s2) {
        auto r = dec128_mul_pow10(c1, s2 - s1);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        a = *r;
    } else if (s2 < s1) {
        auto r = dec128_mul_pow10(c2, s1 - s2);
        if (!r) {
            return make_error(r.error().code, r.error().message);
        }
        b = *r;
    }
    return ok(cmp128(a, b));
}

// ---------------------------------------------------------------------------
// decimal_to_double
// ---------------------------------------------------------------------------

double decimal_to_double(Decimal128 coeff, int32_t scale) {
    // Convert coefficient to double.
    double d = 0.0;
    if (coeff.hi >= 0) {
        d = static_cast<double>(static_cast<uint64_t>(coeff.hi)) * 18446744073709551616.0 +
            static_cast<double>(coeff.lo);
    } else {
        // Negative: take abs then negate.
        auto abs_c = negate128(coeff);
        if (abs_c) {
            d = -(static_cast<double>(static_cast<uint64_t>(abs_c->hi)) * 18446744073709551616.0 +
                  static_cast<double>(abs_c->lo));
        } else {
            // INT128_MIN: approximate.
            d = -170141183460469231731687303715884105728.0;
        }
    }

    if (scale > 0) {
        double divisor = 1.0;
        for (int32_t i = 0; i < scale; ++i) {
            divisor *= 10.0;
        }
        d /= divisor;
    } else if (scale < 0) {
        double multiplier = 1.0;
        for (int32_t i = 0; i < -scale; ++i) {
            multiplier *= 10.0;
        }
        d *= multiplier;
    }
    return d;
}

} // namespace sixseven
