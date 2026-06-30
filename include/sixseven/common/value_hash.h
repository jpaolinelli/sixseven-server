#pragma once

#include "sixseven/common/coercion.h"
#include "sixseven/common/value.h"

#include <compare>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace sixseven {

/// Mix sub-hash @p k into accumulator @p h (boost::hash_combine constant).
[[nodiscard]] inline size_t value_hash_combine(size_t h, size_t k) {
    return h ^ (k + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
}

/// Hash functor for Value, suitable for use with std::unordered_set/map.
///
/// Every comparable Value type produces a well-distributed hash so ValueHash +
/// ValueEqual back a real O(1) HashIndex. Hashing the underlying representation
/// of each wrapper type (rather than returning a constant) is required for
/// GDB-244's same-hash bucket-split guard to ever split: a hash index on a
/// DATE/TIMESTAMP/DECIMAL/UUID/... column previously collapsed every key into
/// one ever-growing bucket (GDB-1042). Non-comparable types (EMBEDDING, PATH)
/// are never equal under ValueEqual, so they are never valid hash keys and keep
/// returning 0.
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
                    // Integer family + FLOAT32/FLOAT64 (std::hash already gives
                    // equal floats equal hashes).
                    return std::hash<T>{}(val);
                } else if constexpr (std::is_same_v<T, Decimal128>) {
                    return value_hash_combine(std::hash<int64_t>{}(val.hi),
                                              std::hash<uint64_t>{}(val.lo));
                } else if constexpr (std::is_same_v<T, Date>) {
                    return std::hash<int32_t>{}(val.days_since_epoch);
                } else if constexpr (std::is_same_v<T, Time>) {
                    return std::hash<int64_t>{}(val.microseconds);
                } else if constexpr (std::is_same_v<T, Timestamp>) {
                    return std::hash<int64_t>{}(val.microseconds);
                } else if constexpr (std::is_same_v<T, Interval>) {
                    return value_hash_combine(std::hash<int64_t>{}(val.months),
                                              std::hash<int64_t>{}(val.microseconds));
                } else if constexpr (std::is_same_v<T, Point>) {
                    return value_hash_combine(std::hash<double>{}(val.x),
                                              std::hash<double>{}(val.y));
                } else if constexpr (std::is_same_v<T, JsonString>) {
                    return std::hash<std::string>{}(val.data);
                } else if constexpr (std::is_same_v<T, Uuid>) {
                    return std::hash<std::string_view>{}(
                        std::string_view(reinterpret_cast<const char*>(val.data()), val.size()));
                } else if constexpr (std::is_same_v<T, Blob>) {
                    return std::hash<std::string_view>{}(
                        std::string_view(reinterpret_cast<const char*>(val.data()), val.size()));
                } else {
                    // EMBEDDING / PATH: not comparable, never a real hash key.
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

} // namespace sixseven
