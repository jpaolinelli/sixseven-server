#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sixseven {

/// Enumerates all 23 data types supported by SixSevenDB.
enum class TypeId : uint8_t {
    // Integer types
    INT8,
    INT16,
    INT32,
    INT64,
    UINT8,
    UINT16,
    UINT32,
    UINT64,

    // Floating-point types
    FLOAT32,
    FLOAT64,

    // Fixed-precision decimal
    DECIMAL,

    // Boolean
    BOOL,

    // Variable-length types
    STRING,
    BLOB,

    // Temporal types
    DATE,
    TIME,
    TIMESTAMP,
    INTERVAL,

    // Spatial
    POINT,

    // Semi-structured
    JSON,

    // Identifiers
    UUID,

    // Vector
    EMBEDDING,

    // Graph path
    PATH,
};

/// Return the human-readable name of a TypeId.
constexpr std::string_view type_name(TypeId id) {
    switch (id) {
    case TypeId::INT8:
        return "INT8";
    case TypeId::INT16:
        return "INT16";
    case TypeId::INT32:
        return "INT32";
    case TypeId::INT64:
        return "INT64";
    case TypeId::UINT8:
        return "UINT8";
    case TypeId::UINT16:
        return "UINT16";
    case TypeId::UINT32:
        return "UINT32";
    case TypeId::UINT64:
        return "UINT64";
    case TypeId::FLOAT32:
        return "FLOAT32";
    case TypeId::FLOAT64:
        return "FLOAT64";
    case TypeId::DECIMAL:
        return "DECIMAL";
    case TypeId::BOOL:
        return "BOOL";
    case TypeId::STRING:
        return "STRING";
    case TypeId::BLOB:
        return "BLOB";
    case TypeId::DATE:
        return "DATE";
    case TypeId::TIME:
        return "TIME";
    case TypeId::TIMESTAMP:
        return "TIMESTAMP";
    case TypeId::INTERVAL:
        return "INTERVAL";
    case TypeId::POINT:
        return "POINT";
    case TypeId::JSON:
        return "JSON";
    case TypeId::UUID:
        return "UUID";
    case TypeId::EMBEDDING:
        return "EMBEDDING";
    case TypeId::PATH:
        return "PATH";
    }
    return "UNKNOWN";
}

/// Parse a type name string (case-insensitive) to a TypeId.
/// Returns nullopt if the string does not match any known type.
inline std::optional<TypeId> parse_type_id(std::string_view name) {
    std::string upper;
    upper.reserve(name.size());
    for (char c : name) {
        upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (upper == "INT8") {
        return TypeId::INT8;
    }
    if (upper == "INT16") {
        return TypeId::INT16;
    }
    if (upper == "INT32") {
        return TypeId::INT32;
    }
    if (upper == "INT64") {
        return TypeId::INT64;
    }
    if (upper == "UINT8") {
        return TypeId::UINT8;
    }
    if (upper == "UINT16") {
        return TypeId::UINT16;
    }
    if (upper == "UINT32") {
        return TypeId::UINT32;
    }
    if (upper == "UINT64") {
        return TypeId::UINT64;
    }
    if (upper == "FLOAT32") {
        return TypeId::FLOAT32;
    }
    if (upper == "FLOAT64") {
        return TypeId::FLOAT64;
    }
    if (upper == "DECIMAL") {
        return TypeId::DECIMAL;
    }
    if (upper == "BOOL") {
        return TypeId::BOOL;
    }
    if (upper == "STRING") {
        return TypeId::STRING;
    }
    if (upper == "BLOB") {
        return TypeId::BLOB;
    }
    if (upper == "DATE") {
        return TypeId::DATE;
    }
    if (upper == "TIME") {
        return TypeId::TIME;
    }
    if (upper == "TIMESTAMP") {
        return TypeId::TIMESTAMP;
    }
    if (upper == "INTERVAL") {
        return TypeId::INTERVAL;
    }
    if (upper == "POINT") {
        return TypeId::POINT;
    }
    if (upper == "JSON") {
        return TypeId::JSON;
    }
    if (upper == "UUID") {
        return TypeId::UUID;
    }
    if (upper == "EMBEDDING") {
        return TypeId::EMBEDDING;
    }
    if (upper == "PATH") {
        return TypeId::PATH;
    }
    return std::nullopt;
}

/// Return the fixed byte size for a type, or nullopt for variable-length types.
/// Sizes: INT8/UINT8/BOOL=1, INT16/UINT16=2, INT32/UINT32/FLOAT32/DATE=4,
///        INT64/UINT64/FLOAT64/TIME/TIMESTAMP=8, DECIMAL/INTERVAL/POINT/UUID=16.
constexpr std::optional<size_t> fixed_size(TypeId id) {
    switch (id) {
    case TypeId::INT8:
    case TypeId::UINT8:
    case TypeId::BOOL:
        return 1;
    case TypeId::INT16:
    case TypeId::UINT16:
        return 2;
    case TypeId::INT32:
    case TypeId::UINT32:
    case TypeId::FLOAT32:
    case TypeId::DATE:
        return 4;
    case TypeId::INT64:
    case TypeId::UINT64:
    case TypeId::FLOAT64:
    case TypeId::TIME:
    case TypeId::TIMESTAMP:
        return 8;
    case TypeId::DECIMAL:
    case TypeId::INTERVAL:
    case TypeId::POINT:
    case TypeId::UUID:
        return 16;
    case TypeId::STRING:
    case TypeId::BLOB:
    case TypeId::JSON:
    case TypeId::EMBEDDING:
    case TypeId::PATH:
        return std::nullopt;
    }
    return std::nullopt;
}

/// Return true if the type is any numeric type (integer, floating, or decimal).
constexpr bool is_numeric(TypeId id) {
    switch (id) {
    case TypeId::INT8:
    case TypeId::INT16:
    case TypeId::INT32:
    case TypeId::INT64:
    case TypeId::UINT8:
    case TypeId::UINT16:
    case TypeId::UINT32:
    case TypeId::UINT64:
    case TypeId::FLOAT32:
    case TypeId::FLOAT64:
    case TypeId::DECIMAL:
        return true;
    default:
        return false;
    }
}

/// Return true if the type is a signed or unsigned integer.
constexpr bool is_integer(TypeId id) {
    switch (id) {
    case TypeId::INT8:
    case TypeId::INT16:
    case TypeId::INT32:
    case TypeId::INT64:
    case TypeId::UINT8:
    case TypeId::UINT16:
    case TypeId::UINT32:
    case TypeId::UINT64:
        return true;
    default:
        return false;
    }
}

/// Return true if the type is a floating-point type (FLOAT32 or FLOAT64).
constexpr bool is_floating(TypeId id) {
    return id == TypeId::FLOAT32 || id == TypeId::FLOAT64;
}

/// Return true if the type supports ordering comparisons.
/// BLOB and EMBEDDING are not directly comparable.
constexpr bool is_comparable(TypeId id) {
    switch (id) {
    case TypeId::BLOB:
    case TypeId::EMBEDDING:
    case TypeId::PATH:
        return false;
    default:
        return true;
    }
}

/// Return true if the type has variable-length storage.
constexpr bool is_variable_length(TypeId id) {
    return !fixed_size(id).has_value();
}

} // namespace sixseven
