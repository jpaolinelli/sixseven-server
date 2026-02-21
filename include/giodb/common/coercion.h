#pragma once

#include "giodb/common/result.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"

#include <compare>

namespace giodb {

/// Return true if a value of type `from` can be implicitly coerced to type `to`.
bool can_coerce(TypeId from, TypeId to);

/// Coerce a value to the target type. Returns an error if the coercion is not supported.
Result<Value> coerce(const Value& value, TypeId target);

/// Compare two values, coercing to a common type if necessary.
/// Returns an error if the types are not comparable (e.g., BLOB, EMBEDDING,
/// or incompatible types with no coercion path).
/// NULL sorts before all non-NULL values.
Result<std::strong_ordering> compare(const Value& lhs, const Value& rhs);

} // namespace giodb
