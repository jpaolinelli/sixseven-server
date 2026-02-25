#include "giodb/executor/index_scan.h"

#include "giodb/executor/expr_evaluator.h"
#include "giodb/table/tuple.h"

namespace giodb {

IndexScanOperator::IndexScanOperator(const BTreeIndex& index,
                                     TableHeap& heap,
                                     const Schema& storage_schema,
                                     OutputSchema output_schema,
                                     std::optional<KeyType> begin_key,
                                     std::optional<KeyType> end_key,
                                     std::vector<size_t> index_col_indexes,
                                     bool index_only,
                                     const Expr* predicate,
                                     const BoundStatement* bound)
    : index_(index), heap_(heap), storage_schema_(storage_schema),
      schema_(std::move(output_schema)), begin_key_(std::move(begin_key)),
      end_key_(std::move(end_key)), index_col_indexes_(std::move(index_col_indexes)),
      index_only_(index_only), predicate_(predicate), bound_(bound) {}

Result<void> IndexScanOperator::open() {
    auto it = index_.range_scan(begin_key_, end_key_);
    if (!it) {
        return make_error(it.error().code, it.error().message);
    }
    iter_.emplace(std::move(*it));
    return ok();
}

Result<std::optional<Tuple>> IndexScanOperator::next() {
    if (!iter_) {
        return make_error(StatusCode::INTERNAL_ERROR, "IndexScan: not opened");
    }

    while (true) {
        auto entry = iter_->next();
        if (!entry) {
            return make_error(entry.error().code, entry.error().message);
        }
        if (!entry->has_value()) {
            // Iterator exhausted.
            return ok(std::optional<Tuple>(std::nullopt));
        }

        auto& [key, rid] = **entry;

        // Index-only path: build tuple from key values, skip heap fetch.
        if (index_only_) {
            Tuple tuple = build_tuple_from_key(key, rid);

            if (predicate_ != nullptr && bound_ != nullptr) {
                auto pass = evaluate_predicate(*predicate_, tuple, schema_, *bound_);
                if (!pass) {
                    return make_error(pass.error().code, pass.error().message);
                }
                if (!*pass) {
                    continue;
                }
            }
            return ok(std::optional<Tuple>(std::move(tuple)));
        }

        // Heap fetch path: retrieve full tuple from table.
        auto data = heap_.get_tuple(rid);
        if (!data) {
            return make_error(data.error().code, data.error().message);
        }

        auto values = TupleSerializer::deserialize(*data, storage_schema_);
        if (!values) {
            return make_error(values.error().code, values.error().message);
        }

        Tuple tuple{std::move(*values), rid};

        // Apply optional residual predicate.
        if (predicate_ != nullptr && bound_ != nullptr) {
            auto pass = evaluate_predicate(*predicate_, tuple, schema_, *bound_);
            if (!pass) {
                return make_error(pass.error().code, pass.error().message);
            }
            if (!*pass) {
                continue;
            }
        }

        return ok(std::optional<Tuple>(std::move(tuple)));
    }
}

void IndexScanOperator::close() {
    iter_.reset();
}

const OutputSchema& IndexScanOperator::output_schema() const {
    return schema_;
}

Tuple IndexScanOperator::build_tuple_from_key(const KeyType& key, const RID& rid) const {
    // Build a full-width tuple with null values for non-indexed columns,
    // then populate indexed column positions from the key.
    std::vector<Value> values(schema_.column_count());

    for (size_t ki = 0; ki < index_col_indexes_.size() && ki < key.size(); ++ki) {
        size_t col_idx = index_col_indexes_[ki];
        if (col_idx < values.size()) {
            values[col_idx] = key[ki];
        }
    }

    return Tuple{std::move(values), rid};
}

} // namespace giodb
