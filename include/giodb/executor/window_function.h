#pragma once

#include "giodb/executor/iterator.h"
#include "giodb/executor/sort.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace giodb {

/// Identifies which window function to compute.
enum class WindowFunc : uint8_t {
    // Ranking functions
    ROW_NUMBER,
    RANK,
    DENSE_RANK,
    NTILE,

    // Offset functions
    LAG,
    LEAD,
    FIRST_VALUE,
    LAST_VALUE,

    // Aggregate window functions
    WIN_SUM,
    WIN_AVG,
    WIN_COUNT,
    WIN_MIN,
    WIN_MAX,
};

/// Frame bound type for window frame specification.
enum class FrameBound : uint8_t {
    UNBOUNDED_PRECEDING,
    N_PRECEDING,
    CURRENT_ROW,
    N_FOLLOWING,
    UNBOUNDED_FOLLOWING,
};

/// Frame type (ROWS vs RANGE).
enum class FrameType : uint8_t {
    ROWS,
    RANGE,
};

/// Specifies the window frame: ROWS/RANGE BETWEEN start AND end.
struct WindowFrameSpec {
    FrameType type = FrameType::ROWS;
    FrameBound start_bound = FrameBound::UNBOUNDED_PRECEDING;
    int64_t start_offset = 0; ///< Used when start_bound is N_PRECEDING or N_FOLLOWING.
    FrameBound end_bound = FrameBound::CURRENT_ROW;
    int64_t end_offset = 0; ///< Used when end_bound is N_PRECEDING or N_FOLLOWING.
};

/// Describes a single window function computation.
struct WindowFunctionDescriptor {
    WindowFunc func;
    const Expr* arg = nullptr; ///< Column expression (e.g., for SUM(salary), LAG(salary)).
    int64_t offset = 1;        ///< Offset for LAG/LEAD (default 1).
    Value default_value;       ///< Default value for LAG/LEAD when out of bounds.
    int64_t ntile_buckets = 1; ///< Number of buckets for NTILE.
    WindowFrameSpec frame;     ///< Frame specification for aggregate window functions.
};

/// Window Function operator: partitions input, sorts within each
/// partition, and evaluates window functions per row.
///
/// The operator materialises all child tuples in open(), partitions
/// them by PARTITION BY columns, sorts each partition by ORDER BY keys,
/// evaluates all window functions, and emits one output tuple per
/// input row in next().
///
/// Output schema layout: [child_col_0, ..., child_col_n, win_0, ..., win_m]
///
/// Supports: ROW_NUMBER, RANK, DENSE_RANK, NTILE, LAG, LEAD,
/// FIRST_VALUE, LAST_VALUE, SUM, AVG, COUNT, MIN, MAX with frame spec.
class WindowOperator : public Iterator {
public:
    /// @param child          Child iterator to read input tuples from.
    /// @param partition_by   PARTITION BY expressions (may be empty for whole-input partition).
    /// @param order_by       ORDER BY keys within each partition.
    /// @param functions      Window function descriptors.
    /// @param bound          BoundStatement with expr_types map.
    /// @param schema         Output schema (child columns + window function result columns).
    WindowOperator(std::unique_ptr<Iterator> child,
                   std::vector<const Expr*> partition_by,
                   std::vector<SortKey> order_by,
                   std::vector<WindowFunctionDescriptor> functions,
                   const BoundStatement& bound,
                   OutputSchema schema);

    Result<void> open() override;
    Result<std::optional<Tuple>> next() override;
    void close() override;
    const OutputSchema& output_schema() const override;

private:
    /// Resolve the absolute row index for a frame bound within a partition.
    static size_t
    resolve_bound(FrameBound bound, int64_t offset, size_t current_row, size_t partition_size);

    std::unique_ptr<Iterator> child_;
    std::vector<const Expr*> partition_by_;
    std::vector<SortKey> order_by_;
    std::vector<WindowFunctionDescriptor> functions_;
    const BoundStatement& bound_;
    OutputSchema schema_;

    /// Pre-computed output tuples, populated in open().
    std::vector<Tuple> output_;
    size_t cursor_ = 0;
};

} // namespace giodb
