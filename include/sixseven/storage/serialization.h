#pragma once

#include "sixseven/common/result.h"
#include "sixseven/common/value.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sixseven {

/// Serialize a Value to a byte buffer.
/// Format: 1-byte null flag (0x00 = null, 0x01 = not null) followed by value data.
/// Fixed-size types use little-endian byte order.
/// Variable-length types use a 4-byte little-endian length prefix followed by data.
std::vector<uint8_t> serialize(const Value& value);

/// Deserialize a Value from a byte buffer with the given TypeId.
/// The buffer must have been produced by serialize().
Result<Value> deserialize(std::span<const uint8_t> data, TypeId type_id);

/// Return the serialized size in bytes for a Value (including the null flag byte).
size_t serialized_size(const Value& value);

} // namespace sixseven
