#include "giodb/executor/update.h"

#include "giodb/common/coercion.h"
#include "giodb/common/types.h"
#include "giodb/executor/expr_evaluator.h"

namespace giodb {

namespace {

/// Fit a Value to the storage type, allowing both widening and narrowing
/// numeric conversions.  The standard coerce() only allows widening; DML
/// operators need to narrow expression results (e.g. INT64 arithmetic back
/// to INT32 storage).
Result<Value> fit_to_storage(const Value& val, TypeId target) {
    if (val.is_null() || val.type_id() == target) {
        return ok(val);
    }
    if (can_coerce(val.type_id(), target)) {
        return coerce(val, target);
    }
    if (is_numeric(val.type_id()) && is_integer(target)) {
        int64_t v = 0;
        switch (val.type_id()) {
        case TypeId::INT8:
            v = val.as_int8();
            break;
        case TypeId::INT16:
            v = val.as_int16();
            break;
        case TypeId::INT32:
            v = val.as_int32();
            break;
        case TypeId::INT64:
            v = val.as_int64();
            break;
        case TypeId::UINT8:
            v = val.as_uint8();
            break;
        case TypeId::UINT16:
            v = val.as_uint16();
            break;
        case TypeId::UINT32:
            v = val.as_uint32();
            break;
        case TypeId::UINT64:
            v = static_cast<int64_t>(val.as_uint64());
            break;
        case TypeId::FLOAT32:
            v = static_cast<int64_t>(val.as_float32());
            break;
        case TypeId::FLOAT64:
            v = static_cast<int64_t>(val.as_float64());
            break;
        default:
            return make_error(StatusCode::TYPE_ERROR,
                              "cannot fit " + std::string(type_name(val.type_id())) + " to " +
                                  std::string(type_name(target)));
        }
        switch (target) {
        case TypeId::INT8:
            return ok(Value(static_cast<int8_t>(v)));
        case TypeId::INT16:
            return ok(Value(static_cast<int16_t>(v)));
        case TypeId::INT32:
            return ok(Value(static_cast<int32_t>(v)));
        case TypeId::INT64:
            return ok(Value(v));
        case TypeId::UINT8:
            return ok(Value(static_cast<uint8_t>(v)));
        case TypeId::UINT16:
            return ok(Value(static_cast<uint16_t>(v)));
        case TypeId::UINT32:
            return ok(Value(static_cast<uint32_t>(v)));
        case TypeId::UINT64:
            return ok(Value(static_cast<uint64_t>(v)));
        default:
            break;
        }
    }
    if (is_numeric(val.type_id()) && is_floating(target)) {
        double d = 0.0;
        switch (val.type_id()) {
        case TypeId::INT8:
            d = val.as_int8();
            break;
        case TypeId::INT16:
            d = val.as_int16();
            break;
        case TypeId::INT32:
            d = val.as_int32();
            break;
        case TypeId::INT64:
            d = static_cast<double>(val.as_int64());
            break;
        case TypeId::UINT8:
            d = val.as_uint8();
            break;
        case TypeId::UINT16:
            d = val.as_uint16();
            break;
        case TypeId::UINT32:
            d = val.as_uint32();
            break;
        case TypeId::UINT64:
            d = static_cast<double>(val.as_uint64());
            break;
        case TypeId::FLOAT32:
            d = val.as_float32();
            break;
        case TypeId::FLOAT64:
            d = val.as_float64();
            break;
        default:
            break;
        }
        if (target == TypeId::FLOAT32)
            return ok(Value(static_cast<float>(d)));
        if (target == TypeId::FLOAT64)
            return ok(Value(d));
    }
    return make_error(StatusCode::TYPE_ERROR,
                      "cannot fit " + std::string(type_name(val.type_id())) + " to " +
                          std::string(type_name(target)));
}

} // namespace

UpdateOperator::UpdateOperator(TableHeap& heap,
                               const Schema& storage_schema,
                               std::unique_ptr<Iterator> child,
                               std::vector<UpdateAssignment> assignments,
                               const BoundStatement& bound)
    : heap_(heap), storage_schema_(storage_schema), child_(std::move(child)),
      assignments_(std::move(assignments)), bound_(bound),
      schema_(OutputSchema({OutputColumn{"", "count", TypeId::INT64, false, 0}})) {}

Result<void> UpdateOperator::open() {
    executed_ = false;
    return child_->open();
}

Result<std::optional<Tuple>> UpdateOperator::next() {
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

void UpdateOperator::close() {
    child_->close();
}

const OutputSchema& UpdateOperator::output_schema() const {
    return schema_;
}

} // namespace giodb
