#pragma once

#include "sixseven/common/result.h"
#include "sixseven/common/value.h"

#include <string>

namespace sixseven {

/// Parse a UUID string into a 16-byte Uuid.
/// Accepts the standard hyphenated format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
/// Hex digits are case-insensitive.
[[nodiscard]] Result<Uuid> parse_uuid(const std::string& s);

} // namespace sixseven
