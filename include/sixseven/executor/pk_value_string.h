#pragma once

#include "sixseven/common/value.h"

#include <string>

namespace sixseven {

/// Serialize a primary-key Value to a deterministic string suitable for hash
/// set lookup (e.g. by QueryEngine::pk_cache_).
///
/// Handles the common PK types: integers, strings, and UUIDs. For unsupported
/// types, falls back to a "?<type_id>" prefix.
///
/// NULL handling: NULL values return a fixed sentinel string ("\x01NULL") that
/// cannot collide with any legal PK string representation. Callers should
/// generally avoid passing NULL here, but the sentinel guarantees we never
/// trip the Value::type_id() == TypeId::INT8 NULL placeholder trap (see
/// value.h:169-175) and never call as_int8() on a std::monostate variant.
[[nodiscard]] std::string pk_value_to_string(const Value& v);

/// Sentinel returned by pk_value_to_string() for NULL values. Exposed so tests
/// (and any caller that wants to detect the NULL case) can compare against it.
inline constexpr const char* pk_value_null_sentinel = "\x01NULL";

} // namespace sixseven
