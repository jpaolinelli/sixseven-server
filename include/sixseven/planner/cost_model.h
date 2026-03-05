#pragma once

#include "sixseven/planner/statistics.h"

#include <cmath>
#include <cstdint>

namespace sixseven {

/// Tunable cost model parameters (PostgreSQL-inspired defaults).
struct CostModelParams {
    double seq_page_cost = 1.0;        ///< Cost of a sequential page read.
    double random_page_cost = 4.0;     ///< Cost of a random page read.
    double cpu_tuple_cost = 0.01;      ///< CPU cost per row processed.
    double cpu_operator_cost = 0.0025; ///< CPU cost per operator evaluation.
};

/// Estimated cost and cardinality for a plan node.
struct PlanCost {
    double startup_cost = 0.0;   ///< Cost before first row is produced.
    double total_cost = 0.0;     ///< Total cost to produce all rows.
    double estimated_rows = 0.0; ///< Estimated output cardinality.
};

/// Cost model: computes estimated costs for physical operators.
class CostModel {
public:
    explicit CostModel(CostModelParams params = {});

    /// Access current parameters.
    [[nodiscard]] const CostModelParams& params() const { return params_; }

    /// Sequential scan cost.
    /// pages * seq_page_cost + rows * cpu_tuple_cost
    [[nodiscard]] PlanCost seq_scan_cost(uint32_t pages, uint64_t rows) const;

    /// Index scan cost.
    /// selectivity * pages * random_page_cost + matching_rows * cpu_tuple_cost
    ///   + index_pages * random_page_cost
    [[nodiscard]] PlanCost index_scan_cost(uint32_t heap_pages,
                                           uint64_t total_rows,
                                           double selectivity,
                                           uint32_t index_pages) const;

    /// Hash join cost.
    /// Build phase: build_rows * cpu_tuple_cost
    /// Probe phase: probe_rows * cpu_tuple_cost
    /// Hash computation: (build_rows + probe_rows) * cpu_operator_cost
    [[nodiscard]] PlanCost hash_join_cost(const PlanCost& build_side,
                                          const PlanCost& probe_side,
                                          double join_selectivity) const;

    /// Sort-merge join cost.
    /// sort_cost(left) + sort_cost(right) + merge_rows * cpu_tuple_cost
    [[nodiscard]] PlanCost sort_merge_join_cost(const PlanCost& left,
                                                const PlanCost& right,
                                                bool left_sorted,
                                                bool right_sorted,
                                                double join_selectivity) const;

    /// Nested-loop join cost.
    /// outer_rows * inner_cost
    [[nodiscard]] PlanCost nested_loop_join_cost(const PlanCost& outer,
                                                 const PlanCost& inner,
                                                 double join_selectivity) const;

    /// Sort cost.
    /// N * log2(N) * cpu_operator_cost + sort_pages * seq_page_cost
    [[nodiscard]] PlanCost sort_cost(const PlanCost& input) const;

    /// Filter cost (adds CPU cost per input row).
    [[nodiscard]] PlanCost filter_cost(const PlanCost& input, double selectivity) const;

    /// Projection cost (negligible I/O, small CPU overhead).
    [[nodiscard]] PlanCost project_cost(const PlanCost& input) const;

    /// Hash aggregate cost.
    [[nodiscard]] PlanCost hash_aggregate_cost(const PlanCost& input, uint64_t num_groups) const;

    /// Limit cost (reduces output rows but doesn't reduce input cost much).
    [[nodiscard]] PlanCost limit_cost(const PlanCost& input, uint64_t limit_rows) const;

private:
    CostModelParams params_;

    /// Helper: estimate sort cost for N rows.
    [[nodiscard]] double sort_cpu_cost(double n) const {
        if (n <= 1.0) {
            return 0.0;
        }
        return n * std::log2(n) * params_.cpu_operator_cost;
    }
};

} // namespace sixseven
