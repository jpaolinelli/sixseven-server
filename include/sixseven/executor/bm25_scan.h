#pragma once

#include "sixseven/common/result.h"
#include "sixseven/executor/iterator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/index/bm25_index.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sixseven {

/// Configuration for a BM25 full-text scan.
struct Bm25ScanConfig {
    /// Raw query text (analyzed by the index's own analyzer for parity).
    std::string query;

    /// Maximum number of hits to materialize. 0 = all matching documents
    /// (ORDER BY / LIMIT downstream handle final ordering and truncation).
    uint32_t k = 0;
};

/// BM25 full-text search operator.
///
/// Planned from a `MATCH(col) TO 'query'` WHERE predicate. On open() it
/// queries the BM25 inverted index for matching documents, resolves each hit's
/// heap RID to a row, applies any residual WHERE predicate, and materializes
/// result tuples with a synthetic `_score` column appended (FLOAT64). Rows are
/// emitted in descending BM25 relevance order.
///
/// The residual WHERE may still contain the MATCH predicate itself; the
/// expression evaluator treats a MatchExpr as TRUE because the index scan has
/// already restricted the row set to matching documents.
class Bm25ScanOperator : public Iterator {
public:
    /// @param heap            Heap file to fetch row data from.
    /// @param storage_schema  Byte-level schema for TupleSerializer::deserialize.
    /// @param config          BM25 scan parameters (query, k).
    /// @param schema          Output schema (table columns + trailing _score).
    /// @param where_expr      Optional residual WHERE predicate (post-filter).
    /// @param bound           BoundStatement for expression evaluation.
    /// @param index           The BM25 index to query.
    Bm25ScanOperator(TableHeap& heap,
                     const Schema& storage_schema,
                     Bm25ScanConfig config,
                     OutputSchema schema,
                     const Expr* where_expr,
                     const BoundStatement& bound,
                     Bm25Index* index);

    const OutputSchema& output_schema() const override;

    [[nodiscard]] std::string plan_node_name() const override;
    [[nodiscard]] std::string plan_node_detail() const override;

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;

private:
    /// Build the WHERE filter schema (table columns without the _score column).
    OutputSchema build_where_filter_schema() const;

    /// Apply the residual WHERE filter to a candidate row and, if it passes,
    /// append the _score column and store the result. Returns 1 if emitted.
    Result<size_t> filter_and_emit(Tuple& candidate_tuple, float score);

    TableHeap& heap_;
    const Schema& storage_schema_;
    Bm25ScanConfig config_;
    OutputSchema schema_;
    const Expr* where_expr_;
    const BoundStatement& bound_;
    Bm25Index* index_;

    OutputSchema where_filter_schema_;
    std::vector<Tuple> results_;
    size_t cursor_ = 0;
};

} // namespace sixseven
