// GDB-1287: DECIMAL column comparisons against numeric literals must compare
// at the DECIMAL's scale rather than falling through to a lossy/incorrect
// cross-type comparison. Regression coverage for the bug where
// `amount > 10.00` matched every row of a DECIMAL(10,2) column regardless of
// value.

#include "sixseven/common/decimal_math.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/planner/rewrite_rules.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace sixseven;

namespace {

/// Build a Decimal128 coefficient from a small signed integer (fits in lo).
Decimal128 d128(int64_t v) {
    if (v >= 0) {
        return Decimal128{0, static_cast<uint64_t>(v)};
    }
    return Decimal128{-1, static_cast<uint64_t>(v)};
}

/// Build a minimal OutputSchema for testing.
OutputSchema make_schema(std::vector<std::pair<std::string, TypeId>> cols) {
    std::vector<OutputColumn> out;
    for (auto& [name, tid] : cols) {
        OutputColumn c;
        c.name = name;
        c.type_id = tid;
        out.push_back(std::move(c));
    }
    return OutputSchema(std::move(out));
}

Tuple make_tuple(std::vector<Value> vals) {
    return Tuple{std::move(vals), std::nullopt};
}

ExprPtr lit_float(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::FLOAT;
    e->value = v;
    return e;
}

ExprPtr lit_int(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::INTEGER;
    e->value = v;
    return e;
}

ExprPtr col_ref(const std::string& name) {
    auto e = std::make_unique<ColumnRefExpr>();
    e->column = name;
    return e;
}

ExprPtr binary(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
    auto e = std::make_unique<BinaryExpr>();
    e->op = op;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    return e;
}

ExprPtr negate(ExprPtr operand) {
    auto e = std::make_unique<UnaryExpr>();
    e->op = UnaryOp::NEGATE;
    e->operand = std::move(operand);
    return e;
}

static BoundStatement empty_bound() {
    return BoundStatement{};
}

/// Evaluate `amount <op> -literal` (or `-literal <op> amount` if
/// `literal_lhs`), where the negative literal is built via the REAL
/// UnaryExpr{NEGATE, LiteralExpr} shape the parser/binder actually produce
/// for a negative literal -- NOT by pre-baking a '-' into LiteralExpr::value.
///
/// `fold_first` mirrors the real planner path (Planner::fold_predicate_for_
/// filter -> simplify_boolean -> fold_constants), which constant-folds
/// UnaryExpr{NEGATE, LiteralExpr} into a single LiteralExpr BEFORE the
/// expression ever reaches eval_binary(); this exercises GDB-1301's actual
/// bug surface (the fold's std::to_string(-safe_stod(...)) round-trip).
/// Passing fold_first=false exercises eval_binary()'s own UnaryExpr-
/// unwrapping defense-in-depth path directly, for the (believed
/// unreachable) case where folding does not run first.
Result<Value> eval_decimal_vs_negative_literal(BinaryOp op,
                                               int64_t coeff,
                                               int32_t column_scale,
                                               const std::string& magnitude_text,
                                               LiteralKind lit_kind,
                                               bool literal_lhs,
                                               bool fold_first) {
    auto schema = make_schema({{"amount", TypeId::DECIMAL}});
    auto tuple = make_tuple({Value(d128(coeff))});
    auto bound = empty_bound();

    ExprPtr col = col_ref("amount");
    const Expr* col_ptr = col.get();
    ExprType col_type;
    col_type.type_id = TypeId::DECIMAL;
    col_type.nullable = false;
    col_type.decimal_scale = column_scale;
    bound.expr_types[col_ptr] = col_type;

    ExprPtr magnitude =
        (lit_kind == LiteralKind::FLOAT) ? lit_float(magnitude_text) : lit_int(magnitude_text);
    ExprPtr neg_lit = negate(std::move(magnitude));
    if (fold_first) {
        auto folded = fold_constants(*neg_lit);
        if (folded == nullptr) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "expected fold_constants to fold NEGATE(literal)");
        }
        neg_lit = std::move(folded);
    }

    ExprPtr expr;
    if (literal_lhs) {
        expr = binary(op, std::move(neg_lit), std::move(col));
    } else {
        expr = binary(op, std::move(col), std::move(neg_lit));
    }
    return evaluate_expr(*expr, tuple, schema, bound);
}

/// Evaluate `amount <op> literal` (or `literal <op> amount` if `literal_lhs`)
/// where `amount` is a DECIMAL(10,2) column holding `coeff` (scale 2), and
/// `literal` is the raw text of a numeric literal of kind `lit_kind`.
Result<Value> eval_decimal_vs_literal(BinaryOp op,
                                      int64_t coeff,
                                      int32_t column_scale,
                                      const std::string& literal_text,
                                      LiteralKind lit_kind,
                                      bool literal_lhs) {
    auto schema = make_schema({{"amount", TypeId::DECIMAL}});
    auto tuple = make_tuple({Value(d128(coeff))});
    auto bound = empty_bound();

    ExprPtr col = col_ref("amount");
    const Expr* col_ptr = col.get();
    ExprType col_type;
    col_type.type_id = TypeId::DECIMAL;
    col_type.nullable = false;
    col_type.decimal_scale = column_scale;
    bound.expr_types[col_ptr] = col_type;

    ExprPtr lit =
        (lit_kind == LiteralKind::FLOAT) ? lit_float(literal_text) : lit_int(literal_text);

    ExprPtr expr;
    if (literal_lhs) {
        expr = binary(op, std::move(lit), std::move(col));
    } else {
        expr = binary(op, std::move(col), std::move(lit));
    }
    return evaluate_expr(*expr, tuple, schema, bound);
}

} // namespace

// =============================================================================
// Exact repro from GDB-1287.
// =============================================================================

TEST(ExprEvaluatorDecimalCompare, GreaterThanReproOnlyMatchesHigherValue) {
    // amount = 19.99 (coeff 1999, scale 2) > 10.00 literal -> true.
    auto r =
        eval_decimal_vs_literal(BinaryOp::GREATER, 1999, 2, "10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_FALSE(r->is_null());
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, GreaterThanReproExcludesEqualValue) {
    // amount = 10.00 (coeff 1000, scale 2) > 10.00 literal -> false.
    auto r =
        eval_decimal_vs_literal(BinaryOp::GREATER, 1000, 2, "10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, GreaterThanReproExcludesLowerValue) {
    // amount = 5.00 (coeff 500, scale 2) > 10.00 literal -> false.
    auto r = eval_decimal_vs_literal(BinaryOp::GREATER, 500, 2, "10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, GreaterThanReproRoundedInsertValue) {
    // 9.995 rounds to scale-2 coefficient 1000 (round-half-away-from-zero) on
    // insert, per existing INSERT/fit_to_storage semantics -- so a row
    // "inserted as 9.995" is stored as 10.00 and should NOT be > 10.00.
    auto r =
        eval_decimal_vs_literal(BinaryOp::GREATER, 1000, 2, "10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

// =============================================================================
// Boundary / all comparison operators, DECIMAL column vs FLOAT64 literal.
// =============================================================================

TEST(ExprEvaluatorDecimalCompare, GreaterEqualIncludesExactMatch) {
    auto r = eval_decimal_vs_literal(
        BinaryOp::GREATER_EQUAL, 1000, 2, "10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, GreaterEqualExcludesLowerValue) {
    auto r = eval_decimal_vs_literal(
        BinaryOp::GREATER_EQUAL, 500, 2, "10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, LessThanTrueForLowerValue) {
    auto r = eval_decimal_vs_literal(BinaryOp::LESS, 500, 2, "10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, LessThanFalseForHigherValue) {
    auto r = eval_decimal_vs_literal(BinaryOp::LESS, 1999, 2, "10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, LessEqualIncludesExactMatch) {
    auto r =
        eval_decimal_vs_literal(BinaryOp::LESS_EQUAL, 1000, 2, "10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, LessEqualExcludesHigherValue) {
    auto r =
        eval_decimal_vs_literal(BinaryOp::LESS_EQUAL, 1999, 2, "10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, EqualTrueForExactMatch) {
    auto r = eval_decimal_vs_literal(BinaryOp::EQUAL, 1000, 2, "10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, EqualFalseForDifferentValue) {
    auto r = eval_decimal_vs_literal(BinaryOp::EQUAL, 1999, 2, "10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, NotEqualFalseForExactMatch) {
    auto r =
        eval_decimal_vs_literal(BinaryOp::NOT_EQUAL, 1000, 2, "10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, NotEqualTrueForDifferentValue) {
    auto r =
        eval_decimal_vs_literal(BinaryOp::NOT_EQUAL, 1999, 2, "10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

// =============================================================================
// Literal on the left-hand side: `10.00 < amount` etc.
// =============================================================================

TEST(ExprEvaluatorDecimalCompare, LiteralOnLeftLessThanColumn) {
    // 10.00 < amount(19.99) -> true.
    auto r = eval_decimal_vs_literal(BinaryOp::LESS, 1999, 2, "10.00", LiteralKind::FLOAT, true);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, LiteralOnLeftGreaterThanColumnFalseAtEquality) {
    // 10.00 > amount(10.00) -> false.
    auto r = eval_decimal_vs_literal(BinaryOp::GREATER, 1000, 2, "10.00", LiteralKind::FLOAT, true);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, LiteralOnLeftGreaterThanColumnTrueForLowerColumnValue) {
    // 10.00 > amount(5.00) -> true.
    auto r = eval_decimal_vs_literal(BinaryOp::GREATER, 500, 2, "10.00", LiteralKind::FLOAT, true);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

// =============================================================================
// Fractional literal finer than the column scale (9.995 vs scale-2 column).
// =============================================================================

TEST(ExprEvaluatorDecimalCompare, ColumnBelowFinerFractionalLiteralIsLess) {
    // amount = 9.98 (coeff 998, scale 2) < 9.995 (literal) -> true.
    auto r = eval_decimal_vs_literal(BinaryOp::LESS, 998, 2, "9.995", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, ColumnAboveFinerFractionalLiteralIsGreater) {
    // amount = 10.00 (coeff 1000, scale 2) > 9.995 (literal) -> true.
    auto r =
        eval_decimal_vs_literal(BinaryOp::GREATER, 1000, 2, "9.995", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, ColumnNotEqualToFinerFractionalLiteral) {
    // GDB-1301: amount = 9.99 (coeff 999, scale 2) vs 9.995 (literal).
    //
    // The literal's source text "9.995" is parsed EXACTLY (coefficient 9995
    // at scale 3) via exact_decimal_from_literal_text() -- it is never
    // rounded down to the column's scale 2. 9.99 != 9.995, so EQUAL must be
    // false and NOT_EQUAL must be true. (Prior to the GDB-1301 fix, the
    // literal was rounded to the column's scale first via fit_to_storage(),
    // producing coefficient 999 == the column's 999, wrongly reporting
    // equality.)
    auto eq = eval_decimal_vs_literal(BinaryOp::EQUAL, 999, 2, "9.995", LiteralKind::FLOAT, false);
    ASSERT_TRUE(eq.has_value()) << eq.error().message;
    EXPECT_FALSE(eq->as_bool());

    auto ne =
        eval_decimal_vs_literal(BinaryOp::NOT_EQUAL, 999, 2, "9.995", LiteralKind::FLOAT, false);
    ASSERT_TRUE(ne.has_value()) << ne.error().message;
    EXPECT_TRUE(ne->as_bool());
}

// =============================================================================
// Negative DECIMAL values.
// =============================================================================

TEST(ExprEvaluatorDecimalCompare, NegativeDecimalLessThanZeroLiteral) {
    // amount = -5.00 (coeff -500, scale 2) < 0.00 (literal) -> true.
    auto r = eval_decimal_vs_literal(BinaryOp::LESS, -500, 2, "0.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, NegativeDecimalGreaterThanMoreNegativeLiteral) {
    // amount = -5.00 (coeff -500, scale 2) > -10.00 (literal) -> true.
    auto r =
        eval_decimal_vs_literal(BinaryOp::GREATER, -500, 2, "-10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, NegativeDecimalEqualsNegativeIntLiteral) {
    // amount = -10.00 (coeff -1000, scale 2) == -10 (int literal) -> true.
    auto r = eval_decimal_vs_literal(BinaryOp::EQUAL, -1000, 2, "-10", LiteralKind::INTEGER, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

// =============================================================================
// DECIMAL vs INT literal.
// =============================================================================

TEST(ExprEvaluatorDecimalCompare, DecimalGreaterThanIntLiteralTrue) {
    // amount = 19.99 (coeff 1999, scale 2) > 10 (int literal) -> true.
    auto r = eval_decimal_vs_literal(BinaryOp::GREATER, 1999, 2, "10", LiteralKind::INTEGER, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, DecimalGreaterThanIntLiteralFalse) {
    // amount = 5.00 (coeff 500, scale 2) > 10 (int literal) -> false.
    auto r = eval_decimal_vs_literal(BinaryOp::GREATER, 500, 2, "10", LiteralKind::INTEGER, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, DecimalEqualsIntLiteralExact) {
    // amount = 10.00 (coeff 1000, scale 2) == 10 (int literal) -> true.
    auto r = eval_decimal_vs_literal(BinaryOp::EQUAL, 1000, 2, "10", LiteralKind::INTEGER, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

// =============================================================================
// DECIMAL vs DECIMAL (both sides truly DECIMAL-typed): existing fast path,
// no regression.
// =============================================================================

TEST(ExprEvaluatorDecimalCompare, DecimalVsDecimalSameScaleGreaterThan) {
    auto schema = make_schema({{"a", TypeId::DECIMAL}, {"b", TypeId::DECIMAL}});
    auto tuple = make_tuple({Value(d128(1999)), Value(d128(1000))});
    auto bound = empty_bound();

    ExprPtr a = col_ref("a");
    ExprPtr b = col_ref("b");
    const Expr* a_ptr = a.get();
    const Expr* b_ptr = b.get();
    ExprType a_type;
    a_type.type_id = TypeId::DECIMAL;
    a_type.decimal_scale = 2;
    bound.expr_types[a_ptr] = a_type;
    ExprType b_type;
    b_type.type_id = TypeId::DECIMAL;
    b_type.decimal_scale = 2;
    bound.expr_types[b_ptr] = b_type;

    auto expr = binary(BinaryOp::GREATER, std::move(a), std::move(b));
    auto r = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, DecimalVsDecimalDifferentScaleEqual) {
    // a = 10.00 at scale 2 (coeff 1000); b = 10.000 at scale 3 (coeff 10000).
    // These represent the same value and must compare equal.
    auto schema = make_schema({{"a", TypeId::DECIMAL}, {"b", TypeId::DECIMAL}});
    auto tuple = make_tuple({Value(d128(1000)), Value(d128(10000))});
    auto bound = empty_bound();

    ExprPtr a = col_ref("a");
    ExprPtr b = col_ref("b");
    const Expr* a_ptr = a.get();
    const Expr* b_ptr = b.get();
    ExprType a_type;
    a_type.type_id = TypeId::DECIMAL;
    a_type.decimal_scale = 2;
    bound.expr_types[a_ptr] = a_type;
    ExprType b_type;
    b_type.type_id = TypeId::DECIMAL;
    b_type.decimal_scale = 3;
    bound.expr_types[b_ptr] = b_type;

    auto expr = binary(BinaryOp::EQUAL, std::move(a), std::move(b));
    auto r = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

// =============================================================================
// GDB-1301: comparisons must NOT round a finer-precision literal down to the
// DECIMAL column's scale. Instead both sides are compared at the MAX of
// their true scales, by scaling up the coarser side (lossless), never
// scaling down (rounding) the finer side.
// =============================================================================

TEST(ExprEvaluatorDecimalCompare, FinerLiteralLessThanEqualColumnIsTrue) {
    // DECIMAL(10,2) column = 10.00 (coeff 1000, scale 2); literal = 10.001
    // (scale 3, coeff 10001 exactly). 10.00 < 10.001 -> true.
    // Pre-GDB-1301: 10.001 was rounded to the column's scale 2 as 10.00,
    // making this wrongly compare equal (false for LESS).
    auto r = eval_decimal_vs_literal(BinaryOp::LESS, 1000, 2, "10.001", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, FinerLiteralEqualsColumnIsFalse) {
    // 10.00 == 10.001 must be false (they are NOT the same value).
    // Pre-GDB-1301: rounding 10.001 down to scale 2 produced 10.00, wrongly
    // reporting equality -- this is the exact GDB-1301 bug.
    auto r = eval_decimal_vs_literal(BinaryOp::EQUAL, 1000, 2, "10.001", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, FinerLiteralGreaterThanColumnIsFalse) {
    // 10.00 > 10.001 must be false.
    auto r =
        eval_decimal_vs_literal(BinaryOp::GREATER, 1000, 2, "10.001", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, FinerLiteralLessEqualColumnIsTrue) {
    // 10.00 <= 10.001 must be true.
    auto r =
        eval_decimal_vs_literal(BinaryOp::LESS_EQUAL, 1000, 2, "10.001", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, FinerLiteralGreaterEqualColumnIsFalse) {
    // 10.00 >= 10.001 must be false.
    auto r = eval_decimal_vs_literal(
        BinaryOp::GREATER_EQUAL, 1000, 2, "10.001", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, FinerLiteralNotEqualColumnIsTrue) {
    // 10.00 != 10.001 must be true.
    auto r =
        eval_decimal_vs_literal(BinaryOp::NOT_EQUAL, 1000, 2, "10.001", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, FinerLiteralOnLeftGreaterThanColumnIsFalse) {
    // 10.001 < amount(10.00) -- literal on the left: `10.001 < amount` is
    // false because 10.001 is actually greater than 10.00.
    auto r = eval_decimal_vs_literal(BinaryOp::LESS, 1000, 2, "10.001", LiteralKind::FLOAT, true);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, FinerLiteralOnLeftGreaterThanColumnIsTrue) {
    // 10.001 > amount(10.00) -- literal on the left: true.
    auto r =
        eval_decimal_vs_literal(BinaryOp::GREATER, 1000, 2, "10.001", LiteralKind::FLOAT, true);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, FinerLiteralNineNineNineFiveExactlyGreaterThanColumn) {
    // GDB-1301's own repro case: DECIMAL(10,2) column = 9.99 (coeff 999,
    // scale 2); literal = 9.995 (exact scale 3, coeff 9995). 9.99 < 9.995 is
    // true; 9.99 == 9.995 is false. This is the corrected behavior for the
    // exact same "9.995" literal used in GDB-1287's own (now-updated) test
    // above -- exact literal parsing means it is compared as truly 9.995,
    // not silently coerced to the column's 2-digit scale.
    auto lt = eval_decimal_vs_literal(BinaryOp::LESS, 999, 2, "9.995", LiteralKind::FLOAT, false);
    ASSERT_TRUE(lt.has_value()) << lt.error().message;
    EXPECT_TRUE(lt->as_bool());

    auto eq = eval_decimal_vs_literal(BinaryOp::EQUAL, 999, 2, "9.995", LiteralKind::FLOAT, false);
    ASSERT_TRUE(eq.has_value()) << eq.error().message;
    EXPECT_FALSE(eq->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, FinerLiteralScaleUpOverflowReturnsCleanError) {
    // A DECIMAL column holding a coefficient near Decimal128's max at scale
    // 2, compared against a literal with one extra fractional digit (scale
    // 3). Aligning to the common scale 3 requires multiplying the column's
    // coefficient by 10, which overflows Decimal128 at this magnitude. This
    // must return a clean TYPE_ERROR (via dec128_mul_pow10 -> decimal_compare
    // propagation), never a silently wrong comparison result.
    auto schema = make_schema({{"amount", TypeId::DECIMAL}});
    Decimal128 near_max{INT64_MAX, 0xFFFFFFFFFFFFFFFFULL};
    auto tuple = make_tuple({Value(near_max)});
    auto bound = empty_bound();

    ExprPtr col = col_ref("amount");
    const Expr* col_ptr = col.get();
    ExprType col_type;
    col_type.type_id = TypeId::DECIMAL;
    col_type.decimal_scale = 2;
    bound.expr_types[col_ptr] = col_type;

    ExprPtr lit = lit_float("1.001");
    auto expr = binary(BinaryOp::GREATER, std::move(col), std::move(lit));
    auto r = evaluate_expr(*expr, tuple, schema, bound);

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

// =============================================================================
// GDB-1301 follow-up: negative finer-precision literals must be exact too.
//
// A negative literal like `-10.001` does NOT reach eval_binary() as a bare
// LiteralExpr -- it is parsed as UnaryExpr{NEGATE, LiteralExpr("10.001")}.
// The real planner path (Planner::fold_predicate_for_filter, invoked for
// every WHERE/HAVING predicate) constant-folds that UnaryExpr into a single
// LiteralExpr via fold_constants() BEFORE eval_binary() ever runs. If that
// fold negates via a float round-trip (std::to_string(-safe_stod(text))),
// the folded literal's text is already lossy ("-10.000999999999999...")
// by the time eval_binary()'s exact-literal-parsing fast path sees it --
// reintroducing the exact rounding bug GDB-1301 fixed, just for negative
// literals. fold_first=true below exercises precisely that real path.
// =============================================================================

TEST(ExprEvaluatorDecimalCompare, NegativeFinerLiteralLessThanColumnFoldedPath) {
    // DECIMAL(10,2) column = -10.00 (coeff -1000); literal = -10.001 (exact
    // scale 3, coeff -10001). -10.001 < -10.00 is TRUE (more negative is
    // smaller). Constructed via the REAL UnaryExpr{NEGATE, LiteralExpr}
    // shape, then folded via fold_constants() exactly as the planner does
    // for a WHERE clause.
    auto r = eval_decimal_vs_negative_literal(
        BinaryOp::LESS, -1000, 2, "10.001", LiteralKind::FLOAT, true, /*fold_first=*/true);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, NegativeFinerLiteralGreaterThanColumnFoldedPath) {
    // amount(-10.00) > -10.001 is TRUE (-10.00 is greater than -10.001).
    auto r = eval_decimal_vs_negative_literal(
        BinaryOp::GREATER, -1000, 2, "10.001", LiteralKind::FLOAT, false, /*fold_first=*/true);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, NegativeFinerLiteralEqualsColumnIsFalseFoldedPath) {
    // amount(-10.00) == -10.001 must be FALSE (they are not the same value).
    // This is the exact GDB-1301-for-negatives bug: if the fold rounds
    // -10.001 to -10.00 via a float round-trip, this wrongly reports true.
    auto r = eval_decimal_vs_negative_literal(
        BinaryOp::EQUAL, -1000, 2, "10.001", LiteralKind::FLOAT, false, /*fold_first=*/true);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, NegativeFinerLiteralLessEqualColumnIsTrueFoldedPath) {
    // amount(-10.00) <= -10.001 must be FALSE: -10.00 > -10.001.
    auto r = eval_decimal_vs_negative_literal(
        BinaryOp::LESS_EQUAL, -1000, 2, "10.001", LiteralKind::FLOAT, false, /*fold_first=*/true);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, NegativeFinerLiteralGreaterEqualColumnIsTrueFoldedPath) {
    // amount(-10.00) >= -10.001 must be TRUE.
    auto r = eval_decimal_vs_negative_literal(BinaryOp::GREATER_EQUAL,
                                              -1000,
                                              2,
                                              "10.001",
                                              LiteralKind::FLOAT,
                                              false,
                                              /*fold_first=*/true);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, NegativeFinerLiteralNotEqualColumnIsTrueFoldedPath) {
    // amount(-10.00) != -10.001 must be TRUE.
    auto r = eval_decimal_vs_negative_literal(
        BinaryOp::NOT_EQUAL, -1000, 2, "10.001", LiteralKind::FLOAT, false, /*fold_first=*/true);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, NegativeFinerLiteralOnLeftLessThanColumnIsTrueFoldedPath) {
    // -10.001 < amount(-10.00): literal on the left, folded path.
    auto r = eval_decimal_vs_negative_literal(
        BinaryOp::LESS, -1000, 2, "10.001", LiteralKind::FLOAT, true, /*fold_first=*/true);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, NegativeFinerLiteralOnLeftGreaterThanColumnIsFalseFoldedPath) {
    // -10.001 > amount(-10.00) must be FALSE.
    auto r = eval_decimal_vs_negative_literal(
        BinaryOp::GREATER, -1000, 2, "10.001", LiteralKind::FLOAT, true, /*fold_first=*/true);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, NegativeFinerLiteralIntOperandFoldedPath) {
    // amount(-10.00) vs negative INT literal -10: DECIMAL vs negative INT,
    // through the real folded UnaryExpr{NEGATE, LiteralExpr(INTEGER)} path.
    auto r = eval_decimal_vs_negative_literal(
        BinaryOp::EQUAL, -1000, 2, "10", LiteralKind::INTEGER, false, /*fold_first=*/true);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

// -----------------------------------------------------------------------------
// Same cases again WITHOUT constant folding (fold_first=false), exercising
// eval_binary()'s own UnaryExpr-unwrapping defense-in-depth directly for the
// (believed unreachable, but not exhaustively proven for every call site)
// case where a negative literal is not folded before reaching eval.
// -----------------------------------------------------------------------------

TEST(ExprEvaluatorDecimalCompare, NegativeFinerLiteralEqualsColumnIsFalseUnfoldedPath) {
    auto r = eval_decimal_vs_negative_literal(
        BinaryOp::EQUAL, -1000, 2, "10.001", LiteralKind::FLOAT, false, /*fold_first=*/false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, NegativeFinerLiteralLessThanColumnUnfoldedPath) {
    auto r = eval_decimal_vs_negative_literal(
        BinaryOp::LESS, -1000, 2, "10.001", LiteralKind::FLOAT, true, /*fold_first=*/false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(ExprEvaluatorDecimalCompare, NegativeFinerLiteralGreaterThanColumnUnfoldedPath) {
    auto r = eval_decimal_vs_negative_literal(
        BinaryOp::GREATER, -1000, 2, "10.001", LiteralKind::FLOAT, false, /*fold_first=*/false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

// -----------------------------------------------------------------------------
// Directly verify the upstream fold itself produces an exact literal (the
// mechanism this fix relies on): fold_constants(NEGATE(LiteralExpr("10.001")))
// must yield LiteralExpr("-10.001") exactly, not a float round-trip.
// -----------------------------------------------------------------------------

TEST(ExprEvaluatorDecimalCompare, FoldConstantsNegatesFloatLiteralTextExactly) {
    ExprPtr neg = negate(lit_float("10.001"));
    auto folded = fold_constants(*neg);
    ASSERT_NE(folded, nullptr);
    const auto* lit = dynamic_cast<const LiteralExpr*>(folded.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->kind, LiteralKind::FLOAT);
    EXPECT_EQ(lit->value, "-10.001")
        << "fold_constants must negate literal TEXT exactly, not round-trip through a double";
}

TEST(ExprEvaluatorDecimalCompare, FoldConstantsNegatesNineNineNineFiveExactly) {
    ExprPtr neg = negate(lit_float("9.995"));
    auto folded = fold_constants(*neg);
    ASSERT_NE(folded, nullptr);
    const auto* lit = dynamic_cast<const LiteralExpr*>(folded.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->value, "-9.995");
}
