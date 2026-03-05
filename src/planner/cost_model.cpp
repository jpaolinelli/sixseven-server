#include "sixseven/planner/cost_model.h"

#include <algorithm>
#include <cmath>

namespace sixseven {

CostModel::CostModel(CostModelParams params) : params_(params) {}

PlanCost CostModel::seq_scan_cost(uint32_t pages, uint64_t rows) const {
    PlanCost cost;
    cost.startup_cost = 0.0;
    cost.total_cost = static_cast<double>(pages) * params_.seq_page_cost +
                      static_cast<double>(rows) * params_.cpu_tuple_cost;
    cost.estimated_rows = static_cast<double>(rows);
    return cost;
}

PlanCost CostModel::index_scan_cost(uint32_t heap_pages,
                                    uint64_t total_rows,
                                    double selectivity,
                                    uint32_t index_pages) const {
    double matching_rows = static_cast<double>(total_rows) * selectivity;
    double pages_fetched = selectivity * static_cast<double>(heap_pages);

    PlanCost cost;
    cost.startup_cost = static_cast<double>(index_pages) * params_.random_page_cost;
    cost.total_cost = pages_fetched * params_.random_page_cost +
                      matching_rows * params_.cpu_tuple_cost +
                      static_cast<double>(index_pages) * params_.random_page_cost;
    cost.estimated_rows = matching_rows;
    return cost;
}

PlanCost CostModel::hash_join_cost(const PlanCost& build_side,
                                   const PlanCost& probe_side,
                                   double join_selectivity) const {
    PlanCost cost;
    // Build phase: materialize the build side.
    cost.startup_cost = build_side.total_cost + build_side.estimated_rows * params_.cpu_tuple_cost;
    // Probe phase: scan probe side and look up in hash table.
    cost.total_cost =
        cost.startup_cost + probe_side.total_cost +
        probe_side.estimated_rows * params_.cpu_tuple_cost +
        (build_side.estimated_rows + probe_side.estimated_rows) * params_.cpu_operator_cost;
    // Output cardinality.
    cost.estimated_rows = build_side.estimated_rows * probe_side.estimated_rows * join_selectivity;
    return cost;
}

PlanCost CostModel::sort_merge_join_cost(const PlanCost& left,
                                         const PlanCost& right,
                                         bool left_sorted,
                                         bool right_sorted,
                                         double join_selectivity) const {
    PlanCost cost;

    double left_sort_cost = left_sorted ? 0.0 : sort_cpu_cost(left.estimated_rows);
    double right_sort_cost = right_sorted ? 0.0 : sort_cpu_cost(right.estimated_rows);

    cost.startup_cost = left.total_cost + right.total_cost + left_sort_cost + right_sort_cost;
    double merge_cost = (left.estimated_rows + right.estimated_rows) * params_.cpu_tuple_cost;
    cost.total_cost = cost.startup_cost + merge_cost;
    cost.estimated_rows = left.estimated_rows * right.estimated_rows * join_selectivity;
    return cost;
}

PlanCost CostModel::nested_loop_join_cost(const PlanCost& outer,
                                          const PlanCost& inner,
                                          double join_selectivity) const {
    PlanCost cost;
    cost.startup_cost = outer.startup_cost + inner.startup_cost;
    cost.total_cost = outer.total_cost + outer.estimated_rows * inner.total_cost +
                      outer.estimated_rows * inner.estimated_rows * params_.cpu_tuple_cost;
    cost.estimated_rows = outer.estimated_rows * inner.estimated_rows * join_selectivity;
    return cost;
}

PlanCost CostModel::sort_cost(const PlanCost& input) const {
    PlanCost cost;
    double sort_overhead = sort_cpu_cost(input.estimated_rows);
    cost.startup_cost = input.total_cost + sort_overhead;
    cost.total_cost = cost.startup_cost + input.estimated_rows * params_.cpu_tuple_cost;
    cost.estimated_rows = input.estimated_rows;
    return cost;
}

PlanCost CostModel::filter_cost(const PlanCost& input, double selectivity) const {
    PlanCost cost;
    cost.startup_cost = input.startup_cost;
    cost.total_cost = input.total_cost + input.estimated_rows * params_.cpu_operator_cost;
    cost.estimated_rows = input.estimated_rows * selectivity;
    return cost;
}

PlanCost CostModel::project_cost(const PlanCost& input) const {
    PlanCost cost;
    cost.startup_cost = input.startup_cost;
    cost.total_cost = input.total_cost + input.estimated_rows * params_.cpu_operator_cost;
    cost.estimated_rows = input.estimated_rows;
    return cost;
}

PlanCost CostModel::hash_aggregate_cost(const PlanCost& input, uint64_t num_groups) const {
    PlanCost cost;
    cost.startup_cost = input.total_cost + input.estimated_rows * params_.cpu_tuple_cost;
    cost.total_cost = cost.startup_cost + static_cast<double>(num_groups) * params_.cpu_tuple_cost;
    cost.estimated_rows = static_cast<double>(num_groups);
    return cost;
}

PlanCost CostModel::limit_cost(const PlanCost& input, uint64_t limit_rows) const {
    PlanCost cost;
    double fraction = input.estimated_rows > 0
                          ? std::min(1.0, static_cast<double>(limit_rows) / input.estimated_rows)
                          : 1.0;
    cost.startup_cost = input.startup_cost;
    cost.total_cost = input.startup_cost + (input.total_cost - input.startup_cost) * fraction;
    cost.estimated_rows = std::min(static_cast<double>(limit_rows), input.estimated_rows);
    return cost;
}

} // namespace sixseven
