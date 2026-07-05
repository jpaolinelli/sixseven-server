#include "sixseven/graph/value_extract.h"

#include <variant>

namespace sixseven {

Result<int64_t> value_to_int64(const Value& v, const std::string& context) {
    if (v.is_null()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "NULL node key in edge");
    }
    return std::visit(
        [&context](const auto& val) -> Result<int64_t> {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, int8_t> || std::is_same_v<T, int16_t> ||
                          std::is_same_v<T, int32_t> || std::is_same_v<T, int64_t>) {
                return ok(static_cast<int64_t>(val));
            } else if constexpr (std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> ||
                                 std::is_same_v<T, uint32_t>) {
                return ok(static_cast<int64_t>(val));
            } else if constexpr (std::is_same_v<T, uint64_t>) {
                return ok(static_cast<int64_t>(val));
            } else {
                return make_error(StatusCode::TYPE_ERROR, context + " requires integer node keys");
            }
        },
        v.data());
}

Result<double> value_to_double(const Value& v, const std::string& context) {
    if (v.is_null()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "NULL parameter value");
    }
    return std::visit(
        [&context](const auto& val) -> Result<double> {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_arithmetic_v<T>) {
                return ok(static_cast<double>(val));
            } else {
                return make_error(StatusCode::TYPE_ERROR, "expected numeric value for " + context);
            }
        },
        v.data());
}

Result<std::string> value_to_string(const Value& v, const std::string& context) {
    if (v.is_null()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "NULL parameter value");
    }
    return std::visit(
        [&context](const auto& val) -> Result<std::string> {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return ok(std::string(val));
            } else {
                return make_error(StatusCode::TYPE_ERROR, "expected string value for " + context);
            }
        },
        v.data());
}

} // namespace sixseven
