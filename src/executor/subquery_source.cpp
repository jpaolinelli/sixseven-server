#include "giodb/executor/subquery_source.h"

namespace giodb {

SubquerySourceOperator::SubquerySourceOperator(std::unique_ptr<Iterator> child, OutputSchema schema)
    : child_(std::move(child)), schema_(std::move(schema)) {}

Result<void> SubquerySourceOperator::open() {
    tuples_.clear();
    cursor_ = 0;

    auto open_result = child_->open();
    if (!open_result) {
        return open_result;
    }

    // Drain all tuples from the child into our buffer.
    while (true) {
        auto row = child_->next();
        if (!row) {
            child_->close();
            return make_error(row.error().code, row.error().message);
        }
        if (!row->has_value()) {
            break;
        }
        tuples_.push_back(std::move(row->value()));
    }

    child_->close();
    return ok();
}

Result<std::optional<Tuple>> SubquerySourceOperator::next() {
    if (cursor_ >= tuples_.size()) {
        return ok(std::optional<Tuple>(std::nullopt));
    }
    return ok(std::optional<Tuple>(tuples_[cursor_++]));
}

void SubquerySourceOperator::close() {
    tuples_.clear();
    cursor_ = 0;
}

const OutputSchema& SubquerySourceOperator::output_schema() const {
    return schema_;
}

} // namespace giodb
