#include "sixseven/executor/bm25_scan.h"

#include "sixseven/common/logging.h"
#include "sixseven/executor/expr_evaluator.h"

#include <utility>

namespace sixseven {

Bm25ScanOperator::Bm25ScanOperator(TableHeap& heap,
                                   const Schema& storage_schema,
                                   Bm25ScanConfig config,
                                   OutputSchema schema,
                                   const Expr* where_expr,
                                   const BoundStatement& bound,
                                   Bm25Index* index)
    : heap_(heap), storage_schema_(storage_schema), config_(std::move(config)),
      schema_(std::move(schema)), where_expr_(where_expr), bound_(bound), index_(index) {}

std::string Bm25ScanOperator::plan_node_name() const {
    return "BM25 Scan";
}

std::string Bm25ScanOperator::plan_node_detail() const {
    return "query='" + config_.query + "'";
}

const OutputSchema& Bm25ScanOperator::output_schema() const {
    return schema_;
}

Result<void> Bm25ScanOperator::do_open() {
    results_.clear();
    cursor_ = 0;

    if (where_expr_ != nullptr) {
        where_filter_schema_ = build_where_filter_schema();
    }

    if (index_ == nullptr) {
        return make_error(StatusCode::INTERNAL_ERROR, "BM25 scan: null index");
    }

    // Query the inverted index. Hits come back in descending relevance order.
    auto hits = index_->search(config_.query, config_.k);

    for (const auto& hit : hits) {
        auto data = heap_.get_tuple(hit.rid);
        if (!data) {
            // Row deleted since the index was last maintained — skip.
            SIXSEVEN_LOG_DEBUG("BM25: get_tuple failed for rid=({},{}): {}",
                               hit.rid.page_id,
                               hit.rid.slot_id,
                               data.error().message);
            continue;
        }
        auto values = TupleSerializer::deserialize(*data, storage_schema_);
        if (!values) {
            return make_error(values.error().code, values.error().message);
        }
        Tuple tuple{std::move(*values), hit.rid};
        auto emitted = filter_and_emit(tuple, hit.score);
        if (!emitted) {
            return make_error(emitted.error().code, emitted.error().message);
        }
    }

    return ok();
}

Result<std::optional<Tuple>> Bm25ScanOperator::do_next() {
    if (cursor_ >= results_.size()) {
        return ok(std::optional<Tuple>(std::nullopt));
    }
    return ok(std::make_optional(std::move(results_[cursor_++])));
}

void Bm25ScanOperator::do_close() {
    results_.clear();
    cursor_ = 0;
}

OutputSchema Bm25ScanOperator::build_where_filter_schema() const {
    // The output schema is table columns + trailing _score. The WHERE predicate
    // references table columns only, so strip the last (_score) column.
    std::vector<OutputColumn> table_cols;
    for (size_t i = 0; i + 1 < schema_.column_count(); ++i) {
        table_cols.push_back(schema_.column(i));
    }
    return OutputSchema(std::move(table_cols));
}

Result<size_t> Bm25ScanOperator::filter_and_emit(Tuple& candidate_tuple, float score) {
    if (where_expr_ != nullptr) {
        auto pass = evaluate_predicate(*where_expr_, candidate_tuple, where_filter_schema_, bound_);
        if (!pass) {
            return make_error(pass.error().code, pass.error().message);
        }
        if (!*pass) {
            return ok(size_t{0});
        }
    }

    Tuple result;
    result.values = std::move(candidate_tuple.values);
    result.values.push_back(Value(static_cast<double>(score)));
    result.rid = candidate_tuple.rid;
    results_.push_back(std::move(result));
    return ok(size_t{1});
}

} // namespace sixseven
