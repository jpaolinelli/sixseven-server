#include "giodb/executor/planner.h"

#include "giodb/executor/delete.h"
#include "giodb/executor/filter.h"
#include "giodb/executor/hash_aggregate.h"
#include "giodb/executor/hash_index_scan.h"
#include "giodb/executor/index_scan.h"
#include "giodb/executor/insert.h"
#include "giodb/executor/limit.h"
#include "giodb/executor/nearest_scan.h"
#include "giodb/executor/nested_loop_join.h"
#include "giodb/executor/pattern_match.h"
#include "giodb/executor/project.h"
#include "giodb/executor/seq_scan.h"
#include "giodb/executor/shortest_path.h"
#include "giodb/executor/sort.h"
#include "giodb/executor/subquery_source.h"
#include "giodb/executor/traversal.h"
#include "giodb/executor/update.h"
#include "giodb/planner/type_resolver.h"
#include "giodb/vector/embedding_column.h"

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

Planner::Planner(const Catalog& catalog,
                 StorageManager& storage,
                 database_id_t database_id,
                 GraphEngine* graph_engine,
                 ProviderRegistry* provider_registry,
                 std::unordered_map<std::string, HnswIndex*>* hnsw_indexes,
                 std::unordered_map<index_id_t, BTreeIndex*>* btree_indexes,
                 std::unordered_map<index_id_t, HashIndex*>* hash_indexes)
    : catalog_(catalog), storage_(storage), database_id_(database_id), graph_engine_(graph_engine),
      provider_registry_(provider_registry), hnsw_indexes_(hnsw_indexes),
      btree_indexes_(btree_indexes), hash_indexes_(hash_indexes),
      subquery_ctx_{catalog_, storage_} {}

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
    if (auto* trav = dynamic_cast<const TraverseStmt*>(bound.stmt)) {
        return plan_traverse(*trav, bound);
    }
    if (auto* sp = dynamic_cast<const ShortestPathStmt*>(bound.stmt)) {
        return plan_shortest_path(*sp, bound);
    }
    if (auto* match = dynamic_cast<const MatchStmt*>(bound.stmt)) {
        return plan_match(*match, bound);
    }
    if (auto* nearest = dynamic_cast<const NearestStmt*>(bound.stmt)) {
        return plan_nearest(*nearest, bound);
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

        Binder binder(catalog_, database_id_);
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

        Binder binder(catalog_, database_id_);
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
    auto table_schema = catalog_.get_table(database_id_, table_ref.name);
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
                Binder binder(catalog_, database_id_);
                // Inject outer CTE bindings so the subquery can reference CTEs.
                for (const auto& [cte_name, cte_sel_ptr] : cte_map) {
                    Binder cte_binder(catalog_, database_id_);
                    auto cte_bound = cte_binder.bind(*cte_sel_ptr);
                    if (cte_bound) {
                        binder.add_cte(cte_name, *cte_bound);
                    }
                }
                auto sub_bound = binder.bind(*sub_sel);
                if (!sub_bound) {
                    return make_error(sub_bound.error().code, sub_bound.error().message);
                }

                auto sub_iter = plan_select(*sub_sel, *sub_bound, owned_exprs, cte_map);
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

                auto join_op = std::make_unique<NestedLoopJoinOperator>(std::move(child),
                                                                        std::move(sub_source),
                                                                        jtype,
                                                                        on_ptr,
                                                                        bound,
                                                                        std::move(combined));
                // NOT IN requires null-aware ANTI join semantics.
                if (in_expr->negated) {
                    join_op->set_null_aware_anti(true);
                }
                child = std::move(join_op);
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
    return plan_select(stmt, bound, owned_exprs, {});
}

Result<std::unique_ptr<Iterator>>
Planner::plan_select(const SelectStmt& stmt,
                     const BoundStatement& bound,
                     std::vector<ExprPtr>& owned_exprs,
                     const std::unordered_map<std::string, const SelectStmt*>& outer_cte_map) {
    // -- 0. Build CTE map -------------------------------------------------------
    std::unordered_map<std::string, const SelectStmt*> cte_map = outer_cte_map;
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
        // Recursively check if WHERE contains any subquery predicates.
        std::function<bool(const Expr*)> contains_subquery = [&](const Expr* e) -> bool {
            if (!e)
                return false;
            if (dynamic_cast<const ExistsExpr*>(e) || dynamic_cast<const SubqueryExpr*>(e))
                return true;
            if (auto* in = dynamic_cast<const InExpr*>(e))
                return in->subquery != nullptr || contains_subquery(in->expr.get());
            if (auto* un = dynamic_cast<const UnaryExpr*>(e))
                return contains_subquery(un->operand.get());
            if (auto* bin = dynamic_cast<const BinaryExpr*>(e))
                return contains_subquery(bin->lhs.get()) || contains_subquery(bin->rhs.get());
            if (auto* fn = dynamic_cast<const FunctionCallExpr*>(e)) {
                for (const auto& arg : fn->args) {
                    if (contains_subquery(arg.get()))
                        return true;
                }
            }
            return false;
        };
        bool has_subquery_predicate = contains_subquery(stmt.where_expr.get());

        if (!has_subquery_predicate) {
            // Re-create the scan with the predicate pushed down.
            auto table_schema = catalog_.get_table(database_id_, table_ref.name);
            if (table_schema) {
                auto ts = storage_.get_table_storage(table_schema->table_id);
                if (ts) {
                    auto* storage = *ts;
                    auto table_output = build_table_output_schema(*table_schema, alias);

                    // Try to use an index scan if a suitable B+ tree index exists.
                    auto idx_scan = try_plan_index_scan(
                        *table_schema, *ts, table_output, stmt.where_expr.get(), bound);
                    if (idx_scan && *idx_scan) {
                        source->iter = std::move(*idx_scan);
                        source->schema = std::move(table_output);
                        pushed_where = true;
                    } else {
                        // Fall back to sequential scan with predicate pushdown.
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

        // -- 3f. ORDER BY (before projection, with aggregate rewriting) --------
        if (!stmt.order_by.empty()) {
            std::vector<SortKey> keys;
            keys.reserve(stmt.order_by.size());
            for (const auto& ob : stmt.order_by) {
                if (contains_any_aggregate(*ob.expr, bound)) {
                    auto rewritten = rewrite_expr(*ob.expr, agg_map);
                    auto* ptr = rewritten.get();
                    owned_exprs.push_back(std::move(rewritten));
                    keys.push_back({ptr, ob.direction});
                } else {
                    keys.push_back({ob.expr.get(), ob.direction});
                }
            }
            child = std::make_unique<SortOperator>(std::move(child), std::move(keys), bound);
        }

        // -- 3g. Build rewritten projections ----------------------------------
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
        // -- 3 (no aggregation). Sort then Projection -------------------------

        // ORDER BY must happen before projection so that sort keys referencing
        // columns not in the SELECT list are still available.
        if (!stmt.order_by.empty()) {
            std::vector<SortKey> keys;
            keys.reserve(stmt.order_by.size());
            for (const auto& ob : stmt.order_by) {
                keys.push_back({ob.expr.get(), ob.direction});
            }
            child = std::make_unique<SortOperator>(std::move(child), std::move(keys), bound);
        }

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
    auto table_schema = catalog_.get_table(database_id_, stmt.table_name);
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
    auto table_schema = catalog_.get_table(database_id_, stmt.table_name);
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
    auto table_schema = catalog_.get_table(database_id_, stmt.table_name);
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

// ---------------------------------------------------------------------------
// TRAVERSE
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Iterator>> Planner::plan_traverse(const TraverseStmt& stmt,
                                                         const BoundStatement& bound) {
    if (!graph_engine_) {
        return make_error(StatusCode::INTERNAL_ERROR, "graph engine not available for TRAVERSE");
    }

    // Evaluate the start key.
    Tuple empty_tuple;
    OutputSchema empty_schema;
    auto key_val = evaluate_expr(*stmt.from_key, empty_tuple, empty_schema, bound);
    if (!key_val) {
        return make_error(key_val.error().code, key_val.error().message);
    }

    TraversalConfig config;
    config.edge_type = stmt.edge_type;
    config.start_key = std::move(*key_val);
    config.direction = stmt.direction;
    config.max_depth = stmt.max_depth.value_or(100);
    config.fetch = stmt.fetch;
    config.collect_edges = true;

    // Build output schema.
    // Determine PK type from the edge type definition.
    auto edge_def = catalog_.get_edge_type(stmt.edge_type);
    if (!edge_def) {
        return make_error(edge_def.error().code, edge_def.error().message);
    }

    // Look up the target table schema for PK type info.
    TypeId pk_type = TypeId::INT64;
    if (!bound.referenced_tables.empty()) {
        auto ts = catalog_.get_table_by_id(bound.referenced_tables[0]);
        if (ts) {
            // Find PK column type.
            if (!ts->pk_columns.empty()) {
                for (const auto& col : ts->columns) {
                    if (col.name == ts->pk_columns) {
                        pk_type = col.type_id;
                        break;
                    }
                }
            }
        }
    }

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"", "node", pk_type, false, 0});
    out_cols.push_back({"", "depth", TypeId::INT64, false, 0});
    if (stmt.fetch) {
        out_cols.push_back({"", "source", pk_type, true, 0});
    }
    auto schema = OutputSchema(std::move(out_cols));

    auto iter = std::make_unique<TraversalOperator>(
        *graph_engine_, std::move(config), std::move(schema), stmt.where_expr.get(), bound);

    return ok(std::unique_ptr<Iterator>(std::move(iter)));
}

// ---------------------------------------------------------------------------
// SHORTEST PATH
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Iterator>> Planner::plan_shortest_path(const ShortestPathStmt& stmt,
                                                              const BoundStatement& bound) {
    if (!graph_engine_) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "graph engine not available for SHORTEST PATH");
    }

    // Evaluate start and end keys.
    Tuple empty_tuple;
    OutputSchema empty_schema;
    auto from_val = evaluate_expr(*stmt.from_key, empty_tuple, empty_schema, bound);
    if (!from_val) {
        return make_error(from_val.error().code, from_val.error().message);
    }
    auto to_val = evaluate_expr(*stmt.to_key, empty_tuple, empty_schema, bound);
    if (!to_val) {
        return make_error(to_val.error().code, to_val.error().message);
    }

    ShortestPathConfig sp_config;
    sp_config.edge_type = stmt.edge_type;
    sp_config.from_key = std::move(*from_val);
    sp_config.to_key = std::move(*to_val);
    sp_config.direction = stmt.direction;
    sp_config.max_depth = stmt.max_depth.value_or(100);

    // Determine PK type.
    TypeId pk_type = TypeId::INT64;
    if (!bound.referenced_tables.empty()) {
        auto ts = catalog_.get_table_by_id(bound.referenced_tables[0]);
        if (ts) {
            if (!ts->pk_columns.empty()) {
                for (const auto& col : ts->columns) {
                    if (col.name == ts->pk_columns) {
                        pk_type = col.type_id;
                        break;
                    }
                }
            }
        }
    }

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"", "node", pk_type, false, 0});
    out_cols.push_back({"", "hop", TypeId::INT64, false, 0});
    auto schema = OutputSchema(std::move(out_cols));

    auto iter = std::make_unique<ShortestPathOperator>(
        *graph_engine_, std::move(sp_config), std::move(schema));

    return ok(std::unique_ptr<Iterator>(std::move(iter)));
}

// ---------------------------------------------------------------------------
// MATCH
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Iterator>> Planner::plan_match(const MatchStmt& stmt,
                                                      const BoundStatement& bound) {
    if (!graph_engine_) {
        return make_error(StatusCode::INTERNAL_ERROR, "graph engine not available for MATCH");
    }

    // Build pattern config from AST pattern elements.
    MatchConfig match_config;
    for (const auto& elem : stmt.pattern) {
        MatchNodeDef node_def;
        node_def.variable = elem.node.variable;
        node_def.label = elem.node.label;
        match_config.nodes.push_back(std::move(node_def));

        if (elem.outgoing_edge) {
            MatchEdgeDef edge_def;
            edge_def.variable = elem.outgoing_edge->variable;
            edge_def.edge_type = elem.outgoing_edge->edge_type;
            edge_def.direction = elem.outgoing_edge->direction;
            match_config.edges.push_back(std::move(edge_def));
        }
    }

    // Build output schema from bound output columns.
    auto schema = build_output_schema(bound.output_columns);

    auto iter = std::make_unique<PatternMatchOperator>(*graph_engine_,
                                                       catalog_,
                                                       storage_,
                                                       database_id_,
                                                       std::move(match_config),
                                                       std::move(schema),
                                                       stmt.where_expr.get(),
                                                       bound);

    return ok(std::unique_ptr<Iterator>(std::move(iter)));
}

// ---------------------------------------------------------------------------
// NEAREST
// ---------------------------------------------------------------------------

namespace {

/// Map AST NearestMetric to vector::DistanceMetric.
DistanceMetric to_distance_metric(NearestMetric m) {
    switch (m) {
    case NearestMetric::L2:
        return DistanceMetric::L2;
    case NearestMetric::DOT:
        return DistanceMetric::DOT_PRODUCT;
    case NearestMetric::COSINE:
    default:
        return DistanceMetric::COSINE;
    }
}

} // anonymous namespace

Result<std::unique_ptr<Iterator>> Planner::plan_nearest(const NearestStmt& stmt,
                                                        const BoundStatement& bound) {
    // Resolve the table.
    auto table_schema = catalog_.get_table(database_id_, stmt.table_name);
    if (!table_schema) {
        return make_error(table_schema.error().code, table_schema.error().message);
    }

    // Get table storage.
    auto ts = storage_.get_table_storage(table_schema->table_id);
    if (!ts) {
        return make_error(ts.error().code, ts.error().message);
    }
    auto* storage = *ts;

    // Find the EMBEDDING column index.
    int32_t emb_col_idx = -1;
    for (size_t i = 0; i < table_schema->columns.size(); ++i) {
        if (to_upper(table_schema->columns[i].name) == to_upper(stmt.column_name)) {
            emb_col_idx = static_cast<int32_t>(i);
            break;
        }
    }
    if (emb_col_idx < 0) {
        return make_error(StatusCode::NOT_FOUND,
                          "column " + stmt.column_name + " not found in table " + stmt.table_name);
    }

    // Evaluate k.
    Tuple empty_tuple;
    OutputSchema empty_schema;
    auto k_val = evaluate_expr(*stmt.k, empty_tuple, empty_schema, bound);
    if (!k_val) {
        return make_error(k_val.error().code, k_val.error().message);
    }
    auto k = static_cast<uint32_t>(k_val->as_int64());

    // Evaluate the target expression to get the query vector.
    auto target_val = evaluate_expr(*stmt.target, empty_tuple, empty_schema, bound);
    if (!target_val) {
        return make_error(target_val.error().code, target_val.error().message);
    }

    std::vector<float> query_vector;

    if (!target_val->is_null() && target_val->type_id() == TypeId::EMBEDDING) {
        // Target is a literal vector.
        query_vector = target_val->as_embedding();
    } else if (!target_val->is_null() && target_val->type_id() == TypeId::STRING) {
        // Target is a text string — auto-embed via the column's provider.
        if (provider_registry_ == nullptr) {
            return make_error(StatusCode::NOT_IMPLEMENTED,
                              "text auto-embedding requires a ProviderRegistry");
        }

        // Look up the embedding column metadata.
        auto emb_cols = catalog_.list_embedding_columns(table_schema->table_id);
        std::string provider_name;
        for (const auto& ec : emb_cols) {
            if (ec.column_id == emb_col_idx) {
                provider_name = ec.provider;
                break;
            }
        }
        if (provider_name.empty()) {
            return make_error(StatusCode::NOT_FOUND,
                              "no embedding provider configured for column " + stmt.column_name);
        }

        auto provider = provider_registry_->resolve(provider_name);
        if (!provider) {
            return make_error(provider.error().code, provider.error().message);
        }

        auto embedding = (*provider)->embed(target_val->as_string());
        if (!embedding) {
            return make_error(embedding.error().code, embedding.error().message);
        }
        query_vector = std::move(*embedding);
    } else {
        return make_error(StatusCode::TYPE_ERROR, "NEAREST target must be a vector or text string");
    }

    // Build NearestScanConfig.
    NearestScanConfig config;
    config.k = k;
    config.query_vector = std::move(query_vector);
    config.metric = to_distance_metric(stmt.metric);
    config.embedding_column_index = emb_col_idx;

    // Handle WITHIN TRAVERSE (graph-scoped search).
    if (stmt.within_traverse) {
        if (!graph_engine_) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "graph engine not available for WITHIN TRAVERSE");
        }

        auto* trav_stmt = dynamic_cast<const TraverseStmt*>(stmt.within_traverse.get());
        if (!trav_stmt) {
            return make_error(StatusCode::INTERNAL_ERROR, "expected TraverseStmt in WITHIN clause");
        }

        // Evaluate the start key.
        auto start_key = evaluate_expr(*trav_stmt->from_key, empty_tuple, empty_schema, bound);
        if (!start_key) {
            return make_error(start_key.error().code, start_key.error().message);
        }

        // Save start PK before moving the value into the config.
        int64_t start_pk = start_key->as_int64();

        TraversalConfig trav_config;
        trav_config.edge_type = trav_stmt->edge_type;
        trav_config.start_key = std::move(*start_key);
        trav_config.direction = trav_stmt->direction;
        trav_config.max_depth = trav_stmt->max_depth.value_or(100);

        // Build a minimal output schema for traversal.
        TypeId pk_type = TypeId::INT64;
        if (!table_schema->pk_columns.empty()) {
            for (const auto& col : table_schema->columns) {
                if (col.name == table_schema->pk_columns) {
                    pk_type = col.type_id;
                    break;
                }
            }
        }
        std::vector<OutputColumn> trav_cols;
        trav_cols.push_back({"", "node", pk_type, false, 0});
        trav_cols.push_back({"", "depth", TypeId::INT64, false, 0});
        auto trav_schema = OutputSchema(std::move(trav_cols));

        // Execute BFS to get the reachable node set.
        BoundStatement trav_bound;
        TraversalOperator trav_op(
            *graph_engine_, std::move(trav_config), std::move(trav_schema), nullptr, trav_bound);
        auto open_res = trav_op.open();
        if (!open_res) {
            return make_error(open_res.error().code, open_res.error().message);
        }

        // Collect reachable node PKs.
        std::unordered_set<int64_t> reachable_pks;
        while (true) {
            auto row = trav_op.next();
            if (!row) {
                return make_error(row.error().code, row.error().message);
            }
            if (!row->has_value()) {
                break;
            }
            reachable_pks.insert(row->value().values[0].as_int64());
        }
        trav_op.close();

        // Also include the start node itself.
        reachable_pks.insert(start_pk);

        // Scan the table to build PK → node_ordinal mapping, then convert
        // reachable PKs to node ordinals for the HNSW filter.
        auto scan_it = storage->heap->begin();
        if (!scan_it) {
            return make_error(scan_it.error().code, scan_it.error().message);
        }

        // Find the PK column index.
        int32_t pk_col_idx = -1;
        for (size_t i = 0; i < table_schema->columns.size(); ++i) {
            if (table_schema->columns[i].name == table_schema->pk_columns) {
                pk_col_idx = static_cast<int32_t>(i);
                break;
            }
        }

        uint32_t ordinal = 0;
        while (true) {
            auto row = scan_it->next();
            if (!row) {
                break;
            }
            auto& [rid, data] = *row;
            auto values = TupleSerializer::deserialize(data, storage->storage_schema);
            if (!values) {
                return make_error(values.error().code, values.error().message);
            }
            if (pk_col_idx >= 0 &&
                reachable_pks.count((*values)[static_cast<size_t>(pk_col_idx)].as_int64()) > 0) {
                config.allowed_node_ids.insert(ordinal);
            }
            ++ordinal;
        }
    }

    // Look up the companion HNSW index.
    HnswIndex* hnsw_index = nullptr;
    if (hnsw_indexes_ != nullptr) {
        auto index_name =
            EmbeddingColumnManager::make_index_name(stmt.table_name, stmt.column_name);
        auto it = hnsw_indexes_->find(index_name);
        if (it != hnsw_indexes_->end()) {
            hnsw_index = it->second;
        }
    }

    // Build output schema: all table columns + _distance.
    auto table_output = build_table_output_schema(*table_schema);
    std::vector<OutputColumn> out_cols = table_output.columns();
    out_cols.push_back(
        {stmt.table_name, "_distance", TypeId::FLOAT64, false, table_schema->table_id});
    auto schema = OutputSchema(std::move(out_cols));

    auto iter = std::make_unique<NearestScanOperator>(*storage->heap,
                                                      storage->storage_schema,
                                                      std::move(config),
                                                      std::move(schema),
                                                      stmt.where_expr.get(),
                                                      bound,
                                                      hnsw_index);

    return ok(std::unique_ptr<Iterator>(std::move(iter)));
}

// ---------------------------------------------------------------------------
// Index scan planning
// ---------------------------------------------------------------------------

namespace {

/// Parse a comma-separated column list (e.g. "a,b,c") into individual names.
std::vector<std::string> parse_index_columns(const std::string& columns) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start < columns.size()) {
        size_t end = columns.find(',', start);
        if (end == std::string::npos) {
            end = columns.size();
        }
        // Trim whitespace.
        auto s = start;
        while (s < end && columns[s] == ' ') {
            ++s;
        }
        auto e = end;
        while (e > s && columns[e - 1] == ' ') {
            --e;
        }
        if (s < e) {
            result.emplace_back(columns.substr(s, e - s));
        }
        start = end + 1;
    }
    return result;
}

/// Check if a binary predicate is a simple comparison between a column and a
/// literal (e.g., `col = 42` or `col > 10`). Returns the column name, the
/// literal Value, and the operator.
struct SimpleComparison {
    std::string column_name;
    Value literal_value;
    BinaryOp op = BinaryOp::EQUAL;
};

std::optional<SimpleComparison> extract_simple_comparison(const Expr* expr) {
    auto* bin = dynamic_cast<const BinaryExpr*>(expr);
    if (bin == nullptr) {
        return std::nullopt;
    }

    // Only handle comparison operators.
    switch (bin->op) {
    case BinaryOp::EQUAL:
    case BinaryOp::LESS:
    case BinaryOp::GREATER:
    case BinaryOp::LESS_EQUAL:
    case BinaryOp::GREATER_EQUAL:
        break;
    default:
        return std::nullopt;
    }

    auto* col = dynamic_cast<const ColumnRefExpr*>(bin->lhs.get());
    auto* lit = dynamic_cast<const LiteralExpr*>(bin->rhs.get());

    if (col == nullptr || lit == nullptr) {
        // Try reversed: literal op column.
        col = dynamic_cast<const ColumnRefExpr*>(bin->rhs.get());
        lit = dynamic_cast<const LiteralExpr*>(bin->lhs.get());
        if (col == nullptr || lit == nullptr) {
            return std::nullopt;
        }
        // Flip the operator for reversed operands.
        SimpleComparison cmp;
        cmp.column_name = col->column;
        switch (bin->op) {
        case BinaryOp::LESS:
            cmp.op = BinaryOp::GREATER;
            break;
        case BinaryOp::GREATER:
            cmp.op = BinaryOp::LESS;
            break;
        case BinaryOp::LESS_EQUAL:
            cmp.op = BinaryOp::GREATER_EQUAL;
            break;
        case BinaryOp::GREATER_EQUAL:
            cmp.op = BinaryOp::LESS_EQUAL;
            break;
        default:
            cmp.op = bin->op;
            break;
        }

        // Parse the literal value.
        if (lit->kind == LiteralKind::INTEGER) {
            cmp.literal_value = Value(static_cast<int64_t>(std::stoll(lit->value)));
        } else if (lit->kind == LiteralKind::FLOAT) {
            cmp.literal_value = Value(std::stod(lit->value));
        } else if (lit->kind == LiteralKind::STRING) {
            cmp.literal_value = Value(lit->value);
        } else {
            return std::nullopt;
        }
        return cmp;
    }

    SimpleComparison cmp;
    cmp.column_name = col->column;
    cmp.op = bin->op;

    // Parse the literal value.
    if (lit->kind == LiteralKind::INTEGER) {
        cmp.literal_value = Value(static_cast<int64_t>(std::stoll(lit->value)));
    } else if (lit->kind == LiteralKind::FLOAT) {
        cmp.literal_value = Value(std::stod(lit->value));
    } else if (lit->kind == LiteralKind::STRING) {
        cmp.literal_value = Value(lit->value);
    } else {
        return std::nullopt;
    }

    return cmp;
}

} // anonymous namespace

Result<std::unique_ptr<Iterator>> Planner::try_plan_index_scan(const TableSchema& table_schema,
                                                               TableStorage* storage,
                                                               const OutputSchema& table_output,
                                                               const Expr* where_expr,
                                                               const BoundStatement& bound) {
    if (where_expr == nullptr) {
        return ok(std::unique_ptr<Iterator>(nullptr));
    }

    // Extract simple comparison from the WHERE clause.
    auto cmp = extract_simple_comparison(where_expr);
    if (!cmp) {
        return ok(std::unique_ptr<Iterator>(nullptr));
    }

    auto indexes = catalog_.list_indexes(table_schema.table_id);

    // --- Try hash index first (preferred for equality predicates) ---
    if (cmp->op == BinaryOp::EQUAL && hash_indexes_ != nullptr && !hash_indexes_->empty()) {
        for (const auto& idx_def : indexes) {
            if (idx_def.index_type != "hash") {
                continue;
            }

            auto it = hash_indexes_->find(idx_def.index_id);
            if (it == hash_indexes_->end()) {
                continue;
            }

            auto idx_columns = parse_index_columns(idx_def.columns);
            if (idx_columns.empty() || idx_columns[0] != cmp->column_name) {
                continue;
            }

            // Found a matching hash index for an equality predicate.
            auto* hash_idx = it->second;
            KeyType lookup_key = {cmp->literal_value};

            std::vector<size_t> index_col_indexes;
            for (const auto& icol : idx_columns) {
                for (size_t ci = 0; ci < table_schema.columns.size(); ++ci) {
                    if (table_schema.columns[ci].name == icol) {
                        index_col_indexes.push_back(ci);
                        break;
                    }
                }
            }

            auto scan = std::make_unique<HashIndexScanOperator>(*hash_idx,
                                                                *storage->heap,
                                                                storage->storage_schema,
                                                                table_output,
                                                                std::move(lookup_key),
                                                                std::move(index_col_indexes),
                                                                false, // index_only
                                                                where_expr,
                                                                &bound);

            return ok(std::unique_ptr<Iterator>(std::move(scan)));
        }
    }

    // --- Fall back to B+ tree index ---
    if (btree_indexes_ == nullptr || btree_indexes_->empty()) {
        return ok(std::unique_ptr<Iterator>(nullptr));
    }

    for (const auto& idx_def : indexes) {
        if (idx_def.index_type != "btree") {
            continue;
        }

        auto it = btree_indexes_->find(idx_def.index_id);
        if (it == btree_indexes_->end()) {
            continue;
        }

        auto idx_columns = parse_index_columns(idx_def.columns);
        if (idx_columns.empty()) {
            continue;
        }

        // Check if the first (prefix) column of the index matches the predicate column.
        if (idx_columns[0] != cmp->column_name) {
            continue;
        }

        // Found a matching index. Build scan bounds.
        auto* btree = it->second;
        KeyType key_value = {cmp->literal_value};

        std::optional<KeyType> begin_key;
        std::optional<KeyType> end_key;
        const Expr* residual = nullptr; // Residual predicate after pushdown.

        switch (cmp->op) {
        case BinaryOp::EQUAL:
            begin_key = key_value;
            // For equality, end_key should be just past the key.
            // The B+ tree range_scan is [begin, end), so we need begin=key and
            // we let the index scan check equality as a post-filter.
            // Actually, for point lookups we scan [key, nullopt) and filter.
            end_key = std::nullopt;
            // Use the WHERE expr as a residual to filter exact matches.
            residual = where_expr;
            break;
        case BinaryOp::GREATER:
        case BinaryOp::GREATER_EQUAL:
            begin_key = key_value;
            end_key = std::nullopt;
            // For GREATER (strict), we need to skip the exact match.
            // Use residual predicate to handle this.
            residual = where_expr;
            break;
        case BinaryOp::LESS:
        case BinaryOp::LESS_EQUAL:
            begin_key = std::nullopt;
            end_key = key_value;
            // Residual handles the exact boundary.
            residual = where_expr;
            break;
        default:
            continue;
        }

        // Build the index column position map for potential index-only scans.
        std::vector<size_t> index_col_indexes;
        for (const auto& icol : idx_columns) {
            for (size_t ci = 0; ci < table_schema.columns.size(); ++ci) {
                if (table_schema.columns[ci].name == icol) {
                    index_col_indexes.push_back(ci);
                    break;
                }
            }
        }

        auto scan = std::make_unique<IndexScanOperator>(*btree,
                                                        *storage->heap,
                                                        storage->storage_schema,
                                                        table_output,
                                                        std::move(begin_key),
                                                        std::move(end_key),
                                                        std::move(index_col_indexes),
                                                        false, // index_only: conservative default
                                                        residual,
                                                        &bound);

        return ok(std::unique_ptr<Iterator>(std::move(scan)));
    }

    // No suitable index found.
    return ok(std::unique_ptr<Iterator>(nullptr));
}

} // namespace giodb
