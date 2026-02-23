#include "giodb/planner/type_resolver.h"

#include "giodb/common/coercion.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace giodb {

namespace {

/// Map from uppercase SQL type name → TypeId.
const std::unordered_map<std::string, TypeId>& type_name_map() {
    static const std::unordered_map<std::string, TypeId> map = {
        // Integer types
        {"TINYINT", TypeId::INT8},
        {"INT8", TypeId::INT8},
        {"SMALLINT", TypeId::INT16},
        {"INT16", TypeId::INT16},
        {"INT", TypeId::INT32},
        {"INTEGER", TypeId::INT32},
        {"INT32", TypeId::INT32},
        {"BIGINT", TypeId::INT64},
        {"INT64", TypeId::INT64},
        // Unsigned integer types
        {"UINT8", TypeId::UINT8},
        {"UINT16", TypeId::UINT16},
        {"UINT32", TypeId::UINT32},
        {"UINT64", TypeId::UINT64},
        // Floating-point types
        {"FLOAT", TypeId::FLOAT32},
        {"FLOAT32", TypeId::FLOAT32},
        {"REAL", TypeId::FLOAT32},
        {"DOUBLE", TypeId::FLOAT64},
        {"FLOAT64", TypeId::FLOAT64},
        {"DOUBLE PRECISION", TypeId::FLOAT64},
        // Decimal
        {"DECIMAL", TypeId::DECIMAL},
        {"NUMERIC", TypeId::DECIMAL},
        // Boolean
        {"BOOL", TypeId::BOOL},
        {"BOOLEAN", TypeId::BOOL},
        // String types
        {"TEXT", TypeId::STRING},
        {"STRING", TypeId::STRING},
        {"VARCHAR", TypeId::STRING},
        {"CHAR", TypeId::STRING},
        {"CHARACTER VARYING", TypeId::STRING},
        // Binary
        {"BLOB", TypeId::BLOB},
        {"BYTEA", TypeId::BLOB},
        // Temporal
        {"DATE", TypeId::DATE},
        {"TIME", TypeId::TIME},
        {"TIMESTAMP", TypeId::TIMESTAMP},
        {"INTERVAL", TypeId::INTERVAL},
        // Spatial
        {"POINT", TypeId::POINT},
        // Semi-structured
        {"JSON", TypeId::JSON},
        {"JSONB", TypeId::JSON},
        // Identifiers
        {"UUID", TypeId::UUID},
        // Vector
        {"EMBEDDING", TypeId::EMBEDDING},
    };
    return map;
}

/// Uppercase a string in-place and return it.
std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

/// Set of known aggregate function names (uppercase).
const std::unordered_set<std::string>& aggregate_names() {
    static const std::unordered_set<std::string> names = {
        "COUNT",
        "SUM",
        "AVG",
        "MIN",
        "MAX",
        "STRING_AGG",
    };
    return names;
}

} // namespace

// -- resolve_type_spec --------------------------------------------------------

Result<TypeId> resolve_type_spec(const TypeSpec& spec) {
    std::string upper = to_upper(spec.name);
    auto& map = type_name_map();
    auto it = map.find(upper);
    if (it == map.end()) {
        return make_error(StatusCode::TYPE_ERROR, "unknown type: " + spec.name);
    }
    return ok(it->second);
}

// -- common_type --------------------------------------------------------------

Result<TypeId> common_type(TypeId a, TypeId b) {
    if (a == b) {
        return ok(a);
    }

    // If one can coerce to the other, pick the wider one.
    if (can_coerce(a, b)) {
        return ok(b);
    }
    if (can_coerce(b, a)) {
        return ok(a);
    }

    return make_error(StatusCode::TYPE_ERROR,
                      "no common type between " + std::string(type_name(a)) + " and " +
                          std::string(type_name(b)));
}

// -- is_aggregate_function ----------------------------------------------------

bool is_aggregate_function(const std::string& name) {
    return aggregate_names().count(to_upper(name)) > 0;
}

// -- aggregate_return_type ----------------------------------------------------

Result<TypeId> aggregate_return_type(const std::string& name, TypeId input_type) {
    std::string upper = to_upper(name);

    if (upper == "COUNT") {
        return ok(TypeId::INT64);
    }
    if (upper == "SUM") {
        if (is_integer(input_type)) {
            return ok(TypeId::INT64);
        }
        if (is_floating(input_type) || input_type == TypeId::DECIMAL) {
            return ok(TypeId::FLOAT64);
        }
        return make_error(StatusCode::TYPE_ERROR,
                          "SUM requires a numeric argument, got " +
                              std::string(type_name(input_type)));
    }
    if (upper == "AVG") {
        if (is_numeric(input_type)) {
            return ok(TypeId::FLOAT64);
        }
        return make_error(StatusCode::TYPE_ERROR,
                          "AVG requires a numeric argument, got " +
                              std::string(type_name(input_type)));
    }
    if (upper == "MIN" || upper == "MAX") {
        if (!is_comparable(input_type)) {
            return make_error(StatusCode::TYPE_ERROR,
                              upper + " requires a comparable argument, got " +
                                  std::string(type_name(input_type)));
        }
        return ok(input_type);
    }
    if (upper == "STRING_AGG") {
        return ok(TypeId::STRING);
    }

    return make_error(StatusCode::TYPE_ERROR, "unknown aggregate function: " + name);
}

// -- function_return_type -----------------------------------------------------

Result<TypeId> function_return_type(const std::string& name, const std::vector<TypeId>& arg_types) {
    std::string upper = to_upper(name);

    // String functions
    if (upper == "LOWER" || upper == "UPPER" || upper == "TRIM" || upper == "LTRIM" ||
        upper == "RTRIM" || upper == "SUBSTRING" || upper == "REPLACE" || upper == "CONCAT" ||
        upper == "LEFT" || upper == "RIGHT" || upper == "REVERSE" || upper == "REPEAT") {
        return ok(TypeId::STRING);
    }
    if (upper == "LENGTH" || upper == "CHAR_LENGTH" || upper == "POSITION" || upper == "STRPOS") {
        return ok(TypeId::INT64);
    }

    // Numeric functions
    if (upper == "ABS") {
        if (!arg_types.empty() && is_numeric(arg_types[0])) {
            return ok(arg_types[0]);
        }
        return make_error(StatusCode::TYPE_ERROR, "ABS requires a numeric argument");
    }
    if (upper == "CEIL" || upper == "FLOOR" || upper == "ROUND" || upper == "TRUNC") {
        return ok(TypeId::FLOAT64);
    }
    if (upper == "SQRT" || upper == "LOG" || upper == "LN" || upper == "EXP" || upper == "POWER" ||
        upper == "MOD") {
        return ok(TypeId::FLOAT64);
    }

    // Date/time functions
    if (upper == "NOW" || upper == "CURRENT_TIMESTAMP") {
        return ok(TypeId::TIMESTAMP);
    }
    if (upper == "CURRENT_DATE") {
        return ok(TypeId::DATE);
    }
    if (upper == "CURRENT_TIME") {
        return ok(TypeId::TIME);
    }
    if (upper == "EXTRACT" || upper == "DATE_PART") {
        return ok(TypeId::FLOAT64);
    }
    if (upper == "DATE_TRUNC") {
        return ok(TypeId::TIMESTAMP);
    }
    if (upper == "AGE") {
        return ok(TypeId::INTERVAL);
    }

    // Type conversion / misc
    if (upper == "COALESCE" || upper == "NULLIF") {
        if (!arg_types.empty()) {
            return ok(arg_types[0]);
        }
        return make_error(StatusCode::TYPE_ERROR, upper + " requires at least one argument");
    }
    if (upper == "GREATEST" || upper == "LEAST") {
        if (!arg_types.empty()) {
            return ok(arg_types[0]);
        }
        return make_error(StatusCode::TYPE_ERROR, upper + " requires at least one argument");
    }

    // JSON functions
    if (upper == "JSON_EXTRACT" || upper == "JSON_ARRAY" || upper == "JSON_OBJECT") {
        return ok(TypeId::JSON);
    }

    // UUID
    if (upper == "GEN_RANDOM_UUID" || upper == "UUID_GENERATE_V4") {
        return ok(TypeId::UUID);
    }

    // Casting helpers
    if (upper == "TO_CHAR") {
        return ok(TypeId::STRING);
    }
    if (upper == "TO_NUMBER") {
        return ok(TypeId::FLOAT64);
    }
    if (upper == "TO_DATE") {
        return ok(TypeId::DATE);
    }
    if (upper == "TO_TIMESTAMP") {
        return ok(TypeId::TIMESTAMP);
    }

    return make_error(StatusCode::TYPE_ERROR, "unknown function: " + name);
}

} // namespace giodb
