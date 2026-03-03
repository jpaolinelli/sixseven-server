#pragma once

#include "giodb/common/result.h"
#include "giodb/executor/iterator.h"
#include "giodb/executor/traversal.h"
#include "giodb/executor/tuple.h"
#include "giodb/graph/graph_engine.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace giodb {

/// Edge-centric BFS traversal operator.
///
/// Returns one row per edge between all discovered nodes, capturing
/// cross-edges, bidirectional edges, and self-loops that a node-centric
/// BFS tree would miss. Designed for graph UI rendering.
///
/// Two-phase materialization in do_open():
///
///   Phase 1 (BFS): Traverse the graph, building a visited set with depths.
///   Phase 2 (Edge scan): For each discovered node, query all outgoing edges
///     and keep those whose target is also discovered.
///
/// Output schema: [__from, __to, __depth, edge_properties...]
class EdgeTraversalOperator : public Iterator {
public:
    /// @param graph_engine  Graph engine for edge lookups.
    /// @param config        BFS traversal parameters.
    /// @param schema        Edge output schema (__from, __to, __depth, props...).
    /// @param where_expr    Optional WHERE predicate (post-filter).
    /// @param bound         BoundStatement for expression evaluation.
    /// @param heterogeneous True when edge connects different tables.
    EdgeTraversalOperator(GraphEngine& graph_engine,
                          TraversalConfig config,
                          OutputSchema schema,
                          const Expr* where_expr,
                          const BoundStatement& bound,
                          bool heterogeneous);

    [[nodiscard]] const OutputSchema& output_schema() const override;

    // -- Plan inspection (for EXPLAIN) ----------------------------------------
    [[nodiscard]] std::string plan_node_name() const override;
    [[nodiscard]] std::string plan_node_detail() const override;

protected:
    [[nodiscard]] Result<void> do_open() override;
    [[nodiscard]] Result<std::optional<Tuple>> do_next() override;
    void do_close() override;

private:
    /// Run BFS and populate bfs_results_.
    [[nodiscard]] Result<void> run_bfs();

    /// Get neighbor PKs from edges in the configured direction.
    [[nodiscard]] Result<std::vector<std::pair<Value, EdgeRow>>>
    get_neighbors(const Value& node_pk) const;

    /// Scan edges between discovered nodes and build edge_results_.
    [[nodiscard]] Result<void> collect_edges();

    GraphEngine& graph_engine_;
    TraversalConfig config_;
    OutputSchema schema_;
    const Expr* where_expr_;
    const BoundStatement& bound_;

    bool heterogeneous_;

    std::vector<TraversalResult> bfs_results_;
    std::vector<Tuple> edge_results_;
    size_t cursor_ = 0;
};

} // namespace giodb
