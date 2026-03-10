#include "sixseven/executor/algorithm_scan.h"

namespace sixseven {

AlgorithmScanOperator::AlgorithmScanOperator(const AlgorithmRegistry::Entry& entry,
                                             AlgorithmContext context,
                                             OutputSchema output_schema)
    : entry_(entry), context_(std::move(context)), output_schema_(std::move(output_schema)) {}

std::string AlgorithmScanOperator::plan_node_detail() const {
    return entry_.def.name + "('" + context_.edge_type + "')";
}

Result<void> AlgorithmScanOperator::do_open() {
    cursor_ = 0;

    auto result = entry_.execute(context_);
    if (!result) {
        return tl::unexpected(result.error());
    }
    rows_ = std::move(*result);

    return ok();
}

Result<std::optional<Tuple>> AlgorithmScanOperator::do_next() {
    if (cursor_ >= rows_.size()) {
        return ok(std::optional<Tuple>(std::nullopt));
    }

    Tuple t;
    t.values = std::move(rows_[cursor_].values);
    ++cursor_;
    return ok(std::optional<Tuple>(std::move(t)));
}

void AlgorithmScanOperator::do_close() {
    rows_.clear();
    cursor_ = 0;
}

} // namespace sixseven
