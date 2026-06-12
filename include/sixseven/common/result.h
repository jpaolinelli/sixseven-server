#pragma once

#include "sixseven/common/status.h"

#include <tl/expected.hpp>

#include <optional>
#include <source_location>
#include <string>
#include <utility>

namespace sixseven {

/// Error type used throughout SixSevenDB for consistent error propagation.
struct Error {
    StatusCode code;
    std::string message;
    std::source_location location;
    /// 1-based byte offset into the query string where the error occurred.
    /// When set for a PARSE_ERROR, the PostgreSQL 'P' (Position) field is emitted
    /// in ErrorResponse so that clients (e.g. psql) can draw a caret.
    std::optional<uint32_t> query_pos;

    Error(StatusCode code,
          std::string message,
          std::source_location location = std::source_location::current())
        : code(code), message(std::move(message)), location(location) {}
};

/// Result type alias: holds either a value T or an Error.
template <typename T>
using Result = tl::expected<T, Error>;

/// Convenience: create a successful Result containing value.
template <typename T>
Result<T> ok(T value) {
    return Result<T>(std::move(value));
}

/// Convenience: create a successful Result<void>.
inline Result<void> ok() {
    return {};
}

/// Convenience: create a failed Result with the given status code and message.
inline tl::unexpected<Error>
make_error(StatusCode code,
           std::string message,
           std::source_location location = std::source_location::current()) {
    return tl::unexpected<Error>(Error(code, std::move(message), location));
}

/// Convenience: create a failed Result with an explicit 1-based query byte offset.
/// Use for PARSE_ERROR so that the PostgreSQL 'P' Position field can be emitted.
inline tl::unexpected<Error>
make_parse_error(std::string message,
                 uint32_t query_pos,
                 std::source_location location = std::source_location::current()) {
    Error err(StatusCode::PARSE_ERROR, std::move(message), location);
    err.query_pos = query_pos;
    return tl::unexpected<Error>(std::move(err));
}

} // namespace sixseven
