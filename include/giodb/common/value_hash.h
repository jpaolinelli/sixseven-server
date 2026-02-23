#pragma once

#include "giodb/common/coercion.h"
#include "giodb/common/value.h"

#include <compare>
#include <functional>
#include <string>
#include <type_traits>
#include <variant>

namespace giodb {

/// Hash functor for Value, suitable for use with std::unordered_set/map.
struct ValueHash {
    size_t operator()(const Value& v) const {
        if (v.is_null()) {
            return 0;
        }
        const auto& data = v.data();
        return std::visit(
            [](const auto& val) -> size_t {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, std::monostate>) {
                    return 0;
                } else if constexpr (std::is_same_v<T, std::string>) {
                    return std::hash<std::string>{}(val);
                } else if constexpr (std::is_same_v<T, bool>) {
                    return std::hash<bool>{}(val);
                } else if constexpr (std::is_arithmetic_v<T>) {
                    return std::hash<T>{}(val);
                } else {
                    return 0;
                }
            },
            data);
    }
};

/// Equality functor for Value, suitable for use with std::unordered_set/map.
struct ValueEqual {
    bool operator()(const Value& a, const Value& b) const {
        if (a.is_null() || b.is_null()) {
            return a.is_null() && b.is_null();
        }
        auto cmp = compare(a, b);
        if (!cmp) {
            return false;
        }
        return *cmp == std::strong_ordering::equal;
    }
};

} // namespace giodb
