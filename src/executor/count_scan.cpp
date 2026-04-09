#include "sixseven/executor/count_scan.h"

#include "sixseven/common/logging.h"

namespace sixseven {

CountScanOperator::CountScanOperator(TableHeap& heap, BufferPoolManager& bpm, OutputSchema schema)
    : heap_(heap), bpm_(bpm), schema_(std::move(schema)) {}

Result<void> CountScanOperator::do_open() {
    emitted_ = false;
    return ok();
}

Result<std::optional<Tuple>> CountScanOperator::do_next() {
    if (emitted_) {
        return ok(std::optional<Tuple>(std::nullopt));
    }
    emitted_ = true;

    auto pc = heap_.page_count();
    if (!pc) {
        return make_error(pc.error().code, pc.error().message);
    }
    uint32_t data_pages = *pc;

    int64_t count = 0;

    // Iterate all data pages (page IDs 1..data_pages).
    // For each page, count live slots by checking slot entries directly.
    // A slot with offset == 0 is deleted; all others are live.
    // This skips TupleSerializer::deserialize() entirely.
    for (uint32_t pid = 1; pid <= data_pages; ++pid) {
        auto page_result = bpm_.fetch_page(pid);
        if (!page_result) {
            // Skip pages that can't be fetched (same as TableIterator).
            continue;
        }

        Page* page = *page_result;
        uint16_t slot_count = page->slot_count();

        for (uint16_t slot = 0; slot < slot_count; ++slot) {
            auto tuple_span = page->get_tuple(slot);
            if (tuple_span) {
                ++count;
            }
            // Deleted slot — skip.
        }

        auto unpin = bpm_.unpin_page(pid, false);
        if (!unpin) {
            SIXSEVEN_LOG_WARN(
                "unpin failed during count scan on page {}: {}", pid, unpin.error().message);
        }
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
