#pragma once

#include "giodb/common/result.h"
#include "giodb/common/value.h"

#include <string>

namespace giodb {

/// Parse a UUID string into a 16-byte Uuid.
/// Accepts the standard hyphenated format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
/// Hex digits are case-insensitive.
[[nodiscard]] Result<Uuid> parse_uuid(const std::string& s);

} // namespace giodb
