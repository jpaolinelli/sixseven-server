#include "sixseven/graph/algorithm_registry.h"

#include <algorithm>
#include <cctype>

namespace sixseven {

// ---------------------------------------------------------------------------
// AlgorithmRegistry
// ---------------------------------------------------------------------------

Result<void> AlgorithmRegistry::register_algorithm(AlgorithmDef def,
                                                   AlgorithmExecuteFn execute_fn) {
    std::lock_guard lock(mu_);

    // Enforce the output schema convention: every algorithm must expose node_id
    // (INT64, non-nullable) as its first output column.  Algorithms that violate
    // this cannot be correctly consumed by NearestScanOperator or the graph
    // result-projection code, which assumes column 0 is always the node identity.
    if (def.output_columns.empty() || def.output_columns[0].name != "node_id" ||
        def.output_columns[0].type_id != TypeId::INT64) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "algorithm '" + def.name +
                              "': output schema must have node_id INT64 as the first column");
    }

    auto key = to_upper(def.name);
    if (algorithms_.count(key)) {
        return make_error(StatusCode::ALREADY_EXISTS,
                          "algorithm '" + def.name + "' is already registered");
    }

    algorithms_.emplace(std::move(key), Entry{std::move(def), std::move(execute_fn)});
    return ok();
}

const AlgorithmRegistry::Entry* AlgorithmRegistry::find(const std::string& name) const {
    std::lock_guard lock(mu_);

    auto it = algorithms_.find(to_upper(name));
    if (it == algorithms_.end()) {
        return nullptr;
    }
    return &it->second;
}

bool AlgorithmRegistry::has(const std::string& name) const {
    std::lock_guard lock(mu_);
    return algorithms_.count(to_upper(name)) > 0;
}

std::vector<std::string> AlgorithmRegistry::list() const {
    std::lock_guard lock(mu_);

    std::vector<std::string> names;
    names.reserve(algorithms_.size());
    for (const auto& [key, entry] : algorithms_) {
        names.push_back(entry.def.name);
    }
    return names;
}

Result<std::unordered_map<std::string, Value>>
AlgorithmRegistry::resolve_params(const AlgorithmDef& def,
                                  const std::unordered_map<std::string, Value>& provided) {
    std::unordered_map<std::string, Value> resolved;

    // Build a set of known parameter names (uppercase) for validation.
    std::unordered_map<std::string, const AlgorithmParamDef*> known;
    for (const auto& param : def.params) {
        known.emplace(to_upper(param.name), &param);
    }

    // Check for unknown parameters.
    for (const auto& [name, val] : provided) {
        auto upper_name = to_upper(name);
        if (!known.count(upper_name)) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "unknown parameter '" + name + "' for algorithm '" + def.name + "'");
        }
    }

    // Resolve each defined parameter: use provided value, default, or error.
    for (const auto& param : def.params) {
        auto upper_name = to_upper(param.name);

        // Check if provided (case-insensitive lookup).
        bool found = false;
        for (const auto& [name, val] : provided) {
            if (to_upper(name) == upper_name) {
                resolved.emplace(param.name, val);
                found = true;
                break;
            }
        }

        if (!found) {
            if (param.default_value.has_value()) {
                resolved.emplace(param.name, *param.default_value);
            } else if (param.required) {
                return make_error(StatusCode::INVALID_ARGUMENT,
                                  "required parameter '" + param.name +
                                      "' not provided for algorithm '" + def.name + "'");
            }
        }
    }

    return ok(std::move(resolved));
}

std::string AlgorithmRegistry::to_upper(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return result;
}

} // namespace sixseven
