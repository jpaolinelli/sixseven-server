#include "sixseven/executor/update.h"

#include "sixseven/common/coercion.h"
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

        // Serialise and update in heap.
        auto bytes = TupleSerializer::serialize(new_values, storage_schema_);
        if (!bytes) {
            return make_error(bytes.error().code, bytes.error().message);
        }
        auto update_result = heap_.update_tuple(*tuple.rid, *bytes);
        if (!update_result) {
            return make_error(update_result.error().code, update_result.error().message);
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
