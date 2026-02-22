#include "giodb/executor/sort.h"

#include "giodb/common/coercion.h"
#include "giodb/executor/expr_evaluator.h"

#include <algorithm>

namespace giodb {

SortOperator::SortOperator(std::unique_ptr<Iterator> child,
                           std::vector<SortKey> keys,
                           const BoundStatement& bound)
    : child_(std::move(child)), keys_(std::move(keys)), bound_(bound) {}

Result<void> SortOperator::open() {
    auto open_result = child_->open();
    if (!open_result) {
        return open_result;
    }

    // Materialise all child tuples.
    sorted_.clear();
    cursor_ = 0;

    while (true) {
        auto row = child_->next();
        if (!row) {
            return make_error(row.error().code, row.error().message);
        }
        if (!row->has_value()) {
            break;
        }
        sorted_.push_back(std::move(row->value()));
    }

    // Sort using the ORDER BY keys.
    // We pre-evaluate sort keys for each tuple, then compare.
    // For simplicity, we evaluate keys during comparison (not pre-materialised).
    const auto& schema = child_->output_schema();
    const auto& keys = keys_;
    const auto& bound = bound_;

    // Use a stable sort to preserve insertion order for equal elements.
    std::stable_sort(sorted_.begin(),
                     sorted_.end(),
                     [&schema, &keys, &bound](const Tuple& a, const Tuple& b) -> bool {
                         for (const auto& key : keys) {
                             auto va = evaluate_expr(*key.expr, a, schema, bound);
                             auto vb = evaluate_expr(*key.expr, b, schema, bound);
                             if (!va || !vb) {
                                 return false; // On error, treat as equal.
                             }
                             // Handle NULLs: NULL sorts last for ASC, first for DESC.
                             if (va->is_null() && vb->is_null()) {
                                 continue;
                             }
                             if (va->is_null()) {
                                 return key.direction == SortDirection::DESC;
                             }
                             if (vb->is_null()) {
                                 return key.direction == SortDirection::ASC;
                             }

                             auto cmp = compare(*va, *vb);
                             if (!cmp) {
                                 return false;
                             }
                             if (*cmp == std::strong_ordering::less) {
                                 return key.direction == SortDirection::ASC;
                             }
                             if (*cmp == std::strong_ordering::greater) {
                                 return key.direction == SortDirection::DESC;
                             }
                             // Equal on this key — continue to next key.
                         }
                         return false; // All keys equal.
                     });

    return ok();
}

Result<std::optional<Tuple>> SortOperator::next() {
    if (cursor_ >= sorted_.size()) {
        return ok(std::optional<Tuple>(std::nullopt));
    }
    return ok(std::optional<Tuple>(std::move(sorted_[cursor_++])));
}

void SortOperator::close() {
    sorted_.clear();
    cursor_ = 0;
    child_->close();
}

const OutputSchema& SortOperator::output_schema() const {
    return child_->output_schema();
}

} // namespace giodb
