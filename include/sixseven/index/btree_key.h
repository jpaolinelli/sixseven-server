#pragma once

#include "sixseven/common/result.h"
#include "sixseven/common/value.h"

#include <compare>
#include <vector>

namespace sixseven {

/// A B+ tree key: one Value per indexed column (supports composite keys).
using KeyType = std::vector<Value>;

/// Lexicographic comparison of two composite keys.
/// Compares element by element using sixseven::compare().
/// Keys must have the same number of columns.
/// Returns error if types are non-comparable (BLOB, EMBEDDING).
Result<std::strong_ordering> compare_keys(const KeyType& lhs, const KeyType& rhs);

} // namespace sixseven
