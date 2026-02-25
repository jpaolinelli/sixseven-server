#pragma once

#include "giodb/executor/iterator.h"
#include "giodb/executor/tuple.h"
#include "giodb/table/table_heap.h"

#include <cstdint>
#include <memory>

namespace giodb {

/// Delete operator: scans tuples from a child iterator and deletes
/// each one from the heap.
///
/// The output is a single-row tuple containing the affected row count.
/// Note: WAL logging and index updates are deferred to a later integration ticket.
class DeleteOperator : public Iterator {
public:
    /// @param heap  Target table heap.
    /// @param child Child iterator yielding tuples to delete (with WHERE applied).
    DeleteOperator(TableHeap& heap, std::unique_ptr<Iterator> child);

    const OutputSchema& output_schema() const override;
    std::string plan_node_name() const override { return "Delete"; }
    std::string plan_node_detail() const override { return ""; }
    std::vector<const Iterator*> plan_children() const override;

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;
    std::vector<Iterator*> plan_children_mutable() override;

private:
    TableHeap& heap_;
    std::unique_ptr<Iterator> child_;
    OutputSchema schema_;
    bool executed_ = false;
};

} // namespace giodb
