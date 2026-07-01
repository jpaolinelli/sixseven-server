#include "sixseven/executor/seq_scan.h"

#include "sixseven/executor/explain.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/table/tuple.h"

namespace sixseven {

SeqScanOperator::SeqScanOperator(TableHeap& heap,
                                 const Schema& storage_schema,
                                 OutputSchema output_schema,
                                 const Expr* predicate,
                                 const BoundStatement* bound)
    : heap_(heap), storage_schema_(storage_schema), schema_(std::move(output_schema)),
      predicate_(predicate), bound_(bound) {}

Result<void> SeqScanOperator::do_open() {
    auto it = heap_.begin();
    if (!it) {
        return make_error(it.error().code, it.error().message);
    }
    iter_.emplace(std::move(*it));

    // Apply skip at the iterator level — no deserialization.
    if (skip_rows_ > 0) {
        iter_->skip(skip_rows_);
    }
    return ok();
}

Result<std::optional<Tuple>> SeqScanOperator::do_next() {
    if (!iter_) {
        return make_error(StatusCode::INTERNAL_ERROR, "SeqScan: not opened");
    }

    while (true) {
        auto row_result = iter_->next();
        if (!row_result) {
            return tl::unexpected(row_result.error());
        }
        if (!row_result->has_value()) {
            // Iterator exhausted.
            return ok(std::optional<Tuple>(std::nullopt));
        }
        auto& row = *row_result;

        auto& [rid, data] = *row;

        // Deserialise raw bytes → vector<Value>.
        auto values = TupleSerializer::deserialize(data, storage_schema_);
        if (!values) {
            return make_error(values.error().code, values.error().message);
        }

        Tuple tuple{std::move(*values), rid};

        // Apply optional predicate.
        if (predicate_ != nullptr && bound_ != nullptr) {
            auto pass = evaluate_predicate(*predicate_, tuple, schema_, *bound_);
            if (!pass) {
                return make_error(pass.error().code, pass.error().message);
            }
            if (!*pass) {
                continue; // Predicate rejected this row.
            }
        }

        return ok(std::optional<Tuple>(std::move(tuple)));
    }
}

void SeqScanOperator::do_close() {
    iter_.reset();
}

const OutputSchema& SeqScanOperator::output_schema() const {
    return schema_;
}

std::string SeqScanOperator::plan_node_name() const {
    return "Seq Scan";
}

std::string SeqScanOperator::plan_node_detail() const {
    std::string detail;
    if (!schema_.columns().empty() && !schema_.columns()[0].table_name.empty()) {
        detail = "on " + schema_.columns()[0].table_name;
    }
    // Surface the pushed-down WHERE predicate so EXPLAIN actually shows the
    // filter rather than hiding it inside the scan.
    if (predicate_ != nullptr) {
        if (!detail.empty()) {
            detail += "  ";
        }
        detail += "(filter: " + expr_to_sql(*predicate_) + ")";
    }
    return detail;
}

} // namespace sixseven
