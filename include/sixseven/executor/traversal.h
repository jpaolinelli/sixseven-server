#pragma once

#include "sixseven/common/result.h"
#include "sixseven/executor/iterator.h"
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

/// Configuration for a BFS traversal operation.
struct TraversalConfig {
    database_id_t database_id = default_database_id;
    std::string edge_type;
    Value start_key;
    TraverseDirection direction = TraverseDirection::OUT;
    int32_t max_depth = 100;
    size_t max_visited = 100000;
    bool fetch = false;
    bool collect_edges = false;
};

/// A single result row from BFS traversal (before FETCH enrichment).
struct TraversalResult {
    Value node_pk;
    int32_t depth = 0;
    Value source_pk;
    std::vector<Value> edge_properties; ///< Properties from the edge used to reach this node.
};

/// BFS traversal executor operator.
///
/// Performs breadth-first search through the graph starting from a given
/// node, using the adjacency indexes in the EdgeTable. Supports:
///   - Direction control (IN/OUT/BOTH)
///   - Depth limiting (MAX_DEPTH)
///   - Memory-bounded visited set (max_visited)
///   - FETCH enrichment (join with source table row data)
///   - WHERE post-filtering on traversal results
///   - Edge collection for visualization
///
/// The operator materialises all BFS results during open(), then emits
/// them one at a time from next().
class TraversalOperator : public Iterator {
public:
    /// @param graph_engine  The graph engine for edge lookups.
    /// @param config        BFS traversal parameters.
    /// @param schema        Output schema for result tuples.
    /// @param where_expr    Optional WHERE predicate (applied post-traversal).
    /// @param bound         BoundStatement for expression evaluation.
    TraversalOperator(GraphEngine& graph_engine,
                      TraversalConfig config,
                      OutputSchema schema,
                      const Expr* where_expr,
                      const BoundStatement& bound);

    const OutputSchema& output_schema() const override;

    /// Access collected edges (for META EDGES protocol).
    [[nodiscard]] const std::vector<EdgeRow>& collected_edges() const { return edges_; }

    // -- Plan inspection (for EXPLAIN) ----------------------------------------
    [[nodiscard]] std::string plan_node_name() const override;
    [[nodiscard]] std::string plan_node_detail() const override;

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;

private:
    /// Run the BFS traversal and populate results_.
    Result<void> run_bfs();

    /// Get neighbor PKs from edges in the configured direction.
    Result<std::vector<std::pair<Value, EdgeRow>>> get_neighbors(const Value& node_pk) const;

    GraphEngine& graph_engine_;
    TraversalConfig config_;
    OutputSchema schema_;
    const Expr* where_expr_;
    const BoundStatement& bound_;

    std::vector<Tuple> results_;
    std::vector<EdgeRow> edges_;
    size_t cursor_ = 0;
};

} // namespace sixseven
