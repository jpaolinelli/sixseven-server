#include "giodb/executor/subquery_source.h"

namespace giodb {

SubquerySourceOperator::SubquerySourceOperator(std::unique_ptr<Iterator> child, OutputSchema schema)
    : child_(std::move(child)), schema_(std::move(schema)) {}

Result<void> SubquerySourceOperator::do_open() {
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

Result<std::optional<Tuple>> SubquerySourceOperator::do_next() {
    if (cursor_ >= tuples_.size()) {
        return ok(std::optional<Tuple>(std::nullopt));
    }
    return ok(std::optional<Tuple>(tuples_[cursor_++]));
}

void SubquerySourceOperator::do_close() {
    tuples_.clear();
    cursor_ = 0;
}

const OutputSchema& SubquerySourceOperator::output_schema() const {
    return schema_;
}

std::string SubquerySourceOperator::plan_node_name() const {
    return "Subquery Scan";
}

std::string SubquerySourceOperator::plan_node_detail() const {
    return "";
}

std::vector<const Iterator*> SubquerySourceOperator::plan_children() const {
    return {child_.get()};
}

std::vector<Iterator*> SubquerySourceOperator::plan_children_mutable() {
    return {child_.get()};
}

} // namespace giodb
