#include "sixseven/executor/hash_index_scan.h"

#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/table/tuple.h"

namespace sixseven {

HashIndexScanOperator::HashIndexScanOperator(const HashIndex& index,
                                             TableHeap& heap,
                                             const Schema& storage_schema,
                                             OutputSchema output_schema,
                                             KeyType lookup_key,
                                             std::vector<size_t> index_col_indexes,
                                             bool index_only,
                                             const Expr* predicate,
                                             const BoundStatement* bound)
    : index_(index), heap_(heap), storage_schema_(storage_schema),
      schema_(std::move(output_schema)), lookup_key_(std::move(lookup_key)),
      index_col_indexes_(std::move(index_col_indexes)), index_only_(index_only),
      predicate_(predicate), bound_(bound) {}

Result<void> HashIndexScanOperator::do_open() {
    // Perform the hash lookup and collect all matching RIDs.
    auto results = index_.search_all(lookup_key_);
    if (!results) {
        return make_error(results.error().code, results.error().message);
    }
    matching_rids_ = std::move(*results);
    current_rid_idx_ = 0;
    return ok();
}

Result<std::optional<Tuple>> HashIndexScanOperator::do_next() {
    while (current_rid_idx_ < matching_rids_.size()) {
        auto rid = matching_rids_[current_rid_idx_++];

        // Index-only path: build tuple from key values, skip heap fetch.
        if (index_only_) {
            Tuple tuple = build_tuple_from_key(lookup_key_, rid);

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

    // All matches exhausted.
    return ok(std::optional<Tuple>(std::nullopt));
}

void HashIndexScanOperator::do_close() {
    matching_rids_.clear();
    current_rid_idx_ = 0;
}

const OutputSchema& HashIndexScanOperator::output_schema() const {
    return schema_;
}

std::string HashIndexScanOperator::plan_node_name() const {
    return "Hash Index Scan";
}

std::string HashIndexScanOperator::plan_node_detail() const {
    if (!schema_.columns().empty() && !schema_.columns()[0].table_name.empty()) {
        return "on " + schema_.columns()[0].table_name + " using hash index";
    }
    return "using hash index";
}

Tuple HashIndexScanOperator::build_tuple_from_key(const KeyType& key, const RID& rid) const {
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
