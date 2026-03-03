#pragma once

#include "giodb/common/result.h"
#include "giodb/executor/iterator.h"
#include "giodb/executor/traversal.h"
#include "giodb/executor/tuple.h"
#include "giodb/graph/graph_engine.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"
#include "giodb/table/table_heap.h"
#include "giodb/table/tuple.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace giodb {

/// Enriched BFS traversal operator.
///
/// Combines graph BFS traversal with batch row data enrichment from the
/// target table. Two-phase materialization in do_open():
///
///   Phase 1 (BFS): Traverse the graph, collecting TraversalResult entries.
///   Phase 2 (Enrichment): Single scan of target table, build PK -> row
///     hash map, assemble enriched tuples: [table_columns..., __node,
///     __depth, __source].
///
/// do_next() yields enriched tuples one at a time with optional WHERE
/// post-filter applied by the executor pipeline above.
class EnrichedTraversalOperator : public Iterator {
public:
    /// @param graph_engine          Graph engine for edge lookups.
    /// @param config                BFS traversal parameters.
    /// @param schema                Enriched output schema.
    /// @param where_expr            Optional WHERE predicate (post-filter).
    /// @param bound                 BoundStatement for expression evaluation.
    /// @param target_heap           Target table heap for row data.
    /// @param target_storage_schema Byte-level schema for deserialization.
    /// @param target_pk_col_idx     PK column index in target table.
    /// @param target_column_count   Number of target table columns in schema.
    /// @param heterogeneous         True when edge connects different tables.
    EnrichedTraversalOperator(GraphEngine& graph_engine,
                              TraversalConfig config,
                              OutputSchema schema,
                              const Expr* where_expr,
                              const BoundStatement& bound,
                              TableHeap& target_heap,
                              const Schema& target_storage_schema,
                              size_t target_pk_col_idx,
                              size_t target_column_count,
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

    /// Scan target table and build PK -> row_data hash map, then assemble
    /// enriched tuples from BFS results.
    [[nodiscard]] Result<void> enrich_results();

    GraphEngine& graph_engine_;
    TraversalConfig config_;
    OutputSchema schema_;
    const Expr* where_expr_;
    const BoundStatement& bound_;

    TableHeap& target_heap_;
    const Schema& target_storage_schema_;
    size_t target_pk_col_idx_;
    size_t target_column_count_;

    bool heterogeneous_;

    std::vector<TraversalResult> bfs_results_;
    std::vector<Tuple> enriched_results_;
    size_t cursor_ = 0;
};

} // namespace giodb
