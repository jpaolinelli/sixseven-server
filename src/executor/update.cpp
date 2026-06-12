#include "sixseven/executor/update.h"

#include "sixseven/common/coercion.h"
#include "sixseven/common/logging.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/expr_evaluator.h"

namespace sixseven {

UpdateOperator::UpdateOperator(TableHeap& heap,
                               const Schema& storage_schema,
                               std::unique_ptr<Iterator> child,
                               std::vector<UpdateAssignment> assignments,
                               const BoundStatement& bound)
    : heap_(heap), storage_schema_(storage_schema), child_(std::move(child)),
      assignments_(std::move(assignments)), bound_(bound),
      schema_(OutputSchema({OutputColumn{"", "count", TypeId::INT64, false, 0}})) {}

Result<void> UpdateOperator::do_open() {
    executed_ = false;
    return child_->open();
}

Result<std::optional<Tuple>> UpdateOperator::do_next() {
    if (executed_) {
        return ok(std::optional<Tuple>(std::nullopt));
    }
    executed_ = true;

    int64_t count = 0;

    // Materialize all target rows before mutating the heap (GDB-747). UPDATE
    // now inserts new tuple versions; draining the child first prevents the
    // scan from encountering — and re-updating — versions this statement just
    // created (the Halloween problem).
    struct PendingUpdate {
        RID old_rid;
        std::vector<Value> new_values;
        std::vector<uint8_t> new_bytes;
    };
    std::vector<PendingUpdate> pending;

    while (true) {
        auto row = child_->next();
        if (!row) {
            return make_error(row.error().code, row.error().message);
        }
        if (!row->has_value()) {
            break;
        }

        auto& tuple = row->value();
        if (!tuple.rid.has_value()) {
            return make_error(StatusCode::INTERNAL_ERROR, "UPDATE: tuple has no RID");
        }

        // Copy current values and apply assignments.
        std::vector<Value> new_values = tuple.values;

        for (const auto& assign : assignments_) {
            auto val = evaluate_expr(*assign.expr, tuple, child_->output_schema(), bound_);
            if (!val) {
                return make_error(val.error().code, val.error().message);
            }
            if (assign.column_index >= new_values.size()) {
                return make_error(StatusCode::INTERNAL_ERROR, "UPDATE: column index out of range");
            }
            // Coerce the expression result to the storage column type so that
            // TupleSerializer::serialize() sees the expected variant alternative.
            auto target_type = storage_schema_.column(assign.column_index).type;
            if (val->type_id() != target_type && !val->is_null()) {
                auto fitted = fit_to_storage(*val, target_type);
                if (!fitted) {
                    return make_error(fitted.error().code, fitted.error().message);
                }
                new_values[assign.column_index] = std::move(*fitted);
            } else {
                new_values[assign.column_index] = std::move(*val);
            }
        }

        auto bytes = TupleSerializer::serialize(new_values, storage_schema_);
        if (!bytes) {
            return make_error(bytes.error().code, bytes.error().message);
        }
        pending.push_back(PendingUpdate{*tuple.rid, std::move(new_values), std::move(*bytes)});
    }

    for (auto& upd : pending) {
        RID bm25_rid = upd.old_rid;

        if (heap_.mvcc_headers()) {
            // MVCC update (GDB-747): insert a new version stamped with this
            // transaction's xmin, then mark the old version deleted (xmax +
            // version-chain pointer to the new RID). No in-place overwrite —
            // an aborted transaction leaves the old version visible and the
            // new version invisible.
            auto new_rid = heap_.insert_tuple(upd.new_bytes, txn_id_);
            if (!new_rid) {
                return make_error(new_rid.error().code, new_rid.error().message);
            }
            auto marked = heap_.mark_deleted(upd.old_rid, txn_id_, *new_rid);
            if (!marked) {
                return make_error(marked.error().code, marked.error().message);
            }
            bm25_rid = *new_rid;

            // Maintain BM25 indexes: the row moved to a new RID, so drop the
            // old version's postings before indexing the new version.
            for (const auto& target : bm25_targets_) {
                if (target.index == nullptr) {
                    continue;
                }
                auto removed = target.index->remove_document(upd.old_rid);
                if (!removed) {
                    SIXSEVEN_LOG_WARN("BM25 update maintenance failed for rid=({},{}): {}",
                                      upd.old_rid.page_id,
                                      upd.old_rid.slot_id,
                                      removed.error().message);
                }
            }
        } else {
            // Legacy headerless heaps: in-place update (RID stable).
            auto update_result = heap_.update_tuple(upd.old_rid, upd.new_bytes);
            if (!update_result) {
                return make_error(update_result.error().code, update_result.error().message);
            }
        }

        // Index the (possibly changed) text under the row's current RID.
        for (const auto& target : bm25_targets_) {
            if (target.index == nullptr || target.text_column_index >= upd.new_values.size()) {
                continue;
            }
            const auto& v = upd.new_values[target.text_column_index];
            Result<void> r = v.is_null() ? target.index->remove_document(bm25_rid)
                                         : target.index->add_document(bm25_rid, v.as_string());
            if (!r) {
                SIXSEVEN_LOG_WARN("BM25 update maintenance failed for rid=({},{}): {}",
                                  bm25_rid.page_id,
                                  bm25_rid.slot_id,
                                  r.error().message);
            }
        }
        ++count;
    }

    Tuple result;
    result.values.push_back(Value(count));
    return ok(std::optional<Tuple>(std::move(result)));
}

void UpdateOperator::do_close() {
    child_->close();
}

const OutputSchema& UpdateOperator::output_schema() const {
    return schema_;
}

std::vector<const Iterator*> UpdateOperator::plan_children() const {
    return {child_.get()};
}

std::vector<Iterator*> UpdateOperator::plan_children_mutable() {
    return {child_.get()};
}

} // namespace sixseven
