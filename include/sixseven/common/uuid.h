#pragma once

#include "sixseven/common/result.h"
#include "sixseven/common/value.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace sixseven {

/// Parse a UUID string into a 16-byte Uuid.
/// Accepts the standard hyphenated format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
/// Hex digits are case-insensitive.
[[nodiscard]] Result<Uuid> parse_uuid(const std::string& s);

/// Render raw bytes as lowercase hex, two digits per byte, zero-padded, with
/// no separators and no prefix (e.g. {0x00, 0x0f, 0xff} -> "000fff"). Shared
/// by callers that previously hand-rolled an ostringstream hex loop (auth
/// digest/salt formatting, UUID text rendering).
[[nodiscard]] std::string to_hex(const uint8_t* data, size_t len);

/// Render a 16-byte Uuid in canonical hyphenated form:
/// xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx, lowercase hex, zero-padded.
/// Inverse of parse_uuid.
[[nodiscard]] std::string format_uuid(const Uuid& uuid);

} // namespace sixseven
