#include "sixseven/executor/count_scan.h"

#include "sixseven/txn/read_view.h"

namespace sixseven {

CountScanOperator::CountScanOperator(TableHeap& heap, OutputSchema schema)
    : heap_(heap), schema_(std::move(schema)) {}

Result<void> CountScanOperator::do_open() {
    emitted_ = false;
    return ok();
}

Result<std::optional<Tuple>> CountScanOperator::do_next() {
    if (emitted_) {
        return ok(std::optional<Tuple>(std::nullopt));
    }
    emitted_ = true;

    // MVCC-aware path (GDB-777): the live row counter includes rows from
    // in-flight (uncommitted) transactions, so when a snapshot read view is
    // installed and the heap filters versions by visibility, bypass the fast
    // path and count visible tuples through the heap iterator (which applies
    // is_visible against the statement snapshot).
    int64_t count = 0;
    if (heap_.mvcc_headers() && heap_.has_txn_manager() && current_mvcc_read_view() != nullptr) {
        auto iter = heap_.begin();
        if (!iter) {
            return make_error(iter.error().code, iter.error().message);
        }
        while (true) {
            auto r = iter->next();
            if (!r || !r->has_value()) {
                break;
            }
            ++count;
        }
    } else {
        // Use the live row count maintained by TableHeap — O(1).
        count = static_cast<int64_t>(heap_.row_count());
    }

    Tuple result;
    result.values.push_back(Value(count));
    return ok(std::optional<Tuple>(std::move(result)));
}

void CountScanOperator::do_close() {
    // No resources to release.
}

const OutputSchema& CountScanOperator::output_schema() const {
    return schema_;
}

std::string CountScanOperator::plan_node_name() const {
    return "Count Scan";
}

std::string CountScanOperator::plan_node_detail() const {
    if (!schema_.columns().empty() && !schema_.columns()[0].table_name.empty()) {
        return "on " + schema_.columns()[0].table_name;
    }
    return "";
}

} // namespace sixseven
