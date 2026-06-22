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
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sixseven {

/// Shortest-path MATCH executor operator.
///
/// Integrates shortest-path finding with the MATCH pattern framework.
/// Supports three path selector modes:
///   - ANY_SHORTEST:  return one shortest path
///   - ALL_SHORTEST:  return all paths tied for shortest hop count
///   - SHORTEST_K:    return K shortest distinct paths
///
/// Materialises all results during open(), then emits them via next().
class MatchShortestPathOperator : public Iterator {
public:
    static constexpr size_t DEFAULT_MAX_VISITED = 100'000;

    MatchShortestPathOperator(GraphEngine& graph_engine,
                              const Catalog& catalog,
                              StorageManager& storage,
                              database_id_t database_id,
                              MatchConfig config,
                              OutputSchema schema,
                              const Expr* where_expr,
                              const BoundStatement& bound,
                              PathSelector path_selector,
                              std::string path_variable,
                              int32_t shortest_k,
                              size_t max_visited = DEFAULT_MAX_VISITED,
                              const Expr* weight_expr = nullptr);

    const OutputSchema& output_schema() const override;

    [[nodiscard]] std::string plan_node_name() const override;
    [[nodiscard]] std::string plan_node_detail() const override;

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;

private:
    /// Run shortest-path BFS for all source-target pairs.
    Result<void> execute_shortest_paths();

    /// BFS-based shortest path finding from a single source to all reachable
    /// targets. Returns paths grouped by target node.
    Result<std::vector<Path>> find_shortest_paths(const Value& src_pk,
                                                  const Value& tgt_pk,
                                                  table_id_t src_table_id,
                                                  table_id_t tgt_table_id,
                                                  const std::string& edge_type,
                                                  TraverseDirection direction,
                                                  int32_t min_hops,
                                                  int32_t max_depth);

    /// Dijkstra-based weighted shortest path finding.
    Result<std::vector<Path>> find_weighted_shortest_paths(const Value& src_pk,
                                                           const Value& tgt_pk,
                                                           table_id_t src_table_id,
                                                           table_id_t tgt_table_id,
                                                           const std::string& edge_type,
                                                           TraverseDirection direction,
                                                           int32_t min_hops,
                                                           int32_t max_depth);

    /// Get neighbors with edge weight extracted from the weight expression.
    Result<std::vector<std::tuple<Value, int64_t, double>>> get_neighbors_with_weight(
        const std::string& edge_type, const Value& pk, TraverseDirection direction);

    /// Fetch full row data for a node PK from a table.
    Result<std::vector<Value>> fetch_node_data(const std::string& table_name,
                                               const Value& pk) const;

    /// Get all PKs from a table.
    Result<std::vector<Value>> get_all_pks(const std::string& table_name) const;

    /// Get neighbors of a node.
    Result<std::vector<std::pair<Value, int64_t>>>
    get_neighbors(const std::string& edge_type, const Value& pk, TraverseDirection direction) const;

    /// Convert a binding + path to an output tuple.
    Result<Tuple> binding_to_tuple(const std::unordered_map<std::string, Value>& binding,
                                   const Path* path_value) const;

    GraphEngine& graph_engine_;
    const Catalog& catalog_;
    StorageManager& storage_;
    database_id_t database_id_;
    MatchConfig config_;
    OutputSchema schema_;
    const Expr* where_expr_;
    const BoundStatement& bound_;
    PathSelector path_selector_;
    std::string path_variable_;
    int32_t shortest_k_;
    const Expr* weight_expr_;
    size_t max_visited_;

    std::vector<Tuple> results_;
    size_t cursor_ = 0;
};

} // namespace sixseven
