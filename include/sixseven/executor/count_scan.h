#pragma once

#include "sixseven/executor/iterator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/table/table_heap.h"

#include <string>

namespace sixseven {

/// Fast-path operator for bare `SELECT COUNT(*) FROM table` queries.
///
/// Uses the live row count maintained by TableHeap for O(1) COUNT(*).
/// Returns a single tuple containing the count as INT64.
class CountScanOperator : public Iterator {
public:
    /// @param heap   The heap file providing the live row count.
    /// @param schema Output schema (single INT64 column for the count).
    CountScanOperator(TableHeap& heap, OutputSchema schema);

    const OutputSchema& output_schema() const override;

    // Plan inspection
    std::string plan_node_name() const override;
    std::string plan_node_detail() const override;

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;

private:
    TableHeap& heap_;
    OutputSchema schema_;
    bool emitted_ = false;
};

} // namespace sixseven
