#include "giodb/executor/seq_scan.h"

#include "giodb/executor/expr_evaluator.h"
#include "giodb/table/tuple.h"

namespace giodb {

SeqScanOperator::SeqScanOperator(TableHeap& heap,
                                 const Schema& storage_schema,
                                 OutputSchema output_schema,
                                 const Expr* predicate,
                                 const BoundStatement* bound)
    : heap_(heap), storage_schema_(storage_schema), schema_(std::move(output_schema)),
      predicate_(predicate), bound_(bound) {}

Result<void> SeqScanOperator::open() {
    auto it = heap_.begin();
    if (!it) {
        return make_error(it.error().code, it.error().message);
    }
    iter_.emplace(std::move(*it));
    return ok();
}

Result<std::optional<Tuple>> SeqScanOperator::next() {
    if (!iter_) {
        return make_error(StatusCode::INTERNAL_ERROR, "SeqScan: not opened");
    }

    while (true) {
        auto row = iter_->next();
        if (!row) {
            // Iterator exhausted.
            return ok(std::optional<Tuple>(std::nullopt));
        }

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

void SeqScanOperator::close() {
    iter_.reset();
}

const OutputSchema& SeqScanOperator::output_schema() const {
    return schema_;
}

} // namespace giodb
