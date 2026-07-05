#pragma once

#include "sixseven/common/result.h"
#include "sixseven/common/value.h"

#include <string>

namespace sixseven {

/// Convert a Value holding an integer type to int64_t.
///
/// `context` is embedded verbatim in the TYPE_ERROR message produced when
/// `v` does not hold an integer type, e.g. "<context> requires integer node
/// keys". Pass the algorithm/usage-specific phrase each call site previously
/// hard-coded so error messages are unchanged.
///
/// NOTE (behavior-preserving, latent quirk kept as-is): uint64_t values are
/// converted with `static_cast<int64_t>`, so inputs greater than
/// INT64_MAX silently wrap to a negative int64_t rather than erroring.
[[nodiscard]] Result<int64_t> value_to_int64(const Value& v, const std::string& context);

/// Convert a Value holding a numeric type to double.
///
/// `context` is embedded verbatim in the TYPE_ERROR message produced when
/// `v` does not hold a numeric type, e.g. "expected numeric value for
/// <context>".
[[nodiscard]] Result<double> value_to_double(const Value& v, const std::string& context);

/// Convert a Value holding a string type to std::string.
///
/// `context` is embedded verbatim in the TYPE_ERROR message produced when
/// `v` does not hold a string type, e.g. "expected string value for
/// <context>".
[[nodiscard]] Result<std::string> value_to_string(const Value& v, const std::string& context);

} // namespace sixseven
