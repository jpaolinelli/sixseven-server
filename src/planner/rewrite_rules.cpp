#include "giodb/planner/rewrite_rules.h"

#include <cmath>
#include <cstdlib>

namespace giodb {

// =============================================================================
// Constant folding
// =============================================================================

static ExprPtr fold_int_arithmetic(BinaryOp op, int64_t l, int64_t r) {
    int64_t result = 0;
    switch (op) {
    case BinaryOp::ADD:
        result = l + r;
        break;
    case BinaryOp::SUBTRACT:
        result = l - r;
        break;
    case BinaryOp::MULTIPLY:
        result = l * r;
        break;
    case BinaryOp::DIVIDE:
        if (r == 0) {
            return nullptr;
        }
        result = l / r;
        break;
    case BinaryOp::MODULO:
        if (r == 0) {
            return nullptr;
        }
        result = l % r;
        break;
    default:
        return nullptr;
    }
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralKind::INTEGER;
    lit->value = std::to_string(result);
    return lit;
}

static ExprPtr fold_float_arithmetic(BinaryOp op, double l, double r) {
    double result = 0.0;
    switch (op) {
    case BinaryOp::ADD:
        result = l + r;
        break;
    case BinaryOp::SUBTRACT:
        result = l - r;
        break;
    case BinaryOp::MULTIPLY:
        result = l * r;
        break;
    case BinaryOp::DIVIDE:
        if (r == 0.0) {
            return nullptr;
        }
        result = l / r;
        break;
    default:
        return nullptr;
    }
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralKind::FLOAT;
    lit->value = std::to_string(result);
    return lit;
}

static ExprPtr fold_int_comparison(BinaryOp op, int64_t l, int64_t r) {
    bool result = false;
    switch (op) {
    case BinaryOp::EQUAL:
        result = (l == r);
        break;
    case BinaryOp::NOT_EQUAL:
        result = (l != r);
        break;
    case BinaryOp::LESS:
        result = (l < r);
        break;
    case BinaryOp::GREATER:
        result = (l > r);
        break;
    case BinaryOp::LESS_EQUAL:
        result = (l <= r);
        break;
    case BinaryOp::GREATER_EQUAL:
        result = (l >= r);
        break;
    default:
        return nullptr;
    }
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralKind::BOOLEAN;
    lit->value = result ? "true" : "false";
    return lit;
}

static ExprPtr fold_string_concat(const std::string& l, const std::string& r) {
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralKind::STRING;
    lit->value = l + r;
    return lit;
}

static const LiteralExpr* as_literal(const Expr& expr) {
    return dynamic_cast<const LiteralExpr*>(&expr);
}

static bool is_literal_true(const Expr& expr) {
    auto* lit = as_literal(expr);
    return lit != nullptr && lit->kind == LiteralKind::BOOLEAN && lit->value == "true";
}

static bool is_literal_false(const Expr& expr) {
    auto* lit = as_literal(expr);
    return lit != nullptr && lit->kind == LiteralKind::BOOLEAN && lit->value == "false";
}

static ExprPtr make_bool_literal(bool val) {
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralKind::BOOLEAN;
    lit->value = val ? "true" : "false";
    return lit;
}

static ExprPtr make_null_literal() {
    auto lit = std::make_unique<LiteralExpr>();
    lit->kind = LiteralKind::NULL_LITERAL;
    lit->value = "NULL";
    return lit;
}

/// Deep-clone an expression tree. Returns nullptr for types that contain
/// Stmt pointers (ExistsExpr, SubqueryExpr) since those can't be trivially cloned.
static ExprPtr clone_expr(const Expr& expr) {
    if (const auto* lit = dynamic_cast<const LiteralExpr*>(&expr)) {
        auto c = std::make_unique<LiteralExpr>();
        c->kind = lit->kind;
        c->value = lit->value;
        return c;
    }
    if (const auto* col = dynamic_cast<const ColumnRefExpr*>(&expr)) {
        auto c = std::make_unique<ColumnRefExpr>();
        c->table = col->table;
        c->column = col->column;
        return c;
    }
    if (const auto* bin = dynamic_cast<const BinaryExpr*>(&expr)) {
        auto cl = clone_expr(*bin->lhs);
        auto cr = clone_expr(*bin->rhs);
        if (!cl || !cr) {
            return nullptr;
        }
        auto c = std::make_unique<BinaryExpr>();
        c->op = bin->op;
        c->lhs = std::move(cl);
        c->rhs = std::move(cr);
        return c;
    }
    if (const auto* u = dynamic_cast<const UnaryExpr*>(&expr)) {
        auto co = clone_expr(*u->operand);
        if (!co) {
            return nullptr;
        }
        auto c = std::make_unique<UnaryExpr>();
        c->op = u->op;
        c->operand = std::move(co);
        return c;
    }
    if (const auto* func = dynamic_cast<const FunctionCallExpr*>(&expr)) {
        auto c = std::make_unique<FunctionCallExpr>();
        c->name = func->name;
        c->distinct = func->distinct;
        for (const auto& arg : func->args) {
            auto ca = clone_expr(*arg);
            if (!ca) {
                return nullptr;
            }
            c->args.push_back(std::move(ca));
        }
        return c;
    }
    if (const auto* cast = dynamic_cast<const CastExpr*>(&expr)) {
        auto ce = clone_expr(*cast->expr);
        if (!ce) {
            return nullptr;
        }
        auto c = std::make_unique<CastExpr>();
        c->expr = std::move(ce);
        c->target_type = cast->target_type;
        return c;
    }
    if (const auto* between = dynamic_cast<const BetweenExpr*>(&expr)) {
        auto ce = clone_expr(*between->expr);
        auto cl = clone_expr(*between->low);
        auto ch = clone_expr(*between->high);
        if (!ce || !cl || !ch) {
            return nullptr;
        }
        auto c = std::make_unique<BetweenExpr>();
        c->expr = std::move(ce);
        c->low = std::move(cl);
        c->high = std::move(ch);
        c->negated = between->negated;
        return c;
    }
    if (const auto* is_null = dynamic_cast<const IsNullExpr*>(&expr)) {
        auto ce = clone_expr(*is_null->expr);
        if (!ce) {
            return nullptr;
        }
        auto c = std::make_unique<IsNullExpr>();
        c->expr = std::move(ce);
        c->negated = is_null->negated;
        return c;
    }
    if (const auto* like = dynamic_cast<const LikeExpr*>(&expr)) {
        auto ce = clone_expr(*like->expr);
        auto cp = clone_expr(*like->pattern);
        if (!ce || !cp) {
            return nullptr;
        }
        auto c = std::make_unique<LikeExpr>();
        c->expr = std::move(ce);
        c->pattern = std::move(cp);
        c->negated = like->negated;
        return c;
    }
    if (const auto* in_expr = dynamic_cast<const InExpr*>(&expr)) {
        if (in_expr->subquery) {
            return nullptr; // Cannot clone subquery.
        }
        auto ce = clone_expr(*in_expr->expr);
        if (!ce) {
            return nullptr;
        }
        auto c = std::make_unique<InExpr>();
        c->expr = std::move(ce);
        c->negated = in_expr->negated;
        for (const auto& val : in_expr->values) {
            auto cv = clone_expr(*val);
            if (!cv) {
                return nullptr;
            }
            c->values.push_back(std::move(cv));
        }
        return c;
    }
    if (const auto* case_expr = dynamic_cast<const CaseExpr*>(&expr)) {
        auto c = std::make_unique<CaseExpr>();
        if (case_expr->operand) {
            c->operand = clone_expr(*case_expr->operand);
            if (!c->operand) {
                return nullptr;
            }
        }
        for (const auto& when : case_expr->whens) {
            CaseWhen cw;
            cw.condition = clone_expr(*when.condition);
            cw.result = clone_expr(*when.result);
            if (!cw.condition || !cw.result) {
                return nullptr;
            }
            c->whens.push_back(std::move(cw));
        }
        if (case_expr->else_expr) {
            c->else_expr = clone_expr(*case_expr->else_expr);
            if (!c->else_expr) {
                return nullptr;
            }
        }
        return c;
    }
    if (const auto* arr = dynamic_cast<const ArrayExpr*>(&expr)) {
        auto c = std::make_unique<ArrayExpr>();
        for (const auto& elem : arr->elements) {
            auto ce = clone_expr(*elem);
            if (!ce) {
                return nullptr;
            }
            c->elements.push_back(std::move(ce));
        }
        return c;
    }
    // ExistsExpr, SubqueryExpr contain StmtPtr — cannot trivially clone.
    return nullptr;
}

ExprPtr fold_constants(const Expr& expr) {
    if (const auto* bin = dynamic_cast<const BinaryExpr*>(&expr)) {
        auto folded_lhs = fold_constants(*bin->lhs);
        auto folded_rhs = fold_constants(*bin->rhs);
        const Expr& lhs = folded_lhs ? *folded_lhs : *bin->lhs;
        const Expr& rhs = folded_rhs ? *folded_rhs : *bin->rhs;

        const auto* left_lit = as_literal(lhs);
        const auto* right_lit = as_literal(rhs);

        if (left_lit != nullptr && right_lit != nullptr) {
            // Integer arithmetic/comparison.
            if (left_lit->kind == LiteralKind::INTEGER && right_lit->kind == LiteralKind::INTEGER) {
                int64_t l = std::stoll(left_lit->value);
                int64_t r = std::stoll(right_lit->value);

                auto arith = fold_int_arithmetic(bin->op, l, r);
                if (arith) {
                    return arith;
                }
                auto cmp = fold_int_comparison(bin->op, l, r);
                if (cmp) {
                    return cmp;
                }
            }

            // Float arithmetic (at least one float).
            if ((left_lit->kind == LiteralKind::FLOAT || left_lit->kind == LiteralKind::INTEGER) &&
                (right_lit->kind == LiteralKind::FLOAT ||
                 right_lit->kind == LiteralKind::INTEGER)) {
                if (!(left_lit->kind == LiteralKind::INTEGER &&
                      right_lit->kind == LiteralKind::INTEGER)) {
                    double l = std::stod(left_lit->value);
                    double r = std::stod(right_lit->value);
                    auto result = fold_float_arithmetic(bin->op, l, r);
                    if (result) {
                        return result;
                    }
                }
            }

            // String concatenation.
            if (left_lit->kind == LiteralKind::STRING && right_lit->kind == LiteralKind::STRING &&
                bin->op == BinaryOp::CONCAT) {
                return fold_string_concat(left_lit->value, right_lit->value);
            }

            // Comparison with NULL -> NULL.
            if (left_lit->kind == LiteralKind::NULL_LITERAL ||
                right_lit->kind == LiteralKind::NULL_LITERAL) {
                if (bin->op == BinaryOp::EQUAL || bin->op == BinaryOp::NOT_EQUAL ||
                    bin->op == BinaryOp::LESS || bin->op == BinaryOp::GREATER ||
                    bin->op == BinaryOp::LESS_EQUAL || bin->op == BinaryOp::GREATER_EQUAL) {
                    return make_null_literal();
                }
            }
        }

        // Boolean simplification.
        if (bin->op == BinaryOp::AND) {
            if (is_literal_false(lhs)) {
                return make_bool_literal(false);
            }
            if (is_literal_false(rhs)) {
                return make_bool_literal(false);
            }
            if (is_literal_true(lhs)) {
                if (folded_rhs) {
                    return folded_rhs;
                }
                return clone_expr(*bin->rhs);
            }
            if (is_literal_true(rhs)) {
                if (folded_lhs) {
                    return folded_lhs;
                }
                return clone_expr(*bin->lhs);
            }
        }

        if (bin->op == BinaryOp::OR) {
            if (is_literal_true(lhs)) {
                return make_bool_literal(true);
            }
            if (is_literal_true(rhs)) {
                return make_bool_literal(true);
            }
            if (is_literal_false(lhs)) {
                if (folded_rhs) {
                    return folded_rhs;
                }
                return clone_expr(*bin->rhs);
            }
            if (is_literal_false(rhs)) {
                if (folded_lhs) {
                    return folded_lhs;
                }
                return clone_expr(*bin->lhs);
            }
        }

        return nullptr;
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        auto folded = fold_constants(*unary->operand);
        const Expr& operand = folded ? *folded : *unary->operand;
        const auto* lit = as_literal(operand);

        if (unary->op == UnaryOp::NOT && lit != nullptr) {
            if (lit->kind == LiteralKind::BOOLEAN) {
                return make_bool_literal(lit->value == "false");
            }
        }

        if (unary->op == UnaryOp::NEGATE && lit != nullptr) {
            if (lit->kind == LiteralKind::INTEGER) {
                auto result = std::make_unique<LiteralExpr>();
                result->kind = LiteralKind::INTEGER;
                result->value = std::to_string(-std::stoll(lit->value));
                return result;
            }
            if (lit->kind == LiteralKind::FLOAT) {
                auto result = std::make_unique<LiteralExpr>();
                result->kind = LiteralKind::FLOAT;
                result->value = std::to_string(-std::stod(lit->value));
                return result;
            }
        }

        return nullptr;
    }

    return nullptr;
}

// =============================================================================
// Predicate pushdown
// =============================================================================

std::vector<const Expr*> extract_conjuncts(const Expr& expr) {
    std::vector<const Expr*> result;

    if (const auto* bin = dynamic_cast<const BinaryExpr*>(&expr)) {
        if (bin->op == BinaryOp::AND) {
            auto left = extract_conjuncts(*bin->lhs);
            auto right = extract_conjuncts(*bin->rhs);
            result.insert(result.end(), left.begin(), left.end());
            result.insert(result.end(), right.begin(), right.end());
            return result;
        }
    }

    result.push_back(&expr);
    return result;
}

static void collect_table_refs(const Expr& expr, std::unordered_set<std::string>& tables) {
    if (const auto* col = dynamic_cast<const ColumnRefExpr*>(&expr)) {
        if (!col->table.empty()) {
            tables.insert(col->table);
        }
        return;
    }

    if (const auto* bin = dynamic_cast<const BinaryExpr*>(&expr)) {
        collect_table_refs(*bin->lhs, tables);
        collect_table_refs(*bin->rhs, tables);
        return;
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        collect_table_refs(*unary->operand, tables);
        return;
    }

    if (const auto* func = dynamic_cast<const FunctionCallExpr*>(&expr)) {
        for (const auto& arg : func->args) {
            collect_table_refs(*arg, tables);
        }
        return;
    }

    if (const auto* between = dynamic_cast<const BetweenExpr*>(&expr)) {
        collect_table_refs(*between->expr, tables);
        collect_table_refs(*between->low, tables);
        collect_table_refs(*between->high, tables);
        return;
    }

    if (const auto* is_null = dynamic_cast<const IsNullExpr*>(&expr)) {
        collect_table_refs(*is_null->expr, tables);
        return;
    }

    if (const auto* like = dynamic_cast<const LikeExpr*>(&expr)) {
        collect_table_refs(*like->expr, tables);
        collect_table_refs(*like->pattern, tables);
        return;
    }

    if (const auto* cast = dynamic_cast<const CastExpr*>(&expr)) {
        collect_table_refs(*cast->expr, tables);
        return;
    }

    if (const auto* in_expr = dynamic_cast<const InExpr*>(&expr)) {
        collect_table_refs(*in_expr->expr, tables);
        for (const auto& val : in_expr->values) {
            collect_table_refs(*val, tables);
        }
        return;
    }

    if (const auto* case_expr = dynamic_cast<const CaseExpr*>(&expr)) {
        if (case_expr->operand) {
            collect_table_refs(*case_expr->operand, tables);
        }
        for (const auto& when : case_expr->whens) {
            collect_table_refs(*when.condition, tables);
            collect_table_refs(*when.result, tables);
        }
        if (case_expr->else_expr) {
            collect_table_refs(*case_expr->else_expr, tables);
        }
        return;
    }
}

std::unordered_set<std::string> referenced_tables(const Expr& expr) {
    std::unordered_set<std::string> tables;
    collect_table_refs(expr, tables);
    return tables;
}

bool is_single_table_predicate(const Expr& expr, const std::string& table_name) {
    auto tables = referenced_tables(expr);
    return tables.size() <= 1 && (tables.empty() || tables.count(table_name) > 0);
}

bool is_join_predicate(const Expr& expr) {
    auto tables = referenced_tables(expr);
    return tables.size() == 2;
}

std::vector<const Expr*> extract_join_predicates(const std::vector<const Expr*>& conjuncts,
                                                 const std::string& left_table,
                                                 const std::string& right_table) {
    std::vector<const Expr*> result;
    for (const auto* pred : conjuncts) {
        auto tables = referenced_tables(*pred);
        if (tables.count(left_table) > 0 && tables.count(right_table) > 0 && tables.size() == 2) {
            result.push_back(pred);
        }
    }
    return result;
}

std::vector<const Expr*> extract_table_predicates(const std::vector<const Expr*>& conjuncts,
                                                  const std::string& table_name) {
    std::vector<const Expr*> result;
    for (const auto* pred : conjuncts) {
        if (is_single_table_predicate(*pred, table_name)) {
            result.push_back(pred);
        }
    }
    return result;
}

// =============================================================================
// Projection pushdown
// =============================================================================

static void collect_refs(const Expr& expr, std::vector<ColumnRef>& refs) {
    if (const auto* col = dynamic_cast<const ColumnRefExpr*>(&expr)) {
        refs.push_back({col->table, col->column});
        return;
    }

    if (const auto* bin = dynamic_cast<const BinaryExpr*>(&expr)) {
        collect_refs(*bin->lhs, refs);
        collect_refs(*bin->rhs, refs);
        return;
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        collect_refs(*unary->operand, refs);
        return;
    }

    if (const auto* func = dynamic_cast<const FunctionCallExpr*>(&expr)) {
        for (const auto& arg : func->args) {
            collect_refs(*arg, refs);
        }
        return;
    }

    if (const auto* between = dynamic_cast<const BetweenExpr*>(&expr)) {
        collect_refs(*between->expr, refs);
        collect_refs(*between->low, refs);
        collect_refs(*between->high, refs);
        return;
    }

    if (const auto* is_null = dynamic_cast<const IsNullExpr*>(&expr)) {
        collect_refs(*is_null->expr, refs);
        return;
    }

    if (const auto* like = dynamic_cast<const LikeExpr*>(&expr)) {
        collect_refs(*like->expr, refs);
        collect_refs(*like->pattern, refs);
        return;
    }

    if (const auto* cast = dynamic_cast<const CastExpr*>(&expr)) {
        collect_refs(*cast->expr, refs);
        return;
    }

    if (const auto* in_expr = dynamic_cast<const InExpr*>(&expr)) {
        collect_refs(*in_expr->expr, refs);
        for (const auto& val : in_expr->values) {
            collect_refs(*val, refs);
        }
        return;
    }
}

std::vector<ColumnRef> collect_column_refs(const Expr& expr) {
    std::vector<ColumnRef> refs;
    collect_refs(expr, refs);
    return refs;
}

std::vector<ColumnRef> collect_column_refs(const std::vector<ExprPtr>& exprs) {
    std::vector<ColumnRef> refs;
    for (const auto& expr : exprs) {
        collect_refs(*expr, refs);
    }
    return refs;
}

std::unordered_set<std::string> needed_columns(const std::vector<ColumnRef>& refs,
                                               const std::string& table_name) {
    std::unordered_set<std::string> columns;
    for (const auto& ref : refs) {
        if (ref.table_name.empty() || ref.table_name == table_name) {
            columns.insert(ref.column_name);
        }
    }
    return columns;
}

// =============================================================================
// Boolean simplification
// =============================================================================

ExprPtr simplify_boolean(const Expr& expr) {
    return fold_constants(expr);
}

// =============================================================================
// Subquery decorrelation helpers
// =============================================================================

bool is_correlated_subquery(const Expr& expr, const std::unordered_set<std::string>& outer_tables) {
    auto tables = referenced_tables(expr);
    for (const auto& t : tables) {
        if (outer_tables.count(t) > 0) {
            return true;
        }
    }
    return false;
}

JoinType subquery_to_join_type(const Expr& expr) {
    // NOT EXISTS -> ANTI join, EXISTS -> SEMI join.
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        if (unary->op == UnaryOp::NOT) {
            if (dynamic_cast<const ExistsExpr*>(unary->operand.get()) != nullptr) {
                return JoinType::ANTI;
            }
        }
    }

    if (dynamic_cast<const ExistsExpr*>(&expr) != nullptr) {
        return JoinType::SEMI;
    }

    // NOT IN -> ANTI join, IN -> SEMI join.
    if (const auto* in_expr = dynamic_cast<const InExpr*>(&expr)) {
        return in_expr->negated ? JoinType::ANTI : JoinType::SEMI;
    }

    return JoinType::INNER;
}

} // namespace giodb
