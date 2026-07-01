#pragma once

#include "sixseven/common/result.h"
#include "sixseven/executor/iterator.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/parser/ast.h"

#include <string>

namespace sixseven {

/// Render an expression tree back to a SQL-like string (e.g. "(age > 25)").
/// Used both for catalog round-tripping of default expressions and for
/// surfacing WHERE predicates in EXPLAIN plan output. Unsupported node types
/// fall back to "NULL".
[[nodiscard]] std::string expr_to_sql(const Expr& expr);

/// Formats an iterator tree as EXPLAIN or EXPLAIN ANALYZE output.
///
/// Walks the tree using plan_node_name(), plan_node_detail(), and
/// plan_children(). For EXPLAIN ANALYZE, also reports actual row counts
/// and execution timing per operator.
class ExplainFormatter {
public:
    /// Format the plan as a text tree (indented lines).
    [[nodiscard]] static std::string format_text(const Iterator& root, bool analyze);

    /// Format the plan as a JSON string.
    [[nodiscard]] static std::string format_json(const Iterator& root, bool analyze);

    /// Build a QueryResult containing the EXPLAIN output.
    [[nodiscard]] static QueryResult
    to_query_result(const Iterator& root, ExplainFormat format, bool analyze);

private:
    /// Recursive helper for text formatting.
    static void format_text_node(const Iterator& node,
                                 bool analyze,
                                 int depth,
                                 bool is_last,
                                 const std::string& prefix,
                                 std::string& out);
};

} // namespace sixseven
