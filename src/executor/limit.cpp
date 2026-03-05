#include "sixseven/executor/limit.h"

namespace sixseven {

LimitOperator::LimitOperator(std::unique_ptr<Iterator> child, int64_t limit, int64_t offset)
    : child_(std::move(child)), limit_(limit), offset_(offset) {}

Result<void> LimitOperator::do_open() {
    skipped_ = 0;
    returned_ = 0;
    return child_->open();
}

Result<std::optional<Tuple>> LimitOperator::do_next() {
    // Skip offset rows.
    while (skipped_ < offset_) {
        auto row = child_->next();
        if (!row) {
            return row;
        }
        if (!row->has_value()) {
            return row; // Child exhausted before offset reached.
        }
        ++skipped_;
    }

    // Check limit.
    if (returned_ >= limit_) {
        return ok(std::optional<Tuple>(std::nullopt));
    }

    auto row = child_->next();
    if (!row) {
        return row;
    }
    if (!row->has_value()) {
        return row;
    }

    ++returned_;
    return row;
}

void LimitOperator::do_close() {
    child_->close();
}

const OutputSchema& LimitOperator::output_schema() const {
    return child_->output_schema();
}

std::string LimitOperator::plan_node_name() const {
    return "Limit";
}

std::string LimitOperator::plan_node_detail() const {
    std::string detail = "limit=" + std::to_string(limit_);
    if (offset_ > 0) {
        detail += " offset=" + std::to_string(offset_);
    }
    return detail;
}

std::vector<const Iterator*> LimitOperator::plan_children() const {
    return {child_.get()};
}

std::vector<Iterator*> LimitOperator::plan_children_mutable() {
    return {child_.get()};
}

} // namespace sixseven
