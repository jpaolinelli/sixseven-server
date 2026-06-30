#pragma once

#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"

#include <compare>
#include <cstdint>

namespace sixseven {

/// Return true if a value of type `from` can be implicitly coerced to type `to`.
bool can_coerce(TypeId from, TypeId to);

/// Coerce a value to the target type. Returns an error if the coercion is not supported.
Result<Value> coerce(const Value& value, TypeId target);

/// Perform an explicit cast (SQL CAST / :: operator). Supports all implicit
/// coercions plus narrowing conversions that require an explicit request:
///   - Float -> integer (truncates toward zero, like C++ static_cast)
///   - Integer narrowing (e.g. INT64 -> INT32)
/// Returns an error for non-numeric cross-category casts (e.g. STRING -> INT32).
Result<Value> explicit_cast(const Value& value, TypeId target);

/// Fit a value to a storage type, allowing both widening and narrowing numeric
/// conversions. Unlike coerce(), which only allows implicit widening, this is
/// intended for DML contexts where the expression evaluator may produce a wider
/// type than the column's declared type (e.g., INT64 literal -> INT32 column).
///
/// For DECIMAL targets, `scale` is the column's declared scale (from
/// CatalogColumnDef::scale). The stored coefficient is computed as
///   coefficient = llround(value * 10^scale)
/// using round-half-away-from-zero semantics (llround). Examples:
///   3.7     into DECIMAL(10,2) -> coefficient 370
///   1.5     into DECIMAL(10,2) -> coefficient 150
///   3.14159 into DECIMAL(10,2) -> coefficient 314
///   3.7     into DECIMAL(10,0) -> coefficient 4
///   -2.5    into DECIMAL(10,2) -> coefficient -250
/// When scale == 0 (unspecified), the value is rounded to the nearest integer
/// coefficient (still better than the old truncate-toward-zero behaviour).
/// All existing 2-argument callers are unaffected by the default.
[[nodiscard]] Result<Value> fit_to_storage(const Value& value, TypeId target, int32_t scale = 0);

/// Compare two values, coercing to a common type if necessary.
/// Returns an error if the types are not comparable (e.g., BLOB, EMBEDDING,
/// or incompatible types with no coercion path).
/// NULL sorts before all non-NULL values.
Result<std::strong_ordering> compare(const Value& lhs, const Value& rhs);

} // namespace sixseven
