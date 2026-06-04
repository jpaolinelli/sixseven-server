#include "sixseven/executor/prepend_tuple.h"

namespace sixseven {

PrependTupleOperator::PrependTupleOperator(std::unique_ptr<Iterator> child,
                                           Tuple prefix,
                                           OutputSchema combined_schema)
    : child_(std::move(child)), prefix_(std::move(prefix)), schema_(std::move(combined_schema)) {}

Result<void> PrependTupleOperator::do_open() {
    return child_->open();
}

Result<std::optional<Tuple>> PrependTupleOperator::do_next() {
    auto row = child_->next();
    if (!row) {
        return make_error(row.error().code, row.error().message);
    }
    if (!row->has_value()) {
        return ok(std::optional<Tuple>(std::nullopt));
    }

    Tuple combined;
    combined.values.reserve(prefix_.values.size() + row->value().values.size());
    combined.values.insert(
        combined.values.end(), prefix_.values.begin(), prefix_.values.end());
    combined.values.insert(combined.values.end(),
                           std::make_move_iterator(row->value().values.begin()),
                           std::make_move_iterator(row->value().values.end()));
    combined.rid = row->value().rid;
    return ok(std::optional<Tuple>(std::move(combined)));
}

void PrependTupleOperator::do_close() {
    child_->close();
}

const OutputSchema& PrependTupleOperator::output_schema() const {
    return schema_;
}

std::string PrependTupleOperator::plan_node_name() const {
    return "Prepend Outer";
}

std::string PrependTupleOperator::plan_node_detail() const {
    return "";
}

std::vector<const Iterator*> PrependTupleOperator::plan_children() const {
    return {child_.get()};
}

std::vector<Iterator*> PrependTupleOperator::plan_children_mutable() {
    return {child_.get()};
}

} // namespace sixseven
