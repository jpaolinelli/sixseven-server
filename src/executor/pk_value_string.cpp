#include "sixseven/executor/pk_value_string.h"

#include "sixseven/common/types.h"

namespace sixseven {

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
        std::string s;
        s.reserve(32);
        static const char hex[] = "0123456789abcdef";
        for (auto b : uuid) {
            s += hex[(b >> 4) & 0xF];
            s += hex[b & 0xF];
        }
        return s;
    }
    default:
        // Fallback: use type prefix + raw representation.
        return std::string("?") + std::to_string(static_cast<int>(v.type_id()));
    }
}

} // namespace sixseven
