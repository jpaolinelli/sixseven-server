#pragma once

#include "sixseven/common/value.h"

#include <string>

namespace sixseven {

/// Serialize a primary-key Value to a deterministic string suitable for hash
/// set lookup (e.g. by QueryEngine::pk_cache_).
///
/// Serializes every scalar PK type injectively (distinct values of the same
/// type map to distinct strings): the integer family, FLOAT32/FLOAT64 (keyed by
/// IEEE-754 bit pattern), DECIMAL, BOOL, DATE/TIME/TIMESTAMP/INTERVAL, POINT,
/// JSON, BLOB, STRING, and UUID. EMBEDDING and PATH are composite, non-scalar
/// types that cannot serve as a primary key; they fall back to a non-injective
/// "?<type_id>" sentinel that must never be relied on for a real PK.
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
