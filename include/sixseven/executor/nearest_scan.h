#pragma once

#include "sixseven/common/result.h"
#include "sixseven/executor/iterator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/index/rid.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"
#include "sixseven/vector/distance.h"
#include "sixseven/vector/embedding_worker.h"
#include "sixseven/vector/hnsw_index.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <unordered_set>
#include <vector>

namespace sixseven {

/// Configuration for a NEAREST scan operation.
struct NearestScanConfig {
    /// Number of nearest neighbors to return.
    uint32_t k = 10;

    /// Query vector for similarity search.
    std::vector<float> query_vector;

    /// Distance metric. DOT_PRODUCT and INNER_PRODUCT are both treated as
    /// dot-product similarity: candidates are ranked most-similar-first
    /// (internally sorted by the negated dot product) and the emitted
    /// _distance column reports the raw dot product (GDB-717).
    DistanceMetric metric = DistanceMetric::COSINE;

    /// Index of the EMBEDDING column in the table schema.
    int32_t embedding_column_index = -1;

    /// Optional set of allowed row RIDs (for graph-scoped search, e.g.
    /// NEAREST ... WITHIN TRAVERSE). When non-empty, only rows whose heap RID
    /// is in this set are eligible.
    ///
    /// Contract (GDB-745): the heap RID is the ONE canonical id space for
    /// graph scoping. Heap ordinals and HNSW node ids diverge as soon as a
    /// row has a NULL embedding (HNSW only assigns node ids to rows with
    /// non-null embeddings) or rows are deleted/inserted after the index
    /// build, so neither is used for scoping. All NEAREST execution paths
    /// (brute-force, HNSW fast path via hnsw_rid_map, HNSW slow fallback)
    /// filter by RID.
    std::set<RID> allowed_rids;

    /// Pre-filtered RIDs from a btree index lookup (for btree-accelerated
    /// filtered NEAREST). When non-empty, the operator computes distances
    /// only for these specific rows via brute-force, skipping HNSW entirely.
    std::vector<RID> prefiltered_rids;
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
/// them one at a time from next(), most similar row first. For L2/COSINE
/// that is _distance ascending; for dot-product metrics the emitted
/// _distance is the raw dot product, so rows surface in descending
/// _distance order (GDB-717).
class NearestScanOperator : public Iterator {
public:
    /// @param heap            The heap file to scan for row data.
    /// @param storage_schema  Byte-level schema for TupleSerializer::deserialize.
    /// @param config          NEAREST scan parameters (k, query, metric, etc.).
    /// @param schema          Output schema for result tuples.
    /// @param where_expr      Optional WHERE predicate (applied post-search).
    /// @param bound           BoundStatement for expression evaluation.
    /// @param hnsw_index      Optional HNSW index for accelerated search.
    /// @param hnsw_rid_map    Optional node_id → RID map for direct tuple lookups.
    NearestScanOperator(TableHeap& heap,
                        const Schema& storage_schema,
                        NearestScanConfig config,
                        OutputSchema schema,
                        const Expr* where_expr,
                        const BoundStatement& bound,
                        HnswIndex* hnsw_index = nullptr,
                        std::vector<RID>* hnsw_rid_map = nullptr);

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

    /// Pre-filtered search: compute distances only for pre-selected RIDs
    /// (from a btree index lookup), sort, and take top-k.
    Result<void> execute_prefiltered_search();

    /// Build the WHERE filter output schema (table columns without _distance).
    /// Called once during open() when where_expr_ is set.
    OutputSchema build_where_filter_schema() const;

    /// Translate a sort-key distance into the user-visible _distance value.
    /// For dot-product metrics the sort key is the negated dot product; the
    /// emitted value is the raw dot product (higher = more similar).
    [[nodiscard]] float display_distance(float sort_distance) const;

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
    std::vector<RID>* hnsw_rid_map_;

    /// Output schema for WHERE predicate evaluation (excludes _distance).
    /// Built once in open() to avoid per-row allocation.
    OutputSchema where_filter_schema_;

    /// RIDs already emitted, so a physical row is never returned twice even if
    /// the HNSW index holds more than one node pointing at it (e.g. a row that
    /// was embedded twice during bootstrap). Reset per search / widening pass.
    std::set<RID> seen_rids_;

    std::vector<Tuple> results_;
    size_t cursor_ = 0;
};

} // namespace sixseven
