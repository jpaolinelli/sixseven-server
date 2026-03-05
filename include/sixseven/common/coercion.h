#pragma once

#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"

#include <compare>

namespace sixseven {

/// Return true if a value of type `from` can be implicitly coerced to type `to`.
bool can_coerce(TypeId from, TypeId to);

/// Coerce a value to the target type. Returns an error if the coercion is not supported.
Result<Value> coerce(const Value& value, TypeId target);

/// Fit a value to a storage type, allowing both widening and narrowing numeric
/// conversions. Unlike coerce(), which only allows implicit widening, this is
/// intended for DML contexts where the expression evaluator may produce a wider
/// type than the column's declared type (e.g., INT64 literal → INT32 column).
[[nodiscard]] Result<Value> fit_to_storage(const Value& value, TypeId target);

/// Compare two values, coercing to a common type if necessary.
/// Returns an error if the types are not comparable (e.g., BLOB, EMBEDDING,
/// or incompatible types with no coercion path).
/// NULL sorts before all non-NULL values.
Result<std::strong_ordering> compare(const Value& lhs, const Value& rhs);

} // namespace sixseven
