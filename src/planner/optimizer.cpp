#include "giodb/planner/optimizer.h"

#include "giodb/common/logging.h"

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace giodb {

// =============================================================================
// Access path selection
// =============================================================================

AccessPath choose_access_path(table_id_t table_id,
                              const TableStats& table_stats,
                              const std::vector<IndexDef>& indexes,
                              double filter_selectivity,
                              const CostModel& cost_model) {
    // Always consider sequential scan.
    AccessPath best;
    best.method = AccessMethod::SEQ_SCAN;
    best.selectivity = 1.0;
    best.cost = cost_model.seq_scan_cost(table_stats.page_count, table_stats.row_count);

    // Consider each available index.
    for (const auto& idx : indexes) {
        if (idx.table_id != table_id) {
            continue;
        }

        // Estimate index pages as ~1% of heap pages (rough approximation).
        uint32_t index_pages = std::max(1u, table_stats.page_count / 100);

        auto idx_cost = cost_model.index_scan_cost(
            table_stats.page_count, table_stats.row_count, filter_selectivity, index_pages);

        if (idx_cost.total_cost < best.cost.total_cost) {
            best.method = AccessMethod::INDEX_SCAN;
            best.index_id = idx.index_id;
            best.selectivity = filter_selectivity;
            best.cost = idx_cost;
        }
    }

    return best;
}

// =============================================================================
// Join method selection
// =============================================================================

std::pair<JoinMethod, PlanCost> choose_join_method(const PlanCost& left,
                                                   const PlanCost& right,
                                                   double join_selectivity,
                                                   bool left_sorted,
                                                   bool right_sorted,
                                                   const CostModel& cost_model) {
    JoinMethod best_method = JoinMethod::HASH_JOIN;
    PlanCost best_cost;
    best_cost.total_cost = std::numeric_limits<double>::max();

    // Nested loop join.
    auto nl_cost = cost_model.nested_loop_join_cost(left, right, join_selectivity);
    if (nl_cost.total_cost < best_cost.total_cost) {
        best_method = JoinMethod::NESTED_LOOP;
        best_cost = nl_cost;
    }

    // Hash join: use smaller side as build, larger as probe.
    PlanCost hj_cost;
    if (left.estimated_rows <= right.estimated_rows) {
        hj_cost = cost_model.hash_join_cost(left, right, join_selectivity);
    } else {
        hj_cost = cost_model.hash_join_cost(right, left, join_selectivity);
    }
    if (hj_cost.total_cost < best_cost.total_cost) {
        best_method = JoinMethod::HASH_JOIN;
        best_cost = hj_cost;
    }

    // Sort-merge join.
    auto smj_cost =
        cost_model.sort_merge_join_cost(left, right, left_sorted, right_sorted, join_selectivity);
    if (smj_cost.total_cost < best_cost.total_cost) {
        best_method = JoinMethod::SORT_MERGE;
        best_cost = smj_cost;
    }

    return {best_method, best_cost};
}

// =============================================================================
// Join ordering helpers
// =============================================================================

/// Find the join selectivity between two table sets.
static double
find_join_selectivity(uint64_t left_set, uint64_t right_set, const std::vector<JoinEdge>& edges) {
    for (const auto& edge : edges) {
        uint64_t left_bit = 1ULL << edge.left_table;
        uint64_t right_bit = 1ULL << edge.right_table;

        if (((left_set & left_bit) != 0 && (right_set & right_bit) != 0) ||
            ((left_set & right_bit) != 0 && (right_set & left_bit) != 0)) {
            return edge.selectivity;
        }
    }
    // No join predicate: use Cartesian product selectivity (high cost).
    return 1.0;
}

/// Create a join PhysicalPlanNode.
static std::unique_ptr<PhysicalPlanNode> make_join_node(std::unique_ptr<PhysicalPlanNode> left,
                                                        std::unique_ptr<PhysicalPlanNode> right,
                                                        JoinMethod method,
                                                        const PlanCost& cost) {
    auto node = std::make_unique<PhysicalPlanNode>();
    node->type = PhysicalPlanNode::Type::JOIN;
    node->join_method = method;
    node->left = std::move(left);
    node->right = std::move(right);
    node->cost = cost;
    return node;
}

/// Deep-clone a PhysicalPlanNode tree.
static std::unique_ptr<PhysicalPlanNode> clone_plan(const PhysicalPlanNode& node) {
    auto cloned = std::make_unique<PhysicalPlanNode>();
    cloned->type = node.type;
    cloned->cost = node.cost;
    cloned->table_id = node.table_id;
    cloned->table_name = node.table_name;
    cloned->access_method = node.access_method;
    cloned->index_id = node.index_id;
    cloned->join_method = node.join_method;
    cloned->sort_orderings = node.sort_orderings;
    cloned->output_orderings = node.output_orderings;
    cloned->limit_rows = node.limit_rows;

    if (node.left) {
        cloned->left = clone_plan(*node.left);
    }
    if (node.right) {
        cloned->right = clone_plan(*node.right);
    }
    if (node.input) {
        cloned->input = clone_plan(*node.input);
    }
    return cloned;
}

// =============================================================================
// Dynamic programming join ordering (for <= 10 tables)
// =============================================================================

static std::unique_ptr<PhysicalPlanNode> dp_join_order(std::vector<JoinRelation>& base_relations,
                                                       const std::vector<JoinEdge>& join_edges,
                                                       const CostModel& cost_model) {
    size_t n = base_relations.size();
    if (n == 0) {
        return nullptr;
    }
    if (n == 1) {
        return std::move(base_relations[0].plan);
    }

    // dp[set] = best plan for the given table set.
    std::unordered_map<uint64_t, JoinRelation> dp;

    // Initialize with base relations.
    for (auto& rel : base_relations) {
        dp[rel.table_set] = JoinRelation{rel.table_set, rel.cost, clone_plan(*rel.plan)};
    }

    // Build up plans for increasingly larger subsets.
    uint64_t full_set = 0;
    for (const auto& rel : base_relations) {
        full_set |= rel.table_set;
    }

    // Enumerate all subsets of full_set in order of increasing size.
    for (uint64_t subset = 1; subset <= full_set; ++subset) {
        if ((subset & full_set) != subset) {
            continue; // Not a valid subset of our tables.
        }

        // Try all ways to split this subset into two non-empty parts.
        for (uint64_t left_set = (subset - 1) & subset; left_set > 0;
             left_set = (left_set - 1) & subset) {
            uint64_t right_set = subset ^ left_set;
            if (right_set == 0 || left_set >= right_set) {
                continue; // Avoid duplicates and empty sets.
            }

            auto left_it = dp.find(left_set);
            auto right_it = dp.find(right_set);
            if (left_it == dp.end() || right_it == dp.end()) {
                continue;
            }

            double sel = find_join_selectivity(left_set, right_set, join_edges);
            auto [method, join_cost] = choose_join_method(
                left_it->second.cost, right_it->second.cost, sel, false, false, cost_model);

            auto existing = dp.find(subset);
            if (existing == dp.end() || join_cost.total_cost < existing->second.cost.total_cost) {
                auto plan = make_join_node(clone_plan(*left_it->second.plan),
                                           clone_plan(*right_it->second.plan),
                                           method,
                                           join_cost);
                dp[subset] = JoinRelation{subset, join_cost, std::move(plan)};
            }
        }
    }

    auto final_it = dp.find(full_set);
    if (final_it != dp.end()) {
        return std::move(final_it->second.plan);
    }

    // Fallback: shouldn't happen, but return left-deep join of all tables.
    return std::move(base_relations[0].plan);
}

// =============================================================================
// Greedy join ordering (for > 10 tables)
// =============================================================================

static std::unique_ptr<PhysicalPlanNode>
greedy_join_order(std::vector<JoinRelation>& base_relations,
                  const std::vector<JoinEdge>& join_edges,
                  const CostModel& cost_model) {
    if (base_relations.empty()) {
        return nullptr;
    }
    if (base_relations.size() == 1) {
        return std::move(base_relations[0].plan);
    }

    // Start with the smallest relation.
    size_t best_idx = 0;
    for (size_t i = 1; i < base_relations.size(); ++i) {
        if (base_relations[i].cost.estimated_rows < base_relations[best_idx].cost.estimated_rows) {
            best_idx = i;
        }
    }

    JoinRelation current = {
        base_relations[best_idx].table_set,
        base_relations[best_idx].cost,
        std::move(base_relations[best_idx].plan),
    };
    base_relations.erase(base_relations.begin() + static_cast<ptrdiff_t>(best_idx));

    // Greedily pick the cheapest join partner at each step.
    while (!base_relations.empty()) {
        size_t best_partner = 0;
        JoinMethod best_method = JoinMethod::HASH_JOIN;
        PlanCost best_cost;
        best_cost.total_cost = std::numeric_limits<double>::max();

        for (size_t i = 0; i < base_relations.size(); ++i) {
            double sel =
                find_join_selectivity(current.table_set, base_relations[i].table_set, join_edges);
            auto [method, jcost] = choose_join_method(
                current.cost, base_relations[i].cost, sel, false, false, cost_model);

            if (jcost.total_cost < best_cost.total_cost) {
                best_partner = i;
                best_method = method;
                best_cost = jcost;
            }
        }

        auto plan = make_join_node(std::move(current.plan),
                                   std::move(base_relations[best_partner].plan),
                                   best_method,
                                   best_cost);

        current.table_set |= base_relations[best_partner].table_set;
        current.cost = best_cost;
        current.plan = std::move(plan);

        base_relations.erase(base_relations.begin() + static_cast<ptrdiff_t>(best_partner));
    }

    return std::move(current.plan);
}

// =============================================================================
// Public API
// =============================================================================

std::unique_ptr<PhysicalPlanNode> optimize_join_order(std::vector<JoinRelation>& base_relations,
                                                      const std::vector<JoinEdge>& join_edges,
                                                      const CostModel& cost_model) {
    static constexpr size_t kDpThreshold = 10;

    if (base_relations.size() <= kDpThreshold) {
        return dp_join_order(base_relations, join_edges, cost_model);
    }
    return greedy_join_order(base_relations, join_edges, cost_model);
}

} // namespace giodb
