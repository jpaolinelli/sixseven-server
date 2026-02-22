#pragma once

#include "giodb/executor/iterator.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace giodb {

/// Sort-Merge Join operator: sort both inputs on their join key, then
/// merge in a single pass.
///
/// Supports INNER, LEFT, RIGHT, and FULL OUTER join types.
/// Both sides are materialised and sorted on `open()`, then the merge
/// produces output tuples via `next()`.
class SortMergeJoinOperator : public Iterator {
public:
    /// @param left      Left child iterator.
    /// @param right     Right child iterator.
    /// @param type      Join type (INNER, LEFT, RIGHT, FULL).
    /// @param left_key  Expression to extract the sort/join key from left tuples.
    /// @param right_key Expression to extract the sort/join key from right tuples.
    /// @param bound     BoundStatement with expr_types map.
    /// @param schema    Combined output schema (left columns + right columns).
    SortMergeJoinOperator(std::unique_ptr<Iterator> left,
                          std::unique_ptr<Iterator> right,
                          JoinType type,
                          const Expr* left_key,
                          const Expr* right_key,
                          const BoundStatement& bound,
                          OutputSchema schema);

    Result<void> open() override;
    Result<std::optional<Tuple>> next() override;
    void close() override;
    const OutputSchema& output_schema() const override;

private:
    std::unique_ptr<Iterator> left_;
    std::unique_ptr<Iterator> right_;
    JoinType type_;
    const Expr* left_key_;
    const Expr* right_key_;
    const BoundStatement& bound_;
    OutputSchema schema_;

    size_t left_col_count_ = 0;
    size_t right_col_count_ = 0;

    // Pre-computed output after open().
    std::vector<Tuple> output_;
    size_t output_cursor_ = 0;
};

} // namespace giodb
