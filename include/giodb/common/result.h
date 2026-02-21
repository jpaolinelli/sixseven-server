#pragma once

#include "giodb/common/status.h"

#include <tl/expected.hpp>

#include <source_location>
#include <string>
#include <utility>

namespace giodb {

/// Error type used throughout GioDB for consistent error propagation.
struct Error {
    StatusCode code;
    std::string message;
    std::source_location location;

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

} // namespace giodb
