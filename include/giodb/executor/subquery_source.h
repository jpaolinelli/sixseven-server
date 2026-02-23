#pragma once

#include "giodb/executor/iterator.h"
#include "giodb/executor/tuple.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace giodb {

/// Materialisation operator that drains a child iterator on open(),
/// stores all tuples in memory, and replays them on subsequent next() calls.
///
/// Used for:
///   - CTE references (WITH clause) — the same CTE result can be scanned
///     multiple times.
///   - Derived tables (FROM subquery) — the subquery is executed once and
///     its results are replayed.
///   - Right-side of semi/anti join rewrites from subquery decorrelation.
class SubquerySourceOperator : public Iterator {
public:
    /// @param child   Child iterator whose results will be materialised.
    /// @param schema  Output schema for this operator.
    SubquerySourceOperator(std::unique_ptr<Iterator> child, OutputSchema schema);

    Result<void> open() override;
    Result<std::optional<Tuple>> next() override;
    void close() override;
    const OutputSchema& output_schema() const override;

private:
    std::unique_ptr<Iterator> child_;
    OutputSchema schema_;
    std::vector<Tuple> tuples_;
    size_t cursor_ = 0;
};

} // namespace giodb
