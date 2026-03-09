#pragma once

#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sixseven {

class GraphEngine;

// ---------------------------------------------------------------------------
// Algorithm parameter definition
// ---------------------------------------------------------------------------

/// Defines a single named parameter for a graph algorithm.
struct AlgorithmParamDef {
    std::string name;
    TypeId type_id = TypeId::STRING;
    bool required = false;
    std::optional<Value> default_value;
};

// ---------------------------------------------------------------------------
// Algorithm output column definition
// ---------------------------------------------------------------------------

/// Defines a single column in an algorithm's output schema.
struct AlgorithmOutputColumn {
    std::string name;
    TypeId type_id = TypeId::INT64;
    bool nullable = false;
};

// ---------------------------------------------------------------------------
// Algorithm result row
// ---------------------------------------------------------------------------

/// A single row produced by an algorithm execution.
struct AlgorithmRow {
    std::vector<Value> values;
};

// ---------------------------------------------------------------------------
// Algorithm definition
// ---------------------------------------------------------------------------

/// Complete definition of a registered graph algorithm.
///
/// Each algorithm has:
///   - A unique name (case-insensitive lookup).
///   - A first positional parameter: the edge type name (always required).
///   - Named parameters for configuration (e.g., damping, iterations).
///   - An output schema: always starts with node_id, followed by
///     algorithm-specific columns.
struct AlgorithmDef {
    std::string name;
    std::vector<AlgorithmParamDef> params;
    std::vector<AlgorithmOutputColumn> output_columns;
};

// ---------------------------------------------------------------------------
// Algorithm execution context
// ---------------------------------------------------------------------------

/// Context passed to an algorithm's execute function.
struct AlgorithmContext {
    GraphEngine& graph_engine;
    std::string edge_type;
    std::unordered_map<std::string, Value> named_args;
};

// ---------------------------------------------------------------------------
// Algorithm execution function type
// ---------------------------------------------------------------------------

/// Signature for an algorithm's execute function.
/// Takes a context and returns a vector of result rows.
using AlgorithmExecuteFn =
    std::function<Result<std::vector<AlgorithmRow>>(const AlgorithmContext&)>;

// ---------------------------------------------------------------------------
// Algorithm registry
// ---------------------------------------------------------------------------

/// Registry for graph algorithms that can be invoked as table-valued functions.
///
/// Algorithms are registered at startup and resolved by name during query
/// planning.  Each algorithm declares its parameter list and output schema
/// so the planner can validate calls and build the correct output columns.
///
/// Usage:
/// ```
///   AlgorithmRegistry registry;
///   registry.register_algorithm(def, execute_fn);
///   auto* algo = registry.find("pagerank");
///   if (algo) { /* use algo->def and algo->execute */ }
/// ```
///
/// Thread safety: All public methods are protected by a mutex.
class AlgorithmRegistry {
public:
    /// A registered algorithm entry: definition + execute function.
    struct Entry {
        AlgorithmDef def;
        AlgorithmExecuteFn execute;
    };

    AlgorithmRegistry() = default;

    /// Register an algorithm. Returns an error if an algorithm with the
    /// same name (case-insensitive) is already registered.
    [[nodiscard]] Result<void> register_algorithm(AlgorithmDef def, AlgorithmExecuteFn execute_fn);

    /// Look up an algorithm by name (case-insensitive).
    /// Returns nullptr if not found.
    [[nodiscard]] const Entry* find(const std::string& name) const;

    /// Check whether an algorithm with the given name is registered.
    [[nodiscard]] bool has(const std::string& name) const;

    /// Return a list of all registered algorithm names.
    [[nodiscard]] std::vector<std::string> list() const;

    /// Validate named arguments against an algorithm's parameter definitions.
    /// Returns an error if a required parameter is missing or an unknown
    /// parameter is provided.
    [[nodiscard]] static Result<std::unordered_map<std::string, Value>>
    resolve_params(const AlgorithmDef& def, const std::unordered_map<std::string, Value>& provided);

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> algorithms_;

    /// Normalise a name to uppercase for case-insensitive lookup.
    [[nodiscard]] static std::string to_upper(const std::string& s);
};

} // namespace sixseven
