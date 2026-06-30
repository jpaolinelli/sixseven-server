#include "sixseven/executor/pk_value_string.h"

#include "sixseven/common/types.h"

#include <bit>
#include <cstdint>

namespace sixseven {

namespace {

/// Lowercase-hex of a byte sequence. Injective for fixed-width inputs; the
/// variable-length BLOB caller prefixes the byte count so two blobs never
/// collide.
std::string hex_bytes(const uint8_t* p, size_t n) {
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += hex[(p[i] >> 4) & 0xF];
        s += hex[p[i] & 0xF];
    }
    return s;
}

/// Key for a 64-bit IEEE-754 double by its exact bit pattern. Using the bits
/// (not std::to_string, which rounds to 6 digits and would collide nearby
/// values) makes the key injective per distinct double: equal doubles share a
/// bit pattern, distinct doubles do not. -0.0/+0.0 and NaN payloads key
/// distinctly, which is acceptable for PK-identity purposes.
std::string double_bits_key(double d) {
    return std::to_string(std::bit_cast<uint64_t>(d));
}

} // namespace

std::string pk_value_to_string(const Value& v) {
    // CRITICAL: NULL must be handled before any type_id() switch. Value::type_id()
    // returns TypeId::INT8 for NULL (std::monostate) as a documented placeholder
    // (see value.h:169-175); without this guard the INT8 case below would call
    // v.as_int8(), which throws std::bad_variant_access on a NULL value and
    // tears down the server via std::terminate.
    if (v.is_null()) {
        return pk_value_null_sentinel;
    }

    switch (v.type_id()) {
    case TypeId::INT8:
        return std::to_string(v.as_int8());
    case TypeId::INT16:
        return std::to_string(v.as_int16());
    case TypeId::INT32:
        return std::to_string(v.as_int32());
    case TypeId::INT64:
        return std::to_string(v.as_int64());
    case TypeId::UINT8:
        return std::to_string(v.as_uint8());
    case TypeId::UINT16:
        return std::to_string(v.as_uint16());
    case TypeId::UINT32:
        return std::to_string(v.as_uint32());
    case TypeId::UINT64:
        return std::to_string(v.as_uint64());
    case TypeId::STRING:
        return v.as_string();
    case TypeId::UUID: {
        // Serialize UUID bytes as hex string for deterministic hashing.
        const auto& uuid = v.as_uuid();
        return hex_bytes(uuid.data(), uuid.size());
    }
    case TypeId::BOOL:
        return v.as_bool() ? "1" : "0";
    case TypeId::FLOAT32:
        // Key on the 32-bit pattern (see double_bits_key rationale).
        return std::to_string(std::bit_cast<uint32_t>(v.as_float32()));
    case TypeId::FLOAT64:
        return double_bits_key(v.as_float64());
    case TypeId::DECIMAL: {
        // Exact 128-bit value: hi:lo is injective (both fixed-width integers).
        const auto& d = v.as_decimal();
        return std::to_string(d.hi) + ":" + std::to_string(d.lo);
    }
    case TypeId::DATE:
        return std::to_string(v.as_date().days_since_epoch);
    case TypeId::TIME:
        return std::to_string(v.as_time().microseconds);
    case TypeId::TIMESTAMP:
        return std::to_string(v.as_timestamp().microseconds);
    case TypeId::INTERVAL: {
        const auto& iv = v.as_interval();
        return std::to_string(iv.months) + ":" + std::to_string(iv.microseconds);
    }
    case TypeId::POINT: {
        const auto& p = v.as_point();
        return double_bits_key(p.x) + ":" + double_bits_key(p.y);
    }
    case TypeId::JSON:
        return v.as_json().data;
    case TypeId::BLOB: {
        // Length-prefixed hex so two blobs of different lengths cannot collide.
        const auto& blob = v.as_blob();
        return std::to_string(blob.size()) + ":" + hex_bytes(blob.data(), blob.size());
    }
    default:
        // EMBEDDING and PATH are composite, non-scalar types that cannot serve
        // as a primary key, so this branch is unreachable for real PKs. The
        // type-tagged sentinel is kept only as a defensive fallback; it is NOT
        // injective and must never be relied on for a value that can be a PK.
        return std::string("?") + std::to_string(static_cast<int>(v.type_id()));
    }
}

} // namespace sixseven
