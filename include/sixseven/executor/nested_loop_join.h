#pragma once

#include "sixseven/executor/iterator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace sixseven {

/// Nested Loop Join operator: for each tuple from the left (outer) child,
/// scan all tuples from the right (inner) child and evaluate the join predicate.
///
/// Supports all join types: INNER, LEFT, RIGHT, FULL, CROSS, SEMI, ANTI.
/// The right child is materialised in memory on open().
class NestedLoopJoinOperator : public Iterator {
public:
    /// @param left     Left (outer) child iterator.
    /// @param right    Right (inner) child iterator.
    /// @param type     Join type (INNER, LEFT, RIGHT, FULL, CROSS, SEMI, ANTI).
    /// @param on_expr  Join predicate (nullptr for CROSS JOIN).
    /// @param bound    BoundStatement with expr_types map.
    /// @param schema   Combined output schema (left columns + right columns).
    NestedLoopJoinOperator(std::unique_ptr<Iterator> left,
                           std::unique_ptr<Iterator> right,
                           JoinType type,
                           const Expr* on_expr,
                           const BoundStatement& bound,
                           OutputSchema schema);

    const OutputSchema& output_schema() const override;
    std::string plan_node_name() const override;
    std::string plan_node_detail() const override;
    std::vector<const Iterator*> plan_children() const override;

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;
    std::vector<Iterator*> plan_children_mutable() override;

private:
    /// Combine a left and right tuple into a single output tuple.
    Tuple combine(const Tuple& left, const Tuple& right) const;

    /// Create a tuple with NULLs for the right side.
    Tuple left_with_null_right(const Tuple& left) const;

    /// Create a tuple with NULLs for the left side.
    Tuple null_left_with_right(const Tuple& right) const;

    /// Evaluate the join predicate against a combined tuple.
    Result<bool> matches(const Tuple& combined) const;

    std::unique_ptr<Iterator> left_;
    std::unique_ptr<Iterator> right_;
    JoinType type_;
    const Expr* on_expr_;
    const BoundStatement& bound_;
    OutputSchema schema_;        // Combined schema for ON condition evaluation.
    OutputSchema output_schema_; // Actual output schema (left-only for SEMI/ANTI).

    size_t left_col_count_ = 0;
    size_t right_col_count_ = 0;

    // Materialised right-side tuples.
    std::vector<Tuple> right_tuples_;

    // Iteration state.
    std::optional<Tuple> current_left_;
    size_t right_cursor_ = 0;
    bool left_exhausted_ = false;
    bool left_matched_ = false;

    // For RIGHT/FULL: track which right tuples matched.
    std::vector<bool> right_matched_;

    // For RIGHT/FULL: emit unmatched right tuples after left is exhausted.
    bool emitting_unmatched_right_ = false;
    size_t unmatched_right_cursor_ = 0;

    // For null-aware ANTI join (NOT IN with NULLs).
    bool null_aware_anti_ = false;
    bool saw_null_ = false;

public:
    /// Enable null-aware ANTI join semantics for NOT IN subquery rewrites.
    /// When set, a NULL comparison result suppresses emission of the outer row.
    void set_null_aware_anti(bool v) { null_aware_anti_ = v; }
};

} // namespace sixseven
