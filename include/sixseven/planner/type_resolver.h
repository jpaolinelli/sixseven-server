#pragma once

#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/parser/ast.h"

#include <string>

namespace sixseven {

/// Resolve a parsed TypeSpec (e.g., "INT", "VARCHAR(255)") to a runtime TypeId.
/// Case-insensitive. Returns TYPE_ERROR for unrecognised type names.
Result<TypeId> resolve_type_spec(const TypeSpec& spec);

/// Compute the common type for a binary operation (arithmetic, comparison).
/// Uses numeric widening via can_coerce(). Returns TYPE_ERROR if the types
/// have no common supertype.
Result<TypeId> common_type(TypeId a, TypeId b);

/// Return true if `name` (uppercase) is a known aggregate function.
bool is_aggregate_function(const std::string& name);

/// Return the result TypeId for an aggregate function applied to `input_type`.
/// E.g., COUNT(*)→INT64, SUM(INT32)→INT64, AVG(*)→FLOAT64.
/// Returns TYPE_ERROR for unknown aggregates.
Result<TypeId> aggregate_return_type(const std::string& name, TypeId input_type);

/// Return the result TypeId for a scalar function applied to given argument types.
/// Returns TYPE_ERROR for unknown functions.
Result<TypeId> function_return_type(const std::string& name, const std::vector<TypeId>& arg_types);

} // namespace sixseven
