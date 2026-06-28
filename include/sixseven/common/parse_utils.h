#pragma once

#include "sixseven/common/result.h"

#include <cstdint>
#include <string_view>

namespace sixseven {

// ---------------------------------------------------------------------------
// Safe numeric-parse helpers
//
// Each function wraps the corresponding std::sto* call in a try/catch and
// returns a Result<T> instead of throwing.  On failure the Result carries
// StatusCode::PARSE_ERROR with a short context message.
//
// Semantics mirror the existing parser.cpp local helpers exactly:
//   - std::stoi / std::stoll / std::stoull / std::stoul / std::stod / std::stof
//     accept leading whitespace and trailing non-numeric characters (e.g.
//     "12abc" -> 12) because the underlying std::sto* functions do, and
//     rejecting trailing garbage would change existing behaviour.
//   - safe_stou32 additionally range-checks the parsed uint64 value against
//     UINT32_MAX and returns PARSE_ERROR if it exceeds it.
// ---------------------------------------------------------------------------

/// Parse a string_view as int using std::stoi.
[[nodiscard]] Result<int> safe_stoi(std::string_view s);

/// Parse a string_view as int64_t using std::stoll.
[[nodiscard]] Result<int64_t> safe_stoll(std::string_view s);

/// Parse a string_view as uint32_t via std::stoull + range check.
[[nodiscard]] Result<uint32_t> safe_stou32(std::string_view s);

/// Parse a string_view as unsigned long using std::stoul.
[[nodiscard]] Result<unsigned long> safe_stoul(std::string_view s);

/// Parse a string_view as uint64_t using std::stoull.
[[nodiscard]] Result<uint64_t> safe_stoull(std::string_view s);

/// Parse a string_view as double using std::stod.
[[nodiscard]] Result<double> safe_stod(std::string_view s);

/// Parse a string_view as float using std::stof.
[[nodiscard]] Result<float> safe_stof(std::string_view s);

} // namespace sixseven
