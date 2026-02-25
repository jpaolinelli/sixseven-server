#pragma once

#include "giodb/executor/iterator.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace giodb {

/// Identifies which aggregate function to compute.
enum class AggFunc : uint8_t {
    COUNT_STAR,
    COUNT,
    COUNT_DISTINCT,
    SUM,
    AVG,
    MIN,
    MAX,
    STRING_AGG,
};

/// Describes a single aggregate computation.
struct AggregateDescriptor {
    AggFunc func;
    const Expr* arg = nullptr; ///< Column expression (nullptr for COUNT(*)).
    std::string separator;     ///< Separator string for STRING_AGG.
};

/// Hash Aggregate operator: groups input tuples by GROUP BY expressions
/// and computes aggregate functions per group.
///
/// The operator materialises all child tuples in open(), builds a hash
/// table keyed on GROUP BY column values, accumulates aggregate state
/// per group, and emits one output tuple per group in next().
///
/// Output schema layout: [group_by_col_0, ..., group_by_col_n, agg_0, ..., agg_m]
///
/// Supports: COUNT(*), COUNT(col), COUNT(DISTINCT col), SUM, AVG, MIN,
/// MAX, STRING_AGG. NULL handling follows the SQL standard.
class HashAggregateOperator : public Iterator {
public:
    /// @param child          Child iterator to read input tuples from.
    /// @param group_by_exprs GROUP BY expressions (may be empty for global aggregation).
    /// @param aggregates     Aggregate function descriptors.
    /// @param bound          BoundStatement with expr_types map.
    /// @param schema         Output schema (GROUP BY columns + aggregate result columns).
    HashAggregateOperator(std::unique_ptr<Iterator> child,
                          std::vector<const Expr*> group_by_exprs,
                          std::vector<AggregateDescriptor> aggregates,
                          const BoundStatement& bound,
                          OutputSchema schema);

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
    /// Hash a vector of Values (the group key).
    static size_t hash_key(const std::vector<Value>& key);

    /// Check if two group keys are equal.
    static bool keys_equal(const std::vector<Value>& a, const std::vector<Value>& b);

    std::unique_ptr<Iterator> child_;
    std::vector<const Expr*> group_by_exprs_;
    std::vector<AggregateDescriptor> aggregates_;
    const BoundStatement& bound_;
    OutputSchema schema_;

    /// Pre-computed output tuples (one per group), populated in open().
    std::vector<Tuple> output_;
    size_t cursor_ = 0;
};

} // namespace giodb
