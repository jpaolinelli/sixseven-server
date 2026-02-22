#pragma once

#include "giodb/common/result.h"
#include "giodb/common/value.h"

#include <compare>
#include <vector>

namespace giodb {

/// A B+ tree key: one Value per indexed column (supports composite keys).
using KeyType = std::vector<Value>;

/// Lexicographic comparison of two composite keys.
/// Compares element by element using giodb::compare().
/// Keys must have the same number of columns.
/// Returns error if types are non-comparable (BLOB, EMBEDDING).
Result<std::strong_ordering> compare_keys(const KeyType& lhs, const KeyType& rhs);

} // namespace giodb
