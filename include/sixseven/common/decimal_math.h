#pragma once

#include "sixseven/common/result.h"
#include "sixseven/common/value.h"

#include <cstdint>

namespace sixseven {

// ---------------------------------------------------------------------------
// Fixed-point Decimal128 arithmetic library
//
// Decimal128 ({int64_t hi, uint64_t lo}) is a scale-free signed 128-bit
// integer coefficient. Scale is threaded in from expression type metadata
// (CAST target TypeSpec.param2, or column CatalogColumnDef.scale) rather
// than stored inside Decimal128.
//
// Result scales follow SQL standard rules:
//   ADD/SUB : max(s1, s2)
//   MUL     : s1 + s2
//   DIV     : max(s1+6, s1)
//   MOD     : max(s1, s2)
//
// Mixed-type rule:
//   DECIMAL + integer  => integer treated as scale-0 DECIMAL (exact path).
//   DECIMAL + FLOAT32/64 => caller keeps the existing double path.
//
// Overflow at any step -> TYPE_ERROR (no silent wrap).
// ---------------------------------------------------------------------------

/// Result of a decimal arithmetic operation: coefficient + result scale.
struct DecimalResult {
    Decimal128 coeff;
    int32_t scale = 0;
};

// ---------------------------------------------------------------------------
// Signed 128-bit primitive helpers (visible for unit tests).
// ---------------------------------------------------------------------------

/// Add two Decimal128 values as signed 128-bit integers.
/// Returns TYPE_ERROR on signed overflow.
[[nodiscard]] Result<Decimal128> dec128_add(Decimal128 a, Decimal128 b);

/// Subtract two Decimal128 values as signed 128-bit integers.
/// Returns TYPE_ERROR on signed overflow.
[[nodiscard]] Result<Decimal128> dec128_sub(Decimal128 a, Decimal128 b);

/// Multiply two Decimal128 values as signed 128-bit integers.
/// Returns TYPE_ERROR on signed overflow.
[[nodiscard]] Result<Decimal128> dec128_mul(Decimal128 a, Decimal128 b);

/// Multiply a Decimal128 coefficient by 10^n (n >= 0).
/// Returns TYPE_ERROR on overflow.
[[nodiscard]] Result<Decimal128> dec128_mul_pow10(Decimal128 a, int32_t n);

/// Integer-divide a by b (b != 0) with round-half-away-from-zero.
/// Returns TYPE_ERROR if b == 0.
[[nodiscard]] Result<Decimal128> dec128_div_round(Decimal128 a, Decimal128 b);

/// Integer remainder (a mod b) -- sign follows dividend, matching SQL.
/// Returns TYPE_ERROR if b == 0.
[[nodiscard]] Result<Decimal128> dec128_mod(Decimal128 a, Decimal128 b);

// ---------------------------------------------------------------------------
// Fixed-point decimal operations
// ---------------------------------------------------------------------------

/// Add two DECIMAL values. Result scale = max(s1, s2).
[[nodiscard]] Result<DecimalResult>
decimal_add(Decimal128 c1, int32_t s1, Decimal128 c2, int32_t s2);

/// Subtract two DECIMAL values. Result scale = max(s1, s2).
[[nodiscard]] Result<DecimalResult>
decimal_sub(Decimal128 c1, int32_t s1, Decimal128 c2, int32_t s2);

/// Multiply two DECIMAL values. Result scale = s1 + s2.
[[nodiscard]] Result<DecimalResult>
decimal_mul(Decimal128 c1, int32_t s1, Decimal128 c2, int32_t s2);

/// Divide two DECIMAL values. Result scale = max(s1+6, s1).
/// Division by zero returns TYPE_ERROR.
[[nodiscard]] Result<DecimalResult>
decimal_div(Decimal128 c1, int32_t s1, Decimal128 c2, int32_t s2);

/// Modulo of two DECIMAL values. Result scale = max(s1, s2).
/// Division by zero returns TYPE_ERROR.
[[nodiscard]] Result<DecimalResult>
decimal_mod(Decimal128 c1, int32_t s1, Decimal128 c2, int32_t s2);

// ---------------------------------------------------------------------------
// Comparison helpers
// ---------------------------------------------------------------------------

/// Compare two DECIMAL values (possibly different scales) as signed integers.
/// Aligns scales before comparing, exactly like ADD/SUB alignment.
/// Returns negative / zero / positive (like memcmp).
[[nodiscard]] Result<int> decimal_compare(Decimal128 c1, int32_t s1, Decimal128 c2, int32_t s2);

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

/// Convert a Decimal128 coefficient + scale to a double (for display / mixed
/// arithmetic fallback). Not used in the exact arithmetic path.
double decimal_to_double(Decimal128 coeff, int32_t scale);

/// Return true if the Decimal128 coefficient is zero.
bool dec128_is_zero(Decimal128 a);

/// Return the sign of a Decimal128 coefficient: -1, 0, or +1.
int dec128_sign(Decimal128 a);

} // namespace sixseven
