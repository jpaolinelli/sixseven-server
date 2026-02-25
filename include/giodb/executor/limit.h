#pragma once

#include "giodb/executor/iterator.h"
#include "giodb/executor/tuple.h"

#include <cstdint>
#include <memory>

namespace giodb {

/// Limit operator: skips `offset` tuples then returns up to `limit` tuples
/// from the child iterator.
class LimitOperator : public Iterator {
public:
    /// @param child  The child operator to pull tuples from.
    /// @param limit  Maximum number of tuples to return.
    /// @param offset Number of tuples to skip before returning.
    LimitOperator(std::unique_ptr<Iterator> child, int64_t limit, int64_t offset = 0);

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
    int64_t limit_;
    int64_t offset_;
    int64_t skipped_ = 0;
    int64_t returned_ = 0;
};

} // namespace giodb
