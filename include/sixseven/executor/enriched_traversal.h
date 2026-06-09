#pragma once

#include "sixseven/common/result.h"
#include "sixseven/executor/iterator.h"
#include "sixseven/executor/traversal.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sixseven {

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
    /// Parent-pointer entry used to reconstruct paths when trace is enabled.
    struct ParentInfo {
        Value parent_pk;          ///< PK of the node this one was first reached from.
        int64_t edge_row_id = -1; ///< Row ID of the edge used to reach this node.
    };

    /// Run BFS and populate bfs_results_.
    [[nodiscard]] Result<void> run_bfs();

    /// Get neighbor PKs from edges in the configured direction.
    [[nodiscard]] Result<std::vector<std::pair<Value, EdgeRow>>>
    get_neighbors(const Value& node_pk) const;

    /// Scan target table and build PK -> row_data hash map, then assemble
    /// enriched tuples from BFS results.
    [[nodiscard]] Result<void> enrich_results();

    /// Reconstruct the full path from the start node to @p target by walking the
    /// parent map backward and reversing. Only valid when trace is enabled.
    ///
    /// @p target_depth is the BFS depth of @p target; the walk stops after that
    /// many hops. The depth guard is required for heterogeneous edges: the
    /// parent map is keyed by PK only, so a node in another table that shares
    /// the start node's PK would otherwise look like its own parent and loop
    /// forever (GDB-694). The walk is additionally bounded by the parent map
    /// size and returns INTERNAL_ERROR if that bound is exceeded.
    [[nodiscard]] Result<Path> reconstruct_path(const Value& target, int32_t target_depth) const;

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
    /// Maps a node PK to the (parent, edge) it was first reached from.
    /// Populated only when config_.trace is true.
    std::unordered_map<Value, ParentInfo, ValueHash, ValueEqual> parent_map_;
    size_t cursor_ = 0;
};

} // namespace sixseven
