#pragma once

#include "giodb/catalog/catalog.h"
#include "giodb/planner/cost_model.h"
#include "giodb/planner/statistics.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace giodb {

// =============================================================================
// Physical plan node types
// =============================================================================

/// Access method for a single table.
enum class AccessMethod : uint8_t {
    SEQ_SCAN,
    INDEX_SCAN,
};

/// Join algorithm.
enum class JoinMethod : uint8_t {
    NESTED_LOOP,
    HASH_JOIN,
    SORT_MERGE,
};

/// Sort ordering for an output.
struct SortOrdering {
    table_id_t table_id = 0;
    int32_t column_ordinal = 0;
    bool ascending = true;
};

/// A physical plan node produced by the optimizer.
/// This is a lightweight description of the chosen physical plan,
/// which can be translated to Volcano iterators by the executor.
struct PhysicalPlanNode {
    enum class Type : uint8_t {
        TABLE_SCAN,
        JOIN,
        SORT,
        FILTER,
        PROJECT,
        AGGREGATE,
        LIMIT,
    };

    Type type = Type::TABLE_SCAN;
    PlanCost cost;

    // -- TABLE_SCAN fields --
    table_id_t table_id = 0;
    std::string table_name;
    AccessMethod access_method = AccessMethod::SEQ_SCAN;
    index_id_t index_id = 0; ///< If INDEX_SCAN, which index to use.

    // -- JOIN fields --
    JoinMethod join_method = JoinMethod::HASH_JOIN;
    std::unique_ptr<PhysicalPlanNode> left;
    std::unique_ptr<PhysicalPlanNode> right;

    // -- SORT fields --
    std::vector<SortOrdering> sort_orderings;
    std::unique_ptr<PhysicalPlanNode> input;

    // -- Interesting orderings that this node produces --
    std::vector<SortOrdering> output_orderings;

    // -- LIMIT fields --
    uint64_t limit_rows = 0;
};

// =============================================================================
// Access path selection
// =============================================================================

/// Represents a candidate access path for a single table.
struct AccessPath {
    AccessMethod method = AccessMethod::SEQ_SCAN;
    index_id_t index_id = 0;
    double selectivity = 1.0; ///< For index scans.
    PlanCost cost;
    std::vector<SortOrdering> output_orderings;
};

/// Choose the best access path for a single table given available indexes
/// and the filter selectivity.
[[nodiscard]] AccessPath choose_access_path(table_id_t table_id,
                                            const TableStats& table_stats,
                                            const std::vector<IndexDef>& indexes,
                                            double filter_selectivity,
                                            const CostModel& cost_model);

// =============================================================================
// Join ordering
// =============================================================================

/// Represents a relation (table or intermediate result) in join ordering.
struct JoinRelation {
    uint64_t table_set = 0; ///< Bitmask of tables in this relation.
    PlanCost cost;
    std::unique_ptr<PhysicalPlanNode> plan;
};

/// Join predicate between two tables.
struct JoinEdge {
    table_id_t left_table = 0;
    table_id_t right_table = 0;
    double selectivity = 0.1; ///< Estimated join selectivity.
};

/// Choose the best join order and algorithm for a set of tables.
/// Uses dynamic programming for <= 10 tables, greedy heuristic otherwise.
///
/// @param base_relations  Best access paths for each individual table.
/// @param join_edges      Available join predicates between tables.
/// @param cost_model      Cost model for evaluating alternatives.
/// @return The best join plan as a PhysicalPlanNode tree.
[[nodiscard]] std::unique_ptr<PhysicalPlanNode>
optimize_join_order(std::vector<JoinRelation>& base_relations,
                    const std::vector<JoinEdge>& join_edges,
                    const CostModel& cost_model);

/// Choose the best join algorithm for joining two relations.
[[nodiscard]] std::pair<JoinMethod, PlanCost> choose_join_method(const PlanCost& left,
                                                                 const PlanCost& right,
                                                                 double join_selectivity,
                                                                 bool left_sorted,
                                                                 bool right_sorted,
                                                                 const CostModel& cost_model);

} // namespace giodb
