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

/// Hash a numeric Value (any integer or floating-point width) so that it
/// matches sixseven::compare()'s cross-width numeric-equality semantics.
///
/// GDB-1224: HashJoinOperator::hash_value() delegates to ValueHash, but
/// values_equal() delegates to compare(), which numerically promotes mixed
/// integer widths (e.g. INT32 vs INT64) and compares them by value -- so
/// INT32(2) and INT64(2) are equal under values_equal()/compare() but,
/// without this normalization, used to hash to different std::hash<T>
/// buckets (one per concrete C++ type), violating the hash/equal contract
/// and silently dropping matching rows from a hash join whenever the two
/// sides of the join key had different (but value-compatible) integer
/// widths -- e.g. joining a table-valued function's INT64 output column
/// against an INT32 primary key column.
///
/// Every numeric TypeId -- integer or floating -- is hashed via its `double`
/// representation. This matches compare()'s own cross-type promotion (it
/// compares any int-vs-float mix, and DECIMAL-vs-other-numeric, as doubles),
/// and guarantees the hash/equal contract holds for every numeric pairing
/// compare() can call equal, including integer-vs-float. Same-type or
/// same-signedness integer-vs-integer comparisons stay exact in compare()
/// itself (see src/common/coercion.cpp), so this hash is intentionally
/// coarser than exact-integer hashing would be: two distinct int64_t/uint64_t
/// values so large they alias to the same double (beyond 2^53) can share a
/// hash bucket. That is only a hash-quality trade-off, never a correctness
/// bug -- ValueEqual (backed by compare()) still separates them exactly, so a
/// collision at worst degrades an O(1) hash index towards O(n) for that
/// bucket, it never causes a false-positive or a missed match.
[[nodiscard]] inline size_t hash_numeric_value(const Value& v) {
    TypeId t = v.type_id();
    double d = 0.0;
    switch (t) {
    case TypeId::INT8:
        d = v.as_int8();
        break;
    case TypeId::INT16:
        d = v.as_int16();
        break;
    case TypeId::INT32:
        d = v.as_int32();
        break;
    case TypeId::INT64:
        d = static_cast<double>(v.as_int64());
        break;
    case TypeId::UINT8:
        d = v.as_uint8();
        break;
    case TypeId::UINT16:
        d = v.as_uint16();
        break;
    case TypeId::UINT32:
        d = v.as_uint32();
        break;
    case TypeId::UINT64:
        d = static_cast<double>(v.as_uint64());
        break;
    case TypeId::FLOAT32:
        d = static_cast<double>(v.as_float32());
        break;
    case TypeId::FLOAT64:
        d = v.as_float64();
        break;
    default:
        break;
    }
    return std::hash<double>{}(d);
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
        // DECIMAL is intentionally excluded here: compare() only promotes it
        // to double against a *different* numeric type, but same-type DECIMAL
        // comparison (the common case for a DECIMAL column) is exact via
        // (hi, lo), so it keeps its own Decimal128 hash below rather than
        // losing precision through a double round-trip.
        if (is_integer(v.type_id()) || is_floating(v.type_id())) {
            return hash_numeric_value(v);
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
