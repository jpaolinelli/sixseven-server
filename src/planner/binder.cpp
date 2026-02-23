#include "giodb/planner/binder.h"

#include "giodb/common/coercion.h"
#include "giodb/planner/type_resolver.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace giodb {

namespace {

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

/// Check if two expressions are the "same" column reference (for GROUP BY validation).
bool same_column_ref(const Expr& a, const Expr& b) {
    auto* ca = dynamic_cast<const ColumnRefExpr*>(&a);
    auto* cb = dynamic_cast<const ColumnRefExpr*>(&b);
    if (!ca || !cb) {
        return false;
    }
    return ca->table == cb->table && ca->column == cb->column;
}

} // namespace

// ===========================================================================
// Scope
// ===========================================================================

void Scope::add_table(ScopeTable table) {
    tables_.push_back(std::move(table));
}

Result<ResolvedColumn> Scope::resolve_column(const std::string& column) const {
    std::string upper_col = to_upper(column);
    const ResolvedColumn* found = nullptr;
    int matches = 0;

    for (auto& tbl : tables_) {
        for (auto& col : tbl.columns) {
            if (to_upper(col.column_name) == upper_col) {
                found = &col;
                ++matches;
            }
        }
    }

    if (matches > 1) {
        return make_error(StatusCode::INVALID_ARGUMENT, "ambiguous column reference: " + column);
    }
    if (matches == 1) {
        return ok(*found);
    }

    // Walk to parent scope for correlated subqueries.
    if (parent_) {
        return parent_->resolve_column(column);
    }
    return make_error(StatusCode::NOT_FOUND, "column not found: " + column);
}

Result<ResolvedColumn> Scope::resolve_column(const std::string& table,
                                             const std::string& column) const {
    std::string upper_table = to_upper(table);
    std::string upper_col = to_upper(column);

    for (auto& tbl : tables_) {
        if (to_upper(tbl.alias) == upper_table) {
            for (auto& col : tbl.columns) {
                if (to_upper(col.column_name) == upper_col) {
                    return ok(col);
                }
            }
            return make_error(StatusCode::NOT_FOUND,
                              "column " + column + " not found in table " + table);
        }
    }

    // Walk to parent scope.
    if (parent_) {
        return parent_->resolve_column(table, column);
    }
    return make_error(StatusCode::NOT_FOUND, "table not found: " + table);
}

std::vector<ResolvedColumn> Scope::all_columns() const {
    std::vector<ResolvedColumn> result;
    for (auto& tbl : tables_) {
        result.insert(result.end(), tbl.columns.begin(), tbl.columns.end());
    }
    return result;
}

Result<std::vector<ResolvedColumn>> Scope::columns_for(const std::string& alias) const {
    std::string upper_alias = to_upper(alias);
    for (auto& tbl : tables_) {
        if (to_upper(tbl.alias) == upper_alias) {
            return ok(tbl.columns);
        }
    }
    return make_error(StatusCode::NOT_FOUND, "table not found: " + alias);
}

// ===========================================================================
// Binder — helpers
// ===========================================================================

ScopeTable Binder::make_scope_table(const TableSchema& schema, const std::string& alias) const {
    ScopeTable st;
    st.table_id = schema.table_id;
    st.alias = alias.empty() ? schema.name : alias;

    for (auto& col : schema.columns) {
        ResolvedColumn rc;
        rc.table_id = schema.table_id;
        rc.ordinal = col.ordinal;
        rc.table_name = st.alias;
        rc.column_name = col.name;
        rc.type_id = col.type_id;
        rc.nullable = col.nullable;
        st.columns.push_back(std::move(rc));
    }
    return st;
}

bool Binder::contains_aggregate(const Expr& expr, const BoundStatement& bound) const {
    auto it = bound.expr_types.find(&expr);
    if (it != bound.expr_types.end()) {
        return it->second.is_aggregate;
    }
    return false;
}

bool Binder::in_group_by(const Expr& expr, const std::vector<ExprPtr>& group_by) const {
    for (auto& gb : group_by) {
        if (gb && same_column_ref(expr, *gb)) {
            return true;
        }
    }
    return false;
}

// ===========================================================================
// Binder — top-level dispatch
// ===========================================================================

Result<BoundStatement> Binder::bind(const Stmt& stmt) {
    // DDL
    if (auto* s = dynamic_cast<const CreateTableStmt*>(&stmt)) {
        return bind_create_table(*s);
    }
    if (auto* s = dynamic_cast<const DropTableStmt*>(&stmt)) {
        return bind_drop_table(*s);
    }
    if (auto* s = dynamic_cast<const AlterTableStmt*>(&stmt)) {
        return bind_alter_table(*s);
    }
    if (auto* s = dynamic_cast<const CreateIndexStmt*>(&stmt)) {
        return bind_create_index(*s);
    }
    if (auto* s = dynamic_cast<const DropIndexStmt*>(&stmt)) {
        return bind_drop_index(*s);
    }
    if (auto* s = dynamic_cast<const CreateEdgeTypeStmt*>(&stmt)) {
        return bind_create_edge_type(*s);
    }
    if (auto* s = dynamic_cast<const DropEdgeTypeStmt*>(&stmt)) {
        return bind_drop_edge_type(*s);
    }

    // DML
    if (auto* s = dynamic_cast<const InsertStmt*>(&stmt)) {
        return bind_insert(*s);
    }
    if (auto* s = dynamic_cast<const UpdateStmt*>(&stmt)) {
        return bind_update(*s);
    }
    if (auto* s = dynamic_cast<const DeleteStmt*>(&stmt)) {
        return bind_delete(*s);
    }
    if (auto* s = dynamic_cast<const LinkStmt*>(&stmt)) {
        return bind_link(*s);
    }
    if (auto* s = dynamic_cast<const UnlinkStmt*>(&stmt)) {
        return bind_unlink(*s);
    }

    // Query
    if (auto* s = dynamic_cast<const SelectStmt*>(&stmt)) {
        return bind_select(*s);
    }
    if (auto* s = dynamic_cast<const TraverseStmt*>(&stmt)) {
        return bind_traverse(*s);
    }
    if (auto* s = dynamic_cast<const NearestStmt*>(&stmt)) {
        return bind_nearest(*s);
    }
    if (auto* s = dynamic_cast<const MatchStmt*>(&stmt)) {
        return bind_match(*s);
    }
    if (auto* s = dynamic_cast<const ShortestPathStmt*>(&stmt)) {
        return bind_shortest_path(*s);
    }

    // Admin
    if (auto* s = dynamic_cast<const ExplainStmt*>(&stmt)) {
        return bind_explain(*s);
    }
    if (auto* s = dynamic_cast<const DescribeStmt*>(&stmt)) {
        return bind_describe(*s);
    }
    if (auto* s = dynamic_cast<const ShowStmt*>(&stmt)) {
        return bind_show(*s);
    }

    // TCL and other admin pass-through (BEGIN, COMMIT, ROLLBACK, SAVEPOINT,
    // SET, REEMBED, VACUUM, ANALYZE).
    return bind_passthrough(stmt);
}

// ===========================================================================
// Expression binding
// ===========================================================================

Result<ExprType> Binder::bind_expr(const Expr& expr, Scope& scope, BoundStatement& bound) {
    Result<ExprType> result = make_error(StatusCode::INTERNAL_ERROR, "unknown expression type");

    if (auto* e = dynamic_cast<const LiteralExpr*>(&expr)) {
        result = bind_literal(*e);
    } else if (auto* e = dynamic_cast<const ColumnRefExpr*>(&expr)) {
        result = bind_column_ref(*e, scope);
    } else if (auto* e = dynamic_cast<const BinaryExpr*>(&expr)) {
        result = bind_binary(*e, scope, bound);
    } else if (auto* e = dynamic_cast<const UnaryExpr*>(&expr)) {
        result = bind_unary(*e, scope, bound);
    } else if (auto* e = dynamic_cast<const FunctionCallExpr*>(&expr)) {
        result = bind_function_call(*e, scope, bound);
    } else if (auto* e = dynamic_cast<const CastExpr*>(&expr)) {
        result = bind_cast(*e, scope, bound);
    } else if (auto* e = dynamic_cast<const CaseExpr*>(&expr)) {
        result = bind_case(*e, scope, bound);
    } else if (auto* e = dynamic_cast<const InExpr*>(&expr)) {
        result = bind_in(*e, scope, bound);
    } else if (auto* e = dynamic_cast<const BetweenExpr*>(&expr)) {
        result = bind_between(*e, scope, bound);
    } else if (auto* e = dynamic_cast<const IsNullExpr*>(&expr)) {
        result = bind_is_null(*e, scope, bound);
    } else if (auto* e = dynamic_cast<const LikeExpr*>(&expr)) {
        result = bind_like(*e, scope, bound);
    } else if (auto* e = dynamic_cast<const ExistsExpr*>(&expr)) {
        result = bind_exists(*e, scope, bound);
    } else if (auto* e = dynamic_cast<const SubqueryExpr*>(&expr)) {
        result = bind_subquery(*e, scope, bound);
    } else if (auto* e = dynamic_cast<const ArrayExpr*>(&expr)) {
        result = bind_array(*e, scope, bound);
    }

    if (result) {
        bound.expr_types[&expr] = *result;
    }
    return result;
}

Result<ExprType> Binder::bind_literal(const LiteralExpr& expr) {
    ExprType et;
    switch (expr.kind) {
    case LiteralKind::INTEGER:
        et.type_id = TypeId::INT64;
        et.nullable = false;
        break;
    case LiteralKind::FLOAT:
        et.type_id = TypeId::FLOAT64;
        et.nullable = false;
        break;
    case LiteralKind::STRING:
        et.type_id = TypeId::STRING;
        et.nullable = false;
        break;
    case LiteralKind::BOOLEAN:
        et.type_id = TypeId::BOOL;
        et.nullable = false;
        break;
    case LiteralKind::NULL_LITERAL:
        et.type_id = TypeId::STRING; // placeholder, null is polymorphic
        et.nullable = true;
        break;
    }
    return ok(et);
}

Result<ExprType> Binder::bind_column_ref(const ColumnRefExpr& expr, Scope& scope) {
    Result<ResolvedColumn> rc;
    if (expr.table.empty()) {
        rc = scope.resolve_column(expr.column);
    } else {
        rc = scope.resolve_column(expr.table, expr.column);
    }
    if (!rc) {
        return tl::unexpected(rc.error());
    }

    ExprType et;
    et.type_id = rc->type_id;
    et.nullable = rc->nullable;
    return ok(et);
}

Result<ExprType> Binder::bind_binary(const BinaryExpr& expr, Scope& scope, BoundStatement& bound) {
    auto lhs = bind_expr(*expr.lhs, scope, bound);
    if (!lhs) {
        return lhs;
    }
    auto rhs = bind_expr(*expr.rhs, scope, bound);
    if (!rhs) {
        return rhs;
    }

    ExprType et;
    et.nullable = lhs->nullable || rhs->nullable;
    et.is_aggregate = lhs->is_aggregate || rhs->is_aggregate;

    switch (expr.op) {
    case BinaryOp::ADD:
    case BinaryOp::SUBTRACT:
    case BinaryOp::MULTIPLY:
    case BinaryOp::DIVIDE:
    case BinaryOp::MODULO: {
        auto ct = common_type(lhs->type_id, rhs->type_id);
        if (!ct) {
            return make_error(
                StatusCode::TYPE_ERROR,
                "incompatible types for arithmetic: " + std::string(type_name(lhs->type_id)) +
                    " and " + std::string(type_name(rhs->type_id)));
        }
        et.type_id = *ct;
        break;
    }
    case BinaryOp::EQUAL:
    case BinaryOp::NOT_EQUAL:
    case BinaryOp::LESS:
    case BinaryOp::GREATER:
    case BinaryOp::LESS_EQUAL:
    case BinaryOp::GREATER_EQUAL:
        et.type_id = TypeId::BOOL;
        break;
    case BinaryOp::AND:
    case BinaryOp::OR:
        if (lhs->type_id != TypeId::BOOL || rhs->type_id != TypeId::BOOL) {
            return make_error(StatusCode::TYPE_ERROR, "AND/OR requires boolean operands");
        }
        et.type_id = TypeId::BOOL;
        break;
    case BinaryOp::CONCAT:
        et.type_id = TypeId::STRING;
        break;
    }

    return ok(et);
}

Result<ExprType> Binder::bind_unary(const UnaryExpr& expr, Scope& scope, BoundStatement& bound) {
    auto operand = bind_expr(*expr.operand, scope, bound);
    if (!operand) {
        return operand;
    }

    ExprType et;
    et.nullable = operand->nullable;
    et.is_aggregate = operand->is_aggregate;

    switch (expr.op) {
    case UnaryOp::NEGATE:
        if (!is_numeric(operand->type_id)) {
            return make_error(StatusCode::TYPE_ERROR,
                              "NEGATE requires a numeric operand, got " +
                                  std::string(type_name(operand->type_id)));
        }
        et.type_id = operand->type_id;
        break;
    case UnaryOp::NOT:
        if (operand->type_id != TypeId::BOOL) {
            return make_error(StatusCode::TYPE_ERROR,
                              "NOT requires a boolean operand, got " +
                                  std::string(type_name(operand->type_id)));
        }
        et.type_id = TypeId::BOOL;
        break;
    }

    return ok(et);
}

Result<ExprType>
Binder::bind_function_call(const FunctionCallExpr& expr, Scope& scope, BoundStatement& bound) {
    // Bind all arguments first.
    std::vector<TypeId> arg_types;
    bool any_nullable = false;
    bool any_agg = false;

    for (auto& arg : expr.args) {
        // Special case: COUNT(*) — the parser represents * as ColumnRefExpr{column="*"}.
        // Skip normal binding for the star argument.
        if (auto* cref = dynamic_cast<const ColumnRefExpr*>(arg.get())) {
            if (cref->column == "*") {
                arg_types.push_back(TypeId::INT64); // placeholder
                continue;
            }
        }
        auto t = bind_expr(*arg, scope, bound);
        if (!t) {
            return t;
        }
        arg_types.push_back(t->type_id);
        any_nullable = any_nullable || t->nullable;
        any_agg = any_agg || t->is_aggregate;
    }

    ExprType et;
    et.nullable = any_nullable;

    if (is_aggregate_function(expr.name)) {
        // COUNT is never null.
        std::string upper = to_upper(expr.name);
        if (upper == "COUNT") {
            et.nullable = false;
        }

        // Aggregate input type: COUNT(*) has no args → use INT64 as placeholder.
        TypeId input = arg_types.empty() ? TypeId::INT64 : arg_types[0];
        auto ret = aggregate_return_type(expr.name, input);
        if (!ret) {
            return tl::unexpected(ret.error());
        }
        et.type_id = *ret;
        et.is_aggregate = true;
    } else {
        auto ret = function_return_type(expr.name, arg_types);
        if (!ret) {
            return tl::unexpected(ret.error());
        }
        et.type_id = *ret;
        et.is_aggregate = any_agg;
    }

    return ok(et);
}

Result<ExprType> Binder::bind_cast(const CastExpr& expr, Scope& scope, BoundStatement& bound) {
    auto inner = bind_expr(*expr.expr, scope, bound);
    if (!inner) {
        return inner;
    }

    auto target = resolve_type_spec(expr.target_type);
    if (!target) {
        return tl::unexpected(target.error());
    }

    ExprType et;
    et.type_id = *target;
    et.nullable = inner->nullable;
    et.is_aggregate = inner->is_aggregate;
    return ok(et);
}

Result<ExprType> Binder::bind_case(const CaseExpr& expr, Scope& scope, BoundStatement& bound) {
    // Bind optional operand.
    if (expr.operand) {
        auto op = bind_expr(*expr.operand, scope, bound);
        if (!op) {
            return op;
        }
    }

    TypeId result_type = TypeId::STRING; // will be overwritten
    bool first_result = true;
    bool any_nullable = false;
    bool any_agg = false;

    for (auto& w : expr.whens) {
        auto cond = bind_expr(*w.condition, scope, bound);
        if (!cond) {
            return cond;
        }
        auto res = bind_expr(*w.result, scope, bound);
        if (!res) {
            return res;
        }
        any_nullable = any_nullable || res->nullable;
        any_agg = any_agg || res->is_aggregate || cond->is_aggregate;

        if (first_result) {
            result_type = res->type_id;
            first_result = false;
        } else {
            auto ct = common_type(result_type, res->type_id);
            if (!ct) {
                return tl::unexpected(ct.error());
            }
            result_type = *ct;
        }
    }

    if (expr.else_expr) {
        auto el = bind_expr(*expr.else_expr, scope, bound);
        if (!el) {
            return el;
        }
        any_nullable = any_nullable || el->nullable;
        any_agg = any_agg || el->is_aggregate;
        if (!first_result) {
            auto ct = common_type(result_type, el->type_id);
            if (!ct) {
                return tl::unexpected(ct.error());
            }
            result_type = *ct;
        }
    } else {
        any_nullable = true; // no ELSE means NULL is possible
    }

    ExprType et;
    et.type_id = result_type;
    et.nullable = any_nullable;
    et.is_aggregate = any_agg;
    return ok(et);
}

Result<ExprType> Binder::bind_in(const InExpr& expr, Scope& scope, BoundStatement& bound) {
    auto lhs = bind_expr(*expr.expr, scope, bound);
    if (!lhs) {
        return lhs;
    }
    bool any_agg = lhs->is_aggregate;

    if (expr.subquery) {
        // IN (SELECT ...) — bind the subquery with parent scope for correlation.
        if (auto* sub_sel = dynamic_cast<const SelectStmt*>(expr.subquery.get())) {
            auto sub = bind_select(*sub_sel, &scope);
            if (!sub) {
                return tl::unexpected(sub.error());
            }
        } else {
            auto sub = bind(*expr.subquery);
            if (!sub) {
                return tl::unexpected(sub.error());
            }
        }
    } else {
        for (auto& val : expr.values) {
            auto v = bind_expr(*val, scope, bound);
            if (!v) {
                return v;
            }
            any_agg = any_agg || v->is_aggregate;
        }
    }

    ExprType et;
    et.type_id = TypeId::BOOL;
    et.nullable = lhs->nullable;
    et.is_aggregate = any_agg;
    return ok(et);
}

Result<ExprType>
Binder::bind_between(const BetweenExpr& expr, Scope& scope, BoundStatement& bound) {
    auto val = bind_expr(*expr.expr, scope, bound);
    if (!val) {
        return val;
    }
    auto lo = bind_expr(*expr.low, scope, bound);
    if (!lo) {
        return lo;
    }
    auto hi = bind_expr(*expr.high, scope, bound);
    if (!hi) {
        return hi;
    }

    ExprType et;
    et.type_id = TypeId::BOOL;
    et.nullable = val->nullable || lo->nullable || hi->nullable;
    et.is_aggregate = val->is_aggregate || lo->is_aggregate || hi->is_aggregate;
    return ok(et);
}

Result<ExprType> Binder::bind_is_null(const IsNullExpr& expr, Scope& scope, BoundStatement& bound) {
    auto inner = bind_expr(*expr.expr, scope, bound);
    if (!inner) {
        return inner;
    }

    ExprType et;
    et.type_id = TypeId::BOOL;
    et.nullable = false; // IS NULL always returns true/false
    et.is_aggregate = inner->is_aggregate;
    return ok(et);
}

Result<ExprType> Binder::bind_like(const LikeExpr& expr, Scope& scope, BoundStatement& bound) {
    auto val = bind_expr(*expr.expr, scope, bound);
    if (!val) {
        return val;
    }
    auto pat = bind_expr(*expr.pattern, scope, bound);
    if (!pat) {
        return pat;
    }

    ExprType et;
    et.type_id = TypeId::BOOL;
    et.nullable = val->nullable || pat->nullable;
    et.is_aggregate = val->is_aggregate || pat->is_aggregate;
    return ok(et);
}

Result<ExprType>
Binder::bind_exists(const ExistsExpr& expr, Scope& scope, BoundStatement& /*bound*/) {
    if (expr.subquery) {
        if (auto* sub_sel = dynamic_cast<const SelectStmt*>(expr.subquery.get())) {
            auto sub = bind_select(*sub_sel, &scope);
            if (!sub) {
                return tl::unexpected(sub.error());
            }
        } else {
            auto sub = bind(*expr.subquery);
            if (!sub) {
                return tl::unexpected(sub.error());
            }
        }
    }

    ExprType et;
    et.type_id = TypeId::BOOL;
    et.nullable = false;
    return ok(et);
}

Result<ExprType>
Binder::bind_subquery(const SubqueryExpr& expr, Scope& scope, BoundStatement& /*bound*/) {
    if (!expr.subquery) {
        return make_error(StatusCode::INTERNAL_ERROR, "subquery expression has no query");
    }

    Result<BoundStatement> sub = make_error(StatusCode::INTERNAL_ERROR, "unexpected");
    if (auto* sub_sel = dynamic_cast<const SelectStmt*>(expr.subquery.get())) {
        sub = bind_select(*sub_sel, &scope);
    } else {
        sub = bind(*expr.subquery);
    }
    if (!sub) {
        return tl::unexpected(sub.error());
    }

    if (sub->output_columns.empty()) {
        return make_error(StatusCode::TYPE_ERROR, "scalar subquery produces no columns");
    }

    ExprType et;
    et.type_id = sub->output_columns[0].type_id;
    et.nullable = true; // subquery may return NULL
    return ok(et);
}

Result<ExprType> Binder::bind_array(const ArrayExpr& expr, Scope& scope, BoundStatement& bound) {
    for (auto& elem : expr.elements) {
        auto t = bind_expr(*elem, scope, bound);
        if (!t) {
            return t;
        }
        // All elements should be numeric for EMBEDDING.
        if (!is_numeric(t->type_id)) {
            return make_error(StatusCode::TYPE_ERROR,
                              "array/embedding element must be numeric, got " +
                                  std::string(type_name(t->type_id)));
        }
    }

    ExprType et;
    et.type_id = TypeId::EMBEDDING;
    et.nullable = false;
    return ok(et);
}

// ===========================================================================
// SELECT binding
// ===========================================================================

Result<Scope>
Binder::build_from_scope(const SelectStmt& stmt, Scope* parent, BoundStatement& bound) {
    Scope scope(parent);

    // CTE references — bind each CTE and store in binder-level map
    // so that nested subquery bindings can also access them.
    for (auto& cte : stmt.ctes) {
        if (!cte.query) {
            continue;
        }
        auto sub = bind(*cte.query);
        if (!sub) {
            return tl::unexpected(sub.error());
        }
        cte_results_.emplace(to_upper(cte.name), std::move(*sub));
    }

    // FROM tables — check CTEs first, then catalog.
    for (auto& tref : stmt.from) {
        if (tref.subquery) {
            // Subquery in FROM.
            auto sub = bind(*tref.subquery);
            if (!sub) {
                return tl::unexpected(sub.error());
            }
            ScopeTable st;
            st.table_id = 0;
            st.alias = tref.alias.empty() ? tref.name : tref.alias;
            st.columns = sub->output_columns;
            for (auto& col : st.columns) {
                col.table_name = st.alias;
            }
            scope.add_table(std::move(st));
        } else {
            // Check if this is a CTE reference.
            auto cte_it = cte_results_.find(to_upper(tref.name));
            if (cte_it != cte_results_.end()) {
                ScopeTable st;
                st.table_id = 0;
                std::string alias = tref.alias.empty() ? tref.name : tref.alias;
                st.alias = alias;
                st.columns = cte_it->second.output_columns;
                for (auto& col : st.columns) {
                    col.table_name = alias;
                }
                scope.add_table(std::move(st));
            } else {
                // Regular table reference.
                auto schema = catalog_.get_table(tref.name);
                if (!schema) {
                    return tl::unexpected(schema.error());
                }
                std::string alias = tref.alias.empty() ? tref.name : tref.alias;
                scope.add_table(make_scope_table(*schema, alias));
                bound.referenced_tables.push_back(schema->table_id);
            }
        }
    }

    // JOIN tables.
    for (auto& join : stmt.joins) {
        auto& jtref = join.table;
        if (jtref.subquery) {
            auto sub = bind(*jtref.subquery);
            if (!sub) {
                return tl::unexpected(sub.error());
            }
            ScopeTable st;
            st.table_id = 0;
            st.alias = jtref.alias.empty() ? jtref.name : jtref.alias;
            st.columns = sub->output_columns;
            for (auto& col : st.columns) {
                col.table_name = st.alias;
            }
            scope.add_table(std::move(st));
        } else {
            // Check if this is a CTE reference.
            auto cte_it = cte_results_.find(to_upper(jtref.name));
            if (cte_it != cte_results_.end()) {
                ScopeTable st;
                st.table_id = 0;
                std::string alias = jtref.alias.empty() ? jtref.name : jtref.alias;
                st.alias = alias;
                st.columns = cte_it->second.output_columns;
                for (auto& col : st.columns) {
                    col.table_name = alias;
                }
                scope.add_table(std::move(st));
            } else {
                auto schema = catalog_.get_table(jtref.name);
                if (!schema) {
                    return tl::unexpected(schema.error());
                }
                std::string alias = jtref.alias.empty() ? jtref.name : jtref.alias;
                scope.add_table(make_scope_table(*schema, alias));
                bound.referenced_tables.push_back(schema->table_id);
            }
        }
    }

    return ok(std::move(scope));
}

Result<std::vector<ResolvedColumn>> Binder::expand_select_items(
    const std::vector<SelectItem>& items, Scope& scope, BoundStatement& bound) {
    std::vector<ResolvedColumn> output;

    for (auto& item : items) {
        if (item.is_star) {
            if (!item.table_star.empty()) {
                // table.*
                auto cols = scope.columns_for(item.table_star);
                if (!cols) {
                    return tl::unexpected(cols.error());
                }
                output.insert(output.end(), cols->begin(), cols->end());
            } else {
                // SELECT *
                auto all = scope.all_columns();
                output.insert(output.end(), all.begin(), all.end());
            }
        } else if (item.expr) {
            auto et = bind_expr(*item.expr, scope, bound);
            if (!et) {
                return tl::unexpected(et.error());
            }

            ResolvedColumn rc;
            rc.table_id = 0;
            rc.ordinal = -1;
            rc.type_id = et->type_id;
            rc.nullable = et->nullable;

            // If it's a simple column ref, copy the resolved info.
            if (auto* cref = dynamic_cast<const ColumnRefExpr*>(item.expr.get())) {
                Result<ResolvedColumn> resolved;
                if (cref->table.empty()) {
                    resolved = scope.resolve_column(cref->column);
                } else {
                    resolved = scope.resolve_column(cref->table, cref->column);
                }
                if (resolved) {
                    rc = *resolved;
                }
            }

            // Use alias if provided.
            if (!item.alias.empty()) {
                rc.column_name = item.alias;
            } else if (rc.column_name.empty()) {
                // For computed expressions with no alias, generate a name.
                rc.column_name = "?column?";
            }

            output.push_back(std::move(rc));
        }
    }

    return ok(std::move(output));
}

Result<void> Binder::validate_aggregates(const SelectStmt& stmt,
                                         const Scope& /*scope*/,
                                         const BoundStatement& bound) {
    bool has_group_by = !stmt.group_by.empty();
    bool has_aggregates = false;

    // Check if any SELECT item contains an aggregate.
    for (auto& item : stmt.items) {
        if (item.expr && contains_aggregate(*item.expr, bound)) {
            has_aggregates = true;
            break;
        }
    }

    if (!has_group_by && !has_aggregates) {
        return ok();
    }

    // If GROUP BY or aggregates are present, every non-aggregate column ref
    // in SELECT must appear in GROUP BY.
    for (auto& item : stmt.items) {
        if (item.is_star) {
            if (has_group_by || has_aggregates) {
                return make_error(StatusCode::TYPE_ERROR,
                                  "SELECT * not allowed with GROUP BY or aggregate functions");
            }
            continue;
        }
        if (!item.expr) {
            continue;
        }

        // If the whole expression is an aggregate, it's fine.
        if (contains_aggregate(*item.expr, bound)) {
            continue;
        }

        // Otherwise, it must be in GROUP BY (for column refs).
        if (auto* cref = dynamic_cast<const ColumnRefExpr*>(item.expr.get())) {
            if (has_group_by && !in_group_by(*cref, stmt.group_by)) {
                return make_error(
                    StatusCode::TYPE_ERROR,
                    "column " + cref->column +
                        " must appear in GROUP BY clause or be used in an aggregate function");
            }
            if (!has_group_by && has_aggregates) {
                return make_error(
                    StatusCode::TYPE_ERROR,
                    "column " + cref->column +
                        " must appear in GROUP BY clause or be used in an aggregate function");
            }
        }
    }

    return ok();
}

Result<std::vector<ResolvedColumn>>
Binder::bind_returning(const std::vector<SelectItem>& items, Scope& scope, BoundStatement& bound) {
    return expand_select_items(items, scope, bound);
}

Result<BoundStatement> Binder::bind_select(const SelectStmt& stmt) {
    return bind_select(stmt, nullptr);
}

Result<BoundStatement> Binder::bind_select(const SelectStmt& stmt, Scope* parent_scope) {
    BoundStatement bound;
    bound.stmt = &stmt;

    // 1. Build FROM scope (includes CTEs).
    auto scope_result = build_from_scope(stmt, parent_scope, bound);
    if (!scope_result) {
        return tl::unexpected(scope_result.error());
    }
    auto scope = std::move(*scope_result);

    // 2. Bind JOIN ON expressions.
    for (auto& join : stmt.joins) {
        if (join.on_expr) {
            auto et = bind_expr(*join.on_expr, scope, bound);
            if (!et) {
                return tl::unexpected(et.error());
            }
        }
    }

    // 3. Bind WHERE.
    if (stmt.where_expr) {
        auto et = bind_expr(*stmt.where_expr, scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
        if (et->type_id != TypeId::BOOL) {
            return make_error(StatusCode::TYPE_ERROR,
                              "WHERE clause must be boolean, got " +
                                  std::string(type_name(et->type_id)));
        }
    }

    // 4. Bind GROUP BY.
    for (auto& gb : stmt.group_by) {
        if (gb) {
            auto et = bind_expr(*gb, scope, bound);
            if (!et) {
                return tl::unexpected(et.error());
            }
        }
    }

    // 5. Expand and bind SELECT items.
    auto output = expand_select_items(stmt.items, scope, bound);
    if (!output) {
        return tl::unexpected(output.error());
    }
    bound.output_columns = std::move(*output);

    // 6. Bind HAVING.
    if (stmt.having_expr) {
        auto et = bind_expr(*stmt.having_expr, scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
        if (et->type_id != TypeId::BOOL) {
            return make_error(StatusCode::TYPE_ERROR,
                              "HAVING clause must be boolean, got " +
                                  std::string(type_name(et->type_id)));
        }
    }

    // 7. Bind ORDER BY.
    for (auto& ob : stmt.order_by) {
        if (ob.expr) {
            auto et = bind_expr(*ob.expr, scope, bound);
            if (!et) {
                return tl::unexpected(et.error());
            }
        }
    }

    // 8. Bind LIMIT / OFFSET.
    if (stmt.limit) {
        auto et = bind_expr(*stmt.limit, scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
    }
    if (stmt.offset) {
        auto et = bind_expr(*stmt.offset, scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
    }

    // 9. Validate aggregates / GROUP BY.
    auto agg_check = validate_aggregates(stmt, scope, bound);
    if (!agg_check) {
        return tl::unexpected(agg_check.error());
    }

    // 10. Set operations (UNION, INTERSECT, EXCEPT).
    if (stmt.set_rhs && stmt.set_op != SelectStmt::SetOp::NONE) {
        auto rhs = bind(*stmt.set_rhs);
        if (!rhs) {
            return tl::unexpected(rhs.error());
        }
        if (bound.output_columns.size() != rhs->output_columns.size()) {
            return make_error(StatusCode::TYPE_ERROR,
                              "set operation requires same number of columns");
        }
    }

    return ok(std::move(bound));
}

// ===========================================================================
// DML binding
// ===========================================================================

Result<BoundStatement> Binder::bind_insert(const InsertStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    auto schema = catalog_.get_table(stmt.table_name);
    if (!schema) {
        return tl::unexpected(schema.error());
    }
    bound.referenced_tables.push_back(schema->table_id);

    // Determine target columns.
    std::vector<const CatalogColumnDef*> target_cols;
    if (stmt.columns.empty()) {
        // All columns in schema order.
        for (auto& col : schema->columns) {
            target_cols.push_back(&col);
        }
    } else {
        for (auto& col_name : stmt.columns) {
            bool found = false;
            for (auto& col : schema->columns) {
                if (to_upper(col.name) == to_upper(col_name)) {
                    target_cols.push_back(&col);
                    found = true;
                    break;
                }
            }
            if (!found) {
                return make_error(StatusCode::NOT_FOUND,
                                  "column " + col_name + " not found in table " + stmt.table_name);
            }
        }
    }

    // Validate VALUES rows.
    Scope empty_scope;
    for (auto& row : stmt.values) {
        if (row.size() != target_cols.size()) {
            return make_error(StatusCode::TYPE_ERROR,
                              "INSERT column count mismatch: expected " +
                                  std::to_string(target_cols.size()) + ", got " +
                                  std::to_string(row.size()));
        }
        for (size_t i = 0; i < row.size(); ++i) {
            auto et = bind_expr(*row[i], empty_scope, bound);
            if (!et) {
                return tl::unexpected(et.error());
            }
            // Check type compatibility. Allow implicit numeric coercion in
            // either direction (literal INT64 can narrow to INT32 column).
            if (et->type_id != target_cols[i]->type_id && !et->nullable) {
                if (!can_coerce(et->type_id, target_cols[i]->type_id) &&
                    !can_coerce(target_cols[i]->type_id, et->type_id)) {
                    return make_error(StatusCode::TYPE_ERROR,
                                      "type mismatch for column " + target_cols[i]->name +
                                          ": cannot coerce " + std::string(type_name(et->type_id)) +
                                          " to " + std::string(type_name(target_cols[i]->type_id)));
                }
            }
        }
    }

    // INSERT ... SELECT.
    if (stmt.select) {
        auto sub = bind(*stmt.select);
        if (!sub) {
            return tl::unexpected(sub.error());
        }
        if (sub->output_columns.size() != target_cols.size()) {
            return make_error(StatusCode::TYPE_ERROR,
                              "INSERT...SELECT column count mismatch: expected " +
                                  std::to_string(target_cols.size()) + ", got " +
                                  std::to_string(sub->output_columns.size()));
        }
    }

    // RETURNING.
    if (!stmt.returning.empty()) {
        std::string alias = schema->name;
        Scope ret_scope;
        ret_scope.add_table(make_scope_table(*schema, alias));
        auto ret = bind_returning(stmt.returning, ret_scope, bound);
        if (!ret) {
            return tl::unexpected(ret.error());
        }
        bound.output_columns = std::move(*ret);
    }

    return ok(std::move(bound));
}

Result<BoundStatement> Binder::bind_update(const UpdateStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    auto schema = catalog_.get_table(stmt.table_name);
    if (!schema) {
        return tl::unexpected(schema.error());
    }
    bound.referenced_tables.push_back(schema->table_id);

    Scope scope;
    scope.add_table(make_scope_table(*schema, schema->name));

    // Validate SET assignments.
    for (auto& assign : stmt.assignments) {
        // Find column.
        const CatalogColumnDef* target_col = nullptr;
        for (auto& col : schema->columns) {
            if (to_upper(col.name) == to_upper(assign.column)) {
                target_col = &col;
                break;
            }
        }
        if (!target_col) {
            return make_error(StatusCode::NOT_FOUND,
                              "column " + assign.column + " not found in table " + stmt.table_name);
        }
        if (assign.value) {
            auto et = bind_expr(*assign.value, scope, bound);
            if (!et) {
                return tl::unexpected(et.error());
            }
            if (et->type_id != target_col->type_id && !et->nullable) {
                if (!can_coerce(et->type_id, target_col->type_id) &&
                    !can_coerce(target_col->type_id, et->type_id)) {
                    return make_error(StatusCode::TYPE_ERROR,
                                      "type mismatch for column " + target_col->name +
                                          ": cannot coerce " + std::string(type_name(et->type_id)) +
                                          " to " + std::string(type_name(target_col->type_id)));
                }
            }
        }
    }

    // WHERE.
    if (stmt.where_expr) {
        auto et = bind_expr(*stmt.where_expr, scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
    }

    // RETURNING.
    if (!stmt.returning.empty()) {
        auto ret = bind_returning(stmt.returning, scope, bound);
        if (!ret) {
            return tl::unexpected(ret.error());
        }
        bound.output_columns = std::move(*ret);
    }

    return ok(std::move(bound));
}

Result<BoundStatement> Binder::bind_delete(const DeleteStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    auto schema = catalog_.get_table(stmt.table_name);
    if (!schema) {
        return tl::unexpected(schema.error());
    }
    bound.referenced_tables.push_back(schema->table_id);

    Scope scope;
    scope.add_table(make_scope_table(*schema, schema->name));

    // WHERE.
    if (stmt.where_expr) {
        auto et = bind_expr(*stmt.where_expr, scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
    }

    // RETURNING.
    if (!stmt.returning.empty()) {
        auto ret = bind_returning(stmt.returning, scope, bound);
        if (!ret) {
            return tl::unexpected(ret.error());
        }
        bound.output_columns = std::move(*ret);
    }

    return ok(std::move(bound));
}

// ===========================================================================
// DDL binding
// ===========================================================================

Result<BoundStatement> Binder::bind_create_table(const CreateTableStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    // Check for duplicate table name (unless IF NOT EXISTS).
    if (!stmt.if_not_exists) {
        auto existing = catalog_.get_table(stmt.name);
        if (existing) {
            return make_error(StatusCode::ALREADY_EXISTS, "table " + stmt.name + " already exists");
        }
    }

    // Validate column types.
    for (auto& col : stmt.columns) {
        auto tid = resolve_type_spec(col.type);
        if (!tid) {
            return tl::unexpected(tid.error());
        }

        // Validate EMBEDDING source/provider.
        if (*tid == TypeId::EMBEDDING) {
            if (col.type.source.empty()) {
                return make_error(StatusCode::TYPE_ERROR,
                                  "EMBEDDING column " + col.name + " requires a source column");
            }
        }
    }

    // Validate table constraints.
    for (auto& constraint : stmt.constraints) {
        // Verify referenced columns exist in this table.
        for (auto& col_name : constraint.columns) {
            bool found = false;
            for (auto& col : stmt.columns) {
                if (to_upper(col.name) == to_upper(col_name)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return make_error(StatusCode::NOT_FOUND,
                                  "constraint references unknown column: " + col_name);
            }
        }

        // FK constraints: verify referenced table exists.
        if (constraint.kind == TableConstraint::Kind::FOREIGN_KEY) {
            if (!constraint.fk_table.empty()) {
                auto fk_schema = catalog_.get_table(constraint.fk_table);
                if (!fk_schema) {
                    return make_error(StatusCode::NOT_FOUND,
                                      "foreign key references unknown table: " +
                                          constraint.fk_table);
                }
            }
        }
    }

    // Validate column-level FK constraints.
    for (auto& col : stmt.columns) {
        if (!col.fk_table.empty()) {
            auto fk_schema = catalog_.get_table(col.fk_table);
            if (!fk_schema) {
                return make_error(StatusCode::NOT_FOUND,
                                  "foreign key references unknown table: " + col.fk_table);
            }
        }
    }

    return ok(std::move(bound));
}

Result<BoundStatement> Binder::bind_drop_table(const DropTableStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    if (!stmt.if_exists) {
        auto schema = catalog_.get_table(stmt.name);
        if (!schema) {
            return tl::unexpected(schema.error());
        }
        bound.referenced_tables.push_back(schema->table_id);
    }

    return ok(std::move(bound));
}

Result<BoundStatement> Binder::bind_alter_table(const AlterTableStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    auto schema = catalog_.get_table(stmt.table_name);
    if (!schema) {
        return tl::unexpected(schema.error());
    }
    bound.referenced_tables.push_back(schema->table_id);

    switch (stmt.action) {
    case AlterAction::ADD_COLUMN: {
        auto tid = resolve_type_spec(stmt.column.type);
        if (!tid) {
            return tl::unexpected(tid.error());
        }
        // Check for duplicate column.
        for (auto& col : schema->columns) {
            if (to_upper(col.name) == to_upper(stmt.column.name)) {
                return make_error(StatusCode::ALREADY_EXISTS,
                                  "column " + stmt.column.name + " already exists in table " +
                                      stmt.table_name);
            }
        }
        break;
    }
    case AlterAction::DROP_COLUMN: {
        bool found = false;
        for (auto& col : schema->columns) {
            if (to_upper(col.name) == to_upper(stmt.column_name)) {
                found = true;
                break;
            }
        }
        if (!found) {
            return make_error(StatusCode::NOT_FOUND,
                              "column " + stmt.column_name + " not found in table " +
                                  stmt.table_name);
        }
        break;
    }
    case AlterAction::RENAME_COLUMN: {
        bool found = false;
        for (auto& col : schema->columns) {
            if (to_upper(col.name) == to_upper(stmt.column_name)) {
                found = true;
                break;
            }
        }
        if (!found) {
            return make_error(StatusCode::NOT_FOUND,
                              "column " + stmt.column_name + " not found in table " +
                                  stmt.table_name);
        }
        break;
    }
    }

    return ok(std::move(bound));
}

Result<BoundStatement> Binder::bind_create_index(const CreateIndexStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    auto schema = catalog_.get_table(stmt.table_name);
    if (!schema) {
        return tl::unexpected(schema.error());
    }
    bound.referenced_tables.push_back(schema->table_id);

    // Verify all indexed columns exist.
    for (auto& col_name : stmt.columns) {
        bool found = false;
        for (auto& col : schema->columns) {
            if (to_upper(col.name) == to_upper(col_name)) {
                found = true;
                // For HNSW index, column must be EMBEDDING type.
                if (to_upper(stmt.method) == "HNSW" && col.type_id != TypeId::EMBEDDING) {
                    return make_error(StatusCode::TYPE_ERROR,
                                      "HNSW index requires EMBEDDING column, got " +
                                          std::string(type_name(col.type_id)));
                }
                break;
            }
        }
        if (!found) {
            return make_error(StatusCode::NOT_FOUND,
                              "column " + col_name + " not found in table " + stmt.table_name);
        }
    }

    // Check for duplicate index name (unless IF NOT EXISTS).
    if (!stmt.if_not_exists) {
        auto existing = catalog_.get_index(stmt.name);
        if (existing) {
            return make_error(StatusCode::ALREADY_EXISTS, "index " + stmt.name + " already exists");
        }
    }

    return ok(std::move(bound));
}

Result<BoundStatement> Binder::bind_drop_index(const DropIndexStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    if (!stmt.if_exists) {
        auto idx = catalog_.get_index(stmt.name);
        if (!idx) {
            return tl::unexpected(idx.error());
        }
    }

    return ok(std::move(bound));
}

Result<BoundStatement> Binder::bind_create_edge_type(const CreateEdgeTypeStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    // Verify FROM and TO tables exist.
    auto from_schema = catalog_.get_table(stmt.from_table);
    if (!from_schema) {
        return make_error(StatusCode::NOT_FOUND, "FROM table not found: " + stmt.from_table);
    }
    bound.referenced_tables.push_back(from_schema->table_id);

    auto to_schema = catalog_.get_table(stmt.to_table);
    if (!to_schema) {
        return make_error(StatusCode::NOT_FOUND, "TO table not found: " + stmt.to_table);
    }
    bound.referenced_tables.push_back(to_schema->table_id);

    // Validate edge property types.
    for (auto& prop : stmt.properties) {
        auto tid = resolve_type_spec(prop.type);
        if (!tid) {
            return tl::unexpected(tid.error());
        }
    }

    // Check for duplicate edge type.
    auto existing = catalog_.get_edge_type(stmt.name);
    if (existing) {
        return make_error(StatusCode::ALREADY_EXISTS, "edge type " + stmt.name + " already exists");
    }

    return ok(std::move(bound));
}

Result<BoundStatement> Binder::bind_drop_edge_type(const DropEdgeTypeStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    if (!stmt.if_exists) {
        auto edge = catalog_.get_edge_type(stmt.name);
        if (!edge) {
            return tl::unexpected(edge.error());
        }
    }

    return ok(std::move(bound));
}

// ===========================================================================
// Graph / Vector binding
// ===========================================================================

Result<BoundStatement> Binder::bind_link(const LinkStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    // Resolve edge type.
    auto edge = catalog_.get_edge_type(stmt.edge_type);
    if (!edge) {
        return tl::unexpected(edge.error());
    }
    bound.referenced_edge_types.push_back(edge->edge_id);

    // Verify source table matches edge definition.
    auto src = catalog_.get_table(stmt.source_table);
    if (!src) {
        return tl::unexpected(src.error());
    }
    bound.referenced_tables.push_back(src->table_id);
    if (src->table_id != edge->source_table_id) {
        return make_error(StatusCode::TYPE_ERROR,
                          "LINK source table " + stmt.source_table +
                              " does not match edge type source");
    }

    // Verify target table matches edge definition.
    auto tgt = catalog_.get_table(stmt.target_table);
    if (!tgt) {
        return tl::unexpected(tgt.error());
    }
    bound.referenced_tables.push_back(tgt->table_id);
    if (tgt->table_id != edge->target_table_id) {
        return make_error(StatusCode::TYPE_ERROR,
                          "LINK target table " + stmt.target_table +
                              " does not match edge type target");
    }

    // Bind key expressions.
    Scope empty_scope;
    if (stmt.source_key) {
        auto et = bind_expr(*stmt.source_key, empty_scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
    }
    if (stmt.target_key) {
        auto et = bind_expr(*stmt.target_key, empty_scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
    }

    return ok(std::move(bound));
}

Result<BoundStatement> Binder::bind_unlink(const UnlinkStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    auto edge = catalog_.get_edge_type(stmt.edge_type);
    if (!edge) {
        return tl::unexpected(edge.error());
    }
    bound.referenced_edge_types.push_back(edge->edge_id);

    auto src = catalog_.get_table(stmt.source_table);
    if (!src) {
        return tl::unexpected(src.error());
    }
    bound.referenced_tables.push_back(src->table_id);
    if (src->table_id != edge->source_table_id) {
        return make_error(StatusCode::TYPE_ERROR,
                          "UNLINK source table " + stmt.source_table +
                              " does not match edge type source");
    }

    auto tgt = catalog_.get_table(stmt.target_table);
    if (!tgt) {
        return tl::unexpected(tgt.error());
    }
    bound.referenced_tables.push_back(tgt->table_id);
    if (tgt->table_id != edge->target_table_id) {
        return make_error(StatusCode::TYPE_ERROR,
                          "UNLINK target table " + stmt.target_table +
                              " does not match edge type target");
    }

    Scope empty_scope;
    if (stmt.source_key) {
        auto et = bind_expr(*stmt.source_key, empty_scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
    }
    if (stmt.target_key) {
        auto et = bind_expr(*stmt.target_key, empty_scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
    }
    if (stmt.where_expr) {
        auto et = bind_expr(*stmt.where_expr, empty_scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
    }

    return ok(std::move(bound));
}

Result<BoundStatement> Binder::bind_traverse(const TraverseStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    auto edge = catalog_.get_edge_type(stmt.edge_type);
    if (!edge) {
        return tl::unexpected(edge.error());
    }
    bound.referenced_edge_types.push_back(edge->edge_id);

    auto from = catalog_.get_table(stmt.from_table);
    if (!from) {
        return tl::unexpected(from.error());
    }
    bound.referenced_tables.push_back(from->table_id);

    // Bind FROM key expression.
    Scope empty_scope;
    if (stmt.from_key) {
        auto et = bind_expr(*stmt.from_key, empty_scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
    }

    // Bind WHERE.
    if (stmt.where_expr) {
        auto et = bind_expr(*stmt.where_expr, empty_scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
    }

    return ok(std::move(bound));
}

Result<BoundStatement> Binder::bind_nearest(const NearestStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    // Resolve table.
    auto schema = catalog_.get_table(stmt.table_name);
    if (!schema) {
        return tl::unexpected(schema.error());
    }
    bound.referenced_tables.push_back(schema->table_id);

    // Verify column is EMBEDDING type.
    bool found = false;
    for (auto& col : schema->columns) {
        if (to_upper(col.name) == to_upper(stmt.column_name)) {
            if (col.type_id != TypeId::EMBEDDING) {
                return make_error(StatusCode::TYPE_ERROR,
                                  "NEAREST requires an EMBEDDING column, got " +
                                      std::string(type_name(col.type_id)));
            }
            found = true;
            break;
        }
    }
    if (!found) {
        return make_error(StatusCode::NOT_FOUND,
                          "column " + stmt.column_name + " not found in table " + stmt.table_name);
    }

    // Bind k (must be integer).
    Scope empty_scope;
    if (stmt.k) {
        auto et = bind_expr(*stmt.k, empty_scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
        if (!is_integer(et->type_id)) {
            return make_error(StatusCode::TYPE_ERROR,
                              "NEAREST k must be an integer, got " +
                                  std::string(type_name(et->type_id)));
        }
    }

    // Bind target expression.
    if (stmt.target) {
        auto et = bind_expr(*stmt.target, empty_scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
    }

    // Bind WHERE.
    if (stmt.where_expr) {
        Scope tbl_scope;
        tbl_scope.add_table(make_scope_table(*schema, schema->name));
        auto et = bind_expr(*stmt.where_expr, tbl_scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
    }

    // Bind WITHIN TRAVERSE.
    if (stmt.within_traverse) {
        auto sub = bind(*stmt.within_traverse);
        if (!sub) {
            return tl::unexpected(sub.error());
        }
    }

    return ok(std::move(bound));
}

Result<BoundStatement> Binder::bind_match(const MatchStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    Scope scope;

    // Resolve pattern: each node label → table, each edge → edge type.
    for (auto& elem : stmt.pattern) {
        if (!elem.node.label.empty()) {
            auto schema = catalog_.get_table(elem.node.label);
            if (!schema) {
                return tl::unexpected(schema.error());
            }
            bound.referenced_tables.push_back(schema->table_id);

            if (!elem.node.variable.empty()) {
                scope.add_table(make_scope_table(*schema, elem.node.variable));
            }
        }
        if (elem.outgoing_edge && !elem.outgoing_edge->edge_type.empty()) {
            auto edge = catalog_.get_edge_type(elem.outgoing_edge->edge_type);
            if (!edge) {
                return tl::unexpected(edge.error());
            }
            bound.referenced_edge_types.push_back(edge->edge_id);
        }
    }

    // Bind WHERE.
    if (stmt.where_expr) {
        auto et = bind_expr(*stmt.where_expr, scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
    }

    // Bind RETURN items.
    if (!stmt.return_items.empty()) {
        auto ret = expand_select_items(stmt.return_items, scope, bound);
        if (!ret) {
            return tl::unexpected(ret.error());
        }
        bound.output_columns = std::move(*ret);
    }

    return ok(std::move(bound));
}

Result<BoundStatement> Binder::bind_shortest_path(const ShortestPathStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    auto edge = catalog_.get_edge_type(stmt.edge_type);
    if (!edge) {
        return tl::unexpected(edge.error());
    }
    bound.referenced_edge_types.push_back(edge->edge_id);

    auto from = catalog_.get_table(stmt.from_table);
    if (!from) {
        return tl::unexpected(from.error());
    }
    bound.referenced_tables.push_back(from->table_id);

    auto to = catalog_.get_table(stmt.to_table);
    if (!to) {
        return tl::unexpected(to.error());
    }
    bound.referenced_tables.push_back(to->table_id);

    // Bind key expressions.
    Scope empty_scope;
    if (stmt.from_key) {
        auto et = bind_expr(*stmt.from_key, empty_scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
    }
    if (stmt.to_key) {
        auto et = bind_expr(*stmt.to_key, empty_scope, bound);
        if (!et) {
            return tl::unexpected(et.error());
        }
    }

    return ok(std::move(bound));
}

// ===========================================================================
// Admin binding
// ===========================================================================

Result<BoundStatement> Binder::bind_explain(const ExplainStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    if (stmt.statement) {
        auto inner = bind(*stmt.statement);
        if (!inner) {
            return tl::unexpected(inner.error());
        }
        // Propagate referenced tables/edges from inner statement.
        bound.referenced_tables = std::move(inner->referenced_tables);
        bound.referenced_edge_types = std::move(inner->referenced_edge_types);
    }

    return ok(std::move(bound));
}

Result<BoundStatement> Binder::bind_describe(const DescribeStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    auto schema = catalog_.get_table(stmt.table_name);
    if (!schema) {
        return tl::unexpected(schema.error());
    }
    bound.referenced_tables.push_back(schema->table_id);

    return ok(std::move(bound));
}

Result<BoundStatement> Binder::bind_show(const ShowStmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    // SHOW COLUMNS FROM table — verify table exists.
    if (stmt.target == ShowTarget::COLUMNS && !stmt.name.empty()) {
        auto schema = catalog_.get_table(stmt.name);
        if (!schema) {
            return tl::unexpected(schema.error());
        }
        bound.referenced_tables.push_back(schema->table_id);
    }

    return ok(std::move(bound));
}

Result<BoundStatement> Binder::bind_passthrough(const Stmt& stmt) {
    BoundStatement bound;
    bound.stmt = &stmt;

    // Validate REEMBED, VACUUM, ANALYZE table references.
    if (auto* s = dynamic_cast<const ReembedStmt*>(&stmt)) {
        if (!s->table_name.empty()) {
            auto schema = catalog_.get_table(s->table_name);
            if (!schema) {
                return tl::unexpected(schema.error());
            }
            bound.referenced_tables.push_back(schema->table_id);
        }
    } else if (auto* s = dynamic_cast<const VacuumStmt*>(&stmt)) {
        if (!s->table_name.empty()) {
            auto schema = catalog_.get_table(s->table_name);
            if (!schema) {
                return tl::unexpected(schema.error());
            }
            bound.referenced_tables.push_back(schema->table_id);
        }
    } else if (auto* s = dynamic_cast<const AnalyzeStmt*>(&stmt)) {
        if (!s->table_name.empty()) {
            auto schema = catalog_.get_table(s->table_name);
            if (!schema) {
                return tl::unexpected(schema.error());
            }
            bound.referenced_tables.push_back(schema->table_id);
        }
    }

    return ok(std::move(bound));
}

} // namespace giodb
