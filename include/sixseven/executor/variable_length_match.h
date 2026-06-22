#pragma once

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/graph_traversal_core.h"
#include "sixseven/executor/iterator.h"
#include "sixseven/executor/pattern_match.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/planner/binder.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace sixseven {

/// Variable-length MATCH pattern executor operator.
///
/// Implements BFS traversal with depth bounds [min_hops, max_hops] for
/// quantified edge patterns such as:
///   (a:persons)-[r:knows]->{1,6}(b:persons)
///
/// Features:
///   - BFS respects min/max hop bounds
///   - Path accumulation tracks node+edge sequences
///   - Cycle prevention via per-path visited set
///   - Memory bounding via max_visited limit
///   - Volcano iterator model: open/next/close
class VariableLengthMatchOperator : public Iterator {
public:
    /// Default maximum number of visited nodes before aborting (memory bound).
    static constexpr size_t DEFAULT_MAX_VISITED = 100'000;

    VariableLengthMatchOperator(GraphEngine& graph_engine,
                                const Catalog& catalog,
                                StorageManager& storage,
                                database_id_t database_id,
                                MatchConfig config,
                                OutputSchema schema,
                                const Expr* where_expr,
                                const BoundStatement& bound,
                                size_t max_visited = DEFAULT_MAX_VISITED);

    const OutputSchema& output_schema() const override;

    // -- Plan inspection (for EXPLAIN) ----------------------------------------
    [[nodiscard]] std::string plan_node_name() const override;
    [[nodiscard]] std::string plan_node_detail() const override;

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;

private:
    /// Perform BFS traversal for the variable-length edge.
    /// Finds the index of the quantified edge in config_.edges and runs BFS
    /// from each source node binding.
    Result<void> execute_variable_length();

    /// Fetch full row data for a node PK from a table.
    Result<std::vector<Value>> fetch_node_data(const std::string& table_name,
                                               const Value& pk) const;

    /// Get all source PKs from a table.
    Result<std::vector<Value>> get_all_pks(const std::string& table_name) const;

    /// Get neighbors of a node via an edge type, respecting direction.
    Result<std::vector<std::pair<Value, int64_t>>>
    get_neighbors(const std::string& edge_type, const Value& pk, TraverseDirection direction) const;

    /// Convert a binding (variable -> PK map) + path to an output tuple.
    Result<Tuple> binding_to_tuple(const std::unordered_map<std::string, Value>& binding,
                                   const Path* path_value) const;

    /// Evaluate a node's inline predicate against its fetched data.
    /// Returns true if predicate passes or no predicate is set.
    Result<bool> evaluate_node_filter(const MatchNodeDef& node_def, const Value& pk) const;

    /// Evaluate an edge's inline predicate against edge properties.
    /// Returns true if predicate passes or no predicate is set.
    Result<bool> evaluate_edge_filter(const MatchEdgeDef& edge_def,
                                      const std::vector<Value>& properties) const;

    GraphEngine& graph_engine_;
    const Catalog& catalog_;
    StorageManager& storage_;
    database_id_t database_id_;
    MatchConfig config_;
    OutputSchema schema_;
    const Expr* where_expr_;
    const BoundStatement& bound_;
    size_t max_visited_;

    std::vector<Tuple> results_;
    size_t cursor_ = 0;
};

} // namespace sixseven
