#include "sixseven/planner/type_resolver.h"

#include "sixseven/common/coercion.h"
#include "sixseven/common/string_util.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace sixseven {

namespace {

/// Map from uppercase SQL type name → TypeId.
///
/// Built by layering SQL-only aliases on top of the canonical name set
/// accepted by `parse_type_id` (see include/sixseven/common/types.h), so the
/// canonical names recognized here always stay in sync with that single
/// source of truth -- adding a new TypeId only requires updating types.h
/// (type_name / parse_type_id / fixed_size switches) and this alias list, not
/// a second independent canonical-name table.
///
/// `parse_type_id` itself is also used directly by the graph edge-property
/// parser (src/graph/graph_engine.cpp), which must keep accepting ONLY
/// canonical type names. The SQL-only aliases below (INT, SMALLINT, BOOLEAN,
/// VARCHAR, etc.) are intentionally NOT added to parse_type_id -- they exist
/// solely for this SQL DDL-facing map.
const std::unordered_map<std::string, TypeId>& type_name_map() {
    static const std::unordered_map<std::string, TypeId> map = [] {
        std::unordered_map<std::string, TypeId> m;

        // Seed with every canonical name recognized by parse_type_id, so a
        // new TypeId added there is automatically resolvable here too.
        for (uint8_t raw = 0; raw <= static_cast<uint8_t>(TypeId::PATH); ++raw) {
            auto id = static_cast<TypeId>(raw);
            m.emplace(std::string(type_name(id)), id);
        }

        // SQL-only aliases layered on top. Accepted by the SQL DDL path
        // (this map) but NOT by parse_type_id (graph edge-property path).
        m.emplace("TINYINT", TypeId::INT8);
        m.emplace("SMALLINT", TypeId::INT16);
        m.emplace("INT", TypeId::INT32);
        m.emplace("INTEGER", TypeId::INT32);
        m.emplace("BIGINT", TypeId::INT64);
        m.emplace("FLOAT", TypeId::FLOAT32);
        m.emplace("REAL", TypeId::FLOAT32);
        m.emplace("DOUBLE", TypeId::FLOAT64);
        m.emplace("DOUBLE PRECISION", TypeId::FLOAT64);
        m.emplace("NUMERIC", TypeId::DECIMAL);
        m.emplace("BOOLEAN", TypeId::BOOL);
        m.emplace("TEXT", TypeId::STRING);
        m.emplace("VARCHAR", TypeId::STRING);
        m.emplace("CHAR", TypeId::STRING);
        m.emplace("CHARACTER VARYING", TypeId::STRING);
        m.emplace("BYTEA", TypeId::BLOB);
        m.emplace("JSONB", TypeId::JSON);

        return m;
    }();
    return map;
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
    if (upper == "GEN_UUID" || upper == "GEN_RANDOM_UUID" || upper == "UUID_GENERATE_V4") {
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

    // Replication system functions
    if (upper == "PG_CURRENT_WAL_LSN") {
        return ok(TypeId::INT64);
    }
    if (upper == "PG_IS_IN_RECOVERY") {
        return ok(TypeId::BOOL);
    }
    if (upper == "PG_LAST_WAL_REPLAY_LSN") {
        return ok(TypeId::INT64);
    }

    return make_error(StatusCode::TYPE_ERROR, "unknown function: " + name);
}

} // namespace sixseven
