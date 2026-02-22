#include "giodb/executor/limit.h"

namespace giodb {

LimitOperator::LimitOperator(std::unique_ptr<Iterator> child, int64_t limit, int64_t offset)
    : child_(std::move(child)), limit_(limit), offset_(offset) {}

Result<void> LimitOperator::open() {
    skipped_ = 0;
    returned_ = 0;
    return child_->open();
}

Result<std::optional<Tuple>> LimitOperator::next() {
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

void LimitOperator::close() {
    child_->close();
}

const OutputSchema& LimitOperator::output_schema() const {
    return child_->output_schema();
}

} // namespace giodb
