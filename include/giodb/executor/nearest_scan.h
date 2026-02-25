#pragma once

#include "giodb/common/result.h"
#include "giodb/executor/iterator.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"
#include "giodb/table/table_heap.h"
#include "giodb/table/tuple.h"
#include "giodb/vector/distance.h"
#include "giodb/vector/embedding_worker.h"
#include "giodb/vector/hnsw_index.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

namespace giodb {

/// Configuration for a NEAREST scan operation.
struct NearestScanConfig {
    /// Number of nearest neighbors to return.
    uint32_t k = 10;

    /// Query vector for similarity search.
    std::vector<float> query_vector;

    /// Distance metric (L2, COSINE, or DOT_PRODUCT).
    DistanceMetric metric = DistanceMetric::COSINE;

    /// Index of the EMBEDDING column in the table schema.
    int32_t embedding_column_index = -1;

    /// Optional set of allowed node IDs (for graph-scoped search).
    /// When non-empty, only rows whose ordinal is in this set are eligible.
    std::unordered_set<uint32_t> allowed_node_ids;
};

/// NEAREST query executor operator.
///
/// Performs k-nearest-neighbor search on an EMBEDDING column using either
/// an HNSW index (when available) or a brute-force sequential scan.
///
/// Supports:
///   - Literal vector queries: `NEAREST 5 FROM t.col TO [1.0, 2.0, ...]`
///   - Text auto-embedding: `NEAREST 5 FROM t.col TO 'search text'`
///   - WHERE post-filtering: `... WHERE category = 'news'`
///   - Graph-scoped search: `... WITHIN TRAVERSE edge FROM t(pk) ...`
///   - Distance metric selection: `... USING L2|COSINE|DOT`
///
/// The operator materializes all search results during open(), then emits
/// them one at a time from next(), sorted by distance ASC.
class NearestScanOperator : public Iterator {
public:
    /// @param heap            The heap file to scan for row data.
    /// @param storage_schema  Byte-level schema for TupleSerializer::deserialize.
    /// @param config          NEAREST scan parameters (k, query, metric, etc.).
    /// @param schema          Output schema for result tuples.
    /// @param where_expr      Optional WHERE predicate (applied post-search).
    /// @param bound           BoundStatement for expression evaluation.
    /// @param hnsw_index      Optional HNSW index for accelerated search.
    NearestScanOperator(TableHeap& heap,
                        const Schema& storage_schema,
                        NearestScanConfig config,
                        OutputSchema schema,
                        const Expr* where_expr,
                        const BoundStatement& bound,
                        HnswIndex* hnsw_index = nullptr);

    const OutputSchema& output_schema() const override;

    // -- Plan inspection (for EXPLAIN) ----------------------------------------
    [[nodiscard]] std::string plan_node_name() const override;
    [[nodiscard]] std::string plan_node_detail() const override;

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;

private:
    /// Brute-force scan: compute distances for all rows, sort, take top-k.
    Result<void> execute_brute_force();

    /// HNSW index search: use the index for approximate nearest neighbors.
    Result<void> execute_hnsw_search();

    /// Build the WHERE filter output schema (table columns without _distance).
    /// Called once during open() when where_expr_ is set.
    OutputSchema build_where_filter_schema() const;

    /// Apply WHERE post-filter to a candidate and, if it passes, build the
    /// result tuple (table columns + _distance) and append it to results_.
    /// Returns the number of results emitted (0 or 1).
    Result<size_t> filter_and_emit(Tuple& candidate_tuple, float distance);

    TableHeap& heap_;
    const Schema& storage_schema_;
    NearestScanConfig config_;
    OutputSchema schema_;
    const Expr* where_expr_;
    const BoundStatement& bound_;
    HnswIndex* hnsw_index_;

    /// Output schema for WHERE predicate evaluation (excludes _distance).
    /// Built once in open() to avoid per-row allocation.
    OutputSchema where_filter_schema_;

    std::vector<Tuple> results_;
    size_t cursor_ = 0;
};

} // namespace giodb
