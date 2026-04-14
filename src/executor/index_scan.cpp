#include "sixseven/executor/index_scan.h"

#include "sixseven/common/coercion.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/table/tuple.h"

namespace sixseven {

IndexScanOperator::IndexScanOperator(const BTreeIndex& index,
                                     TableHeap& heap,
                                     const Schema& storage_schema,
                                     OutputSchema output_schema,
                                     std::optional<KeyType> begin_key,
                                     std::optional<KeyType> end_key,
                                     std::vector<size_t> index_col_indexes,
                                     bool index_only,
                                     const Expr* predicate,
                                     const BoundStatement* bound,
                                     std::optional<KeyType> equality_key)
    : index_(index), heap_(heap), storage_schema_(storage_schema),
      schema_(std::move(output_schema)), begin_key_(std::move(begin_key)),
      end_key_(std::move(end_key)), index_col_indexes_(std::move(index_col_indexes)),
      index_only_(index_only), predicate_(predicate), bound_(bound),
      equality_key_(std::move(equality_key)) {}

Result<void> IndexScanOperator::do_open() {
    auto it = index_.range_scan(begin_key_, end_key_);
    if (!it) {
        return make_error(it.error().code, it.error().message);
    }
    iter_.emplace(std::move(*it));
    return ok();
}

Result<std::optional<Tuple>> IndexScanOperator::do_next() {
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

        // For equality scans, terminate as soon as the key moves past the
        // target. The btree is sorted, so no further entries can match.
        if (equality_key_ && key.size() == equality_key_->size()) {
            bool keys_match = true;
            for (size_t i = 0; i < key.size(); ++i) {
                auto cmp = compare(key[i], (*equality_key_)[i]);
                if (!cmp || *cmp != std::strong_ordering::equal) {
                    keys_match = false;
                    break;
                }
            }
            if (!keys_match) {
                return ok(std::optional<Tuple>(std::nullopt));
            }
        }

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

void IndexScanOperator::do_close() {
    iter_.reset();
}

const OutputSchema& IndexScanOperator::output_schema() const {
    return schema_;
}

std::string IndexScanOperator::plan_node_name() const {
    return "Index Scan";
}

std::string IndexScanOperator::plan_node_detail() const {
    if (!schema_.columns().empty() && !schema_.columns()[0].table_name.empty()) {
        return "on " + schema_.columns()[0].table_name + " using index";
    }
    return "using index";
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

} // namespace sixseven
