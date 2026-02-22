#include "giodb/executor/delete.h"

namespace giodb {

DeleteOperator::DeleteOperator(TableHeap& heap, std::unique_ptr<Iterator> child)
    : heap_(heap),
      child_(std::move(child)),
      schema_(OutputSchema({OutputColumn{"", "count", TypeId::INT64, false, 0}})) {}

Result<void> DeleteOperator::open() {
    executed_ = false;
    return child_->open();
}

Result<std::optional<Tuple>> DeleteOperator::next() {
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
            return make_error(StatusCode::INTERNAL_ERROR,
                              "DELETE: tuple has no RID");
        }

        auto del = heap_.delete_tuple(*tuple.rid);
        if (!del) {
            return make_error(del.error().code, del.error().message);
        }
        ++count;
    }

    Tuple result;
    result.values.push_back(Value(count));
    return ok(std::optional<Tuple>(std::move(result)));
}

void DeleteOperator::close() {
    child_->close();
}

const OutputSchema& DeleteOperator::output_schema() const {
    return schema_;
}

} // namespace giodb
