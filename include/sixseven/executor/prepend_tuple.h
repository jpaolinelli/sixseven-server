#pragma once

#include "sixseven/executor/iterator.h"
#include "sixseven/executor/tuple.h"

#include <memory>
#include <optional>
#include <vector>

namespace sixseven {

/// Streaming operator that prepends a fixed prefix tuple to every row produced
/// by its child, exposing a combined (prefix + child) schema.
///
/// Used to make a correlated subquery's inner predicates resolve the enclosing
/// query's columns: the outer row is materialised once as the prefix, so a
/// FilterOperator over this operator can reference both outer and inner columns.
class PrependTupleOperator : public Iterator {
public:
    /// @param child            Child iterator whose rows are extended.
    /// @param prefix           Values prepended to every child row.
    /// @param combined_schema  Schema describing prefix columns followed by the
    ///                         child's columns (must match prefix.size() + child width).
    PrependTupleOperator(std::unique_ptr<Iterator> child,
                         Tuple prefix,
                         OutputSchema combined_schema);

    const OutputSchema& output_schema() const override;

    // Plan inspection
    std::string plan_node_name() const override;
    std::string plan_node_detail() const override;
    std::vector<const Iterator*> plan_children() const override;

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;
    std::vector<Iterator*> plan_children_mutable() override;

private:
    std::unique_ptr<Iterator> child_;
    Tuple prefix_;
    OutputSchema schema_;
};

} // namespace sixseven
