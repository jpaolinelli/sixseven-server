#include "sixseven/executor/planner.h"

#include "sixseven/common/coercion.h"
#include "sixseven/common/logging.h"
#include "sixseven/common/value_hash.h"
#include "sixseven/executor/algorithm_scan.h"
#include "sixseven/executor/bitmap_scan.h"
#include "sixseven/executor/bm25_scan.h"
#include "sixseven/executor/count_scan.h"
#include "sixseven/executor/delete.h"
#include "sixseven/executor/edge_traversal.h"
#include "sixseven/executor/enriched_traversal.h"
#include "sixseven/executor/external_sort.h"
#include "sixseven/executor/filter.h"
#include "sixseven/executor/hash_aggregate.h"
#include "sixseven/executor/hash_index_scan.h"
#include "sixseven/executor/hash_join.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/index_scan.h"
#include "sixseven/executor/insert.h"
#include "sixseven/executor/limit.h"
#include "sixseven/executor/match_shortest_path.h"
#include "sixseven/executor/nearest_scan.h"
#include "sixseven/executor/nested_loop_join.h"
#include "sixseven/executor/pattern_match.h"
#include "sixseven/executor/prepend_tuple.h"
#include "sixseven/executor/project.h"
#include "sixseven/executor/seq_scan.h"
#include "sixseven/executor/shortest_path.h"
#include "sixseven/executor/sort.h"
#include "sixseven/executor/sort_merge_join.h"
#include "sixseven/executor/subquery_source.h"
#include "sixseven/executor/traversal.h"
#include "sixseven/executor/update.h"
#include "sixseven/executor/variable_length_match.h"
#include "sixseven/executor/virtual_catalog_scan.h"
#include "sixseven/executor/window_function.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/cost_model.h"
#include "sixseven/planner/optimizer.h"
#include "sixseven/planner/rewrite_rules.h"
#include "sixseven/planner/statistics.h"
#include "sixseven/planner/type_resolver.h"
#include "sixseven/vector/embedding_column.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <limits>
#include <string>
#include <unordered_map>

namespace sixseven {

namespace {

// Defined further below; forward-declared for use in plan_select.
bool expr_contains_match(const Expr* e);
bool expr_contains_nearest(const Expr* e);
const NearestExpr* extract_nearest_predicate(const Expr* e);
size_t count_nearest_conjuncts(const std::vector<const Expr*>& conjuncts);
const Expr* build_owned_and_chain(const std::vector<const Expr*>& conjuncts,
                                  std::vector<ExprPtr>& owned);

/// Resolved plan for a NEAREST predicate that must be pushed through a JOIN.
/// Computed once up front (validating all v1 restrictions) and then consumed
/// when planning the owning table's scan, whether it is the base (left) source
/// or a join (right) source (GDB-1250).
struct NearestJoinPlan {
    bool active = false; ///< true once a NEAREST+JOIN query is validated.
    const NearestExpr* nearest = nullptr;
    std::string owner_alias;        ///< query alias of the table owning the EMBEDDING column.
    std::string owner_table;        ///< physical table name of that owner.
    const Expr* residual = nullptr; ///< owned AND-chain: NEAREST + owner's sibling conjuncts.
};

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

/// Walk down a single-child operator chain (e.g. Filter -> Seq Scan) looking
/// for the first node that carries an optimizer cost estimate. Returns
/// nullptr when no estimate is found.
const PlanCostEstimate* find_cost_in_chain(const Iterator* iter) {
    while (iter != nullptr) {
        if (iter->estimated_cost().has_value()) {
            return &*iter->estimated_cost();
        }
        auto children = iter->plan_children();
        if (children.size() != 1) {
            return nullptr;
        }
        iter = children[0];
    }
    return nullptr;
}

/// Convert an executor cost annotation to the optimizer's PlanCost.
PlanCost to_plan_cost(const PlanCostEstimate& e) {
    PlanCost c;
    c.startup_cost = e.startup_cost;
    c.total_cost = e.total_cost;
    c.estimated_rows = e.estimated_rows;
    return c;
}

/// Convert an optimizer PlanCost to the executor cost annotation.
PlanCostEstimate to_cost_estimate(const PlanCost& c) {
    PlanCostEstimate e;
    e.startup_cost = c.startup_cost;
    e.total_cost = c.total_cost;
    e.estimated_rows = c.estimated_rows;
    return e;
}
/// Return true when the operator chain rooted at \p iter produces rows already
/// ordered by the column named in \p join_key.
///
/// An input is considered sorted on the join key when the topmost significant
/// node (skipping transparent wrappers that preserve row order such as Filter
/// and Projection) is:
///   - An ExternalSortOperator whose leading sort key is the same column as
///     \p join_key (SortMergeJoin can leverage this without an extra sort pass).
///   - An IndexScanOperator whose leading indexed column is the same column as
///     \p join_key (B+ tree scans always deliver rows in index key order).
///
/// All other operator types (SeqScan, HashJoin, etc.) return false.
bool is_sorted_on_join_key(const Iterator* iter, const Expr* join_key) {
    if (iter == nullptr || join_key == nullptr) {
        return false;
    }
    const auto* key_col = dynamic_cast<const ColumnRefExpr*>(join_key);
    if (key_col == nullptr) {
        return false;
    }

    while (iter != nullptr) {
        const std::string name = iter->plan_node_name();

        // ExternalSortOperator: rows are sorted on the leading ORDER BY key.
        if (name == "External Sort") {
            const auto* es = dynamic_cast<const ExternalSortOperator*>(iter);
            if (es != nullptr && !es->sort_keys().empty()) {
                const auto* sk = dynamic_cast<const ColumnRefExpr*>(es->sort_keys()[0].expr);
                return sk != nullptr && sk->column == key_col->column;
            }
            return false;
        }

        // IndexScanOperator: rows come out in B+ tree key (ascending) order.
        if (name == "Index Scan") {
            const auto* is_op = dynamic_cast<const IndexScanOperator*>(iter);
            if (is_op == nullptr) {
                return false;
            }
            const auto& col_idxs = is_op->index_col_indexes();
            const auto& schema = is_op->output_schema();
            if (col_idxs.empty() || col_idxs[0] >= schema.column_count()) {
                return false;
            }
            return schema.column(col_idxs[0]).name == key_col->column;
        }

        // Transparent single-child operators that preserve row order.
        if (name == "Filter" || name == "Project" || name == "Subquery Scan") {
            auto children = iter->plan_children();
            if (children.size() == 1) {
                iter = children[0];
                continue;
            }
        }

        // Any other operator is not ordered on the join key.
        break;
    }
    return false;
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

/// Check if an expression is a window function.
bool is_window_expr(const Expr& expr, const BoundStatement& bound) {
    auto it = bound.expr_types.find(&expr);
    return it != bound.expr_types.end() && it->second.is_window;
}

/// Resolve a window function name to the WindowFunc enum.
WindowFunc resolve_window_func(const std::string& name) {
    std::string upper = to_upper(name);
    if (upper == "ROW_NUMBER")
        return WindowFunc::ROW_NUMBER;
    if (upper == "RANK")
        return WindowFunc::RANK;
    if (upper == "DENSE_RANK")
        return WindowFunc::DENSE_RANK;
    if (upper == "NTILE")
        return WindowFunc::NTILE;
    if (upper == "LAG")
        return WindowFunc::LAG;
    if (upper == "LEAD")
        return WindowFunc::LEAD;
    if (upper == "FIRST_VALUE")
        return WindowFunc::FIRST_VALUE;
    if (upper == "LAST_VALUE")
        return WindowFunc::LAST_VALUE;
    if (upper == "SUM")
        return WindowFunc::WIN_SUM;
    if (upper == "AVG")
        return WindowFunc::WIN_AVG;
    if (upper == "COUNT")
        return WindowFunc::WIN_COUNT;
    if (upper == "MIN")
        return WindowFunc::WIN_MIN;
    if (upper == "MAX")
        return WindowFunc::WIN_MAX;
    return WindowFunc::ROW_NUMBER; // fallback
}

/// Convert AST frame bound to executor frame bound.
FrameBound convert_frame_bound(AstFrameBound b) {
    switch (b) {
    case AstFrameBound::UNBOUNDED_PRECEDING:
        return FrameBound::UNBOUNDED_PRECEDING;
    case AstFrameBound::N_PRECEDING:
        return FrameBound::N_PRECEDING;
    case AstFrameBound::CURRENT_ROW:
        return FrameBound::CURRENT_ROW;
    case AstFrameBound::N_FOLLOWING:
        return FrameBound::N_FOLLOWING;
    case AstFrameBound::UNBOUNDED_FOLLOWING:
        return FrameBound::UNBOUNDED_FOLLOWING;
    }
    return FrameBound::UNBOUNDED_PRECEDING;
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
        // Non-aggregate function call â€” clone it and rewrite args.
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

Planner::Planner(Catalog& catalog,
                 StorageManager& storage,
                 database_id_t database_id,
                 GraphEngine* graph_engine,
                 ProviderRegistry* provider_registry,
                 std::unordered_map<index_id_t, HnswIndex*>* hnsw_indexes,
                 std::unordered_map<index_id_t, BTreeIndex*>* btree_indexes,
                 std::unordered_map<index_id_t, HashIndex*>* hash_indexes,
                 EmbeddingWorkerPool* embedding_pool,
                 AlgorithmRegistry* algorithm_registry,
                 std::unordered_map<index_id_t, std::vector<RID>>* hnsw_rid_maps,
                 std::unordered_map<index_id_t, Bm25Index*>* bm25_indexes)
    : catalog_(catalog), storage_(storage), database_id_(database_id), graph_engine_(graph_engine),
      provider_registry_(provider_registry), hnsw_indexes_(hnsw_indexes),
      btree_indexes_(btree_indexes), hash_indexes_(hash_indexes), hnsw_rid_maps_(hnsw_rid_maps),
      bm25_indexes_(bm25_indexes), embedding_pool_(embedding_pool),
      algorithm_registry_(algorithm_registry),
      subquery_ctx_{catalog_, storage_, graph_engine_, provider_registry_} {}

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
        return plan_match(*match, bound, owned_exprs);
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

OutputSchema Planner::build_match_scope_schema(const MatchConfig& config) const {
    std::vector<OutputColumn> out;
    for (const auto& node : config.nodes) {
        if (node.label.empty()) {
            continue;
        }
        auto ts = catalog_.get_table(database_id_, node.label);
        if (!ts) {
            continue;
        }
        const auto& alias = node.variable.empty() ? ts->name : node.variable;
        for (const auto& col : ts->columns) {
            // Avoid duplicate columns when the same variable appears multiple
            // times (e.g. self-join patterns like (a:users)-[r]->(a:users)).
            bool dup = false;
            for (const auto& existing : out) {
                if (existing.table_name == alias && existing.name == col.name) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                out.push_back({alias, col.name, col.type_id, col.nullable, ts->table_id});
            }
        }
    }
    return OutputSchema(std::move(out));
}

// ---------------------------------------------------------------------------
// CTE injection helper
// ---------------------------------------------------------------------------

void Planner::inject_cte_bindings(
    Binder& binder, const std::unordered_map<std::string, const SelectStmt*>& cte_map) {
    // Use a fixpoint loop: CTEs may reference earlier CTEs, and since
    // cte_map is unordered we retry until no more progress is made.
    std::unordered_map<std::string, BoundStatement> already_bound;
    bool progress = true;
    while (progress) {
        progress = false;
        for (const auto& [cte_name, cte_sel_ptr] : cte_map) {
            if (already_bound.count(cte_name))
                continue;
            Binder cte_binder(catalog_, database_id_);
            for (const auto& [prev_name, prev_bound] : already_bound) {
                cte_binder.add_cte(prev_name, prev_bound);
            }
            auto cte_bound = cte_binder.bind(*cte_sel_ptr);
            if (cte_bound) {
                binder.add_cte(cte_name, *cte_bound);
                already_bound[cte_name] = *cte_bound;
                progress = true;
            }
        }
    }
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
    // Case 1: Derived table (FROM (SELECT|TRAVERSE|NEAREST|MATCH ...) AS sub).
    if (table_ref.subquery) {
        Binder binder(catalog_, database_id_, algorithm_registry_);
        auto sub_bound = binder.bind(*table_ref.subquery);
        if (!sub_bound) {
            return make_error(sub_bound.error().code, sub_bound.error().message);
        }

        // Generic dispatch: SELECT / TRAVERSE / NEAREST / MATCH all produce a
        // standard (Tuple, OutputSchema) stream that SubquerySourceOperator wraps.
        auto sub_iter = plan(*sub_bound, owned_exprs);
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

        // Bind with sibling CTEs available so nested CTE references
        // (e.g. eng_users referencing base_depts) resolve correctly.
        // Exclude the CTE itself to prevent self-shadowing: within its
        // own definition, table names resolve to the real catalog table.
        Binder binder(catalog_, database_id_, algorithm_registry_);
        auto siblings = cte_map;
        siblings.erase(to_upper(table_ref.name));
        inject_cte_bindings(binder, siblings);
        auto sub_bound = binder.bind(*cte_sel);
        if (!sub_bound) {
            return make_error(sub_bound.error().code, sub_bound.error().message);
        }

        auto sub_iter = plan_select(*cte_sel, *sub_bound, owned_exprs, siblings);
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

    // Case 3: Algorithm function call (FROM pagerank('knows', ...)).
    if (table_ref.algorithm_call) {
        auto* fn = dynamic_cast<const FunctionCallExpr*>(table_ref.algorithm_call.get());
        if (!fn) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "expected FunctionCallExpr in algorithm_call");
        }

        if (!algorithm_registry_) {
            return make_error(StatusCode::NOT_FOUND, "unknown table '" + fn->name + "'");
        }

        auto* entry = algorithm_registry_->find(fn->name);
        if (!entry) {
            return make_error(StatusCode::NOT_FOUND,
                              "unknown algorithm function '" + fn->name + "'");
        }

        // First positional arg must be the edge type name (string literal).
        if (fn->args.empty()) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "algorithm '" + fn->name +
                                  "' requires an edge type name as the first argument");
        }

        auto* edge_arg = dynamic_cast<const LiteralExpr*>(fn->args[0].get());
        if (!edge_arg || edge_arg->kind != LiteralKind::STRING) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "first argument to algorithm '" + fn->name +
                                  "' must be a string literal (edge type name)");
        }

        // Evaluate named parameters.
        Tuple empty_tuple;
        OutputSchema empty_schema;
        std::unordered_map<std::string, Value> named_args;
        for (const auto& narg : fn->named_args) {
            auto val = evaluate_expr(*narg.value, empty_tuple, empty_schema, bound);
            if (!val) {
                return make_error(val.error().code, val.error().message);
            }
            named_args.emplace(narg.name, std::move(*val));
        }

        // Validate parameters against the algorithm definition.
        auto resolved = AlgorithmRegistry::resolve_params(entry->def, named_args);
        if (!resolved) {
            return make_error(resolved.error().code, resolved.error().message);
        }

        if (!graph_engine_) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "graph engine not available for algorithm function");
        }

        AlgorithmContext ctx{*graph_engine_, database_id_, edge_arg->value, std::move(*resolved)};

        // Build output schema from algorithm definition.
        const auto& algo_alias = alias.empty() ? fn->name : alias;
        std::vector<OutputColumn> out_cols;
        out_cols.reserve(entry->def.output_columns.size());
        for (const auto& out_col : entry->def.output_columns) {
            out_cols.push_back({algo_alias, out_col.name, out_col.type_id, out_col.nullable, 0});
        }
        auto algo_schema = OutputSchema(std::move(out_cols));

        auto op = std::make_unique<AlgorithmScanOperator>(*entry, std::move(ctx), algo_schema);
        return ok(PlannedSource{std::move(op), std::move(algo_schema)});
    }

    // Case 4: TRAVERSE source (FROM TRAVERSE ...).
    if (table_ref.traverse_source) {
        auto* trav = dynamic_cast<const TraverseStmt*>(table_ref.traverse_source.get());
        if (!trav) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "expected TraverseStmt in traverse_source");
        }

        if (!graph_engine_) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "graph engine not available for TRAVERSE");
        }

        // Evaluate the start key.
        Tuple empty_tuple;
        OutputSchema empty_schema;
        auto key_val = evaluate_expr(*trav->from_key, empty_tuple, empty_schema, bound);
        if (!key_val) {
            return make_error(key_val.error().code, key_val.error().message);
        }

        // Build TraversalConfig.
        TraversalConfig config;
        config.database_id = database_id_;
        config.edge_type = trav->edge_type;
        config.start_key = std::move(*key_val);
        config.direction = trav->direction;
        config.max_depth = trav->max_depth.value_or(100);
        config.fetch = true; // Always fetch for enrichment.
        config.trace = trav->trace;

        // Resolve edge type.
        auto edge_def = catalog_.get_edge_type(database_id_, trav->edge_type);
        if (!edge_def) {
            return make_error(edge_def.error().code, edge_def.error().message);
        }

        // Resolve FROM table.
        auto from_schema = catalog_.get_table(database_id_, trav->from_table);
        if (!from_schema) {
            return make_error(from_schema.error().code, from_schema.error().message);
        }

        // Reject DIRECTION BOTH on heterogeneous edges (different source/target tables).
        if (trav->direction == TraverseDirection::BOTH &&
            edge_def->source_table_id != edge_def->target_table_id) {
            return make_error(
                StatusCode::TYPE_ERROR,
                "DIRECTION BOTH not supported for edge type '" + trav->edge_type +
                    "' because it connects different tables; use DIRECTION OUT or DIRECTION IN");
        }

        // Determine target table based on direction.
        table_id_t target_table_id = 0;
        if (trav->direction == TraverseDirection::BOTH) {
            target_table_id = (from_schema->table_id == edge_def->source_table_id)
                                  ? edge_def->target_table_id
                                  : edge_def->source_table_id;
        } else if (trav->direction == TraverseDirection::OUT) {
            target_table_id = edge_def->target_table_id;
        } else {
            target_table_id = edge_def->source_table_id;
        }

        // Resolve target table schema.
        auto target_schema = catalog_.get_table_by_id(target_table_id);
        if (!target_schema) {
            return make_error(target_schema.error().code, target_schema.error().message);
        }

        // Get target table storage.
        auto target_ts = storage_.get_table_storage(target_table_id);
        if (!target_ts) {
            return make_error(target_ts.error().code, target_ts.error().message);
        }
        auto* target_storage = *target_ts;

        // Find PK column info.
        TypeId pk_type = TypeId::INT64;
        size_t pk_col_idx = 0;
        bool pk_found = false;
        for (size_t i = 0; i < target_schema->columns.size(); ++i) {
            if (target_schema->columns[i].name == target_schema->pk_columns) {
                pk_type = target_schema->columns[i].type_id;
                pk_col_idx = i;
                pk_found = true;
                break;
            }
        }
        if (!pk_found) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "PK column '" + target_schema->pk_columns +
                                  "' not found in target table for enriched traversal");
        }

        // Coerce the start key to the FROM table's PK type (not the target's).
        // For heterogeneous edges the source and target tables may have
        // different key types; the start key must match the FROM table.
        TypeId from_pk_type = TypeId::INT64;
        for (const auto& col : from_schema->columns) {
            if (col.name == from_schema->pk_columns) {
                from_pk_type = col.type_id;
                break;
            }
        }
        auto coerced_key = fit_to_storage(config.start_key, from_pk_type);
        if (!coerced_key) {
            return make_error(coerced_key.error().code,
                              "TRAVERSE start key: " + coerced_key.error().message);
        }
        config.start_key = std::move(*coerced_key);

        // Use edge type as default alias when no explicit alias is provided
        // (matches the binder's behavior in build_traverse_scope).
        const auto& trav_alias = alias.empty() ? trav->edge_type : alias;

        const bool heterogeneous = edge_def->source_table_id != edge_def->target_table_id;
        const Expr* trav_where = trav->where_expr ? trav->where_expr.get() : nullptr;

        // For heterogeneous edges (different source/target tables), depth > 1 is
        // impossible: target nodes live in a different table and have no further
        // edges of the same type.  Cap max_depth to avoid a type mismatch when
        // the BFS tries to look up edges from the target table's PK in an index
        // keyed by the source table's PK type.
        if (heterogeneous && config.max_depth > 1) {
            config.max_depth = 1;
        }

        // Branch on mode: edge-centric vs node-centric.
        if (trav->mode == TraverseMode::EDGES) {
            // Edge mode: __from, __to, __depth + edge property columns.
            // Resolve source PK type.
            auto source_schema = catalog_.get_table_by_id(edge_def->source_table_id);
            if (!source_schema) {
                return make_error(source_schema.error().code, source_schema.error().message);
            }
            TypeId source_pk_type = TypeId::INT64;
            for (const auto& col : source_schema->columns) {
                if (col.name == source_schema->pk_columns) {
                    source_pk_type = col.type_id;
                    break;
                }
            }

            // Resolve target PK type from the edge's target table (not the BFS
            // target, which may differ for IN direction).
            TypeId edge_target_pk_type = TypeId::INT64;
            if (heterogeneous) {
                auto edge_tgt_schema = catalog_.get_table_by_id(edge_def->target_table_id);
                if (!edge_tgt_schema) {
                    return make_error(edge_tgt_schema.error().code,
                                      edge_tgt_schema.error().message);
                }
                for (const auto& col : edge_tgt_schema->columns) {
                    if (col.name == edge_tgt_schema->pk_columns) {
                        edge_target_pk_type = col.type_id;
                        break;
                    }
                }
            } else {
                edge_target_pk_type = source_pk_type; // homogeneous
            }

            std::vector<OutputColumn> out_cols;
            out_cols.push_back({trav_alias, "__from", source_pk_type, false, 0});
            out_cols.push_back({trav_alias, "__to", edge_target_pk_type, false, 0});
            out_cols.push_back({trav_alias, "__depth", TypeId::INT64, false, 0});
            if (trav->trace) {
                out_cols.push_back({trav_alias, "__path", TypeId::PATH, false, 0});
            }

            // Append edge property columns (qualified by edge type name, not alias,
            // to support the edge_type.property access syntax).
            auto edge_table = graph_engine_->get_edge_table(database_id_, trav->edge_type);
            if (edge_table) {
                for (const auto& prop_col : (*edge_table)->config().property_columns) {
                    out_cols.push_back(
                        {trav->edge_type, prop_col.name, prop_col.type, true /*nullable*/, 0});
                }
            }

            auto edge_schema = OutputSchema(std::move(out_cols));

            auto op = std::make_unique<EdgeTraversalOperator>(
                *graph_engine_, std::move(config), edge_schema, trav_where, bound, heterogeneous);

            return ok(PlannedSource{std::move(op), std::move(edge_schema)});
        }

        // Node mode (default): target columns + meta-columns + edge properties.
        std::vector<OutputColumn> out_cols;
        for (const auto& col : target_schema->columns) {
            out_cols.push_back({trav_alias, col.name, col.type_id, col.nullable, target_table_id});
        }
        out_cols.push_back({trav_alias, "__node", pk_type, false, 0});
        out_cols.push_back({trav_alias, "__depth", TypeId::INT64, false, 0});
        out_cols.push_back({trav_alias, "__source", pk_type, true, 0});
        if (trav->trace) {
            out_cols.push_back({trav_alias, "__path", TypeId::PATH, false, 0});
        }

        // Append edge property columns (nullable â€” start node has no incoming edge).
        // Qualified by edge type name for edge_type.property access syntax.
        auto edge_table = graph_engine_->get_edge_table(database_id_, trav->edge_type);
        if (edge_table) {
            for (const auto& prop_col : (*edge_table)->config().property_columns) {
                out_cols.push_back(
                    {trav->edge_type, prop_col.name, prop_col.type, true /*nullable*/, 0});
            }
        }

        auto enriched_schema = OutputSchema(std::move(out_cols));

        auto op = std::make_unique<EnrichedTraversalOperator>(*graph_engine_,
                                                              std::move(config),
                                                              enriched_schema,
                                                              trav_where,
                                                              bound,
                                                              *target_storage->heap,
                                                              target_storage->storage_schema,
                                                              pk_col_idx,
                                                              target_schema->columns.size(),
                                                              heterogeneous);

        return ok(PlannedSource{std::move(op), std::move(enriched_schema)});
    }

    // Case 5: MATCH source (FROM MATCH ...).
    if (table_ref.match_source) {
        auto* match = dynamic_cast<const MatchStmt*>(table_ref.match_source.get());
        if (!match) {
            return make_error(StatusCode::INTERNAL_ERROR, "expected MatchStmt in match_source");
        }

        if (!graph_engine_) {
            return make_error(StatusCode::INTERNAL_ERROR, "graph engine not available for MATCH");
        }

        MatchConfig match_config;
        bool has_variable_length = false;
        for (const auto& elem : match->pattern) {
            MatchNodeDef node_def;
            node_def.variable = elem.node.variable;
            node_def.label = elem.node.label;
            match_config.nodes.push_back(std::move(node_def));

            if (elem.outgoing_edge) {
                MatchEdgeDef edge_def(elem.outgoing_edge->variable,
                                      elem.outgoing_edge->edge_type,
                                      elem.outgoing_edge->direction,
                                      elem.outgoing_edge->min_hops,
                                      elem.outgoing_edge->max_hops);
                if (edge_def.is_variable_length()) {
                    has_variable_length = true;
                }
                match_config.edges.push_back(std::move(edge_def));
            }
        }

        // Use full-scope schema (all columns from all node tables) so that
        // the outer SELECT's WHERE clause can reference columns not in the
        // SELECT list.  The outer ProjectOperator narrows to SELECT columns.
        auto scope_schema = build_match_scope_schema(match_config);

        std::unique_ptr<Iterator> iter;
        if (has_variable_length) {
            iter = std::make_unique<VariableLengthMatchOperator>(
                *graph_engine_,
                catalog_,
                storage_,
                database_id_,
                std::move(match_config),
                std::move(scope_schema),
                nullptr, // WHERE handled by outer SELECT
                bound);
        } else {
            iter = std::make_unique<PatternMatchOperator>(*graph_engine_,
                                                          catalog_,
                                                          storage_,
                                                          database_id_,
                                                          std::move(match_config),
                                                          std::move(scope_schema),
                                                          nullptr, // WHERE handled by outer SELECT
                                                          bound);
        }

        auto out_schema = iter->output_schema();
        return ok(PlannedSource{std::move(iter), std::move(out_schema)});
    }

    // Case 6: Virtual catalog table (pg_catalog).
    if (Catalog::is_virtual_schema(table_ref.schema)) {
        auto vt = catalog_.get_virtual_table(table_ref.name);
        if (!vt) {
            return make_error(vt.error().code, vt.error().message);
        }
        auto ts = vt->to_table_schema();
        auto table_output = build_table_output_schema(ts, alias);
        auto scan = std::make_unique<VirtualCatalogScanOperator>(std::move(*vt), table_output);
        return ok(PlannedSource{std::move(scan), std::move(table_output)});
    }

    // Case 7: Physical table.
    auto table_schema = catalog_.get_table(database_id_, table_ref.name);
    if (!table_schema) {
        // Fallback: unqualified pg_catalog table names (e.g. "pg_type" without
        // "pg_catalog." prefix). psqlODBC and other PostgreSQL tools query these
        // without a schema qualifier.
        if (table_ref.schema.empty()) {
            auto vt = catalog_.get_virtual_table(table_ref.name);
            if (vt) {
                auto ts = vt->to_table_schema();
                auto table_output = build_table_output_schema(ts, alias);
                auto scan =
                    std::make_unique<VirtualCatalogScanOperator>(std::move(*vt), table_output);
                return ok(PlannedSource{std::move(scan), std::move(table_output)});
            }
        }
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
    annotate_seq_scan_cost(*table_schema, *scan);

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
    // --- EXISTS (subquery) â†’ SEMI join ---
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

    // --- NOT EXISTS (subquery) â†’ ANTI join ---
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

    // --- IN (subquery) â†’ SEMI join ---
    if (auto* in_expr = dynamic_cast<const InExpr*>(&where_expr)) {
        if (in_expr->subquery) {
            auto* sub_sel = dynamic_cast<const SelectStmt*>(in_expr->subquery.get());
            // SELECT subqueries are decorrelated only when they have a FROM
            // clause; TRAVERSE / NEAREST / MATCH subqueries (sub_sel == null)
            // always produce a tuple stream we can materialise and SEMI-join.
            bool can_rewrite = sub_sel ? !sub_sel->from.empty() : true;
            if (can_rewrite) {
                // Plan the entire subquery (not just the FROM table), so that
                // any filters, aggregations, etc. inside the subquery are
                // correctly applied.
                Binder binder(catalog_, database_id_, algorithm_registry_);
                // Inject outer CTE bindings so the subquery can reference CTEs.
                inject_cte_bindings(binder, cte_map);
                auto sub_bound = binder.bind(*in_expr->subquery);
                if (!sub_bound) {
                    // The subquery does not bind standalone â€” most likely it is
                    // correlated (references the outer row). Decorrelation into a
                    // join is not possible; leave the predicate to be applied as a
                    // filter, where eval_in re-executes it per outer row.
                    return ok(static_cast<const Expr*>(&where_expr));
                }

                // SELECT keeps cte_map propagation for nested CTE references;
                // graph/vector statements dispatch through the generic planner.
                auto sub_iter = sub_sel ? plan_select(*sub_sel, *sub_bound, owned_exprs, cte_map)
                                        : plan(*sub_bound, owned_exprs);
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
                    // Use the original expression pointer â€” it outlives the plan.
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

            // Both sides have remaining conditions â€” recombine with AND.
            // We need to return the original expression since both sub-parts
            // are still the original AST nodes connected by the AND.
            return ok(static_cast<const Expr*>(&where_expr));
        }
    }

    // No subquery found â€” return the expression as-is for filter.
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

    // -- 1b. Analyze NEAREST pushdown through JOINs (GDB-1250). ------------------
    // A NEAREST predicate in a query with JOINs is planned by pushing the vector
    // scan down to the table that owns the searched EMBEDDING column; that scan
    // becomes the owning side's source so the join consumes its k nearest rows
    // (and the synthetic `_distance` column lands in the combined schema). All v1
    // restrictions are validated here, before any operator is built.
    NearestJoinPlan nearest_plan;
    if (has_joins) {
        // NEAREST inside a join ON clause is not supported, regardless of
        // whether the WHERE clause also contains a NEAREST predicate.
        for (const auto& join_clause : stmt.joins) {
            if (expr_contains_nearest(join_clause.on_expr.get())) {
                return make_error(StatusCode::INVALID_ARGUMENT,
                                  "NEAREST(...) is not supported inside a JOIN ON clause");
            }
        }
    }
    if (has_joins && expr_contains_nearest(stmt.where_expr.get())) {
        auto nearest_conjuncts = extract_conjuncts(*stmt.where_expr);

        // v1: exactly one NEAREST conjunct keeps the single `_distance` column
        // unambiguous.
        if (count_nearest_conjuncts(nearest_conjuncts) > 1) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "only one NEAREST(...) predicate is supported per query with a JOIN");
        }

        // NEAREST and MATCH cannot drive the same scan.
        if (expr_contains_match(stmt.where_expr.get())) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "cannot combine NEAREST and MATCH in the same WHERE clause");
        }

        // The NEAREST must sit in a usable position (top-level / AND conjunct).
        const NearestExpr* nearest = extract_nearest_predicate(stmt.where_expr.get());
        if (nearest == nullptr) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "NEAREST(...) is only supported as a top-level AND predicate");
        }
        auto* nearest_col = dynamic_cast<const ColumnRefExpr*>(nearest->column.get());
        if (nearest_col == nullptr) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "NEAREST(...) requires a column reference");
        }

        // Resolve which FROM/JOIN entry owns the searched column. The column's
        // qualifier (if any) names a query alias; an unqualified column resolves
        // against whichever table actually has it.
        struct CandidateTable {
            std::string alias;
            std::string table_name;
            bool is_base;
            JoinType join_type; ///< INNER for the base table.
            bool is_nullable;   ///< EFFECTIVE nullability of this input's rows.
            TableSchema schema; ///< owned copy â€” catalog returns by value.
        };
        std::vector<CandidateTable> candidates;
        {
            // The base (left-most) input is NULL-padded â€” and therefore nullable â€”
            // when any join in the statement is RIGHT or FULL, since those preserve
            // the right side and null-fill unmatched base rows. INNER/LEFT/CROSS all
            // preserve the base side. This is what makes `books b RIGHT JOIN reviews r`
            // expose the base table `books` as the nullable side (GDB-1254): gating on
            // is_base alone wrongly assumed the base was always preserved.
            bool base_is_nullable = false;
            for (const auto& jc : stmt.joins) {
                if (jc.type == JoinType::RIGHT || jc.type == JoinType::FULL) {
                    base_is_nullable = true;
                    break;
                }
            }
            auto base_schema = catalog_.get_table(database_id_, table_ref.name);
            if (base_schema && !table_ref.subquery && !table_ref.traverse_source &&
                !table_ref.match_source) {
                candidates.push_back({alias,
                                      table_ref.name,
                                      true,
                                      JoinType::INNER,
                                      base_is_nullable,
                                      std::move(*base_schema)});
            }
            for (const auto& jc : stmt.joins) {
                const auto& jtref = jc.table;
                const auto& jalias = jtref.alias.empty() ? jtref.name : jtref.alias;
                auto js = catalog_.get_table(database_id_, jtref.name);
                if (js && !jtref.subquery && !jtref.traverse_source && !jtref.match_source) {
                    // A joined (right) input is NULL-padded when its own join is LEFT
                    // or FULL; RIGHT and INNER preserve it.
                    const bool joined_is_nullable =
                        (jc.type == JoinType::LEFT || jc.type == JoinType::FULL);
                    candidates.push_back(
                        {jalias, jtref.name, false, jc.type, joined_is_nullable, std::move(*js)});
                }
            }
        }

        const CandidateTable* owner = nullptr;
        for (const auto& cand : candidates) {
            bool matches = false;
            if (!nearest_col->table.empty()) {
                matches = (cand.alias == nearest_col->table);
            } else {
                // Unqualified: the owner is the table that has the column.
                for (const auto& col : cand.schema.columns) {
                    if (to_upper(col.name) == to_upper(nearest_col->column)) {
                        matches = true;
                        break;
                    }
                }
            }
            if (matches) {
                owner = &cand;
                break;
            }
        }
        if (owner == nullptr) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "NEAREST(...) references a column on a table that is not a base or "
                              "joined physical table");
        }

        // The searched column must be an EMBEDDING column on the owner table.
        const CatalogColumnDef* emb_col = nullptr;
        for (const auto& col : owner->schema.columns) {
            if (to_upper(col.name) == to_upper(nearest_col->column)) {
                emb_col = &col;
                break;
            }
        }
        if (emb_col == nullptr) {
            return make_error(StatusCode::NOT_FOUND,
                              "column " + nearest_col->column + " not found in table " +
                                  owner->table_name);
        }
        if (emb_col->type_id != TypeId::EMBEDDING) {
            return make_error(StatusCode::TYPE_ERROR,
                              "NEAREST(...) requires an EMBEDDING column; " + nearest_col->column +
                                  " is not an embedding");
        }

        // v1: NEAREST on the nullable side of an OUTER join is rejected â€” the
        // scan would produce padding NULL rows that have no meaningful distance.
        // Any preserved position (the base under INNER/LEFT, a joined table under
        // RIGHT, etc.) is allowed. We gate on the EFFECTIVE nullability of the
        // owning input rather than on is_base, because under a RIGHT/FULL join the
        // base table is itself the nullable side (GDB-1254).
        if (owner->is_nullable) {
            return make_error(
                StatusCode::INVALID_ARGUMENT,
                "NEAREST(...) on the nullable side of an OUTER JOIN is not supported");
        }

        // Synthesize the residual: the NEAREST conjunct (evaluates TRUE; the scan
        // already produced the k nearest rows) plus the owner's own sibling
        // conjuncts, so filtered-kNN semantics (filter BEFORE top-k) are
        // preserved inside the scan rather than applied above it.
        std::vector<const Expr*> residual_conjuncts;
        for (const Expr* c : nearest_conjuncts) {
            if (expr_contains_nearest(c) || is_single_table_predicate(*c, owner->alias)) {
                residual_conjuncts.push_back(c);
            }
        }
        const Expr* residual = build_owned_and_chain(residual_conjuncts, owned_exprs);
        if (residual == nullptr) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "failed to synthesize NEAREST residual predicate for JOIN pushdown");
        }

        nearest_plan.active = true;
        nearest_plan.nearest = nearest;
        nearest_plan.owner_alias = owner->alias;
        nearest_plan.owner_table = owner->table_name;
        nearest_plan.residual = residual;
    }

    // -- 2. Optionally push WHERE into scan (only for physical tables with no joins).
    // For subquery/CTE sources or when joins are present, don't push WHERE.
    bool pushed_where = false;
    if (!has_joins && stmt.where_expr && !table_ref.subquery && !table_ref.traverse_source &&
        !table_ref.match_source && cte_map.find(to_upper(table_ref.name)) == cte_map.end()) {
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

                    // NEAREST vector predicate: build a distance-ranked scan
                    // that appends a synthetic _distance column. Checked first so
                    // it takes precedence; combining NEAREST and MATCH in one
                    // WHERE is unsupported (a single scan drives the result).
                    if (expr_contains_nearest(stmt.where_expr.get())) {
                        if (expr_contains_match(stmt.where_expr.get())) {
                            return make_error(
                                StatusCode::INVALID_ARGUMENT,
                                "cannot combine NEAREST and MATCH in the same WHERE clause");
                        }
                        auto vec_scan =
                            try_plan_vector_scan(*table_schema, stmt.where_expr.get(), bound);
                        if (!vec_scan) {
                            return make_error(vec_scan.error().code, vec_scan.error().message);
                        }
                        // plan_nearest_impl builds the (table columns + _distance)
                        // output schema; adopt it so downstream projection /
                        // ORDER BY resolve against the same columns.
                        source->schema = (*vec_scan)->output_schema();
                        source->iter = std::move(*vec_scan);
                        pushed_where = true;
                    } else if (expr_contains_match(stmt.where_expr.get())) {
                        // BM25 full-text predicate: build a relevance-ranked scan
                        // that appends a synthetic _score column.
                        std::vector<OutputColumn> score_cols = table_output.columns();
                        score_cols.push_back({alias.empty() ? table_ref.name : alias,
                                              "_score",
                                              TypeId::FLOAT64,
                                              false,
                                              table_schema->table_id});
                        auto score_output = OutputSchema(score_cols);
                        auto bm25_scan = try_plan_bm25_scan(
                            *table_schema, *ts, score_output, stmt.where_expr.get(), bound);
                        if (!bm25_scan) {
                            return make_error(bm25_scan.error().code, bm25_scan.error().message);
                        }
                        source->iter = std::move(*bm25_scan);
                        source->schema = std::move(score_output);
                        pushed_where = true;
                    } else {
                        // Cost-based access path selection (GDB-754): when
                        // ANALYZE statistics exist for this table, let the
                        // optimizer decide between index scan and seq scan.
                        // Without statistics this preserves the default
                        // behavior (use a matching index when available).
                        auto decision = decide_access_path(*table_schema, stmt.where_expr.get());

                        std::unique_ptr<Iterator> chosen;
                        if (decision.try_index) {
                            auto idx_scan = try_plan_index_scan(
                                *table_schema, *ts, table_output, stmt.where_expr.get(), bound);
                            if (idx_scan && *idx_scan) {
                                chosen = std::move(*idx_scan);
                                if (decision.cost.has_value()) {
                                    chosen->set_estimated_cost(*decision.cost);
                                }
                            }
                        }
                        if (!chosen) {
                            // Sequential scan with predicate pushdown â€” either
                            // the optimizer chose it, or no usable index exists.
                            chosen = std::make_unique<SeqScanOperator>(*storage->heap,
                                                                       storage->storage_schema,
                                                                       table_output,
                                                                       stmt.where_expr.get(),
                                                                       &bound);
                            if (decision.seq_cost.has_value()) {
                                chosen->set_estimated_cost(*decision.seq_cost);
                            }
                        }
                        source->iter = std::move(chosen);
                        source->schema = std::move(table_output);
                        pushed_where = true;
                    }
                }
            }
        }
    }

    std::unique_ptr<Iterator> child = std::move(source->iter);

    // Predicate pushdown state â€” declared here so the post-join filter
    // logic can access which conjuncts were consumed.
    std::vector<const Expr*> where_conjuncts;
    std::vector<bool> conjunct_consumed;

    // -- 2b. JOIN operators ---------------------------------------------------
    if (has_joins) {
        // â”€â”€ Predicate pushdown: decompose WHERE into per-table filters â”€â”€â”€â”€
        // For INNER joins, single-table predicates can be pushed down to
        // the individual table scans, dramatically reducing join input size.
        if (stmt.where_expr) {
            where_conjuncts = extract_conjuncts(*stmt.where_expr);
            conjunct_consumed.resize(where_conjuncts.size(), false);

            // â”€â”€ NEAREST pushdown: base table owns the EMBEDDING column â”€â”€â”€â”€â”€â”€â”€
            // Replace the base scan with a NearestScanOperator carrying the
            // synthesized residual (NEAREST + the base table's sibling
            // conjuncts, applied BEFORE top-k). Mark every residual conjunct
            // consumed so the generic pushdown below does not also wrap them in
            // plain FilterOperators (which would re-apply or no-op them).
            if (nearest_plan.active && nearest_plan.owner_alias == alias) {
                auto* owner_col =
                    dynamic_cast<const ColumnRefExpr*>(nearest_plan.nearest->column.get());
                // Validated during analysis; defensive only.
                if (owner_col == nullptr) {
                    return make_error(StatusCode::INVALID_ARGUMENT,
                                      "NEAREST(...) requires a column reference");
                }
                // Plan directly from the original NEAREST node so the graph
                // scope (WITHIN TRAVERSE) is preserved; the synthesized residual
                // (NEAREST + owner siblings) is the filter applied BEFORE top-k.
                auto vec_scan = plan_nearest_impl(nearest_plan.owner_table,
                                                  owner_col->column,
                                                  nearest_plan.nearest->k.get(),
                                                  nearest_plan.nearest->target.get(),
                                                  nearest_plan.nearest->metric,
                                                  nearest_plan.nearest->within_traverse.get(),
                                                  nearest_plan.residual,
                                                  bound,
                                                  nearest_plan.owner_alias);
                if (!vec_scan) {
                    return make_error(vec_scan.error().code, vec_scan.error().message);
                }
                child = std::move(*vec_scan);
                for (size_t i = 0; i < where_conjuncts.size(); ++i) {
                    if (expr_contains_nearest(where_conjuncts[i]) ||
                        is_single_table_predicate(*where_conjuncts[i], alias)) {
                        conjunct_consumed[i] = true;
                    }
                }
            }

            // Push predicates that belong to the base table. Skip conjuncts
            // already consumed by a NEAREST base scan above (they travelled into
            // the scan's residual and must not be re-applied here).
            for (size_t i = 0; i < where_conjuncts.size(); ++i) {
                if (!conjunct_consumed[i] &&
                    is_single_table_predicate(*where_conjuncts[i], alias)) {
                    child = std::make_unique<FilterOperator>(
                        std::move(child), *where_conjuncts[i], bound, &subquery_ctx_);
                    conjunct_consumed[i] = true;
                }
            }
        }

        for (const auto& join_clause : stmt.joins) {
            const auto& jtref = join_clause.table;
            const auto& join_alias = jtref.alias.empty() ? jtref.name : jtref.alias;

            auto join_source = plan_from_source(jtref, join_alias, cte_map, bound, owned_exprs);
            if (!join_source) {
                return make_error(join_source.error().code, join_source.error().message);
            }

            // â”€â”€ NEAREST pushdown: this join source owns the EMBEDDING column â”€â”€
            // Replace the join source with a NearestScanOperator carrying the
            // synthesized residual (applied BEFORE top-k). Its output schema
            // (table columns aliased + `_distance`) is adopted so `_distance`
            // lands in the join's combined schema, and the residual conjuncts
            // are marked consumed so the generic pushdown below skips them.
            if (nearest_plan.active && nearest_plan.owner_alias == join_alias) {
                auto* owner_col =
                    dynamic_cast<const ColumnRefExpr*>(nearest_plan.nearest->column.get());
                if (owner_col == nullptr) {
                    return make_error(StatusCode::INVALID_ARGUMENT,
                                      "NEAREST(...) requires a column reference");
                }
                // Plan directly from the original NEAREST node so the graph
                // scope (WITHIN TRAVERSE) is preserved; the synthesized residual
                // is applied BEFORE top-k inside the scan.
                auto vec_scan = plan_nearest_impl(nearest_plan.owner_table,
                                                  owner_col->column,
                                                  nearest_plan.nearest->k.get(),
                                                  nearest_plan.nearest->target.get(),
                                                  nearest_plan.nearest->metric,
                                                  nearest_plan.nearest->within_traverse.get(),
                                                  nearest_plan.residual,
                                                  bound,
                                                  nearest_plan.owner_alias);
                if (!vec_scan) {
                    return make_error(vec_scan.error().code, vec_scan.error().message);
                }
                join_source->schema = (*vec_scan)->output_schema();
                join_source->iter = std::move(*vec_scan);
                for (size_t i = 0; i < where_conjuncts.size(); ++i) {
                    if (expr_contains_nearest(where_conjuncts[i]) ||
                        is_single_table_predicate(*where_conjuncts[i], join_alias)) {
                        conjunct_consumed[i] = true;
                    }
                }
            }

            // â”€â”€ Push single-table predicates into the join source â”€â”€â”€â”€â”€â”€â”€â”€â”€
            // Only safe for INNER joins â€” outer join nullable-side predicates
            // must remain in the post-join filter.
            if (join_clause.type == JoinType::INNER && !where_conjuncts.empty()) {
                for (size_t i = 0; i < where_conjuncts.size(); ++i) {
                    if (!conjunct_consumed[i] &&
                        is_single_table_predicate(*where_conjuncts[i], join_alias)) {
                        join_source->iter =
                            std::make_unique<FilterOperator>(std::move(join_source->iter),
                                                             *where_conjuncts[i],
                                                             bound,
                                                             &subquery_ctx_);
                        conjunct_consumed[i] = true;
                    }
                }
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

            // â”€â”€ Choose join method: HashJoin or SortMergeJoin for equi-joins,
            // NestedLoop otherwise.
            // When both inputs carry optimizer cost estimates (i.e. ANALYZE
            // statistics exist), the cost-based optimizer picks the method
            // instead (GDB-754). GDB-803: SORT_MERGE is now wired directly to
            // SortMergeJoinOperator instead of falling back to HashJoin.
            bool used_equi_join = false;
            bool stats_prefer_nested_loop = false;
            bool stats_prefer_sort_merge = false;
            std::optional<PlanCostEstimate> join_cost;
            {
                const PlanCostEstimate* lcost = find_cost_in_chain(child.get());
                const PlanCostEstimate* rcost = find_cost_in_chain(join_source->iter.get());
                if (lcost != nullptr && rcost != nullptr) {
                    const CostModel cost_model;
                    // Compute ACTUAL sortedness of each join input on the join key.
                    // An input is considered pre-sorted only when it genuinely
                    // produces rows ordered by the equi-join key — i.e. the child
                    // is an IndexScanOperator on that column (B+ tree key order),
                    // or an ExternalSortOperator whose leading key matches.
                    // This prevents SMJ from winning the cost comparison when
                    // inputs are NOT actually ordered (e.g. SeqScan + HashJoin
                    // is cheaper for small unsorted tables). SMJ remains reachable
                    // when inputs ARE genuinely pre-sorted on the join key.
                    const Expr* lkey_hint = nullptr;
                    const Expr* rkey_hint = nullptr;
                    if (on_expr != nullptr) {
                        const auto* bin = dynamic_cast<const BinaryExpr*>(on_expr);
                        if (bin != nullptr && bin->op == BinaryOp::EQUAL) {
                            // Determine which side of the binary expr belongs to
                            // the left vs. right child (same logic as below).
                            const auto* lhs_col =
                                dynamic_cast<const ColumnRefExpr*>(bin->lhs.get());
                            bool lhs_is_right = false;
                            if (lhs_col != nullptr) {
                                for (size_t c = 0; c < right_schema_ref.column_count(); ++c) {
                                    if (right_schema_ref.column(c).table_name == lhs_col->table) {
                                        lhs_is_right = true;
                                        break;
                                    }
                                }
                            }
                            lkey_hint = lhs_is_right ? bin->rhs.get() : bin->lhs.get();
                            rkey_hint = lhs_is_right ? bin->lhs.get() : bin->rhs.get();
                        }
                    }
                    const bool left_sorted = is_sorted_on_join_key(child.get(), lkey_hint);
                    const bool right_sorted =
                        is_sorted_on_join_key(join_source->iter.get(), rkey_hint);
                    auto [method, jc] = choose_join_method(to_plan_cost(*lcost),
                                                           to_plan_cost(*rcost),
                                                           kDefaultSelectivity,
                                                           left_sorted,
                                                           right_sorted,
                                                           cost_model);
                    join_cost = to_cost_estimate(jc);
                    stats_prefer_nested_loop = (method == JoinMethod::NESTED_LOOP);
                    stats_prefer_sort_merge = (method == JoinMethod::SORT_MERGE);
                }
            }
            if (on_expr && !stats_prefer_nested_loop) {
                auto* bin = dynamic_cast<const BinaryExpr*>(on_expr);
                if (bin && bin->op == BinaryOp::EQUAL) {
                    auto* lhs_col = dynamic_cast<const ColumnRefExpr*>(bin->lhs.get());
                    auto* rhs_col = dynamic_cast<const ColumnRefExpr*>(bin->rhs.get());
                    if (lhs_col && rhs_col) {
                        // Equi-join detected. Determine which key belongs to which side.
                        // The left child's schema contains columns from previously joined
                        // tables; the right child is the current join source.
                        const Expr* left_key = bin->lhs.get();
                        const Expr* right_key = bin->rhs.get();

                        // Check if lhs belongs to the right side (join source).
                        // If so, swap left/right keys.
                        bool lhs_is_right = false;
                        for (size_t c = 0; c < right_schema_ref.column_count(); ++c) {
                            if (right_schema_ref.column(c).table_name == lhs_col->table) {
                                lhs_is_right = true;
                                break;
                            }
                        }
                        if (lhs_is_right) {
                            std::swap(left_key, right_key);
                        }

                        if (stats_prefer_sort_merge) {
                            // Cost model chose sort-merge: use SortMergeJoinOperator.
                            child = std::make_unique<SortMergeJoinOperator>(
                                std::move(child),
                                std::move(join_source->iter),
                                join_clause.type,
                                left_key,
                                right_key,
                                bound,
                                std::move(combined));
                        } else {
                            // Default equi-join: HashJoin.
                            child = std::make_unique<HashJoinOperator>(std::move(child),
                                                                       std::move(join_source->iter),
                                                                       join_clause.type,
                                                                       left_key,
                                                                       right_key,
                                                                       bound,
                                                                       std::move(combined));
                        }
                        used_equi_join = true;
                    }
                }
            }

            if (!used_equi_join) {
                child = std::make_unique<NestedLoopJoinOperator>(std::move(child),
                                                                 std::move(join_source->iter),
                                                                 join_clause.type,
                                                                 on_expr,
                                                                 bound,
                                                                 std::move(combined));
            }
            if (join_cost.has_value()) {
                child->set_estimated_cost(*join_cost);
            }
        }

        // Mark WHERE as pushed if all conjuncts were consumed.
        if (!where_conjuncts.empty()) {
            bool all_consumed = true;
            for (bool c : conjunct_consumed) {
                if (!c) {
                    all_consumed = false;
                    break;
                }
            }
            if (all_consumed) {
                pushed_where = true;
            }
        }
    }

    // -- 2c. Subquery predicate rewriting (EXISTS/NOT EXISTS/IN) ----------------
    const Expr* remaining_where = nullptr;
    if (stmt.where_expr && !pushed_where) {
        // When some conjuncts were pushed but others remain, rebuild a WHERE
        // from just the unconsumed conjuncts.
        if (has_joins && !conjunct_consumed.empty()) {
            std::vector<const Expr*> remaining;
            for (size_t i = 0; i < where_conjuncts.size(); ++i) {
                if (!conjunct_consumed[i]) {
                    remaining.push_back(where_conjuncts[i]);
                }
            }
            if (remaining.empty()) {
                // All pushed â€” nothing to apply.
                pushed_where = true;
            } else if (remaining.size() == 1) {
                remaining_where = remaining[0];
            } else {
                // Rebuild AND chain from remaining predicates.
                // Build right-to-left: (a AND (b AND c))
                const Expr* chain = remaining.back();
                for (int i = static_cast<int>(remaining.size()) - 2; i >= 0; --i) {
                    auto and_expr = std::make_unique<BinaryExpr>();
                    and_expr->op = BinaryOp::AND;
                    // Clone references (non-owning) by wrapping.
                    and_expr->lhs = nullptr; // placeholder
                    and_expr->rhs = nullptr; // placeholder
                    // Since we can't easily own new AND nodes, apply remaining
                    // predicates as chained FilterOperators instead.
                    (void)chain;
                    (void)and_expr;
                }
                // Simpler approach: chain FilterOperators for each remaining predicate.
                for (const Expr* pred : remaining) {
                    child = std::make_unique<FilterOperator>(
                        std::move(child), *pred, bound, &subquery_ctx_);
                }
                pushed_where = true;
                remaining_where = nullptr;
            }
        } else {
            auto rewrite_result =
                rewrite_subquery_predicates(*stmt.where_expr, child, bound, cte_map, owned_exprs);
            if (!rewrite_result) {
                return make_error(rewrite_result.error().code, rewrite_result.error().message);
            }
            remaining_where = *rewrite_result;
        }
    }

    // -- 2d. Apply remaining WHERE as a filter ---------------------------------
    if (remaining_where) {
        child = std::make_unique<FilterOperator>(
            std::move(child), *remaining_where, bound, &subquery_ctx_);
    }

    // -- 2e. Window functions -------------------------------------------------
    // Detect window function expressions in SELECT items and build a
    // WindowOperator to evaluate them. Window functions see all input rows
    // (after filtering) and append result columns to the child schema.
    std::vector<const WindowFunctionExpr*> win_exprs;
    for (auto& item : stmt.items) {
        if (item.expr && is_window_expr(*item.expr, bound)) {
            win_exprs.push_back(dynamic_cast<const WindowFunctionExpr*>(item.expr.get()));
        }
    }

    // Map from WindowFunctionExpr* â†’ synthesised column name in WindowOperator output.
    std::unordered_map<const Expr*, std::string> win_map;

    if (!win_exprs.empty()) {
        // All window functions in this query share the same WindowOperator.
        // Use PARTITION BY / ORDER BY from the first window function that has them.
        // (Full SQL allows per-function OVER specs; we support one spec per query
        // here, which covers the common case.)
        std::vector<const Expr*> partition_by_ptrs;
        std::vector<SortKey> order_by_keys;

        for (auto* wexpr : win_exprs) {
            if (!wexpr->partition_by.empty() && partition_by_ptrs.empty()) {
                for (auto& pb : wexpr->partition_by) {
                    partition_by_ptrs.push_back(pb.get());
                }
            }
            if (!wexpr->order_by.empty() && order_by_keys.empty()) {
                for (auto& ob : wexpr->order_by) {
                    order_by_keys.push_back({ob.expr.get(), ob.direction});
                }
            }
        }

        // Build WindowFunctionDescriptors.
        std::vector<WindowFunctionDescriptor> win_descs;
        for (size_t i = 0; i < win_exprs.size(); ++i) {
            auto* wexpr = win_exprs[i];
            WindowFunctionDescriptor desc;
            desc.func = resolve_window_func(wexpr->name);

            // Set argument expression (for LAG, LEAD, SUM, AVG, etc.).
            if (!wexpr->args.empty()) {
                auto* first_arg = wexpr->args[0].get();
                // Skip COUNT(*) star argument.
                if (auto* cref = dynamic_cast<const ColumnRefExpr*>(first_arg)) {
                    if (cref->column != "*") {
                        desc.arg = first_arg;
                    }
                } else {
                    desc.arg = first_arg;
                }
            }

            // LAG/LEAD offset (second argument).
            if ((desc.func == WindowFunc::LAG || desc.func == WindowFunc::LEAD) &&
                wexpr->args.size() >= 2) {
                if (auto* lit = dynamic_cast<const LiteralExpr*>(wexpr->args[1].get())) {
                    desc.offset = std::stoll(lit->value);
                }
            }

            // NTILE bucket count (first argument).
            if (desc.func == WindowFunc::NTILE && !wexpr->args.empty()) {
                if (auto* lit = dynamic_cast<const LiteralExpr*>(wexpr->args[0].get())) {
                    desc.ntile_buckets = std::stoll(lit->value);
                }
            }

            // Frame specification.
            if (wexpr->frame.has_value()) {
                auto& af = *wexpr->frame;
                desc.frame.type =
                    (af.type == AstFrameType::ROWS) ? FrameType::ROWS : FrameType::RANGE;
                desc.frame.start_bound = convert_frame_bound(af.start_bound);
                desc.frame.start_offset = af.start_offset;
                desc.frame.end_bound = convert_frame_bound(af.end_bound);
                desc.frame.end_offset = af.end_offset;
            } else if (wexpr->order_by.empty()) {
                // SQL standard: when no ORDER BY and no explicit frame, the
                // default frame is the entire partition (UNBOUNDED PRECEDING
                // to UNBOUNDED FOLLOWING), not UNBOUNDED PRECEDING to CURRENT ROW.
                desc.frame.end_bound = FrameBound::UNBOUNDED_FOLLOWING;
            }

            std::string col_name = "__win_" + std::to_string(i);
            win_map[wexpr] = col_name;
            win_descs.push_back(std::move(desc));
        }

        // Build output schema: child columns + window result columns.
        const auto& child_schema = child->output_schema();
        std::vector<OutputColumn> win_out_cols;
        for (size_t i = 0; i < child_schema.column_count(); ++i) {
            win_out_cols.push_back(child_schema.column(i));
        }
        for (size_t i = 0; i < win_exprs.size(); ++i) {
            std::string col_name = "__win_" + std::to_string(i);
            auto it = bound.expr_types.find(win_exprs[i]);
            TypeId type = TypeId::INT64;
            bool nullable = true;
            if (it != bound.expr_types.end()) {
                type = it->second.type_id;
                nullable = it->second.nullable;
            }
            win_out_cols.push_back({"", col_name, type, nullable, 0});
        }
        auto win_schema = OutputSchema(std::move(win_out_cols));

        child = std::make_unique<WindowOperator>(std::move(child),
                                                 std::move(partition_by_ptrs),
                                                 std::move(order_by_keys),
                                                 std::move(win_descs),
                                                 bound,
                                                 std::move(win_schema));
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
        std::unordered_map<const Expr*, std::string>
            agg_map; // FunctionCallExpr* â†’ output col name

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

        // -- 3c. COUNT(*) fast path -----------------------------------------------
        // Detect bare COUNT(*): single COUNT_STAR aggregate, no GROUP BY,
        // no WHERE, no JOINs, single physical table source. When matched,
        // use CountScanOperator to skip tuple deserialization entirely.
        bool used_count_fast_path = false;
        if (!has_group_by && agg_descs.size() == 1 && agg_descs[0].func == AggFunc::COUNT_STAR &&
            !stmt.where_expr && !stmt.having_expr && stmt.joins.empty() && stmt.from.size() == 1 &&
            !stmt.from[0].subquery && !stmt.from[0].traverse_source && !stmt.from[0].match_source &&
            cte_map.find(to_upper(stmt.from[0].name)) == cte_map.end()) {
            const auto& tref = stmt.from[0];
            auto table_schema = catalog_.get_table(database_id_, tref.name);
            if (table_schema) {
                auto ts = storage_.get_table_storage(table_schema->table_id);
                if (ts) {
                    auto* storage = *ts;
                    // Build output schema matching what HashAggregate would produce:
                    // a single __agg_0 column of type INT64.
                    std::string col_name = "__agg_0";
                    auto it = bound.expr_types.find(unique_aggs[0]);
                    TypeId type = TypeId::INT64;
                    bool nullable = true;
                    if (it != bound.expr_types.end()) {
                        type = it->second.type_id;
                        nullable = it->second.nullable;
                    }
                    auto count_schema = OutputSchema({{"", col_name, type, nullable, 0}});
                    child = std::make_unique<CountScanOperator>(*storage->heap,
                                                                std::move(count_schema));
                    used_count_fast_path = true;
                }
            }
        }

        if (!used_count_fast_path) {
            // -- 3c-normal. Build aggregate output schema -------------------------
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
                    // Resolve SELECT-list aliases: if the ORDER BY expression is an
                    // unqualified column ref matching a SELECT alias, use the
                    // original SELECT expression so aggregate rewriting applies.
                    const Expr* effective_expr = ob.expr.get();
                    if (auto* cref = dynamic_cast<const ColumnRefExpr*>(ob.expr.get());
                        cref && cref->table.empty()) {
                        std::string upper = to_upper(cref->column);
                        for (const auto& item : stmt.items) {
                            if (!item.alias.empty() && to_upper(item.alias) == upper && item.expr) {
                                effective_expr = item.expr.get();
                                break;
                            }
                        }
                    }

                    if (contains_any_aggregate(*effective_expr, bound)) {
                        auto rewritten = rewrite_expr(*effective_expr, agg_map);
                        auto* ptr = rewritten.get();
                        owned_exprs.push_back(std::move(rewritten));
                        keys.push_back({ptr, ob.direction});
                    } else {
                        keys.push_back({effective_expr, ob.direction});
                    }
                }
                // GDB-803: Use ExternalSortOperator so large sorts spill to disk
                // rather than running out of memory. It degrades gracefully to
                // an in-memory sort when the input fits in work_mem.
                child = std::make_unique<ExternalSortOperator>(
                    std::move(child), std::move(keys), bound);
            }

            // -- 3g. Build rewritten projections ----------------------------------
            std::vector<ProjectionExpr> projections;
            for (const auto& item : stmt.items) {
                if (item.is_star || !item.table_star.empty()) {
                    // SELECT * with GROUP BY â€” handled by binder validation (error).
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
                    // Pure column reference or literal â€” use as-is.
                    projections.push_back({item.expr.get(), col_alias});
                }
            }

            auto output_schema = build_output_schema(bound.output_columns);
            child = std::make_unique<ProjectOperator>(std::move(child),
                                                      std::move(projections),
                                                      std::move(output_schema),
                                                      bound,
                                                      &subquery_ctx_);
        } // end if (!used_count_fast_path)

        // For the fast path, we still need projection to produce the final
        // output schema (e.g. renaming __agg_0 â†’ count).
        if (used_count_fast_path) {
            std::vector<ProjectionExpr> projections;
            for (const auto& item : stmt.items) {
                if (!item.expr) {
                    continue;
                }
                std::string col_alias = item.alias;
                if (col_alias.empty()) {
                    if (auto* fn = dynamic_cast<const FunctionCallExpr*>(item.expr.get())) {
                        col_alias = fn->name;
                    } else {
                        col_alias = "?column?";
                    }
                }
                // Rewrite the aggregate expression to reference __agg_0.
                auto rewritten = rewrite_expr(*item.expr, agg_map);
                auto* ptr = rewritten.get();
                owned_exprs.push_back(std::move(rewritten));
                projections.push_back({ptr, col_alias});
            }
            auto output_schema = build_output_schema(bound.output_columns);
            child = std::make_unique<ProjectOperator>(std::move(child),
                                                      std::move(projections),
                                                      std::move(output_schema),
                                                      bound,
                                                      &subquery_ctx_);
        }
    } else {
        // -- 3 (no aggregation). Sort then Projection -------------------------

        // ORDER BY must happen before projection so that sort keys referencing
        // columns not in the SELECT list are still available.
        if (!stmt.order_by.empty()) {
            std::vector<SortKey> keys;
            keys.reserve(stmt.order_by.size());
            for (const auto& ob : stmt.order_by) {
                // Resolve SELECT-list aliases to the underlying expression.
                const Expr* effective_expr = ob.expr.get();
                if (auto* cref = dynamic_cast<const ColumnRefExpr*>(ob.expr.get());
                    cref && cref->table.empty()) {
                    std::string upper = to_upper(cref->column);
                    for (const auto& item : stmt.items) {
                        if (!item.alias.empty() && to_upper(item.alias) == upper && item.expr) {
                            effective_expr = item.expr.get();
                            break;
                        }
                    }
                }
                keys.push_back({effective_expr, ob.direction});
            }
            // GDB-803: Use ExternalSortOperator so large sorts spill to disk
            // rather than running out of memory. It degrades gracefully to
            // an in-memory sort when the input fits in work_mem.
            child =
                std::make_unique<ExternalSortOperator>(std::move(child), std::move(keys), bound);
        }

        std::vector<ProjectionExpr> projections;
        projections.reserve(stmt.items.size());

        for (const auto& item : stmt.items) {
            if (item.is_star || !item.table_star.empty()) {
                // Expand * or table.* using bound output columns (which the binder
                // already resolved for all tables including joins).
                // Only include actual table columns (table_id != 0), not computed
                // columns like window function results.
                for (const auto& rc : bound.output_columns) {
                    if (!item.table_star.empty() && rc.table_name != item.table_star) {
                        continue;
                    }
                    // Skip computed columns (window functions, expressions).
                    // Edge property and meta-columns also have table_id=0,
                    // ordinal=-1 but carry a non-empty table_name.
                    if (rc.table_id == 0 && rc.ordinal == -1 && rc.table_name.empty()) {
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
                    } else if (auto* wf =
                                   dynamic_cast<const WindowFunctionExpr*>(item.expr.get())) {
                        col_alias = wf->name;
                    } else {
                        col_alias = "?column?";
                    }
                }

                // If this is a window function, rewrite to reference its output column.
                auto wit = win_map.find(item.expr.get());
                if (wit != win_map.end()) {
                    auto cr = std::make_unique<ColumnRefExpr>();
                    cr->column = wit->second;
                    projections.push_back({cr.get(), col_alias});
                    owned_exprs.push_back(std::move(cr));
                } else {
                    projections.push_back({item.expr.get(), col_alias});
                }
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

        // Optimization: push OFFSET into SeqScan to skip rows at the iterator
        // level (slot directory check only, no tuple deserialization).
        // Safe when the scan has no WHERE predicate and there is at most a
        // pass-through Project between Limit and SeqScan.
        if (offset_val > 0) {
            auto* seq_scan = dynamic_cast<SeqScanOperator*>(child.get());
            if (!seq_scan) {
                // Check for Project â†’ SeqScan (the common SELECT * case).
                auto children = child->plan_children();
                if (children.size() == 1) {
                    // Safe: we own the full operator tree, so the const_cast
                    // just recovers the mutability we already have.
                    seq_scan = const_cast<SeqScanOperator*>(
                        dynamic_cast<const SeqScanOperator*>(children[0]));
                }
            }
            if (seq_scan != nullptr && !seq_scan->has_predicate()) {
                seq_scan->set_skip_rows(static_cast<size_t>(offset_val));
                offset_val = 0;
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

    // Collect auto-increment column info.
    std::vector<AutoIncrementCol> ai_cols;
    for (size_t j = 0; j < table_schema->columns.size(); ++j) {
        if (table_schema->columns[j].is_autoincrement) {
            // Determine if the column is in the explicit column list.
            bool in_list = false;
            if (!stmt.columns.empty()) {
                for (const auto& col_name : stmt.columns) {
                    if (table_schema->columns[j].name == col_name) {
                        in_list = true;
                        break;
                    }
                }
            }
            ai_cols.push_back(
                {j, table_schema->columns[j].type_id, table_schema->table_id, !in_list});
        }
    }

    // -------------------------------------------------------------------------
    // INSERT ... SELECT path
    // -------------------------------------------------------------------------
    if (stmt.select) {
        const auto* sel = dynamic_cast<const SelectStmt*>(stmt.select.get());
        if (sel == nullptr) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "INSERT...SELECT: select sub-statement is not a SelectStmt");
        }

        // Re-bind the SELECT sub-query to get a proper BoundStatement.
        // The binder already validated column counts; this re-bind is cheap
        // and gives the planner the expr_types map needed for plan_select().
        Binder sel_binder(catalog_, database_id_, algorithm_registry_);
        auto sel_bound = sel_binder.bind(*sel);
        if (!sel_bound) {
            return make_error(sel_bound.error().code, sel_bound.error().message);
        }

        // Validate column count against target (binder already did this for
        // the VALUES path; mirror it here for safety).
        size_t target_col_count =
            stmt.columns.empty() ? table_schema->columns.size() : stmt.columns.size();
        if (sel_bound->output_columns.size() != target_col_count) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "INSERT...SELECT column count mismatch: target expects " +
                                  std::to_string(target_col_count) +
                                  " column(s), SELECT produces " +
                                  std::to_string(sel_bound->output_columns.size()));
        }

        std::vector<ExprPtr> select_owned_exprs;
        auto child_result = plan_select(*sel, *sel_bound, select_owned_exprs);
        if (!child_result) {
            return make_error(child_result.error().code, child_result.error().message);
        }
        auto child_iter = std::move(*child_result);

        // If an explicit column list was given (INSERT INTO t(b, a) SELECT ...),
        // build a column map: storage_col_idx -> child_output_idx.
        // Applied in InsertOperator::do_next() via child_col_map_.
        std::vector<size_t> col_map;
        if (!stmt.columns.empty()) {
            size_t ncols = table_schema->columns.size();
            col_map.reserve(ncols);
            const size_t unmapped = std::numeric_limits<size_t>::max();
            for (size_t storage_idx = 0; storage_idx < ncols; ++storage_idx) {
                const auto& storage_col_name = table_schema->columns[storage_idx].name;
                bool found = false;
                for (size_t ci = 0; ci < stmt.columns.size(); ++ci) {
                    if (stmt.columns[ci] == storage_col_name) {
                        col_map.push_back(ci);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    col_map.push_back(unmapped);
                }
            }
        }

        auto iter = std::make_unique<InsertOperator>(
            *storage->heap, storage->storage_schema, std::move(child_iter));
        // Transfer ownership of planner-created expressions into the operator.
        iter->owned_default_exprs_ = std::move(select_owned_exprs);
        if (!col_map.empty()) {
            // Also populate nullability metadata so InsertOperator can reject
            // unmapped NOT NULL columns (GDB-1269).
            std::vector<bool> col_nullable;
            std::vector<std::string> col_names;
            col_nullable.reserve(table_schema->columns.size());
            col_names.reserve(table_schema->columns.size());
            for (const auto& col : table_schema->columns) {
                col_nullable.push_back(col.nullable);
                col_names.push_back(col.name);
            }
            iter->child_col_map_ = std::move(col_map);
            iter->col_nullable_ = std::move(col_nullable);
            iter->col_names_for_null_check_ = std::move(col_names);
        }
        if (!ai_cols.empty()) {
            iter->autoincrement_cols_ = std::move(ai_cols);
            iter->catalog_ = &catalog_;
        }

        if (embedding_pool_ != nullptr) {
            auto emb_cols = catalog_.list_embedding_columns(table_schema->table_id);
            if (!emb_cols.empty()) {
                iter->embedding_table_id_ = table_schema->table_id;
                iter->embedding_pool_ = embedding_pool_;
                iter->embedding_cols_ = std::move(emb_cols);
                for (const auto& col : table_schema->columns) {
                    iter->column_names_.push_back(col.name);
                }
            }
        }
        iter->bm25_targets_ = collect_bm25_targets(*table_schema);
        iter->btree_targets_ = collect_btree_targets(*table_schema);
        iter->hash_targets_ = collect_hash_targets(*table_schema);

        return ok(std::unique_ptr<Iterator>(std::move(iter)));
    }

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

        // Pre-parse default expressions for columns not in the INSERT list.
        // These are owned by the InsertOperator and live as long as it does.
        std::vector<ExprPtr> owned_defaults;
        std::vector<const Expr*> default_ptrs(ncols, nullptr);
        for (size_t j = 0; j < ncols; ++j) {
            // Skip columns that appear in the INSERT column list.
            bool in_list = false;
            for (const auto& col_name : stmt.columns) {
                if (table_schema->columns[j].name == col_name) {
                    in_list = true;
                    break;
                }
            }
            if (in_list)
                continue;

            const auto& def = table_schema->columns[j].default_expr;
            if (!def.empty()) {
                // Parse the default expression string back into an AST node.
                Lexer lexer(def);
                auto tokens = lexer.tokenize();
                if (!tokens) {
                    return make_error(StatusCode::INTERNAL_ERROR,
                                      "failed to parse default for column: " +
                                          table_schema->columns[j].name);
                }
                Parser parser(std::move(*tokens));
                auto expr = parser.parse_expression();
                if (!expr) {
                    return make_error(StatusCode::INTERNAL_ERROR,
                                      "failed to parse default for column: " +
                                          table_schema->columns[j].name);
                }
                default_ptrs[j] = expr->get();
                owned_defaults.push_back(std::move(*expr));
            } else if (table_schema->columns[j].is_autoincrement) {
                // Auto-increment column omitted â€” use NULL placeholder.
                // The InsertOperator will replace it with the next counter value.
                auto null_expr = std::make_unique<LiteralExpr>();
                null_expr->kind = LiteralKind::NULL_LITERAL;
                null_expr->value = "NULL";
                default_ptrs[j] = null_expr.get();
                owned_defaults.push_back(std::move(null_expr));
            } else if (table_schema->columns[j].nullable) {
                // No default, but nullable â€” use NULL.
                auto null_expr = std::make_unique<LiteralExpr>();
                null_expr->kind = LiteralKind::NULL_LITERAL;
                null_expr->value = "NULL";
                default_ptrs[j] = null_expr.get();
                owned_defaults.push_back(std::move(null_expr));
            }
            // If not nullable and no default, default_ptrs[j] stays nullptr
            // and we'll error below when the column isn't provided.
        }

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
            // Fill missing columns with defaults or NULL.
            for (size_t j = 0; j < ncols; ++j) {
                if (reordered[j] == nullptr) {
                    if (default_ptrs[j] != nullptr) {
                        reordered[j] = default_ptrs[j];
                    } else {
                        return make_error(StatusCode::CONSTRAINT_VIOLATION,
                                          "NOT NULL constraint violated: column '" +
                                              table_schema->columns[j].name + "' cannot be NULL");
                    }
                }
            }
            value_rows.push_back(std::move(reordered));
        }

        auto iter = std::make_unique<InsertOperator>(
            *storage->heap, storage->storage_schema, std::move(value_rows), bound);
        iter->owned_default_exprs_ = std::move(owned_defaults);
        if (!ai_cols.empty()) {
            iter->autoincrement_cols_ = std::move(ai_cols);
            iter->catalog_ = &catalog_;
        }

        // Wire embedding infrastructure if the table has EMBEDDING columns.
        if (embedding_pool_ != nullptr) {
            auto emb_cols = catalog_.list_embedding_columns(table_schema->table_id);
            if (!emb_cols.empty()) {
                iter->embedding_table_id_ = table_schema->table_id;
                iter->embedding_pool_ = embedding_pool_;
                iter->embedding_cols_ = std::move(emb_cols);
                for (const auto& col : table_schema->columns) {
                    iter->column_names_.push_back(col.name);
                }
            }
        }
        iter->bm25_targets_ = collect_bm25_targets(*table_schema);
        iter->btree_targets_ = collect_btree_targets(*table_schema);
        iter->hash_targets_ = collect_hash_targets(*table_schema);

        return ok(std::unique_ptr<Iterator>(std::move(iter)));
    }

    auto iter = std::make_unique<InsertOperator>(
        *storage->heap, storage->storage_schema, std::move(value_rows), bound);
    if (!ai_cols.empty()) {
        iter->autoincrement_cols_ = std::move(ai_cols);
        iter->catalog_ = &catalog_;
    }

    // Wire embedding infrastructure if the table has EMBEDDING columns.
    if (embedding_pool_ != nullptr) {
        auto emb_cols = catalog_.list_embedding_columns(table_schema->table_id);
        if (!emb_cols.empty()) {
            iter->embedding_table_id_ = table_schema->table_id;
            iter->embedding_pool_ = embedding_pool_;
            iter->embedding_cols_ = std::move(emb_cols);
            for (const auto& col : table_schema->columns) {
                iter->column_names_.push_back(col.name);
            }
        }
    }
    iter->bm25_targets_ = collect_bm25_targets(*table_schema);
    iter->btree_targets_ = collect_btree_targets(*table_schema);
    iter->hash_targets_ = collect_hash_targets(*table_schema);

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
    iter->bm25_targets_ = collect_bm25_targets(*table_schema);
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
    iter->bm25_targets_ = collect_bm25_targets(*table_schema);
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

    // Evaluate the start key. When this is a correlated subquery, the start key
    // (e.g. TRAVERSE edge FROM t(outer.id)) evaluates against the outer row.
    Tuple fallback_tuple;
    OutputSchema fallback_schema;
    const Tuple& empty_tuple = outer_tuple_ != nullptr ? *outer_tuple_ : fallback_tuple;
    const OutputSchema& empty_schema = outer_schema_ != nullptr ? *outer_schema_ : fallback_schema;
    auto key_val = evaluate_expr(*stmt.from_key, empty_tuple, empty_schema, bound);
    if (!key_val) {
        return make_error(key_val.error().code, key_val.error().message);
    }

    // Build output schema.
    // Determine PK type from the edge type definition.
    auto edge_def = catalog_.get_edge_type(database_id_, stmt.edge_type);
    if (!edge_def) {
        return make_error(edge_def.error().code, edge_def.error().message);
    }

    // Look up the FROM table schema for PK type info (referenced_tables[0]
    // is the FROM table, populated by the binder).
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

    // Coerce the start key to the FROM table's PK type.
    auto coerced_key = fit_to_storage(*key_val, pk_type);
    if (!coerced_key) {
        return make_error(coerced_key.error().code,
                          "TRAVERSE start key: " + coerced_key.error().message);
    }

    TraversalConfig config;
    config.database_id = database_id_;
    config.edge_type = stmt.edge_type;
    config.start_key = std::move(*coerced_key);
    config.direction = stmt.direction;
    config.max_depth = stmt.max_depth.value_or(100);
    config.fetch = stmt.fetch;
    config.collect_edges = true;
    config.trace = stmt.trace;
    // Heterogeneous edges (different source/target tables) must not seed the
    // BFS visited set with the start key (GDB-696, mirroring GDB-304).
    config.heterogeneous = edge_def->source_table_id != edge_def->target_table_id;

    // For heterogeneous edges, cap depth to 1 (target nodes live in a
    // different table and cannot have further edges of the same type).
    if (config.heterogeneous && config.max_depth > 1) {
        config.max_depth = 1;
    }

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"", "node", pk_type, false, 0});
    out_cols.push_back({"", "depth", TypeId::INT64, false, 0});
    if (stmt.fetch) {
        out_cols.push_back({"", "source", pk_type, true, 0});
    }
    if (stmt.trace) {
        out_cols.push_back({"", "__path", TypeId::PATH, false, 0});
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

    // Determine PK types for from and to tables.
    // referenced_tables[0] = FROM table, referenced_tables[1] = TO table.
    // For heterogeneous edges, these may have different key types.
    auto get_pk_type = [&](table_id_t tid) -> TypeId {
        auto ts = catalog_.get_table_by_id(tid);
        if (ts && !ts->pk_columns.empty()) {
            for (const auto& col : ts->columns) {
                if (col.name == ts->pk_columns) {
                    return col.type_id;
                }
            }
        }
        return TypeId::INT64;
    };

    TypeId from_pk_type = TypeId::INT64;
    TypeId to_pk_type = TypeId::INT64;
    if (bound.referenced_tables.size() >= 2) {
        from_pk_type = get_pk_type(bound.referenced_tables[0]);
        to_pk_type = get_pk_type(bound.referenced_tables[1]);
    } else if (!bound.referenced_tables.empty()) {
        from_pk_type = get_pk_type(bound.referenced_tables[0]);
        to_pk_type = from_pk_type;
    }

    // Coerce the from/to keys to their respective table PK types.
    auto coerced_from = fit_to_storage(*from_val, from_pk_type);
    if (!coerced_from) {
        return make_error(coerced_from.error().code,
                          "SHORTEST PATH from key: " + coerced_from.error().message);
    }
    auto coerced_to = fit_to_storage(*to_val, to_pk_type);
    if (!coerced_to) {
        return make_error(coerced_to.error().code,
                          "SHORTEST PATH to key: " + coerced_to.error().message);
    }

    // Resolve the edge type to determine source/target table IDs and whether
    // the edge is heterogeneous (GDB-842: needed to correctly key the BFS
    // visited/parent maps by (table_id, pk) rather than bare pk).
    auto edge_def = catalog_.get_edge_type(database_id_, stmt.edge_type);
    if (!edge_def) {
        return make_error(edge_def.error().code, edge_def.error().message);
    }
    const bool heterogeneous = edge_def->source_table_id != edge_def->target_table_id;

    // Reject DIRECTION BOTH on heterogeneous edges, mirroring the TRAVERSE
    // restriction: per-hop table identity is ambiguous when source and target
    // tables differ and the BFS can expand in both directions (GDB-842).
    if (heterogeneous && stmt.direction == TraverseDirection::BOTH) {
        return make_error(StatusCode::TYPE_ERROR,
                          "DIRECTION BOTH not supported for heterogeneous edge type '" +
                              stmt.edge_type + "'");
    }

    ShortestPathConfig sp_config;
    sp_config.database_id = database_id_;
    sp_config.edge_type = stmt.edge_type;
    sp_config.from_key = std::move(*coerced_from);
    sp_config.to_key = std::move(*coerced_to);
    sp_config.direction = stmt.direction;
    sp_config.max_depth = stmt.max_depth.value_or(100);
    sp_config.heterogeneous = heterogeneous;
    sp_config.source_table_id = edge_def->source_table_id;
    sp_config.target_table_id = edge_def->target_table_id;

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"", "node", from_pk_type, false, 0});
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
                                                      const BoundStatement& bound,
                                                      std::vector<ExprPtr>& owned_exprs) {
    if (!graph_engine_) {
        return make_error(StatusCode::INTERNAL_ERROR, "graph engine not available for MATCH");
    }

    // Build pattern config from AST pattern elements.
    MatchConfig match_config;
    bool has_variable_length = false;
    for (const auto& elem : stmt.pattern) {
        MatchNodeDef node_def;
        node_def.variable = elem.node.variable;
        node_def.label = elem.node.label;
        node_def.filter_expr = elem.node.filter_expr.get();
        match_config.nodes.push_back(std::move(node_def));

        if (elem.outgoing_edge) {
            MatchEdgeDef edge_def(elem.outgoing_edge->variable,
                                  elem.outgoing_edge->edge_type,
                                  elem.outgoing_edge->direction,
                                  elem.outgoing_edge->min_hops,
                                  elem.outgoing_edge->max_hops);
            edge_def.filter_expr = elem.outgoing_edge->filter_expr.get();
            if (edge_def.is_variable_length()) {
                has_variable_length = true;
            }
            match_config.edges.push_back(std::move(edge_def));
        }
    }

    // Build a full-scope schema that includes ALL columns from every node
    // table in the pattern.  This allows the WHERE clause to reference
    // columns that are not in the SELECT / RETURN list.
    auto scope_schema = build_match_scope_schema(match_config);

    // Create the MATCH operator with the full scope schema and NO WHERE
    // (WHERE is applied as a separate FilterOperator below so that it
    // evaluates against the full scope, not just the projected output).
    std::unique_ptr<Iterator> child;
    if (stmt.path_selector != PathSelector::NONE) {
        child = std::make_unique<MatchShortestPathOperator>(
            *graph_engine_,
            catalog_,
            storage_,
            database_id_,
            std::move(match_config),
            std::move(scope_schema),
            nullptr, // WHERE handled below
            bound,
            stmt.path_selector,
            stmt.path_variable,
            stmt.shortest_k,
            MatchShortestPathOperator::DEFAULT_MAX_VISITED,
            stmt.weight_expr.get());
    } else if (has_variable_length) {
        child = std::make_unique<VariableLengthMatchOperator>(*graph_engine_,
                                                              catalog_,
                                                              storage_,
                                                              database_id_,
                                                              std::move(match_config),
                                                              std::move(scope_schema),
                                                              nullptr, // WHERE handled below
                                                              bound);
    } else {
        child = std::make_unique<PatternMatchOperator>(*graph_engine_,
                                                       catalog_,
                                                       storage_,
                                                       database_id_,
                                                       std::move(match_config),
                                                       std::move(scope_schema),
                                                       nullptr, // WHERE handled below
                                                       bound);
    }

    // Correlated MATCH: prepend the fixed outer row so the WHERE / pattern
    // filters can resolve outer columns alongside the matched pattern columns.
    if (outer_tuple_ != nullptr && outer_schema_ != nullptr) {
        std::vector<OutputColumn> combined_cols;
        for (size_t i = 0; i < outer_schema_->column_count(); ++i) {
            combined_cols.push_back(outer_schema_->column(i));
        }
        const auto& match_schema = child->output_schema();
        for (size_t i = 0; i < match_schema.column_count(); ++i) {
            combined_cols.push_back(match_schema.column(i));
        }
        child = std::make_unique<PrependTupleOperator>(
            std::move(child), *outer_tuple_, OutputSchema(std::move(combined_cols)));
    }

    // Apply WHERE as a FilterOperator over the full-scope output.
    if (stmt.where_expr) {
        child =
            std::make_unique<FilterOperator>(std::move(child), *stmt.where_expr, bound, nullptr);
    }

    // Project down to the RETURN / SELECT columns requested by the user.
    std::vector<ProjectionExpr> projections;
    projections.reserve(bound.output_columns.size());
    for (const auto& rc : bound.output_columns) {
        auto cr = std::make_unique<ColumnRefExpr>();
        cr->table = rc.table_name;
        cr->column = rc.column_name;
        projections.push_back({cr.get(), rc.column_name});
        owned_exprs.push_back(std::move(cr));
    }
    auto output_schema = build_output_schema(bound.output_columns);
    child = std::make_unique<ProjectOperator>(
        std::move(child), std::move(projections), std::move(output_schema), bound);

    return ok(std::move(child));
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
        // GDB-717: the raw dot product is a similarity (higher = more
        // similar), but NEAREST orders candidates by distance ascending.
        // Use the negated INNER_PRODUCT variant so the ascending sort
        // yields the k MOST similar rows. NearestScanOperator negates the
        // sort key back when emitting the user-visible _distance column,
        // so USING DOT still reports the raw dot product.
        return DistanceMetric::INNER_PRODUCT;
    case NearestMetric::COSINE:
    default:
        return DistanceMetric::COSINE;
    }
}

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

/// True if the expression tree contains a BM25 MATCH(...) predicate anywhere.
bool expr_contains_match(const Expr* e) {
    if (e == nullptr) {
        return false;
    }
    if (dynamic_cast<const MatchExpr*>(e) != nullptr) {
        return true;
    }
    if (auto* b = dynamic_cast<const BinaryExpr*>(e)) {
        return expr_contains_match(b->lhs.get()) || expr_contains_match(b->rhs.get());
    }
    if (auto* u = dynamic_cast<const UnaryExpr*>(e)) {
        return expr_contains_match(u->operand.get());
    }
    return false;
}

/// Find a BM25 MATCH(...) predicate that sits in a "usable" position: either the
/// whole WHERE predicate, or a conjunct of a top-level AND chain. MATCH under
/// OR / NOT is not usable (the index scan cannot represent those semantics in
/// v1), so this returns nullptr for those cases â€” the caller distinguishes
/// "no match" from "unusable match" via expr_contains_match().
const MatchExpr* extract_match_predicate(const Expr* e) {
    if (e == nullptr) {
        return nullptr;
    }
    if (auto* m = dynamic_cast<const MatchExpr*>(e)) {
        return m;
    }
    if (auto* b = dynamic_cast<const BinaryExpr*>(e)) {
        if (b->op == BinaryOp::AND) {
            if (auto* l = extract_match_predicate(b->lhs.get())) {
                return l;
            }
            return extract_match_predicate(b->rhs.get());
        }
    }
    return nullptr;
}

/// True if the expression tree contains a NEAREST(...) predicate anywhere.
bool expr_contains_nearest(const Expr* e) {
    if (e == nullptr) {
        return false;
    }
    if (dynamic_cast<const NearestExpr*>(e) != nullptr) {
        return true;
    }
    if (auto* b = dynamic_cast<const BinaryExpr*>(e)) {
        return expr_contains_nearest(b->lhs.get()) || expr_contains_nearest(b->rhs.get());
    }
    if (auto* u = dynamic_cast<const UnaryExpr*>(e)) {
        return expr_contains_nearest(u->operand.get());
    }
    return false;
}

/// Find a NEAREST(...) predicate in a usable position: the whole WHERE
/// predicate, or a conjunct of a top-level AND chain (mirrors
/// extract_match_predicate).
const NearestExpr* extract_nearest_predicate(const Expr* e) {
    if (e == nullptr) {
        return nullptr;
    }
    if (auto* n = dynamic_cast<const NearestExpr*>(e)) {
        return n;
    }
    if (auto* b = dynamic_cast<const BinaryExpr*>(e)) {
        if (b->op == BinaryOp::AND) {
            if (auto* l = extract_nearest_predicate(b->lhs.get())) {
                return l;
            }
            return extract_nearest_predicate(b->rhs.get());
        }
    }
    return nullptr;
}

/// Count how many distinct NEAREST(...) predicates appear in a conjunct list.
/// Used to enforce the v1 "exactly one NEAREST per query" restriction so that
/// the synthetic `_distance` column stays unambiguous (GDB-1250).
size_t count_nearest_conjuncts(const std::vector<const Expr*>& conjuncts) {
    size_t count = 0;
    for (const Expr* c : conjuncts) {
        if (expr_contains_nearest(c)) {
            ++count;
        }
    }
    return count;
}

/// Synthesize an owned right-associated AND-chain from a list of borrowed
/// conjuncts: {a, b, c} -> (a AND (b AND c)). Every node (the AND nodes and the
/// deep-cloned leaves) is appended to `owned`, which retains ownership; the
/// returned pointer is borrowed from it. Returns nullptr on an empty list or if
/// any leaf cannot be cloned (e.g. it holds a subquery). For a single conjunct
/// the cloned leaf is returned directly with no enclosing AND. Cloning the
/// leaves keeps the synthesized residual independent of the source AST's
/// structure so it can be passed as one `const Expr*` (GDB-1250).
const Expr* build_owned_and_chain(const std::vector<const Expr*>& conjuncts,
                                  std::vector<ExprPtr>& owned) {
    if (conjuncts.empty()) {
        return nullptr;
    }

    // Deep-clone every leaf once into local owned storage.
    std::vector<ExprPtr> leaves;
    leaves.reserve(conjuncts.size());
    for (const Expr* c : conjuncts) {
        if (c == nullptr) {
            return nullptr;
        }
        auto cloned = clone_expr_public(*c);
        if (!cloned) {
            return nullptr;
        }
        leaves.push_back(std::move(cloned));
    }

    // Assemble right-to-left so evaluation order matches the original WHERE:
    // {a, b, c} -> a AND (b AND c). `chain` owns the accumulated subtree until
    // it is finally transferred into `owned`.
    ExprPtr chain = std::move(leaves.back());
    for (int i = static_cast<int>(leaves.size()) - 2; i >= 0; --i) {
        auto and_node = std::make_unique<BinaryExpr>();
        and_node->op = BinaryOp::AND;
        and_node->lhs = std::move(leaves[static_cast<size_t>(i)]);
        and_node->rhs = std::move(chain);
        chain = std::move(and_node);
    }
    const Expr* result = chain.get();
    owned.push_back(std::move(chain));
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

Result<std::unique_ptr<Iterator>> Planner::plan_nearest_impl(const std::string& table_name,
                                                             const std::string& column_name,
                                                             const Expr* k_expr,
                                                             const Expr* target_expr,
                                                             NearestMetric metric,
                                                             const Stmt* within_traverse,
                                                             const Expr* where_expr,
                                                             const BoundStatement& bound,
                                                             const std::string& table_alias) {
    // Resolve the table.
    auto table_schema = catalog_.get_table(database_id_, table_name);
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
        if (to_upper(table_schema->columns[i].name) == to_upper(column_name)) {
            emb_col_idx = static_cast<int32_t>(i);
            break;
        }
    }
    if (emb_col_idx < 0) {
        return make_error(StatusCode::NOT_FOUND,
                          "column " + column_name + " not found in table " + table_name);
    }

    // Configuration expressions (k, target vector, WITHIN start key) evaluate
    // against the outer row when this is a correlated subquery, else an empty
    // tuple. This lets `NEAREST k FROM t.vec TO outer.embedding` work.
    Tuple fallback_tuple;
    OutputSchema fallback_schema;
    const Tuple& empty_tuple = outer_tuple_ != nullptr ? *outer_tuple_ : fallback_tuple;
    const OutputSchema& empty_schema = outer_schema_ != nullptr ? *outer_schema_ : fallback_schema;

    // Evaluate k.
    auto k_val = evaluate_expr(*k_expr, empty_tuple, empty_schema, bound);
    if (!k_val) {
        return make_error(k_val.error().code, k_val.error().message);
    }
    auto k = static_cast<uint32_t>(k_val->as_int64());

    // Evaluate the target expression to get the query vector.
    auto target_val = evaluate_expr(*target_expr, empty_tuple, empty_schema, bound);
    if (!target_val) {
        return make_error(target_val.error().code, target_val.error().message);
    }

    std::vector<float> query_vector;

    if (!target_val->is_null() && target_val->type_id() == TypeId::EMBEDDING) {
        // Target is a literal vector.
        query_vector = target_val->as_embedding();
    } else if (!target_val->is_null() && target_val->type_id() == TypeId::STRING) {
        // Target is a text string â€” auto-embed via the column's provider.
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
                              "no embedding provider configured for column " + column_name);
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
    config.metric = to_distance_metric(metric);
    config.embedding_column_index = emb_col_idx;

    // Handle WITHIN TRAVERSE (graph-scoped search).
    if (within_traverse != nullptr) {
        // Mark the scan as graph-scoped up front. Even if the traversal below
        // resolves to zero reachable rows, the operator must emit an empty
        // result rather than fall back to a global search (GDB-1257).
        config.graph_scoped = true;

        if (!graph_engine_) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "graph engine not available for WITHIN TRAVERSE");
        }

        auto* trav_stmt = dynamic_cast<const TraverseStmt*>(within_traverse);
        if (!trav_stmt) {
            return make_error(StatusCode::INTERNAL_ERROR, "expected TraverseStmt in WITHIN clause");
        }

        // GDB-1251: Validate that the traversal's REACHED node table is the
        // table that owns the EMBEDDING column. Otherwise the reachable PKs are
        // intersected with the vector table's PKs by raw numeric equality,
        // producing a silent garbage scope built from cross-table PK collisions.
        auto edge_def = catalog_.get_edge_type(database_id_, trav_stmt->edge_type);
        if (!edge_def) {
            return make_error(edge_def.error().code, edge_def.error().message);
        }

        // Resolve the reached side(s) per direction.
        //   OUT  â†’ reached = target_table_id
        //   IN   â†’ reached = source_table_id
        //   BOTH â†’ both endpoints must equal the vector table (a heterogeneous
        //          BOTH would mix two PK id-spaces into a single scope set).
        const table_id_t vector_table_id = table_schema->table_id;
        std::vector<table_id_t> reached_ids;
        if (trav_stmt->direction == TraverseDirection::OUT) {
            reached_ids.push_back(edge_def->target_table_id);
        } else if (trav_stmt->direction == TraverseDirection::IN) {
            reached_ids.push_back(edge_def->source_table_id);
        } else { // BOTH
            reached_ids.push_back(edge_def->source_table_id);
            reached_ids.push_back(edge_def->target_table_id);
        }

        auto direction_name = [](TraverseDirection d) -> const char* {
            switch (d) {
            case TraverseDirection::OUT:
                return "OUT";
            case TraverseDirection::IN:
                return "IN";
            case TraverseDirection::BOTH:
            default:
                return "BOTH";
            }
        };

        for (table_id_t reached_id : reached_ids) {
            if (reached_id != vector_table_id) {
                std::string reached_name = std::to_string(reached_id);
                if (auto reached_schema = catalog_.get_table_by_id(reached_id)) {
                    reached_name = reached_schema->name;
                }
                return make_error(
                    StatusCode::INVALID_ARGUMENT,
                    "WITHIN TRAVERSE '" + trav_stmt->edge_type + "' DIRECTION " +
                        direction_name(trav_stmt->direction) + " reaches table '" + reached_name +
                        "', but NEAREST column '" + table_name + "." + column_name +
                        "' belongs to table '" + table_name +
                        "'; the traversal must end at the table that owns the embedding column");
            }
        }

        // Resolve the START table (the side the start key is keyed against).
        // The start key must be coerced against the START table's PK type, not
        // the vector table's (GDB-1251: these differ for heterogeneous edges).
        auto start_schema = catalog_.get_table(database_id_, trav_stmt->from_table);
        if (!start_schema) {
            return make_error(start_schema.error().code, start_schema.error().message);
        }
        const bool start_is_vector_table = start_schema->table_id == vector_table_id;

        // Evaluate the start key.
        auto start_key = evaluate_expr(*trav_stmt->from_key, empty_tuple, empty_schema, bound);
        if (!start_key) {
            return make_error(start_key.error().code, start_key.error().message);
        }

        // Determine the PK type for the START table.
        TypeId start_pk_type = TypeId::INT64;
        if (!start_schema->pk_columns.empty()) {
            for (const auto& col : start_schema->columns) {
                if (col.name == start_schema->pk_columns) {
                    start_pk_type = col.type_id;
                    break;
                }
            }
        }

        // Determine the PK type for the vector table (used to type the traversal
        // node-id column, which holds reached-node PKs == vector-table PKs).
        TypeId pk_type = TypeId::INT64;
        if (!table_schema->pk_columns.empty()) {
            for (const auto& col : table_schema->columns) {
                if (col.name == table_schema->pk_columns) {
                    pk_type = col.type_id;
                    break;
                }
            }
        }

        // Coerce the start key to match the START table's PK type (e.g. STRING â†’
        // UUID, INT64 literal â†’ INT32 column).
        if (start_key->type_id() != start_pk_type) {
            auto coerced = fit_to_storage(*start_key, start_pk_type);
            if (!coerced) {
                return make_error(coerced.error().code, coerced.error().message);
            }
            *start_key = std::move(*coerced);
        }

        // Save start PK before moving the value into the config.
        Value start_pk = *start_key;

        TraversalConfig trav_config;
        trav_config.edge_type = trav_stmt->edge_type;
        trav_config.start_key = std::move(*start_key);
        trav_config.direction = trav_stmt->direction;
        trav_config.max_depth = trav_stmt->max_depth.value_or(100);
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
        std::unordered_set<Value, ValueHash, ValueEqual> reachable_pks;
        while (true) {
            auto row = trav_op.next();
            if (!row) {
                return make_error(row.error().code, row.error().message);
            }
            if (!row->has_value()) {
                break;
            }
            reachable_pks.insert(row->value().values[0]);
        }
        trav_op.close();

        // Also include the start node itself â€” but only when the start table IS
        // the vector table. For heterogeneous edges (start table â‰  vector table)
        // the start PK lives in a different id-space; seeding it here would admit
        // an unrelated vector-table row by accidental PK collision (GDB-1251).
        if (start_is_vector_table) {
            reachable_pks.insert(start_pk);
        }

        // Scan the table to convert reachable PKs to heap RIDs. The RID is
        // the canonical id space for graph-scoped NEAREST (GDB-745): heap
        // ordinals and HNSW node ids diverge whenever a row has a NULL
        // embedding or rows were deleted/inserted after the index build, so
        // the operator filters by RID on every execution path.
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

        while (true) {
            auto row_result = scan_it->next();
            if (!row_result) {
                return make_error(row_result.error().code, row_result.error().message);
            }
            if (!row_result->has_value()) {
                break;
            }
            auto& [rid, data] = **row_result;
            auto values = TupleSerializer::deserialize(data, storage->storage_schema);
            if (!values) {
                return make_error(values.error().code, values.error().message);
            }
            if (pk_col_idx >= 0 &&
                reachable_pks.count((*values)[static_cast<size_t>(pk_col_idx)]) > 0) {
                config.allowed_rids.insert(rid);
            }
        }
    }

    // --- Btree-accelerated pre-filtering ---
    // When the WHERE clause matches a btree index and the filter is selective
    // (â‰¤ 10,000 candidates), collect RIDs from the btree and compute distances
    // only for those rows via brute-force. This avoids searching the entire
    // HNSW index for highly selective predicates.
    static constexpr size_t kPrefilterThreshold = 10'000;

    if (where_expr != nullptr && btree_indexes_ != nullptr && !btree_indexes_->empty() &&
        config.allowed_rids.empty()) {
        auto cmp = extract_simple_comparison(where_expr);
        if (cmp) {
            auto indexes = catalog_.list_indexes(table_schema->table_id);
            for (const auto& idx_def : indexes) {
                if (idx_def.index_type != "btree") {
                    continue;
                }
                auto it = btree_indexes_->find(idx_def.index_id);
                if (it == btree_indexes_->end()) {
                    continue;
                }
                auto idx_columns = parse_index_columns(idx_def.columns);
                if (idx_columns.empty() || idx_columns[0] != cmp->column_name) {
                    continue;
                }

                auto* btree = it->second;

                // Coerce the literal to the column's actual type.
                Value coerced_value = cmp->literal_value;
                for (const auto& col : table_schema->columns) {
                    if (col.name == cmp->column_name && coerced_value.type_id() != col.type_id &&
                        can_coerce(coerced_value.type_id(), col.type_id)) {
                        auto cv = coerce(coerced_value, col.type_id);
                        if (cv) {
                            coerced_value = std::move(*cv);
                        }
                        break;
                    }
                }
                KeyType key_value = {coerced_value};

                // Determine scan bounds.
                std::optional<KeyType> begin_key;
                std::optional<KeyType> end_key;
                switch (cmp->op) {
                case BinaryOp::EQUAL:
                case BinaryOp::GREATER:
                case BinaryOp::GREATER_EQUAL:
                    begin_key = key_value;
                    break;
                case BinaryOp::LESS:
                case BinaryOp::LESS_EQUAL:
                    end_key = key_value;
                    break;
                default:
                    break;
                }

                // Perform the btree range scan to collect candidate RIDs.
                auto scan_result = btree->range_scan(begin_key, end_key);
                if (!scan_result) {
                    break; // Fall through to HNSW path.
                }
                auto& btree_iter = *scan_result;

                std::vector<RID> candidate_rids;
                bool exceeded_threshold = false;
                while (true) {
                    auto entry = btree_iter.next();
                    if (!entry || !entry->has_value()) {
                        break;
                    }
                    auto& [entry_key, rid] = **entry;

                    // For equality scans, stop once key moves past target.
                    if (cmp->op == BinaryOp::EQUAL && entry_key.size() == key_value.size()) {
                        bool keys_match = true;
                        for (size_t i = 0; i < entry_key.size(); ++i) {
                            auto kcmp = compare(entry_key[i], key_value[i]);
                            if (!kcmp || *kcmp != std::strong_ordering::equal) {
                                keys_match = false;
                                break;
                            }
                        }
                        if (!keys_match) {
                            break;
                        }
                    }

                    candidate_rids.push_back(rid);
                    if (candidate_rids.size() > kPrefilterThreshold) {
                        exceeded_threshold = true;
                        break;
                    }
                }

                if (!exceeded_threshold && !candidate_rids.empty()) {
                    SIXSEVEN_LOG_DEBUG("plan_nearest: btree prefilter on '{}' found {} candidates",
                                       cmp->column_name,
                                       candidate_rids.size());
                    config.prefiltered_rids = std::move(candidate_rids);
                } else if (exceeded_threshold) {
                    SIXSEVEN_LOG_DEBUG(
                        "plan_nearest: btree prefilter on '{}' exceeded {} threshold, "
                        "falling back to HNSW",
                        cmp->column_name,
                        kPrefilterThreshold);
                }
                break; // Use first matching index.
            }
        }
    }

    // Look up the companion HNSW index by index_id (globally unique).
    HnswIndex* hnsw_index = nullptr;
    std::vector<RID>* rid_map = nullptr;
    if (hnsw_indexes_ != nullptr) {
        // Find the HNSW IndexDef for this table's embedding column.
        auto indexes = catalog_.list_indexes(table_schema->table_id);
        index_id_t hnsw_idx_id = 0;
        for (const auto& idx : indexes) {
            if (idx.index_type == "hnsw" && idx.columns == column_name) {
                hnsw_idx_id = idx.index_id;
                break;
            }
        }
        if (hnsw_idx_id > 0) {
            auto it = hnsw_indexes_->find(hnsw_idx_id);
            if (it != hnsw_indexes_->end()) {
                hnsw_index = it->second;
            }
            if (hnsw_rid_maps_ != nullptr) {
                auto rm = hnsw_rid_maps_->find(hnsw_idx_id);
                if (rm != hnsw_rid_maps_->end()) {
                    rid_map = &rm->second;
                }
            }
        }
        SIXSEVEN_LOG_DEBUG("plan_nearest: table_id={} col={} idx_id={} hnsw={} rid_map={}",
                           table_schema->table_id,
                           column_name,
                           hnsw_idx_id,
                           hnsw_index != nullptr ? "found" : "miss",
                           rid_map != nullptr ? "found" : "miss");
    }

    // Build output schema: all table columns + _distance. When the scan feeds a
    // JOIN the columns must carry the query alias (e.g. `b`) so that qualified
    // references like `b.id` resolve unambiguously against the join's combined
    // schema; an empty alias falls back to the raw table name (GDB-1250).
    const std::string& output_qualifier = table_alias.empty() ? table_name : table_alias;
    auto table_output = build_table_output_schema(*table_schema, table_alias);
    std::vector<OutputColumn> out_cols = table_output.columns();
    out_cols.push_back(
        {output_qualifier, "_distance", TypeId::FLOAT64, false, table_schema->table_id});
    auto schema = OutputSchema(std::move(out_cols));

    auto iter = std::make_unique<NearestScanOperator>(*storage->heap,
                                                      storage->storage_schema,
                                                      std::move(config),
                                                      std::move(schema),
                                                      where_expr,
                                                      bound,
                                                      hnsw_index,
                                                      rid_map);

    return ok(std::unique_ptr<Iterator>(std::move(iter)));
}

// ---------------------------------------------------------------------------
// BM25 full-text scan planning
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Iterator>> Planner::try_plan_bm25_scan(const TableSchema& table_schema,
                                                              TableStorage* storage,
                                                              const OutputSchema& score_output,
                                                              const Expr* where_expr,
                                                              const BoundStatement& bound) {
    if (!expr_contains_match(where_expr)) {
        return ok(std::unique_ptr<Iterator>(nullptr));
    }

    // The MATCH must be a top-level predicate or AND conjunct.
    const MatchExpr* match = extract_match_predicate(where_expr);
    if (match == nullptr) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "MATCH(...) is only supported as a top-level AND predicate");
    }

    // Resolve the searched column name.
    auto* col_ref = dynamic_cast<const ColumnRefExpr*>(match->column.get());
    if (col_ref == nullptr) {
        return make_error(StatusCode::INVALID_ARGUMENT, "MATCH(...) requires a column reference");
    }
    const std::string& column_name = col_ref->column;

    // The query must be a string literal (v1 limitation).
    auto* query_lit = dynamic_cast<const LiteralExpr*>(match->query.get());
    if (query_lit == nullptr || query_lit->kind != LiteralKind::STRING) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "MATCH(...) TO requires a string literal query");
    }

    // Find the BM25 index for this column.
    if (bm25_indexes_ == nullptr) {
        return make_error(StatusCode::NOT_FOUND,
                          "no BM25 index available; CREATE INDEX ... USING bm25 on '" +
                              column_name + "'");
    }
    auto indexes = catalog_.list_indexes(table_schema.table_id);
    index_id_t bm25_idx_id = 0;
    for (const auto& idx : indexes) {
        if (idx.index_type == "bm25" && idx.columns == column_name) {
            bm25_idx_id = idx.index_id;
            break;
        }
    }
    if (bm25_idx_id == 0) {
        return make_error(StatusCode::NOT_FOUND,
                          "no BM25 index on column '" + column_name +
                              "'; CREATE INDEX ... USING bm25 first");
    }
    auto it = bm25_indexes_->find(bm25_idx_id);
    if (it == bm25_indexes_->end() || it->second == nullptr) {
        return make_error(StatusCode::NOT_FOUND,
                          "BM25 index for '" + column_name + "' is not loaded");
    }

    Bm25ScanConfig config;
    config.query = query_lit->value;
    config.k = 0; // all matches; ORDER BY / LIMIT downstream handle truncation.

    // The full WHERE is passed as the residual filter: the MATCH conjunct
    // evaluates to TRUE (every emitted row already matched), so any sibling
    // AND conjuncts (e.g. id > 5) are still applied correctly.
    auto iter = std::make_unique<Bm25ScanOperator>(*storage->heap,
                                                   storage->storage_schema,
                                                   std::move(config),
                                                   score_output,
                                                   where_expr,
                                                   bound,
                                                   it->second);
    return ok(std::unique_ptr<Iterator>(std::move(iter)));
}

// ---------------------------------------------------------------------------
// Vector (NEAREST) scan planning
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Iterator>> Planner::try_plan_vector_scan(const TableSchema& table_schema,
                                                                const Expr* where_expr,
                                                                const BoundStatement& bound,
                                                                const std::string& table_alias) {
    if (!expr_contains_nearest(where_expr)) {
        return ok(std::unique_ptr<Iterator>(nullptr));
    }

    // The NEAREST must be a top-level predicate or AND conjunct.
    const NearestExpr* nearest = extract_nearest_predicate(where_expr);
    if (nearest == nullptr) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "NEAREST(...) is only supported as a top-level AND predicate");
    }

    auto* col_ref = dynamic_cast<const ColumnRefExpr*>(nearest->column.get());
    if (col_ref == nullptr) {
        return make_error(StatusCode::INVALID_ARGUMENT, "NEAREST(...) requires a column reference");
    }

    // Desugar into the existing NEAREST execution. The full WHERE is passed as
    // the residual filter: the NEAREST conjunct evaluates TRUE (every emitted
    // row already matched), so any sibling AND conjuncts still apply. The
    // resulting operator's output schema is table columns + _distance.
    return plan_nearest_impl(table_schema.name,
                             col_ref->column,
                             nearest->k.get(),
                             nearest->target.get(),
                             nearest->metric,
                             nearest->within_traverse.get(),
                             where_expr,
                             bound,
                             table_alias);
}

std::vector<Bm25MaintenanceTarget> Planner::collect_bm25_targets(const TableSchema& schema) const {
    std::vector<Bm25MaintenanceTarget> targets;
    if (bm25_indexes_ == nullptr) {
        return targets;
    }
    auto indexes = catalog_.list_indexes(schema.table_id);
    for (const auto& idx : indexes) {
        if (idx.index_type != "bm25") {
            continue;
        }
        auto it = bm25_indexes_->find(idx.index_id);
        if (it == bm25_indexes_->end() || it->second == nullptr) {
            continue;
        }
        for (const auto& col : schema.columns) {
            if (col.name == idx.columns) {
                targets.push_back({it->second, static_cast<size_t>(col.ordinal)});
                break;
            }
        }
    }
    return targets;
}

std::vector<BtreeMaintenanceTarget>
Planner::collect_btree_targets(const TableSchema& schema) const {
    std::vector<BtreeMaintenanceTarget> targets;
    if (btree_indexes_ == nullptr) {
        return targets;
    }
    auto indexes = catalog_.list_indexes(schema.table_id);
    for (const auto& idx : indexes) {
        if (idx.index_type != "btree") {
            continue;
        }
        auto it = btree_indexes_->find(idx.index_id);
        if (it == btree_indexes_->end() || it->second == nullptr) {
            continue;
        }

        // Resolve each comma-separated column name to its storage-schema ordinal.
        BtreeMaintenanceTarget target;
        target.index = it->second;

        // Split idx.columns on commas.
        std::string col_str = idx.columns;
        size_t start = 0;
        while (start <= col_str.size()) {
            size_t end = col_str.find(',', start);
            if (end == std::string::npos) {
                end = col_str.size();
            }
            // Trim whitespace.
            size_t s = start;
            while (s < end && col_str[s] == ' ') {
                ++s;
            }
            size_t e = end;
            while (e > s && col_str[e - 1] == ' ') {
                --e;
            }
            std::string col_name = col_str.substr(s, e - s);
            if (!col_name.empty()) {
                for (const auto& col : schema.columns) {
                    if (col.name == col_name) {
                        target.key_column_ordinals.push_back(static_cast<size_t>(col.ordinal));
                        break;
                    }
                }
            }
            start = end + 1;
        }

        if (!target.key_column_ordinals.empty()) {
            targets.push_back(std::move(target));
        }
    }
    return targets;
}

std::vector<HashMaintenanceTarget> Planner::collect_hash_targets(const TableSchema& schema) const {
    std::vector<HashMaintenanceTarget> targets;
    if (hash_indexes_ == nullptr) {
        return targets;
    }
    auto indexes = catalog_.list_indexes(schema.table_id);
    for (const auto& idx : indexes) {
        if (idx.index_type != "hash") {
            continue;
        }
        auto it = hash_indexes_->find(idx.index_id);
        if (it == hash_indexes_->end() || it->second == nullptr) {
            continue;
        }

        HashMaintenanceTarget target;
        target.index = it->second;

        std::string col_str = idx.columns;
        size_t start = 0;
        while (start <= col_str.size()) {
            size_t end = col_str.find(',', start);
            if (end == std::string::npos) {
                end = col_str.size();
            }
            size_t s = start;
            while (s < end && col_str[s] == ' ') {
                ++s;
            }
            size_t e = end;
            while (e > s && col_str[e - 1] == ' ') {
                --e;
            }
            std::string col_name = col_str.substr(s, e - s);
            if (!col_name.empty()) {
                for (const auto& col : schema.columns) {
                    if (col.name == col_name) {
                        target.key_column_ordinals.push_back(static_cast<size_t>(col.ordinal));
                        break;
                    }
                }
            }
            start = end + 1;
        }

        if (!target.key_column_ordinals.empty()) {
            targets.push_back(std::move(target));
        }
    }
    return targets;
}

// ---------------------------------------------------------------------------
// Index scan planning
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Iterator>> Planner::try_plan_index_scan(const TableSchema& table_schema,
                                                               TableStorage* storage,
                                                               const OutputSchema& table_output,
                                                               const Expr* where_expr,
                                                               const BoundStatement& bound) {
    if (where_expr == nullptr) {
        return ok(std::unique_ptr<Iterator>(nullptr));
    }

    auto indexes = catalog_.list_indexes(table_schema.table_id);

    // -- GDB-803: BitmapScan for OR-of-predicates on different indexed columns --
    // When the WHERE is a top-level OR of two simple comparisons, each on a
    // different B+ tree-indexed column, build a BitmapScanOperator (OR mode)
    // that collects RIDs from both indexes and fetches heap pages in order.
    if (btree_indexes_ != nullptr && !btree_indexes_->empty()) {
        auto* or_bin = dynamic_cast<const BinaryExpr*>(where_expr);
        if (or_bin && or_bin->op == BinaryOp::OR) {
            auto cmp_l = extract_simple_comparison(or_bin->lhs.get());
            auto cmp_r = extract_simple_comparison(or_bin->rhs.get());
            if (cmp_l && cmp_r && cmp_l->column_name != cmp_r->column_name) {
                // Try to resolve a B+ tree index for each side.
                auto find_btree = [&](const SimpleComparison& sc)
                    -> std::pair<const BTreeIndex*, std::optional<KeyType>> {
                    for (const auto& idx_def : indexes) {
                        if (idx_def.index_type != "btree")
                            continue;
                        auto it = btree_indexes_->find(idx_def.index_id);
                        if (it == btree_indexes_->end())
                            continue;
                        auto idx_cols = parse_index_columns(idx_def.columns);
                        if (idx_cols.empty() || idx_cols[0] != sc.column_name)
                            continue;
                        Value v = sc.literal_value;
                        for (const auto& col : table_schema.columns) {
                            if (col.name == sc.column_name && v.type_id() != col.type_id &&
                                can_coerce(v.type_id(), col.type_id)) {
                                auto cv = coerce(v, col.type_id);
                                if (cv)
                                    v = std::move(*cv);
                                break;
                            }
                        }
                        KeyType key = {v};
                        return {it->second, std::move(key)};
                    }
                    return {nullptr, std::nullopt};
                };

                auto [btree_l, key_l] = find_btree(*cmp_l);
                auto [btree_r, key_r] = find_btree(*cmp_r);

                if (btree_l != nullptr && btree_r != nullptr) {
                    // Build BitmapScan with OR combine mode.
                    std::vector<BitmapIndexScan> scans;
                    BitmapIndexScan s_l;
                    s_l.index = btree_l;
                    // Assign begin_key/end_key for the scan direction implied by
                    // the comparison operator.
                    //
                    // The BTreeIterator ends when current_key >= end_key (exclusive).
                    // For LESS (strict): end_key = boundary is correct.
                    // For LESS_EQUAL (inclusive): passing end_key = boundary would
                    //   exclude the boundary row; use end_key = nullopt (scan to +inf)
                    //   and rely on the OR-expression residual applied by
                    //   BitmapScanOperator to filter correctly.
                    // For EQUAL / GREATER / GREATER_EQUAL: begin_key = boundary.
                    auto assign_scan_range =
                        [](BitmapIndexScan& s, BinaryOp op, std::optional<KeyType> key) {
                            if (op == BinaryOp::LESS) {
                                // Strict upper bound: stop before boundary.
                                s.begin_key = std::nullopt;
                                s.end_key = std::move(key);
                            } else if (op == BinaryOp::LESS_EQUAL) {
                                // Inclusive upper bound: BTreeIterator end_key is
                                // exclusive so the boundary row would be missed.
                                // Scan the full index; the WHERE residual handles
                                // boundary inclusion correctly.
                                s.begin_key = std::nullopt;
                                s.end_key = std::nullopt;
                            } else {
                                // EQUAL / GREATER / GREATER_EQUAL: scan [key, +inf).
                                s.begin_key = std::move(key);
                                s.end_key = std::nullopt;
                            }
                        };
                    assign_scan_range(s_l, cmp_l->op, std::move(key_l));
                    scans.push_back(std::move(s_l));

                    BitmapIndexScan s_r;
                    s_r.index = btree_r;
                    assign_scan_range(s_r, cmp_r->op, std::move(key_r));
                    scans.push_back(std::move(s_r));

                    auto bitmap_scan = std::make_unique<BitmapScanOperator>(std::move(scans),
                                                                            BitmapCombineMode::OR,
                                                                            *storage->heap,
                                                                            storage->storage_schema,
                                                                            table_output,
                                                                            where_expr,
                                                                            &bound);
                    return ok(std::unique_ptr<Iterator>(std::move(bitmap_scan)));
                }
            }
        }
    }

    // Extract simple comparison from the WHERE clause.
    auto cmp = extract_simple_comparison(where_expr);
    if (!cmp) {
        return ok(std::unique_ptr<Iterator>(nullptr));
    }

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

            // Coerce the literal to the column's actual type (e.g. STRING â†’ UUID).
            Value coerced_value = cmp->literal_value;
            for (const auto& col : table_schema.columns) {
                if (col.name == cmp->column_name && coerced_value.type_id() != col.type_id &&
                    can_coerce(coerced_value.type_id(), col.type_id)) {
                    auto cv = coerce(coerced_value, col.type_id);
                    if (cv) {
                        coerced_value = std::move(*cv);
                    }
                    break;
                }
            }
            KeyType lookup_key = {coerced_value};

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

        // Coerce the literal to the column's actual type (e.g. STRING â†’ UUID).
        Value coerced_value = cmp->literal_value;
        for (const auto& col : table_schema.columns) {
            if (col.name == cmp->column_name && coerced_value.type_id() != col.type_id &&
                can_coerce(coerced_value.type_id(), col.type_id)) {
                auto cv = coerce(coerced_value, col.type_id);
                if (cv) {
                    coerced_value = std::move(*cv);
                }
                break;
            }
        }
        KeyType key_value = {coerced_value};

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

        // For equality scans, pass the key so IndexScan can terminate early
        // once the btree cursor moves past the matching entries.
        std::optional<KeyType> eq_key;
        if (cmp->op == BinaryOp::EQUAL) {
            eq_key = key_value;
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
                                                        &bound,
                                                        std::move(eq_key));

        return ok(std::unique_ptr<Iterator>(std::move(scan)));
    }

    // No suitable index found.
    return ok(std::unique_ptr<Iterator>(nullptr));
}

// ---------------------------------------------------------------------------
// Cost-based access path selection (GDB-754)
// ---------------------------------------------------------------------------

Planner::AccessPathDecision Planner::decide_access_path(const TableSchema& table_schema,
                                                        const Expr* where_expr) const {
    AccessPathDecision decision; // Default: try index, no cost annotations.
    if (stats_ == nullptr) {
        return decision;
    }
    const TableStats* table_stats = stats_->get_table_stats(table_schema.table_id);
    if (table_stats == nullptr) {
        return decision;
    }

    const CostModel cost_model;

    // Estimate the filter selectivity from column statistics.
    double selectivity = kDefaultSelectivity;
    std::string predicate_column;
    auto cmp = extract_simple_comparison(where_expr);
    if (cmp.has_value()) {
        predicate_column = cmp->column_name;
        const ColumnStats* col_stats = nullptr;
        for (size_t ci = 0; ci < table_schema.columns.size(); ++ci) {
            if (table_schema.columns[ci].name == cmp->column_name) {
                col_stats =
                    stats_->get_column_stats(table_schema.table_id, static_cast<int32_t>(ci));
                break;
            }
        }
        if (col_stats != nullptr) {
            switch (cmp->op) {
            case BinaryOp::EQUAL:
                selectivity = estimate_equality_selectivity(*col_stats, cmp->literal_value);
                break;
            case BinaryOp::LESS:
            case BinaryOp::LESS_EQUAL:
            case BinaryOp::GREATER:
            case BinaryOp::GREATER_EQUAL:
                selectivity = estimate_range_selectivity(*col_stats, cmp->op, cmp->literal_value);
                break;
            default:
                selectivity = kDefaultSelectivity;
                break;
            }
        } else if (cmp->op == BinaryOp::EQUAL) {
            selectivity = kDefaultEqualitySelectivity;
        }
    }

    // Only indexes whose leading column matches the predicate column are
    // usable by try_plan_index_scan; restrict the optimizer to those.
    std::vector<IndexDef> usable_indexes;
    if (!predicate_column.empty()) {
        for (const auto& idx_def : catalog_.list_indexes(table_schema.table_id)) {
            auto idx_columns = parse_index_columns(idx_def.columns);
            if (idx_columns.empty() || idx_columns[0] != predicate_column) {
                continue;
            }
            const bool btree_loaded = idx_def.index_type == "btree" && btree_indexes_ != nullptr &&
                                      btree_indexes_->count(idx_def.index_id) > 0;
            const bool hash_loaded = idx_def.index_type == "hash" && cmp.has_value() &&
                                     cmp->op == BinaryOp::EQUAL && hash_indexes_ != nullptr &&
                                     hash_indexes_->count(idx_def.index_id) > 0;
            if (btree_loaded || hash_loaded) {
                usable_indexes.push_back(idx_def);
            }
        }
    }

    auto path = choose_access_path(
        table_schema.table_id, *table_stats, usable_indexes, selectivity, cost_model);

    decision.try_index = (path.method == AccessMethod::INDEX_SCAN);
    decision.cost = to_cost_estimate(path.cost);

    // Always compute the seq-scan fallback cost so the chosen plan is
    // annotated even when no index is usable or the index path falls through.
    auto seq = cost_model.seq_scan_cost(table_stats->page_count, table_stats->row_count);
    seq.estimated_rows = static_cast<double>(table_stats->row_count) * selectivity;
    decision.seq_cost = to_cost_estimate(seq);

    if (decision.try_index) {
        // The index scan returns only the matching fraction of rows.
        decision.cost->estimated_rows = static_cast<double>(table_stats->row_count) * selectivity;
    } else {
        decision.cost = decision.seq_cost;
    }

    SIXSEVEN_LOG_DEBUG("access path for table {} ({}): {} (selectivity={:.4f}, cost={:.2f})",
                       table_schema.name,
                       table_schema.table_id,
                       decision.try_index ? "index scan" : "seq scan",
                       selectivity,
                       decision.cost->total_cost);
    return decision;
}

void Planner::annotate_seq_scan_cost(const TableSchema& table_schema, Iterator& iter) const {
    if (stats_ == nullptr) {
        return;
    }
    const TableStats* table_stats = stats_->get_table_stats(table_schema.table_id);
    if (table_stats == nullptr) {
        return;
    }
    const CostModel cost_model;
    auto cost = cost_model.seq_scan_cost(table_stats->page_count, table_stats->row_count);
    iter.set_estimated_cost(to_cost_estimate(cost));
}

} // namespace sixseven
