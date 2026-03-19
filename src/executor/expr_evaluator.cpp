#include "sixseven/executor/expr_evaluator.h"

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/coercion.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/planner.h"
#include "sixseven/executor/seq_scan.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/type_resolver.h"
#include "sixseven/server/wal_receiver.h"
#include "sixseven/storage/wal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>

namespace sixseven {

// Thread-local system function context for replication functions.
static thread_local const SystemFunctionContext* tl_sys_fn_ctx = nullptr;

void set_system_function_context(const SystemFunctionContext* ctx) {
    tl_sys_fn_ctx = ctx;
}

const SystemFunctionContext* get_system_function_context() {
    return tl_sys_fn_ctx;
}

namespace {

// ---------------------------------------------------------------------------
// UUID v4 generation
// ---------------------------------------------------------------------------

Uuid generate_uuid_v4() {
    static thread_local std::mt19937_64 rng([] {
        std::random_device rd;
        return rd();
    }());

    Uuid uuid{};
    // Fill with random bytes (two 64-bit randoms cover 16 bytes).
    auto r0 = rng();
    auto r1 = rng();
    std::memcpy(uuid.data(), &r0, 8);
    std::memcpy(uuid.data() + 8, &r1, 8);

    // Set version 4: byte 6 high nibble = 0100.
    uuid[6] = static_cast<uint8_t>((uuid[6] & 0x0F) | 0x40);
    // Set variant 1: byte 8 high bits = 10xx.
    uuid[8] = static_cast<uint8_t>((uuid[8] & 0x3F) | 0x80);

    return uuid;
}

// ---------------------------------------------------------------------------
// Temporal helpers (wall-clock time)
// ---------------------------------------------------------------------------

Timestamp current_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    return Timestamp{us};
}

Date current_date() {
    auto now = std::chrono::system_clock::now();
    auto days = std::chrono::duration_cast<std::chrono::days>(
                    std::chrono::floor<std::chrono::days>(now.time_since_epoch()))
                    .count();
    return Date{static_cast<int32_t>(days)};
}

Time current_time() {
    auto now = std::chrono::system_clock::now();
    auto since_epoch = now.time_since_epoch();
    auto today = std::chrono::floor<std::chrono::days>(since_epoch);
    auto time_of_day =
        std::chrono::duration_cast<std::chrono::microseconds>(since_epoch - today).count();
    return Time{time_of_day};
}

// ---------------------------------------------------------------------------
// Forward declaration of the internal evaluator
// ---------------------------------------------------------------------------

Result<Value> eval(const Expr& expr,
                   const Tuple& tuple,
                   const OutputSchema& schema,
                   const BoundStatement& bound,
                   const SubqueryContext* subquery_ctx);

// ---------------------------------------------------------------------------
// Literal evaluation
// ---------------------------------------------------------------------------

Result<Value> eval_literal(const LiteralExpr& expr) {
    switch (expr.kind) {
    case LiteralKind::INTEGER: {
        try {
            int64_t v = std::stoll(expr.value);
            return ok(Value(v));
        } catch (...) {
            return make_error(StatusCode::TYPE_ERROR,
                              "cannot parse integer literal: " + expr.value);
        }
    }
    case LiteralKind::FLOAT: {
        try {
            double v = std::stod(expr.value);
            return ok(Value(v));
        } catch (...) {
            return make_error(StatusCode::TYPE_ERROR, "cannot parse float literal: " + expr.value);
        }
    }
    case LiteralKind::STRING:
        return ok(Value(expr.value));
    case LiteralKind::BOOLEAN: {
        bool v = (expr.value == "true" || expr.value == "TRUE");
        return ok(Value(v));
    }
    case LiteralKind::NULL_LITERAL:
        return ok(Value::make_null());
    }
    return make_error(StatusCode::INTERNAL_ERROR, "unknown literal kind");
}

// ---------------------------------------------------------------------------
// Column reference evaluation
// ---------------------------------------------------------------------------

Result<Value>
eval_column_ref(const ColumnRefExpr& expr, const Tuple& tuple, const OutputSchema& schema) {
    // Handle temporal pseudo-columns parsed as bare identifiers.
    if (expr.table.empty()) {
        std::string upper;
        upper.reserve(expr.column.size());
        for (char c : expr.column) {
            upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        if (upper == "CURRENT_TIMESTAMP") {
            return ok(Value(current_timestamp()));
        }
        if (upper == "CURRENT_DATE") {
            return ok(Value(current_date()));
        }
        if (upper == "CURRENT_TIME") {
            return ok(Value(current_time()));
        }
    }

    std::optional<size_t> idx;
    if (!expr.table.empty()) {
        idx = schema.find_column(expr.table, expr.column);
    } else {
        idx = schema.find_column(expr.column);
    }

    if (!idx.has_value()) {
        std::string name = expr.table.empty() ? expr.column : (expr.table + "." + expr.column);
        return make_error(StatusCode::NOT_FOUND, "column not found: " + name);
    }

    if (*idx >= tuple.values.size()) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "column index out of range: " + std::to_string(*idx));
    }

    return ok(tuple.values[*idx]);
}

// ---------------------------------------------------------------------------
// Arithmetic helpers
// ---------------------------------------------------------------------------

/// Convert a numeric Value to double for arithmetic.
Result<double> to_double(const Value& v) {
    if (v.is_null()) {
        return make_error(StatusCode::TYPE_ERROR, "NULL in arithmetic");
    }
    switch (v.type_id()) {
    case TypeId::INT8:
        return ok(static_cast<double>(v.as_int8()));
    case TypeId::INT16:
        return ok(static_cast<double>(v.as_int16()));
    case TypeId::INT32:
        return ok(static_cast<double>(v.as_int32()));
    case TypeId::INT64:
        return ok(static_cast<double>(v.as_int64()));
    case TypeId::UINT8:
        return ok(static_cast<double>(v.as_uint8()));
    case TypeId::UINT16:
        return ok(static_cast<double>(v.as_uint16()));
    case TypeId::UINT32:
        return ok(static_cast<double>(v.as_uint32()));
    case TypeId::UINT64:
        return ok(static_cast<double>(v.as_uint64()));
    case TypeId::FLOAT32:
        return ok(static_cast<double>(v.as_float32()));
    case TypeId::FLOAT64:
        return ok(v.as_float64());
    default:
        return make_error(StatusCode::TYPE_ERROR,
                          "cannot convert " + std::string(type_name(v.type_id())) + " to numeric");
    }
}

/// Convert a numeric Value to int64 for integer arithmetic.
Result<int64_t> to_int64(const Value& v) {
    if (v.is_null()) {
        return make_error(StatusCode::TYPE_ERROR, "NULL in arithmetic");
    }
    switch (v.type_id()) {
    case TypeId::INT8:
        return ok(static_cast<int64_t>(v.as_int8()));
    case TypeId::INT16:
        return ok(static_cast<int64_t>(v.as_int16()));
    case TypeId::INT32:
        return ok(static_cast<int64_t>(v.as_int32()));
    case TypeId::INT64:
        return ok(v.as_int64());
    case TypeId::UINT8:
        return ok(static_cast<int64_t>(v.as_uint8()));
    case TypeId::UINT16:
        return ok(static_cast<int64_t>(v.as_uint16()));
    case TypeId::UINT32:
        return ok(static_cast<int64_t>(v.as_uint32()));
    case TypeId::UINT64:
        return ok(static_cast<int64_t>(v.as_uint64()));
    default:
        return make_error(StatusCode::TYPE_ERROR,
                          "cannot convert " + std::string(type_name(v.type_id())) + " to integer");
    }
}

/// True if both sides are integer types (no floats involved).
bool both_integer(const Value& a, const Value& b) {
    return !a.is_null() && !b.is_null() && is_integer(a.type_id()) && is_integer(b.type_id());
}

Result<Value> eval_arithmetic(BinaryOp op, const Value& lhs, const Value& rhs) {
    // NULL propagation: any arithmetic with NULL yields NULL.
    if (lhs.is_null() || rhs.is_null()) {
        return ok(Value::make_null());
    }

    // Integer path: if both sides are integers, stay in int64.
    if (both_integer(lhs, rhs) && op != BinaryOp::DIVIDE) {
        auto la = to_int64(lhs);
        auto ra = to_int64(rhs);
        if (!la || !ra) {
            return make_error(StatusCode::TYPE_ERROR, "arithmetic type error");
        }
        int64_t l = *la;
        int64_t r = *ra;
        switch (op) {
        case BinaryOp::ADD:
            return ok(Value(static_cast<int64_t>(l + r)));
        case BinaryOp::SUBTRACT:
            return ok(Value(static_cast<int64_t>(l - r)));
        case BinaryOp::MULTIPLY:
            return ok(Value(static_cast<int64_t>(l * r)));
        case BinaryOp::MODULO:
            if (r == 0) {
                return make_error(StatusCode::INVALID_ARGUMENT, "division by zero");
            }
            return ok(Value(static_cast<int64_t>(l % r)));
        default:
            break;
        }
    }

    // Float path.
    auto la = to_double(lhs);
    auto ra = to_double(rhs);
    if (!la || !ra) {
        return make_error(StatusCode::TYPE_ERROR, "arithmetic requires numeric operands");
    }
    double l = *la;
    double r = *ra;
    switch (op) {
    case BinaryOp::ADD:
        return ok(Value(l + r));
    case BinaryOp::SUBTRACT:
        return ok(Value(l - r));
    case BinaryOp::MULTIPLY:
        return ok(Value(l * r));
    case BinaryOp::DIVIDE:
        if (r == 0.0) {
            return make_error(StatusCode::INVALID_ARGUMENT, "division by zero");
        }
        return ok(Value(l / r));
    case BinaryOp::MODULO:
        if (r == 0.0) {
            return make_error(StatusCode::INVALID_ARGUMENT, "division by zero");
        }
        return ok(Value(std::fmod(l, r)));
    default:
        return make_error(StatusCode::INTERNAL_ERROR, "unexpected arithmetic op");
    }
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

Result<Value> eval_comparison(BinaryOp op, const Value& lhs, const Value& rhs) {
    // NULL comparisons always yield NULL (not true/false).
    if (lhs.is_null() || rhs.is_null()) {
        return ok(Value::make_null());
    }

    auto cmp = compare(lhs, rhs);
    if (!cmp) {
        return make_error(cmp.error().code, cmp.error().message);
    }

    bool result = false;
    switch (op) {
    case BinaryOp::EQUAL:
        result = (*cmp == std::strong_ordering::equal);
        break;
    case BinaryOp::NOT_EQUAL:
        result = (*cmp != std::strong_ordering::equal);
        break;
    case BinaryOp::LESS:
        result = (*cmp == std::strong_ordering::less);
        break;
    case BinaryOp::GREATER:
        result = (*cmp == std::strong_ordering::greater);
        break;
    case BinaryOp::LESS_EQUAL:
        result = (*cmp != std::strong_ordering::greater);
        break;
    case BinaryOp::GREATER_EQUAL:
        result = (*cmp != std::strong_ordering::less);
        break;
    default:
        return make_error(StatusCode::INTERNAL_ERROR, "unexpected comparison op");
    }
    return ok(Value(result));
}

// ---------------------------------------------------------------------------
// Binary expression
// ---------------------------------------------------------------------------

Result<Value> eval_binary(const BinaryExpr& expr,
                          const Tuple& tuple,
                          const OutputSchema& schema,
                          const BoundStatement& bound,
                          const SubqueryContext* subquery_ctx) {
    // Short-circuit for AND/OR.
    if (expr.op == BinaryOp::AND) {
        auto lhs = eval(*expr.lhs, tuple, schema, bound, subquery_ctx);
        if (!lhs) {
            return lhs;
        }
        // SQL three-valued logic: FALSE AND anything = FALSE.
        if (!lhs->is_null() && lhs->type_id() == TypeId::BOOL && !lhs->as_bool()) {
            return ok(Value(false));
        }
        auto rhs = eval(*expr.rhs, tuple, schema, bound, subquery_ctx);
        if (!rhs) {
            return rhs;
        }
        // NULL AND TRUE = NULL, NULL AND FALSE = FALSE.
        if (lhs->is_null()) {
            if (!rhs->is_null() && rhs->type_id() == TypeId::BOOL && !rhs->as_bool()) {
                return ok(Value(false));
            }
            return ok(Value::make_null());
        }
        if (rhs->is_null()) {
            return ok(Value::make_null());
        }
        if (rhs->type_id() == TypeId::BOOL) {
            return ok(Value(rhs->as_bool()));
        }
        return make_error(StatusCode::TYPE_ERROR, "AND requires boolean operands");
    }

    if (expr.op == BinaryOp::OR) {
        auto lhs = eval(*expr.lhs, tuple, schema, bound, subquery_ctx);
        if (!lhs) {
            return lhs;
        }
        // SQL three-valued logic: TRUE OR anything = TRUE.
        if (!lhs->is_null() && lhs->type_id() == TypeId::BOOL && lhs->as_bool()) {
            return ok(Value(true));
        }
        auto rhs = eval(*expr.rhs, tuple, schema, bound, subquery_ctx);
        if (!rhs) {
            return rhs;
        }
        // NULL OR FALSE = NULL, NULL OR TRUE = TRUE.
        if (lhs->is_null()) {
            if (!rhs->is_null() && rhs->type_id() == TypeId::BOOL && rhs->as_bool()) {
                return ok(Value(true));
            }
            return ok(Value::make_null());
        }
        if (rhs->is_null()) {
            return ok(Value::make_null());
        }
        if (rhs->type_id() == TypeId::BOOL) {
            return ok(Value(rhs->as_bool()));
        }
        return make_error(StatusCode::TYPE_ERROR, "OR requires boolean operands");
    }

    // Evaluate both sides.
    auto lhs = eval(*expr.lhs, tuple, schema, bound, subquery_ctx);
    if (!lhs) {
        return lhs;
    }
    auto rhs = eval(*expr.rhs, tuple, schema, bound, subquery_ctx);
    if (!rhs) {
        return rhs;
    }

    // CONCAT.
    if (expr.op == BinaryOp::CONCAT) {
        if (lhs->is_null() || rhs->is_null()) {
            return ok(Value::make_null());
        }
        // Coerce both to string if needed.
        auto ls = coerce(*lhs, TypeId::STRING);
        auto rs = coerce(*rhs, TypeId::STRING);
        if (!ls || !rs) {
            return make_error(StatusCode::TYPE_ERROR, "CONCAT requires string-coercible operands");
        }
        return ok(Value(ls->as_string() + rs->as_string()));
    }

    // Arithmetic.
    switch (expr.op) {
    case BinaryOp::ADD:
    case BinaryOp::SUBTRACT:
    case BinaryOp::MULTIPLY:
    case BinaryOp::DIVIDE:
    case BinaryOp::MODULO:
        return eval_arithmetic(expr.op, *lhs, *rhs);
    default:
        break;
    }

    // Comparison.
    switch (expr.op) {
    case BinaryOp::EQUAL:
    case BinaryOp::NOT_EQUAL:
    case BinaryOp::LESS:
    case BinaryOp::GREATER:
    case BinaryOp::LESS_EQUAL:
    case BinaryOp::GREATER_EQUAL:
        return eval_comparison(expr.op, *lhs, *rhs);
    default:
        break;
    }

    return make_error(StatusCode::INTERNAL_ERROR, "unhandled binary op");
}

// ---------------------------------------------------------------------------
// Unary expression
// ---------------------------------------------------------------------------

Result<Value> eval_unary(const UnaryExpr& expr,
                         const Tuple& tuple,
                         const OutputSchema& schema,
                         const BoundStatement& bound,
                         const SubqueryContext* subquery_ctx) {
    auto val = eval(*expr.operand, tuple, schema, bound, subquery_ctx);
    if (!val) {
        return val;
    }

    if (val->is_null()) {
        return ok(Value::make_null());
    }

    switch (expr.op) {
    case UnaryOp::NEGATE: {
        if (!is_numeric(val->type_id())) {
            return make_error(StatusCode::TYPE_ERROR, "NEGATE requires a numeric operand");
        }
        // Coerce to double for simplicity; integer negate stays int if possible.
        if (is_integer(val->type_id())) {
            auto i = to_int64(*val);
            if (i) {
                return ok(Value(static_cast<int64_t>(-*i)));
            }
        }
        auto d = to_double(*val);
        if (!d) {
            return make_error(StatusCode::TYPE_ERROR, "NEGATE type error");
        }
        return ok(Value(-*d));
    }
    case UnaryOp::NOT: {
        if (val->type_id() != TypeId::BOOL) {
            return make_error(StatusCode::TYPE_ERROR, "NOT requires a boolean operand");
        }
        return ok(Value(!val->as_bool()));
    }
    }
    return make_error(StatusCode::INTERNAL_ERROR, "unknown unary op");
}

// ---------------------------------------------------------------------------
// CAST expression
// ---------------------------------------------------------------------------

Result<Value> eval_cast(const CastExpr& expr,
                        const Tuple& tuple,
                        const OutputSchema& schema,
                        const BoundStatement& bound,
                        const SubqueryContext* subquery_ctx) {
    auto val = eval(*expr.expr, tuple, schema, bound, subquery_ctx);
    if (!val) {
        return val;
    }
    if (val->is_null()) {
        return ok(Value::make_null());
    }

    auto target = resolve_type_spec(expr.target_type);
    if (!target) {
        return make_error(target.error().code, target.error().message);
    }
    return explicit_cast(*val, *target);
}

// ---------------------------------------------------------------------------
// IS NULL
// ---------------------------------------------------------------------------

Result<Value> eval_is_null(const IsNullExpr& expr,
                           const Tuple& tuple,
                           const OutputSchema& schema,
                           const BoundStatement& bound,
                           const SubqueryContext* subquery_ctx) {
    auto val = eval(*expr.expr, tuple, schema, bound, subquery_ctx);
    if (!val) {
        return val;
    }
    bool result = val->is_null();
    if (expr.negated) {
        result = !result;
    }
    return ok(Value(result));
}

// ---------------------------------------------------------------------------
// BETWEEN
// ---------------------------------------------------------------------------

Result<Value> eval_between(const BetweenExpr& expr,
                           const Tuple& tuple,
                           const OutputSchema& schema,
                           const BoundStatement& bound,
                           const SubqueryContext* subquery_ctx) {
    auto val = eval(*expr.expr, tuple, schema, bound, subquery_ctx);
    if (!val) {
        return val;
    }
    auto low = eval(*expr.low, tuple, schema, bound, subquery_ctx);
    if (!low) {
        return low;
    }
    auto high = eval(*expr.high, tuple, schema, bound, subquery_ctx);
    if (!high) {
        return high;
    }

    if (val->is_null() || low->is_null() || high->is_null()) {
        return ok(Value::make_null());
    }

    auto cmp_low = compare(*val, *low);
    if (!cmp_low) {
        return make_error(cmp_low.error().code, cmp_low.error().message);
    }
    auto cmp_high = compare(*val, *high);
    if (!cmp_high) {
        return make_error(cmp_high.error().code, cmp_high.error().message);
    }

    bool in_range =
        (*cmp_low != std::strong_ordering::less) && (*cmp_high != std::strong_ordering::greater);
    if (expr.negated) {
        in_range = !in_range;
    }
    return ok(Value(in_range));
}

// ---------------------------------------------------------------------------
// IN
// ---------------------------------------------------------------------------

Result<Value> eval_in(const InExpr& expr,
                      const Tuple& tuple,
                      const OutputSchema& schema,
                      const BoundStatement& bound,
                      const SubqueryContext* subquery_ctx) {
    auto val = eval(*expr.expr, tuple, schema, bound, subquery_ctx);
    if (!val) {
        return val;
    }

    if (val->is_null()) {
        return ok(Value::make_null());
    }

    // IN (subquery) — execute the subquery and check membership.
    if (expr.subquery && subquery_ctx) {
        auto* sel = dynamic_cast<const SelectStmt*>(expr.subquery.get());
        if (!sel) {
            return make_error(StatusCode::INTERNAL_ERROR, "IN subquery is not a SELECT");
        }

        Binder binder(subquery_ctx->catalog);
        auto sub_bound = binder.bind(*sel);
        if (!sub_bound) {
            return make_error(sub_bound.error().code, sub_bound.error().message);
        }

        Planner planner(subquery_ctx->catalog, subquery_ctx->storage);
        std::vector<ExprPtr> owned;
        auto iter = planner.plan(*sub_bound, owned);
        if (!iter) {
            return make_error(iter.error().code, iter.error().message);
        }

        auto open_res = (*iter)->open();
        if (!open_res) {
            return make_error(open_res.error().code, open_res.error().message);
        }

        bool found = false;
        bool has_null = false;
        while (true) {
            auto row = (*iter)->next();
            if (!row) {
                (*iter)->close();
                return make_error(row.error().code, row.error().message);
            }
            if (!row->has_value()) {
                break;
            }
            const auto& sub_val = row->value().values[0];
            if (sub_val.is_null()) {
                has_null = true;
                continue;
            }
            auto cmp = compare(*val, sub_val);
            if (cmp && *cmp == std::strong_ordering::equal) {
                found = true;
                break;
            }
        }
        (*iter)->close();

        if (found) {
            return ok(Value(!expr.negated));
        }
        if (has_null) {
            return ok(Value::make_null());
        }
        return ok(Value(expr.negated));
    }

    if (expr.subquery) {
        return make_error(StatusCode::NOT_IMPLEMENTED, "IN subquery evaluation not yet supported");
    }

    // IN-list (not subquery).
    bool found = false;
    bool has_null = false;
    for (const auto& elem : expr.values) {
        auto ev = eval(*elem, tuple, schema, bound, subquery_ctx);
        if (!ev) {
            return ev;
        }
        if (ev->is_null()) {
            has_null = true;
            continue;
        }
        auto cmp = compare(*val, *ev);
        if (!cmp) {
            return make_error(cmp.error().code, cmp.error().message);
        }
        if (*cmp == std::strong_ordering::equal) {
            found = true;
            break;
        }
    }

    if (found) {
        return ok(Value(!expr.negated));
    }
    if (has_null) {
        return ok(Value::make_null());
    }
    return ok(Value(expr.negated));
}

// ---------------------------------------------------------------------------
// LIKE  (simple % and _ pattern matching)
// ---------------------------------------------------------------------------

namespace {

bool match_like(const std::string& text, const std::string& pattern, size_t ti, size_t pi) {
    while (pi < pattern.size()) {
        char pc = pattern[pi];
        if (pc == '%') {
            // Skip consecutive %.
            while (pi < pattern.size() && pattern[pi] == '%') {
                ++pi;
            }
            if (pi == pattern.size()) {
                return true; // trailing % matches everything.
            }
            // Try matching rest of pattern at every remaining position.
            for (size_t i = ti; i <= text.size(); ++i) {
                if (match_like(text, pattern, i, pi)) {
                    return true;
                }
            }
            return false;
        }
        if (ti >= text.size()) {
            return false;
        }
        if (pc == '_') {
            // _ matches exactly one character.
            ++ti;
            ++pi;
        } else {
            if (text[ti] != pc) {
                return false;
            }
            ++ti;
            ++pi;
        }
    }
    return ti == text.size();
}

} // namespace

Result<Value> eval_like(const LikeExpr& expr,
                        const Tuple& tuple,
                        const OutputSchema& schema,
                        const BoundStatement& bound,
                        const SubqueryContext* subquery_ctx) {
    auto val = eval(*expr.expr, tuple, schema, bound, subquery_ctx);
    if (!val) {
        return val;
    }
    auto pat = eval(*expr.pattern, tuple, schema, bound, subquery_ctx);
    if (!pat) {
        return pat;
    }

    if (val->is_null() || pat->is_null()) {
        return ok(Value::make_null());
    }

    if (val->type_id() != TypeId::STRING || pat->type_id() != TypeId::STRING) {
        return make_error(StatusCode::TYPE_ERROR, "LIKE requires string operands");
    }

    bool result = match_like(val->as_string(), pat->as_string(), 0, 0);
    if (expr.negated) {
        result = !result;
    }
    return ok(Value(result));
}

// ---------------------------------------------------------------------------
// CASE expression
// ---------------------------------------------------------------------------

Result<Value> eval_case(const CaseExpr& expr,
                        const Tuple& tuple,
                        const OutputSchema& schema,
                        const BoundStatement& bound,
                        const SubqueryContext* subquery_ctx) {
    if (expr.operand) {
        // Simple CASE: CASE operand WHEN val THEN result ...
        auto operand = eval(*expr.operand, tuple, schema, bound, subquery_ctx);
        if (!operand) {
            return operand;
        }
        for (const auto& when : expr.whens) {
            auto cond_val = eval(*when.condition, tuple, schema, bound, subquery_ctx);
            if (!cond_val) {
                return cond_val;
            }
            if (!operand->is_null() && !cond_val->is_null()) {
                auto cmp = compare(*operand, *cond_val);
                if (cmp && *cmp == std::strong_ordering::equal) {
                    return eval(*when.result, tuple, schema, bound, subquery_ctx);
                }
            }
        }
    } else {
        // Searched CASE: CASE WHEN condition THEN result ...
        for (const auto& when : expr.whens) {
            auto cond = eval(*when.condition, tuple, schema, bound, subquery_ctx);
            if (!cond) {
                return cond;
            }
            if (!cond->is_null() && cond->type_id() == TypeId::BOOL && cond->as_bool()) {
                return eval(*when.result, tuple, schema, bound, subquery_ctx);
            }
        }
    }

    // No match — return ELSE or NULL.
    if (expr.else_expr) {
        return eval(*expr.else_expr, tuple, schema, bound, subquery_ctx);
    }
    return ok(Value::make_null());
}

// ---------------------------------------------------------------------------
// Function call (stub — full function evaluation is a later ticket)
// ---------------------------------------------------------------------------

Result<Value> eval_function(const FunctionCallExpr& expr,
                            const Tuple& tuple,
                            const OutputSchema& schema,
                            const BoundStatement& bound,
                            const SubqueryContext* subquery_ctx) {
    // Uppercase the function name for case-insensitive matching.
    std::string upper;
    upper.reserve(expr.name.size());
    for (char c : expr.name) {
        upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    // UUID generation.
    if (upper == "GEN_UUID" || upper == "GEN_RANDOM_UUID" || upper == "UUID_GENERATE_V4") {
        return ok(Value(generate_uuid_v4()));
    }

    // Date/time functions.
    if (upper == "NOW" || upper == "CURRENT_TIMESTAMP") {
        return ok(Value(current_timestamp()));
    }
    if (upper == "CURRENT_DATE") {
        return ok(Value(current_date()));
    }
    if (upper == "CURRENT_TIME") {
        return ok(Value(current_time()));
    }

    // Replication system functions.
    if (upper == "PG_CURRENT_WAL_LSN") {
        auto* ctx = tl_sys_fn_ctx;
        if (!ctx || !ctx->wal_writer) {
            return ok(Value::make_null());
        }
        return ok(Value(static_cast<int64_t>(ctx->wal_writer->current_lsn())));
    }

    if (upper == "PG_IS_IN_RECOVERY") {
        auto* ctx = tl_sys_fn_ctx;
        if (!ctx) {
            return ok(Value(false));
        }
        return ok(Value(ctx->standby_mode));
    }

    if (upper == "PG_LAST_WAL_REPLAY_LSN") {
        auto* ctx = tl_sys_fn_ctx;
        if (!ctx || !ctx->wal_receiver) {
            return ok(Value::make_null());
        }
        auto state = ctx->wal_receiver->get_state();
        if (state.applied_lsn == invalid_lsn) {
            return ok(Value::make_null());
        }
        return ok(Value(static_cast<int64_t>(state.applied_lsn)));
    }

    // Graph path functions.
    if (upper == "PATH_LENGTH") {
        if (expr.args.size() != 1) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "path_length() requires exactly 1 argument");
        }
        auto arg_val = eval(*expr.args[0], tuple, schema, bound, subquery_ctx);
        if (!arg_val) {
            return tl::unexpected(arg_val.error());
        }
        if (arg_val->is_null()) {
            return ok(Value::make_null());
        }
        auto path_ptr = arg_val->try_as_path();
        if (!path_ptr) {
            return make_error(StatusCode::TYPE_ERROR,
                              "path_length() argument must be a PATH value");
        }
        return ok(Value((*path_ptr)->length()));
    }

    // path_cost(p) — return the total weight of a weighted shortest path.
    if (upper == "PATH_COST") {
        if (expr.args.size() != 1) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "path_cost() requires exactly 1 argument");
        }
        auto arg_val = eval(*expr.args[0], tuple, schema, bound, subquery_ctx);
        if (!arg_val) {
            return tl::unexpected(arg_val.error());
        }
        if (arg_val->is_null()) {
            return ok(Value::make_null());
        }
        auto path_ptr = arg_val->try_as_path();
        if (!path_ptr) {
            return make_error(StatusCode::TYPE_ERROR, "path_cost() argument must be a PATH value");
        }
        return ok(Value((*path_ptr)->total_weight));
    }

    // nodes(p) — return a JSON array of node PKs in the path.
    if (upper == "NODES") {
        if (expr.args.size() != 1) {
            return make_error(StatusCode::INVALID_ARGUMENT, "nodes() requires exactly 1 argument");
        }
        auto arg_val = eval(*expr.args[0], tuple, schema, bound, subquery_ctx);
        if (!arg_val) {
            return tl::unexpected(arg_val.error());
        }
        if (arg_val->is_null()) {
            return ok(Value::make_null());
        }
        auto path_ptr = arg_val->try_as_path();
        if (!path_ptr) {
            return make_error(StatusCode::TYPE_ERROR, "nodes() argument must be a PATH value");
        }
        std::string json = "[";
        for (size_t i = 0; i < (*path_ptr)->steps.size(); ++i) {
            if (i > 0) {
                json += ",";
            }
            json += std::to_string((*path_ptr)->steps[i].node_pk);
        }
        json += "]";
        return ok(Value(JsonString{std::move(json)}));
    }

    // edges(p) / relationships(p) — return a JSON array of edge IDs in the path.
    if (upper == "EDGES" || upper == "RELATIONSHIPS") {
        if (expr.args.size() != 1) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              std::string(upper == "EDGES" ? "edges" : "relationships") +
                                  "() requires exactly 1 argument");
        }
        auto arg_val = eval(*expr.args[0], tuple, schema, bound, subquery_ctx);
        if (!arg_val) {
            return tl::unexpected(arg_val.error());
        }
        if (arg_val->is_null()) {
            return ok(Value::make_null());
        }
        auto path_ptr = arg_val->try_as_path();
        if (!path_ptr) {
            return make_error(StatusCode::TYPE_ERROR,
                              std::string(upper == "EDGES" ? "edges" : "relationships") +
                                  "() argument must be a PATH value");
        }
        std::string json = "[";
        bool first = true;
        for (const auto& step : (*path_ptr)->steps) {
            if (step.edge_id >= 0) {
                if (!first) {
                    json += ",";
                }
                json += std::to_string(step.edge_id);
                first = false;
            }
        }
        json += "]";
        return ok(Value(JsonString{std::move(json)}));
    }

    return make_error(StatusCode::NOT_IMPLEMENTED,
                      "function '" + expr.name + "' is not yet implemented");
}

// ---------------------------------------------------------------------------
// Array expression
// ---------------------------------------------------------------------------

Result<Value> eval_array(const ArrayExpr& expr,
                         const Tuple& tuple,
                         const OutputSchema& schema,
                         const BoundStatement& bound,
                         const SubqueryContext* subquery_ctx) {
    Embedding result;
    result.reserve(expr.elements.size());
    for (const auto& elem : expr.elements) {
        auto val = eval(*elem, tuple, schema, bound, subquery_ctx);
        if (!val) {
            return val;
        }
        if (val->is_null()) {
            return make_error(StatusCode::TYPE_ERROR, "NULL element in array literal");
        }
        auto d = to_double(*val);
        if (!d) {
            return make_error(StatusCode::TYPE_ERROR, "array elements must be numeric");
        }
        result.push_back(static_cast<float>(*d));
    }
    return ok(Value(std::move(result)));
}

// ---------------------------------------------------------------------------
// EXISTS expression
// ---------------------------------------------------------------------------

Result<Value> eval_exists(const ExistsExpr& expr,
                          const Tuple& outer_tuple,
                          const OutputSchema& outer_schema,
                          const BoundStatement& outer_bound,
                          const SubqueryContext* subquery_ctx) {
    if (!subquery_ctx) {
        return make_error(StatusCode::NOT_IMPLEMENTED,
                          "EXISTS subquery evaluation requires SubqueryContext");
    }

    auto* sel = dynamic_cast<const SelectStmt*>(expr.subquery.get());
    if (!sel) {
        return make_error(StatusCode::INTERNAL_ERROR, "EXISTS subquery is not a SELECT");
    }

    // Try non-correlated binding first.
    Binder binder(subquery_ctx->catalog);
    auto sub_bound = binder.bind(*sel);

    if (sub_bound) {
        // Non-correlated: plan and execute normally.
        Planner planner(subquery_ctx->catalog, subquery_ctx->storage);
        std::vector<ExprPtr> owned;
        auto iter = planner.plan(*sub_bound, owned);
        if (!iter) {
            return make_error(iter.error().code, iter.error().message);
        }

        auto open_res = (*iter)->open();
        if (!open_res) {
            return make_error(open_res.error().code, open_res.error().message);
        }

        auto row = (*iter)->next();
        (*iter)->close();

        if (!row) {
            return make_error(row.error().code, row.error().message);
        }
        return ok(Value(row->has_value()));
    }

    // Correlated subquery: binding failed because of outer column references.
    // Fall back to manual correlation: scan the inner table, combine each row
    // with the outer tuple, and evaluate the WHERE condition.
    if (sel->from.empty()) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "correlated EXISTS subquery has no FROM clause");
    }

    const auto& inner_ref = sel->from[0];
    const auto& inner_name = inner_ref.name;
    const auto& inner_alias = inner_ref.alias.empty() ? inner_name : inner_ref.alias;

    auto table_schema = subquery_ctx->catalog.get_table(default_database_id, inner_name);
    if (!table_schema) {
        return make_error(table_schema.error().code, table_schema.error().message);
    }

    auto ts = subquery_ctx->storage.get_table_storage(table_schema->table_id);
    if (!ts) {
        return make_error(ts.error().code, ts.error().message);
    }

    // Build inner output schema using the inner table alias.
    std::vector<OutputColumn> inner_cols;
    inner_cols.reserve(table_schema->columns.size());
    for (const auto& col : table_schema->columns) {
        inner_cols.push_back({inner_alias, col.name, col.type_id, col.nullable, 0});
    }
    auto inner_schema = OutputSchema(std::move(inner_cols));

    // Build combined schema: outer columns + inner columns, so the WHERE
    // expression can resolve references from both scopes.
    std::vector<OutputColumn> combined_cols;
    combined_cols.reserve(outer_schema.column_count() + inner_schema.column_count());
    for (size_t i = 0; i < outer_schema.column_count(); ++i) {
        combined_cols.push_back(outer_schema.column(i));
    }
    for (size_t i = 0; i < inner_schema.column_count(); ++i) {
        combined_cols.push_back(inner_schema.column(i));
    }
    auto combined_schema = OutputSchema(std::move(combined_cols));

    // Scan the inner table without a predicate.
    auto* storage = *ts;
    SeqScanOperator scan(*storage->heap, storage->storage_schema, inner_schema);
    auto open_res = scan.open();
    if (!open_res) {
        return make_error(open_res.error().code, open_res.error().message);
    }

    while (true) {
        auto row = scan.next();
        if (!row) {
            scan.close();
            return make_error(row.error().code, row.error().message);
        }
        if (!row->has_value()) {
            break;
        }

        // Combine outer + inner tuple.
        Tuple combined;
        combined.values = outer_tuple.values;
        for (auto& v : row->value().values) {
            combined.values.push_back(v);
        }

        // Evaluate the subquery WHERE against the combined row.
        if (sel->where_expr) {
            auto pass =
                evaluate_predicate(*sel->where_expr, combined, combined_schema, outer_bound);
            if (!pass) {
                scan.close();
                return make_error(pass.error().code, pass.error().message);
            }
            if (*pass) {
                scan.close();
                return ok(Value(true));
            }
        } else {
            // No WHERE — any row existing makes EXISTS true.
            scan.close();
            return ok(Value(true));
        }
    }

    scan.close();
    return ok(Value(false));
}

// ---------------------------------------------------------------------------
// Scalar subquery expression
// ---------------------------------------------------------------------------

Result<Value> eval_scalar_subquery(const SubqueryExpr& expr, const SubqueryContext* subquery_ctx) {
    if (!subquery_ctx) {
        return make_error(StatusCode::NOT_IMPLEMENTED,
                          "scalar subquery evaluation requires SubqueryContext");
    }

    auto* sel = dynamic_cast<const SelectStmt*>(expr.subquery.get());
    if (!sel) {
        return make_error(StatusCode::INTERNAL_ERROR, "scalar subquery is not a SELECT");
    }

    Binder binder(subquery_ctx->catalog);
    auto sub_bound = binder.bind(*sel);
    if (!sub_bound) {
        return make_error(sub_bound.error().code, sub_bound.error().message);
    }

    Planner planner(subquery_ctx->catalog, subquery_ctx->storage);
    std::vector<ExprPtr> owned;
    auto iter = planner.plan(*sub_bound, owned);
    if (!iter) {
        return make_error(iter.error().code, iter.error().message);
    }

    auto open_res = (*iter)->open();
    if (!open_res) {
        return make_error(open_res.error().code, open_res.error().message);
    }

    // Get first row.
    auto first_row = (*iter)->next();
    if (!first_row) {
        (*iter)->close();
        return make_error(first_row.error().code, first_row.error().message);
    }
    if (!first_row->has_value()) {
        (*iter)->close();
        return ok(Value::make_null()); // No rows → NULL.
    }

    Value result = first_row->value().values[0];

    // Check for more than one row.
    auto second_row = (*iter)->next();
    (*iter)->close();

    if (!second_row) {
        return make_error(second_row.error().code, second_row.error().message);
    }
    if (second_row->has_value()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "scalar subquery must return at most one row");
    }

    return ok(std::move(result));
}

// ---------------------------------------------------------------------------
// Main expression evaluator
// ---------------------------------------------------------------------------

Result<Value> eval(const Expr& expr,
                   const Tuple& tuple,
                   const OutputSchema& schema,
                   const BoundStatement& bound,
                   const SubqueryContext* subquery_ctx) {
    // Dispatch via dynamic_cast (matches binder pattern).
    if (auto* lit = dynamic_cast<const LiteralExpr*>(&expr)) {
        return eval_literal(*lit);
    }
    if (auto* col = dynamic_cast<const ColumnRefExpr*>(&expr)) {
        return eval_column_ref(*col, tuple, schema);
    }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(&expr)) {
        return eval_binary(*bin, tuple, schema, bound, subquery_ctx);
    }
    if (auto* un = dynamic_cast<const UnaryExpr*>(&expr)) {
        return eval_unary(*un, tuple, schema, bound, subquery_ctx);
    }
    if (auto* cast = dynamic_cast<const CastExpr*>(&expr)) {
        return eval_cast(*cast, tuple, schema, bound, subquery_ctx);
    }
    if (auto* is_null = dynamic_cast<const IsNullExpr*>(&expr)) {
        return eval_is_null(*is_null, tuple, schema, bound, subquery_ctx);
    }
    if (auto* between = dynamic_cast<const BetweenExpr*>(&expr)) {
        return eval_between(*between, tuple, schema, bound, subquery_ctx);
    }
    if (auto* in = dynamic_cast<const InExpr*>(&expr)) {
        return eval_in(*in, tuple, schema, bound, subquery_ctx);
    }
    if (auto* like = dynamic_cast<const LikeExpr*>(&expr)) {
        return eval_like(*like, tuple, schema, bound, subquery_ctx);
    }
    if (auto* case_e = dynamic_cast<const CaseExpr*>(&expr)) {
        return eval_case(*case_e, tuple, schema, bound, subquery_ctx);
    }
    if (auto* fn = dynamic_cast<const FunctionCallExpr*>(&expr)) {
        return eval_function(*fn, tuple, schema, bound, subquery_ctx);
    }
    if (auto* arr = dynamic_cast<const ArrayExpr*>(&expr)) {
        return eval_array(*arr, tuple, schema, bound, subquery_ctx);
    }
    if (auto* exists = dynamic_cast<const ExistsExpr*>(&expr)) {
        return eval_exists(*exists, tuple, schema, bound, subquery_ctx);
    }
    if (auto* sub = dynamic_cast<const SubqueryExpr*>(&expr)) {
        return eval_scalar_subquery(*sub, subquery_ctx);
    }

    return make_error(StatusCode::INTERNAL_ERROR, "unknown expression type in evaluator");
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<Value> evaluate_expr(const Expr& expr,
                            const Tuple& tuple,
                            const OutputSchema& schema,
                            const BoundStatement& bound,
                            const SubqueryContext* subquery_ctx) {
    return eval(expr, tuple, schema, bound, subquery_ctx);
}

Result<bool> evaluate_predicate(const Expr& expr,
                                const Tuple& tuple,
                                const OutputSchema& schema,
                                const BoundStatement& bound,
                                const SubqueryContext* subquery_ctx) {
    auto val = eval(expr, tuple, schema, bound, subquery_ctx);
    if (!val) {
        return make_error(val.error().code, val.error().message);
    }
    // NULL treated as false for predicates (WHERE filters out NULLs).
    if (val->is_null()) {
        return ok(false);
    }
    if (val->type_id() == TypeId::BOOL) {
        return ok(val->as_bool());
    }
    return make_error(StatusCode::TYPE_ERROR,
                      "predicate must evaluate to BOOL, got " +
                          std::string(type_name(val->type_id())));
}

} // namespace sixseven
