#include "giodb/executor/planner.h"

#include "giodb/executor/delete.h"
#include "giodb/executor/filter.h"
#include "giodb/executor/hash_aggregate.h"
#include "giodb/executor/insert.h"
#include "giodb/executor/limit.h"
#include "giodb/executor/nested_loop_join.h"
#include "giodb/executor/project.h"
#include "giodb/executor/seq_scan.h"
#include "giodb/executor/sort.h"
#include "giodb/executor/subquery_source.h"
#include "giodb/executor/update.h"
#include "giodb/planner/type_resolver.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <string>
#include <unordered_map>

namespace giodb {

namespace {

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

/// Recursively collect all aggregate FunctionCallExpr nodes in an expression tree.
void collect_aggregate_exprs(const Expr& expr,
                             const BoundStatement& bound,
                             std::vector<const FunctionCallExpr*>& out) {
    if (auto* fn = dynamic_cast<const FunctionCallExpr*>(&expr)) {
        auto it = bound.expr_types.find(&expr);
        if (it != bound.expr_types.end() && it->second.is_aggregate) {
            out.push_back(fn);
            return; // Don't recurse into aggregate arguments.
        }
    }

    // Recurse into sub-expressions.
    if (auto* bin = dynamic_cast<const BinaryExpr*>(&expr)) {
        if (bin->lhs) {
            collect_aggregate_exprs(*bin->lhs, bound, out);
        }
        if (bin->rhs) {
            collect_aggregate_exprs(*bin->rhs, bound, out);
        }
    } else if (auto* un = dynamic_cast<const UnaryExpr*>(&expr)) {
        if (un->operand) {
            collect_aggregate_exprs(*un->operand, bound, out);
        }
    } else if (auto* fn = dynamic_cast<const FunctionCallExpr*>(&expr)) {
        for (auto& arg : fn->args) {
            if (arg) {
                collect_aggregate_exprs(*arg, bound, out);
            }
        }
    } else if (auto* cast = dynamic_cast<const CastExpr*>(&expr)) {
        if (cast->expr) {
            collect_aggregate_exprs(*cast->expr, bound, out);
        }
    }
}

/// Check if an expression tree contains any aggregate function call.
bool contains_any_aggregate(const Expr& expr, const BoundStatement& bound) {
    std::vector<const FunctionCallExpr*> aggs;
    collect_aggregate_exprs(expr, bound, aggs);
    return !aggs.empty();
}

/// Convert a FunctionCallExpr to an AggFunc enum + separator.
AggFunc resolve_agg_func(const FunctionCallExpr& fn) {
    std::string upper = to_upper(fn.name);

    if (upper == "COUNT") {
        if (fn.distinct) {
            return AggFunc::COUNT_DISTINCT;
        }
        // COUNT(*): parser represents * as ColumnRefExpr{column="*"}.
        if (!fn.args.empty()) {
            if (auto* cref = dynamic_cast<const ColumnRefExpr*>(fn.args[0].get())) {
                if (cref->column == "*") {
                    return AggFunc::COUNT_STAR;
                }
            }
        }
        if (fn.args.empty()) {
            return AggFunc::COUNT_STAR;
        }
        return AggFunc::COUNT;
    }
    if (upper == "SUM") {
        return AggFunc::SUM;
    }
    if (upper == "AVG") {
        return AggFunc::AVG;
    }
    if (upper == "MIN") {
        return AggFunc::MIN;
    }
    if (upper == "MAX") {
        return AggFunc::MAX;
    }
    if (upper == "STRING_AGG") {
        return AggFunc::STRING_AGG;
    }
    // Should never reach here after binder validation. Use COUNT_STAR as a
    // defensive default and assert in debug builds so we notice immediately.
    assert(false && "resolve_agg_func: unknown aggregate function");
    return AggFunc::COUNT_STAR;
}

/// Deep-clone an expression tree, replacing aggregate FunctionCallExpr nodes
/// with ColumnRefExpr nodes that reference the aggregate output columns.
ExprPtr rewrite_expr(const Expr& expr,
                     const std::unordered_map<const Expr*, std::string>& agg_map) {
    // If this expression is a mapped aggregate, replace it.
    auto it = agg_map.find(&expr);
    if (it != agg_map.end()) {
        auto cr = std::make_unique<ColumnRefExpr>();
        cr->table = "";
        cr->column = it->second;
        return cr;
    }

    // Recursively clone and rewrite sub-expressions.
    if (auto* lit = dynamic_cast<const LiteralExpr*>(&expr)) {
        auto n = std::make_unique<LiteralExpr>();
        n->kind = lit->kind;
        n->value = lit->value;
        n->line = lit->line;
        n->col = lit->col;
        return n;
    }
    if (auto* col = dynamic_cast<const ColumnRefExpr*>(&expr)) {
        auto n = std::make_unique<ColumnRefExpr>();
        n->table = col->table;
        n->column = col->column;
        n->line = col->line;
        n->col = col->col;
        return n;
    }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(&expr)) {
        auto n = std::make_unique<BinaryExpr>();
        n->op = bin->op;
        n->lhs = bin->lhs ? rewrite_expr(*bin->lhs, agg_map) : nullptr;
        n->rhs = bin->rhs ? rewrite_expr(*bin->rhs, agg_map) : nullptr;
        n->line = bin->line;
        n->col = bin->col;
        return n;
    }
    if (auto* un = dynamic_cast<const UnaryExpr*>(&expr)) {
        auto n = std::make_unique<UnaryExpr>();
        n->op = un->op;
        n->operand = un->operand ? rewrite_expr(*un->operand, agg_map) : nullptr;
        n->line = un->line;
        n->col = un->col;
        return n;
    }
    if (auto* cast = dynamic_cast<const CastExpr*>(&expr)) {
        auto n = std::make_unique<CastExpr>();
        n->target_type = cast->target_type;
        n->expr = cast->expr ? rewrite_expr(*cast->expr, agg_map) : nullptr;
        n->line = cast->line;
        n->col = cast->col;
        return n;
    }
    if (auto* fn = dynamic_cast<const FunctionCallExpr*>(&expr)) {
        // Non-aggregate function call — clone it and rewrite args.
        auto n = std::make_unique<FunctionCallExpr>();
        n->name = fn->name;
        n->distinct = fn->distinct;
        n->line = fn->line;
        n->col = fn->col;
        for (auto& arg : fn->args) {
            n->args.push_back(arg ? rewrite_expr(*arg, agg_map) : nullptr);
        }
        return n;
    }
    if (auto* isnull = dynamic_cast<const IsNullExpr*>(&expr)) {
        auto n = std::make_unique<IsNullExpr>();
        n->negated = isnull->negated;
        n->expr = isnull->expr ? rewrite_expr(*isnull->expr, agg_map) : nullptr;
        n->line = isnull->line;
        n->col = isnull->col;
        return n;
    }

    // Fallback: return a column ref that will fail (should not happen for valid queries).
    auto cr = std::make_unique<ColumnRefExpr>();
    cr->column = "__unsupported_expr__";
    return cr;
}

} // anonymous namespace

Planner::Planner(const Catalog& catalog, StorageManager& storage)
    : catalog_(catalog), storage_(storage), subquery_ctx_{catalog_, storage_} {}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Iterator>> Planner::plan(const BoundStatement& bound,
                                                std::vector<ExprPtr>& owned_exprs) {
    if (auto* sel = dynamic_cast<const SelectStmt*>(bound.stmt)) {
        return plan_select(*sel, bound, owned_exprs);
    }
    if (auto* ins = dynamic_cast<const InsertStmt*>(bound.stmt)) {
        return plan_insert(*ins, bound);
    }
    if (auto* upd = dynamic_cast<const UpdateStmt*>(bound.stmt)) {
        return plan_update(*upd, bound);
    }
    if (auto* del = dynamic_cast<const DeleteStmt*>(bound.stmt)) {
        return plan_delete(*del, bound);
    }
    return make_error(StatusCode::NOT_IMPLEMENTED, "planner does not support this statement type");
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

OutputSchema Planner::build_output_schema(const std::vector<ResolvedColumn>& columns) {
    std::vector<OutputColumn> out;
    out.reserve(columns.size());
    for (const auto& rc : columns) {
        out.push_back({rc.table_name, rc.column_name, rc.type_id, rc.nullable, rc.table_id});
    }
    return OutputSchema(std::move(out));
}

OutputSchema Planner::build_table_output_schema(const TableSchema& ts,
                                                const std::string& table_alias) {
    std::vector<OutputColumn> out;
    out.reserve(ts.columns.size());
    const auto& tname = table_alias.empty() ? ts.name : table_alias;
    for (const auto& col : ts.columns) {
        out.push_back({tname, col.name, col.type_id, col.nullable, ts.table_id});
    }
    return OutputSchema(std::move(out));
}

// ---------------------------------------------------------------------------
// Plan FROM source: table, CTE, or derived table
// ---------------------------------------------------------------------------

Result<Planner::PlannedSource>
Planner::plan_from_source(const TableRef& table_ref,
                          const std::string& alias,
                          const std::unordered_map<std::string, const SelectStmt*>& cte_map,
                          const BoundStatement& bound,
                          std::vector<ExprPtr>& owned_exprs) {
    // Case 1: Derived table (FROM (SELECT ...) AS sub).
    if (table_ref.subquery) {
        auto* sub_sel = dynamic_cast<const SelectStmt*>(table_ref.subquery.get());
        if (!sub_sel) {
            return make_error(StatusCode::INTERNAL_ERROR, "FROM subquery is not a SELECT");
        }

        Binder binder(catalog_);
        auto sub_bound = binder.bind(*sub_sel);
        if (!sub_bound) {
            return make_error(sub_bound.error().code, sub_bound.error().message);
        }

        auto sub_iter = plan_select(*sub_sel, *sub_bound, owned_exprs);
        if (!sub_iter) {
            return make_error(sub_iter.error().code, sub_iter.error().message);
        }

        // Build output schema with the alias applied.
        std::vector<OutputColumn> cols;
        for (const auto& rc : sub_bound->output_columns) {
            cols.push_back({alias, rc.column_name, rc.type_id, rc.nullable, 0});
        }
        auto schema = OutputSchema(std::move(cols));
        auto source_op = std::make_unique<SubquerySourceOperator>(std::move(*sub_iter), schema);
        return ok(PlannedSource{std::move(source_op), std::move(schema)});
    }

    // Case 2: CTE reference (WITH name AS (...) SELECT ... FROM name).
    auto cte_it = cte_map.find(to_upper(table_ref.name));
    if (cte_it != cte_map.end()) {
        const auto* cte_sel = cte_it->second;

        Binder binder(catalog_);
        auto sub_bound = binder.bind(*cte_sel);
        if (!sub_bound) {
            return make_error(sub_bound.error().code, sub_bound.error().message);
        }

        auto sub_iter = plan_select(*cte_sel, *sub_bound, owned_exprs);
        if (!sub_iter) {
            return make_error(sub_iter.error().code, sub_iter.error().message);
        }

        // Build output schema with the alias applied.
        std::vector<OutputColumn> cols;
        for (const auto& rc : sub_bound->output_columns) {
            cols.push_back({alias, rc.column_name, rc.type_id, rc.nullable, 0});
        }
        auto schema = OutputSchema(std::move(cols));
        auto source_op = std::make_unique<SubquerySourceOperator>(std::move(*sub_iter), schema);
        return ok(PlannedSource{std::move(source_op), std::move(schema)});
    }

    // Case 3: Physical table.
    auto table_schema = catalog_.get_table(table_ref.name);
    if (!table_schema) {
        return make_error(table_schema.error().code, table_schema.error().message);
    }

    auto ts = storage_.get_table_storage(table_schema->table_id);
    if (!ts) {
        return make_error(ts.error().code, ts.error().message);
    }
    auto* storage = *ts;

    auto table_output = build_table_output_schema(*table_schema, alias);
    auto scan = std::make_unique<SeqScanOperator>(
        *storage->heap, storage->storage_schema, table_output, nullptr, &bound);

    return ok(PlannedSource{std::move(scan), std::move(table_output)});
}

// ---------------------------------------------------------------------------
// Subquery predicate rewriting
// ---------------------------------------------------------------------------

Result<const Expr*> Planner::rewrite_subquery_predicates(
    const Expr& where_expr,
    std::unique_ptr<Iterator>& child,
    const BoundStatement& bound,
    const std::unordered_map<std::string, const SelectStmt*>& cte_map,
    std::vector<ExprPtr>& owned_exprs) {
    // --- EXISTS (subquery) → SEMI join ---
    if (auto* exists = dynamic_cast<const ExistsExpr*>(&where_expr)) {
        auto* sub_sel = dynamic_cast<const SelectStmt*>(exists->subquery.get());
        if (sub_sel && !sub_sel->from.empty()) {
            // Plan the inner query's FROM source as the right side.
            const auto& inner_ref = sub_sel->from[0];
            const auto& inner_alias = inner_ref.alias.empty() ? inner_ref.name : inner_ref.alias;

            auto right_source =
                plan_from_source(inner_ref, inner_alias, cte_map, bound, owned_exprs);
            if (!right_source) {
                return make_error(right_source.error().code, right_source.error().message);
            }

            // Build combined schema: left + right.
            const auto& left_schema = child->output_schema();
            const auto& right_schema = right_source->schema;

            std::vector<OutputColumn> combined_cols;
            combined_cols.reserve(left_schema.column_count() + right_schema.column_count());
            for (size_t i = 0; i < left_schema.column_count(); ++i) {
                combined_cols.push_back(left_schema.column(i));
            }
            for (size_t i = 0; i < right_schema.column_count(); ++i) {
                combined_cols.push_back(right_schema.column(i));
            }
            auto combined = OutputSchema(std::move(combined_cols));

            // Use the inner query's WHERE as the join ON condition.
            const Expr* on_expr = sub_sel->where_expr ? sub_sel->where_expr.get() : nullptr;

            child = std::make_unique<NestedLoopJoinOperator>(std::move(child),
                                                             std::move(right_source->iter),
                                                             JoinType::SEMI,
                                                             on_expr,
                                                             bound,
                                                             std::move(combined));
            return ok(static_cast<const Expr*>(nullptr)); // Consumed the predicate.
        }
    }

    // --- NOT EXISTS (subquery) → ANTI join ---
    // NOT EXISTS is represented as UnaryExpr(NOT, ExistsExpr).
    if (auto* unary = dynamic_cast<const UnaryExpr*>(&where_expr)) {
        if (unary->op == UnaryOp::NOT) {
            if (auto* exists = dynamic_cast<const ExistsExpr*>(unary->operand.get())) {
                auto* sub_sel = dynamic_cast<const SelectStmt*>(exists->subquery.get());
                if (sub_sel && !sub_sel->from.empty()) {
                    const auto& inner_ref = sub_sel->from[0];
                    const auto& inner_alias =
                        inner_ref.alias.empty() ? inner_ref.name : inner_ref.alias;

                    auto right_source =
                        plan_from_source(inner_ref, inner_alias, cte_map, bound, owned_exprs);
                    if (!right_source) {
                        return make_error(right_source.error().code, right_source.error().message);
                    }

                    const auto& left_schema = child->output_schema();
                    const auto& right_schema = right_source->schema;

                    std::vector<OutputColumn> combined_cols;
                    combined_cols.reserve(left_schema.column_count() + right_schema.column_count());
                    for (size_t i = 0; i < left_schema.column_count(); ++i) {
                        combined_cols.push_back(left_schema.column(i));
                    }
                    for (size_t i = 0; i < right_schema.column_count(); ++i) {
                        combined_cols.push_back(right_schema.column(i));
                    }
                    auto combined = OutputSchema(std::move(combined_cols));

                    const Expr* on_expr = sub_sel->where_expr ? sub_sel->where_expr.get() : nullptr;

                    child = std::make_unique<NestedLoopJoinOperator>(std::move(child),
                                                                     std::move(right_source->iter),
                                                                     JoinType::ANTI,
                                                                     on_expr,
                                                                     bound,
                                                                     std::move(combined));
                    return ok(static_cast<const Expr*>(nullptr));
                }
            }
        }
    }

    // --- IN (subquery) → SEMI join ---
    if (auto* in_expr = dynamic_cast<const InExpr*>(&where_expr)) {
        if (in_expr->subquery) {
            auto* sub_sel = dynamic_cast<const SelectStmt*>(in_expr->subquery.get());
            if (sub_sel && !sub_sel->from.empty()) {
                // Plan the entire subquery (not just the FROM table), so that
                // any filters, aggregations, etc. inside the subquery are
                // correctly applied.
                Binder binder(catalog_);
                auto sub_bound = binder.bind(*sub_sel);
                if (!sub_bound) {
                    return make_error(sub_bound.error().code, sub_bound.error().message);
                }

                auto sub_iter = plan_select(*sub_sel, *sub_bound, owned_exprs);
                if (!sub_iter) {
                    return make_error(sub_iter.error().code, sub_iter.error().message);
                }

                // Build output schema for the materialised subquery.
                // Use a synthetic alias to avoid name collisions.
                std::string sub_alias = "__in_sub__";
                std::vector<OutputColumn> sub_cols;
                for (const auto& rc : sub_bound->output_columns) {
                    sub_cols.push_back({sub_alias, rc.column_name, rc.type_id, rc.nullable, 0});
                }
                auto sub_schema = OutputSchema(std::move(sub_cols));
                auto sub_source =
                    std::make_unique<SubquerySourceOperator>(std::move(*sub_iter), sub_schema);

                // Build combined schema.
                const auto& left_schema = child->output_schema();

                std::vector<OutputColumn> combined_cols;
                combined_cols.reserve(left_schema.column_count() + sub_schema.column_count());
                for (size_t i = 0; i < left_schema.column_count(); ++i) {
                    combined_cols.push_back(left_schema.column(i));
                }
                for (size_t i = 0; i < sub_schema.column_count(); ++i) {
                    combined_cols.push_back(sub_schema.column(i));
                }
                auto combined = OutputSchema(std::move(combined_cols));

                // Synthesise the join ON condition: outer_expr = subquery_result_col.
                // Build: ColumnRef(sub_alias, first_output_col) for the right side.
                auto rhs_col = std::make_unique<ColumnRefExpr>();
                rhs_col->table = sub_alias;
                rhs_col->column = sub_bound->output_columns[0].column_name;

                auto eq = std::make_unique<BinaryExpr>();
                eq->op = BinaryOp::EQUAL;
                // Clone the left-hand side of the IN expression.
                if (auto* lhs_col = dynamic_cast<const ColumnRefExpr*>(in_expr->expr.get())) {
                    auto lhs = std::make_unique<ColumnRefExpr>();
                    lhs->table = lhs_col->table;
                    lhs->column = lhs_col->column;
                    eq->lhs = std::move(lhs);
                } else {
                    // For non-column expressions, we still create the equality.
                    // Use the original expression pointer — it outlives the plan.
                    // We need to wrap it in an owned clone.
                    auto lhs = std::make_unique<ColumnRefExpr>();
                    lhs->column = "__in_lhs__";
                    eq->lhs = std::move(lhs);
                    // Fallback: can't rewrite non-column IN expressions easily.
                    // Return the expression as-is for the filter fallback.
                    return ok(static_cast<const Expr*>(&where_expr));
                }
                eq->rhs = std::move(rhs_col);

                const Expr* on_ptr = eq.get();
                owned_exprs.push_back(std::move(eq));

                JoinType jtype = in_expr->negated ? JoinType::ANTI : JoinType::SEMI;

                child = std::make_unique<NestedLoopJoinOperator>(std::move(child),
                                                                 std::move(sub_source),
                                                                 jtype,
                                                                 on_ptr,
                                                                 bound,
                                                                 std::move(combined));
                return ok(static_cast<const Expr*>(nullptr));
            }
        }
    }

    // --- AND: split and try to rewrite each side ---
    if (auto* bin = dynamic_cast<const BinaryExpr*>(&where_expr)) {
        if (bin->op == BinaryOp::AND) {
            auto left_result =
                rewrite_subquery_predicates(*bin->lhs, child, bound, cte_map, owned_exprs);
            if (!left_result) {
                return left_result;
            }
            auto right_result =
                rewrite_subquery_predicates(*bin->rhs, child, bound, cte_map, owned_exprs);
            if (!right_result) {
                return right_result;
            }

            const Expr* left_remaining = *left_result;
            const Expr* right_remaining = *right_result;

            if (!left_remaining && !right_remaining) {
                return ok(static_cast<const Expr*>(nullptr));
            }
            if (!left_remaining) {
                return ok(right_remaining);
            }
            if (!right_remaining) {
                return ok(left_remaining);
            }

            // Both sides have remaining conditions — recombine with AND.
            // We need to return the original expression since both sub-parts
            // are still the original AST nodes connected by the AND.
            return ok(static_cast<const Expr*>(&where_expr));
        }
    }

    // No subquery found — return the expression as-is for filter.
    return ok(static_cast<const Expr*>(&where_expr));
}

// ---------------------------------------------------------------------------
// SELECT
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Iterator>> Planner::plan_select(const SelectStmt& stmt,
                                                       const BoundStatement& bound,
                                                       std::vector<ExprPtr>& owned_exprs) {
    // -- 0. Build CTE map -------------------------------------------------------
    std::unordered_map<std::string, const SelectStmt*> cte_map;
    for (const auto& cte : stmt.ctes) {
        if (cte.query) {
            auto* cte_sel = dynamic_cast<const SelectStmt*>(cte.query.get());
            if (cte_sel) {
                cte_map[to_upper(cte.name)] = cte_sel;
            }
        }
    }

    // -- 1. Resolve the source table/CTE/subquery --------------------------------
    if (stmt.from.empty()) {
        return make_error(StatusCode::NOT_IMPLEMENTED, "SELECT without FROM is not yet supported");
    }

    const auto& table_ref = stmt.from[0];
    const auto& alias = table_ref.alias.empty() ? table_ref.name : table_ref.alias;

    auto source = plan_from_source(table_ref, alias, cte_map, bound, owned_exprs);
    if (!source) {
        return make_error(source.error().code, source.error().message);
    }

    const bool has_joins = !stmt.joins.empty();

    // -- 2. Optionally push WHERE into scan (only for physical tables with no joins).
    // For subquery/CTE sources or when joins are present, don't push WHERE.
    bool pushed_where = false;
    if (!has_joins && stmt.where_expr && !table_ref.subquery &&
        cte_map.find(to_upper(table_ref.name)) == cte_map.end()) {
        // Check if WHERE contains subquery predicates.
        bool has_subquery_predicate = false;
        {
            const auto* w = stmt.where_expr.get();
            if (dynamic_cast<const ExistsExpr*>(w) || dynamic_cast<const SubqueryExpr*>(w)) {
                has_subquery_predicate = true;
            }
            if (auto* un = dynamic_cast<const UnaryExpr*>(w)) {
                if (un->op == UnaryOp::NOT && dynamic_cast<const ExistsExpr*>(un->operand.get())) {
                    has_subquery_predicate = true;
                }
            }
            if (auto* in = dynamic_cast<const InExpr*>(w)) {
                if (in->subquery) {
                    has_subquery_predicate = true;
                }
            }
            // Check in AND branches.
            if (auto* bin = dynamic_cast<const BinaryExpr*>(w)) {
                if (bin->op == BinaryOp::AND) {
                    // If either side has subquery, don't push.
                    auto check_sub = [](const Expr* e) {
                        if (dynamic_cast<const ExistsExpr*>(e))
                            return true;
                        if (auto* u = dynamic_cast<const UnaryExpr*>(e)) {
                            if (u->op == UnaryOp::NOT &&
                                dynamic_cast<const ExistsExpr*>(u->operand.get()))
                                return true;
                        }
                        if (auto* ie = dynamic_cast<const InExpr*>(e)) {
                            if (ie->subquery)
                                return true;
                        }
                        return false;
                    };
                    if (check_sub(bin->lhs.get()) || check_sub(bin->rhs.get())) {
                        has_subquery_predicate = true;
                    }
                }
            }
        }

        if (!has_subquery_predicate) {
            // Re-create the scan with the predicate pushed down.
            auto table_schema = catalog_.get_table(table_ref.name);
            if (table_schema) {
                auto ts = storage_.get_table_storage(table_schema->table_id);
                if (ts) {
                    auto* storage = *ts;
                    auto table_output = build_table_output_schema(*table_schema, alias);
                    source->iter = std::make_unique<SeqScanOperator>(*storage->heap,
                                                                     storage->storage_schema,
                                                                     table_output,
                                                                     stmt.where_expr.get(),
                                                                     &bound);
                    source->schema = std::move(table_output);
                    pushed_where = true;
                }
            }
        }
    }

    std::unique_ptr<Iterator> child = std::move(source->iter);

    // -- 2b. JOIN operators ---------------------------------------------------
    if (has_joins) {
        for (const auto& join_clause : stmt.joins) {
            const auto& jtref = join_clause.table;
            const auto& join_alias = jtref.alias.empty() ? jtref.name : jtref.alias;

            auto join_source = plan_from_source(jtref, join_alias, cte_map, bound, owned_exprs);
            if (!join_source) {
                return make_error(join_source.error().code, join_source.error().message);
            }

            // Build combined output schema: left columns + right columns.
            const auto& left_schema = child->output_schema();
            const auto& right_schema_ref = join_source->schema;

            std::vector<OutputColumn> combined_cols;
            combined_cols.reserve(left_schema.column_count() + right_schema_ref.column_count());
            for (size_t i = 0; i < left_schema.column_count(); ++i) {
                combined_cols.push_back(left_schema.column(i));
            }
            for (size_t i = 0; i < right_schema_ref.column_count(); ++i) {
                combined_cols.push_back(right_schema_ref.column(i));
            }
            auto combined = OutputSchema(std::move(combined_cols));

            const Expr* on_expr = join_clause.on_expr ? join_clause.on_expr.get() : nullptr;

            child = std::make_unique<NestedLoopJoinOperator>(std::move(child),
                                                             std::move(join_source->iter),
                                                             join_clause.type,
                                                             on_expr,
                                                             bound,
                                                             std::move(combined));
        }
    }

    // -- 2c. Subquery predicate rewriting (EXISTS/NOT EXISTS/IN) ----------------
    const Expr* remaining_where = nullptr;
    if (stmt.where_expr && !pushed_where) {
        auto rewrite_result =
            rewrite_subquery_predicates(*stmt.where_expr, child, bound, cte_map, owned_exprs);
        if (!rewrite_result) {
            return make_error(rewrite_result.error().code, rewrite_result.error().message);
        }
        remaining_where = *rewrite_result;
    }

    // -- 2d. Apply remaining WHERE as a filter ---------------------------------
    if (remaining_where) {
        child = std::make_unique<FilterOperator>(
            std::move(child), *remaining_where, bound, &subquery_ctx_);
    }

    // -- 3. Detect GROUP BY / aggregation ------------------------------------
    bool has_group_by = !stmt.group_by.empty();
    bool has_aggregates = false;
    for (auto& item : stmt.items) {
        if (item.expr && contains_any_aggregate(*item.expr, bound)) {
            has_aggregates = true;
            break;
        }
    }

    if (has_group_by || has_aggregates) {
        // -- 3a. Collect all aggregate function calls from SELECT + HAVING ----
        std::vector<const FunctionCallExpr*> all_agg_exprs;
        for (auto& item : stmt.items) {
            if (item.expr) {
                collect_aggregate_exprs(*item.expr, bound, all_agg_exprs);
            }
        }
        if (stmt.having_expr) {
            collect_aggregate_exprs(*stmt.having_expr, bound, all_agg_exprs);
        }

        // Deduplicate aggregate expressions (same pointer may appear in SELECT and HAVING).
        std::vector<const FunctionCallExpr*> unique_aggs;
        for (auto* agg : all_agg_exprs) {
            bool found = false;
            for (auto* u : unique_aggs) {
                if (u == agg) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                unique_aggs.push_back(agg);
            }
        }

        // -- 3b. Build AggregateDescriptors and agg_map ----------------------
        std::vector<AggregateDescriptor> agg_descs;
        std::unordered_map<const Expr*, std::string> agg_map; // FunctionCallExpr* → output col name

        for (size_t i = 0; i < unique_aggs.size(); ++i) {
            auto* fn = unique_aggs[i];
            AggregateDescriptor desc;
            desc.func = resolve_agg_func(*fn);

            // Set the argument expression.
            if (desc.func == AggFunc::COUNT_STAR) {
                desc.arg = nullptr;
            } else if (!fn->args.empty()) {
                desc.arg = fn->args[0].get();
            }

            // STRING_AGG separator (second argument).
            if (desc.func == AggFunc::STRING_AGG && fn->args.size() >= 2) {
                if (auto* sep_lit = dynamic_cast<const LiteralExpr*>(fn->args[1].get())) {
                    desc.separator = sep_lit->value;
                } else {
                    desc.separator = ","; // default
                }
            }

            std::string col_name = "__agg_" + std::to_string(i);
            agg_map[fn] = col_name;
            agg_descs.push_back(std::move(desc));
        }

        // -- 3c. Build aggregate output schema --------------------------------
        const auto& child_schema = child->output_schema();
        std::vector<OutputColumn> agg_out_cols;

        // GROUP BY columns first (preserve original names from child schema).
        std::vector<const Expr*> group_by_ptrs;
        for (auto& gb : stmt.group_by) {
            if (gb) {
                group_by_ptrs.push_back(gb.get());

                // Find the column info from the child schema.
                if (auto* cref = dynamic_cast<const ColumnRefExpr*>(gb.get())) {
                    std::optional<size_t> idx;
                    if (!cref->table.empty()) {
                        idx = child_schema.find_column(cref->table, cref->column);
                    } else {
                        idx = child_schema.find_column(cref->column);
                    }
                    if (idx) {
                        agg_out_cols.push_back(child_schema.column(*idx));
                    } else {
                        // Fallback for expressions.
                        agg_out_cols.push_back({"", cref->column, TypeId::STRING, true, 0});
                    }
                } else {
                    // Non-column GROUP BY expression.
                    agg_out_cols.push_back({"", "?group?", TypeId::STRING, true, 0});
                }
            }
        }

        // Aggregate result columns.
        for (size_t i = 0; i < unique_aggs.size(); ++i) {
            std::string col_name = "__agg_" + std::to_string(i);
            auto it = bound.expr_types.find(unique_aggs[i]);
            TypeId type = TypeId::INT64;
            bool nullable = true;
            if (it != bound.expr_types.end()) {
                type = it->second.type_id;
                nullable = it->second.nullable;
            }
            agg_out_cols.push_back({"", col_name, type, nullable, 0});
        }

        auto agg_schema = OutputSchema(std::move(agg_out_cols));

        // -- 3d. Create HashAggregateOperator ---------------------------------
        child = std::make_unique<HashAggregateOperator>(std::move(child),
                                                        std::move(group_by_ptrs),
                                                        std::move(agg_descs),
                                                        bound,
                                                        std::move(agg_schema));

        // -- 3e. Apply HAVING filter ------------------------------------------
        if (stmt.having_expr) {
            auto having_rewritten = rewrite_expr(*stmt.having_expr, agg_map);
            auto* having_ptr = having_rewritten.get();
            owned_exprs.push_back(std::move(having_rewritten));
            child = std::make_unique<FilterOperator>(
                std::move(child), *having_ptr, bound, &subquery_ctx_);
        }

        // -- 3f. Build rewritten projections ----------------------------------
        std::vector<ProjectionExpr> projections;
        for (const auto& item : stmt.items) {
            if (item.is_star || !item.table_star.empty()) {
                // SELECT * with GROUP BY — handled by binder validation (error).
                continue;
            }
            if (!item.expr) {
                continue;
            }

            std::string col_alias = item.alias;
            if (col_alias.empty()) {
                if (auto* cr = dynamic_cast<const ColumnRefExpr*>(item.expr.get())) {
                    col_alias = cr->column;
                } else if (auto* fn = dynamic_cast<const FunctionCallExpr*>(item.expr.get())) {
                    col_alias = fn->name;
                } else {
                    col_alias = "?column?";
                }
            }

            // Check if this SELECT item needs aggregate rewriting.
            if (contains_any_aggregate(*item.expr, bound)) {
                auto rewritten = rewrite_expr(*item.expr, agg_map);
                auto* ptr = rewritten.get();
                owned_exprs.push_back(std::move(rewritten));
                projections.push_back({ptr, col_alias});
            } else {
                // Pure column reference or literal — use as-is.
                projections.push_back({item.expr.get(), col_alias});
            }
        }

        auto output_schema = build_output_schema(bound.output_columns);
        child = std::make_unique<ProjectOperator>(std::move(child),
                                                  std::move(projections),
                                                  std::move(output_schema),
                                                  bound,
                                                  &subquery_ctx_);
    } else {
        // -- 3 (no aggregation). Projection ----------------------------------
        std::vector<ProjectionExpr> projections;
        projections.reserve(stmt.items.size());

        for (const auto& item : stmt.items) {
            if (item.is_star || !item.table_star.empty()) {
                // Expand * or table.* using bound output columns (which the binder
                // already resolved for all tables including joins).
                for (const auto& rc : bound.output_columns) {
                    if (!item.table_star.empty() && rc.table_name != item.table_star) {
                        continue;
                    }
                    auto cr = std::make_unique<ColumnRefExpr>();
                    cr->table = rc.table_name;
                    cr->column = rc.column_name;
                    projections.push_back({cr.get(), rc.column_name});
                    owned_exprs.push_back(std::move(cr));
                }
            } else {
                // Named expression.
                std::string col_alias = item.alias;
                if (col_alias.empty()) {
                    // Try to derive a name from a column reference.
                    if (auto* cr = dynamic_cast<const ColumnRefExpr*>(item.expr.get())) {
                        col_alias = cr->column;
                    } else {
                        col_alias = "?column?";
                    }
                }
                projections.push_back({item.expr.get(), col_alias});
            }
        }

        auto output_schema = build_output_schema(bound.output_columns);
        child = std::make_unique<ProjectOperator>(std::move(child),
                                                  std::move(projections),
                                                  std::move(output_schema),
                                                  bound,
                                                  &subquery_ctx_);
    }

    // -- 4. ORDER BY ---------------------------------------------------------
    if (!stmt.order_by.empty()) {
        std::vector<SortKey> keys;
        keys.reserve(stmt.order_by.size());
        for (const auto& ob : stmt.order_by) {
            keys.push_back({ob.expr.get(), ob.direction});
        }
        child = std::make_unique<SortOperator>(std::move(child), std::move(keys), bound);
    }

    // -- 5. LIMIT / OFFSET ---------------------------------------------------
    if (stmt.limit) {
        int64_t limit_val = 0;
        if (auto* lit = dynamic_cast<const LiteralExpr*>(stmt.limit.get())) {
            limit_val = std::stoll(lit->value);
        } else {
            return make_error(StatusCode::NOT_IMPLEMENTED,
                              "non-literal LIMIT is not yet supported");
        }

        int64_t offset_val = 0;
        if (stmt.offset) {
            if (auto* olit = dynamic_cast<const LiteralExpr*>(stmt.offset.get())) {
                offset_val = std::stoll(olit->value);
            }
        }

        child = std::make_unique<LimitOperator>(std::move(child), limit_val, offset_val);
    }

    return ok(std::move(child));
}

// ---------------------------------------------------------------------------
// INSERT
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Iterator>> Planner::plan_insert(const InsertStmt& stmt,
                                                       const BoundStatement& bound) {
    auto table_schema = catalog_.get_table(stmt.table_name);
    if (!table_schema) {
        return make_error(table_schema.error().code, table_schema.error().message);
    }

    auto ts = storage_.get_table_storage(table_schema->table_id);
    if (!ts) {
        return make_error(ts.error().code, ts.error().message);
    }
    auto* storage = *ts;

    // Build value rows (raw pointers into the AST).
    // If explicit column list is provided, reorder to match storage schema.
    std::vector<std::vector<const Expr*>> value_rows;
    value_rows.reserve(stmt.values.size());

    if (stmt.columns.empty()) {
        // All columns in schema order.
        for (const auto& row : stmt.values) {
            std::vector<const Expr*> ptrs;
            ptrs.reserve(row.size());
            for (const auto& expr : row) {
                ptrs.push_back(expr.get());
            }
            value_rows.push_back(std::move(ptrs));
        }
    } else {
        // Build mapping: storage_col_index -> expression_index_in_values.
        size_t ncols = table_schema->columns.size();
        for (const auto& row : stmt.values) {
            std::vector<const Expr*> reordered(ncols, nullptr);
            for (size_t i = 0; i < stmt.columns.size(); ++i) {
                // Find the column's ordinal in the table schema.
                bool found = false;
                for (size_t j = 0; j < ncols; ++j) {
                    if (table_schema->columns[j].name == stmt.columns[i]) {
                        reordered[j] = row[i].get();
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    return make_error(StatusCode::NOT_FOUND,
                                      "column not found: " + stmt.columns[i]);
                }
            }
            // Columns not mentioned get nullptr — InsertOperator evaluates
            // nullptr as NULL (the expression evaluator is not called).
            // For now we require all columns to be provided.
            for (size_t j = 0; j < ncols; ++j) {
                if (reordered[j] == nullptr) {
                    return make_error(StatusCode::INVALID_ARGUMENT,
                                      "missing value for column: " + table_schema->columns[j].name);
                }
            }
            value_rows.push_back(std::move(reordered));
        }
    }

    auto iter = std::make_unique<InsertOperator>(
        *storage->heap, storage->storage_schema, std::move(value_rows), bound);
    return ok(std::unique_ptr<Iterator>(std::move(iter)));
}

// ---------------------------------------------------------------------------
// UPDATE
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Iterator>> Planner::plan_update(const UpdateStmt& stmt,
                                                       const BoundStatement& bound) {
    auto table_schema = catalog_.get_table(stmt.table_name);
    if (!table_schema) {
        return make_error(table_schema.error().code, table_schema.error().message);
    }

    auto ts = storage_.get_table_storage(table_schema->table_id);
    if (!ts) {
        return make_error(ts.error().code, ts.error().message);
    }
    auto* storage = *ts;

    auto table_output = build_table_output_schema(*table_schema);

    // SeqScan with optional WHERE predicate.
    const Expr* predicate = stmt.where_expr ? stmt.where_expr.get() : nullptr;
    auto scan = std::make_unique<SeqScanOperator>(
        *storage->heap, storage->storage_schema, table_output, predicate, &bound);

    // Build assignment vector.
    std::vector<UpdateAssignment> assignments;
    assignments.reserve(stmt.assignments.size());
    for (const auto& assign : stmt.assignments) {
        // Find column index in table schema.
        bool found = false;
        for (size_t i = 0; i < table_schema->columns.size(); ++i) {
            if (table_schema->columns[i].name == assign.column) {
                assignments.push_back({i, assign.value.get()});
                found = true;
                break;
            }
        }
        if (!found) {
            return make_error(StatusCode::NOT_FOUND, "column not found: " + assign.column);
        }
    }

    auto iter = std::make_unique<UpdateOperator>(
        *storage->heap, storage->storage_schema, std::move(scan), std::move(assignments), bound);
    return ok(std::unique_ptr<Iterator>(std::move(iter)));
}

// ---------------------------------------------------------------------------
// DELETE
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Iterator>> Planner::plan_delete(const DeleteStmt& stmt,
                                                       const BoundStatement& bound) {
    auto table_schema = catalog_.get_table(stmt.table_name);
    if (!table_schema) {
        return make_error(table_schema.error().code, table_schema.error().message);
    }

    auto ts = storage_.get_table_storage(table_schema->table_id);
    if (!ts) {
        return make_error(ts.error().code, ts.error().message);
    }
    auto* storage = *ts;

    auto table_output = build_table_output_schema(*table_schema);

    const Expr* predicate = stmt.where_expr ? stmt.where_expr.get() : nullptr;
    auto scan = std::make_unique<SeqScanOperator>(
        *storage->heap, storage->storage_schema, table_output, predicate, &bound);

    auto iter = std::make_unique<DeleteOperator>(*storage->heap, std::move(scan));
    return ok(std::unique_ptr<Iterator>(std::move(iter)));
}

} // namespace giodb
