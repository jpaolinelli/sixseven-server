// GDB-1287: DECIMAL column vs bare numeric literal comparison correctness.
//
// Root cause (pre-fix): the comparison fast path in expr_evaluator.cpp only
// fired when BOTH sides were already DECIMAL-typed. Numeric literals bind as
// FLOAT64 (Binder::bind_literal), so `amount > 10.00` fell through to the
// generic cross-type compare() path, which converts DECIMAL to double via the
// *unscaled* coefficient -- silently comparing the wrong magnitude and
// corrupting every comparison operator (=, !=, <, <=, >, >=).
//
// Fix: the fast path now fires whenever at least one side is DECIMAL and the
// other is any numeric type, promoting the non-decimal side to Decimal128 at
// the DECIMAL side's scale via fit_to_storage() (round-half-away-from-zero),
// then comparing via decimal_compare().
//
// This file adversarially probes eval_binary() directly (the exact code path
// GDB-1287 modified), mirroring the harness pattern established in
// tests/unit/test_expr_evaluator_decimal_compare.cpp. This harness was chosen
// over a full QueryEngine/SQL-path test because the shared local build
// environment's vcpkg-managed ICU package is broken in this worktree
// (icu4c install-sh relative-path failure in the msys2 make install step,
// unrelated to any GDB-1287 source change -- see QA report) which blocks
// linking sixseven_qa_tests end-to-end. eval_binary() is the exact function
// GDB-1287 touched, and this harness requires no ICU/catalog/QueryEngine
// dependency, so it still exercises the real fix under adversarial inputs.
//
// Probes:
//   1. The exact repro + all six comparison operators, both operand orders.
//   2. Rounding-before-compare correctness at the boundary (9.995, 10.001,
//      10.005 against a DECIMAL(10,2) column) -- verifying whether the fix
//      introduces a new wrong-result bug via premature rounding of the
//      literal to the column's scale.
//   3. DECIMAL vs INT literal, negative decimals, zero.
//   4. Large-scale/precision DECIMAL(38,x) and overflow handling.
//   5. NULL DECIMAL vs literal (three-valued logic, row excluded).
//   6. Regression: DECIMAL vs DECIMAL, FLOAT vs FLOAT, INT vs INT unaffected.
//   7. The documented-but-unfixed FLOAT64 literal-binding IEEE-754 error.

#include "sixseven/common/decimal_math.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <gtest/gtest.h>

#include <cstdint>
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

static BoundStatement empty_bound() {
    return BoundStatement{};
}

/// Evaluate `amount <op> literal` (or `literal <op> amount` if `literal_lhs`)
/// where `amount` is a DECIMAL(precision, scale) column holding `coeff`, and
/// `literal` is the raw text of a numeric literal of kind `lit_kind`.
/// `is_null_col`, if true, makes the column value NULL instead of `coeff`.
Result<Value> eval_decimal_vs_literal(BinaryOp op,
                                      int64_t coeff,
                                      int32_t column_scale,
                                      const std::string& literal_text,
                                      LiteralKind lit_kind,
                                      bool literal_lhs,
                                      bool is_null_col = false) {
    auto schema = make_schema({{"amount", TypeId::DECIMAL}});
    Value col_val = is_null_col ? Value::make_null() : Value(d128(coeff));
    auto tuple = make_tuple({col_val});
    auto bound = empty_bound();

    ExprPtr col = col_ref("amount");
    const Expr* col_ptr = col.get();
    ExprType col_type;
    col_type.type_id = TypeId::DECIMAL;
    col_type.nullable = true;
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

/// Evaluate `a <op> b` where both are DECIMAL columns at (possibly different)
/// scales, holding the given coefficients.
Result<Value> eval_decimal_vs_decimal(BinaryOp op,
                                      int64_t coeff_a,
                                      int32_t scale_a,
                                      int64_t coeff_b,
                                      int32_t scale_b) {
    auto schema = make_schema({{"a", TypeId::DECIMAL}, {"b", TypeId::DECIMAL}});
    auto tuple = make_tuple({Value(d128(coeff_a)), Value(d128(coeff_b))});
    auto bound = empty_bound();

    ExprPtr a = col_ref("a");
    ExprPtr b = col_ref("b");
    const Expr* a_ptr = a.get();
    const Expr* b_ptr = b.get();
    ExprType a_type;
    a_type.type_id = TypeId::DECIMAL;
    a_type.nullable = false;
    a_type.decimal_scale = scale_a;
    bound.expr_types[a_ptr] = a_type;
    ExprType b_type;
    b_type.type_id = TypeId::DECIMAL;
    b_type.nullable = false;
    b_type.decimal_scale = scale_b;
    bound.expr_types[b_ptr] = b_type;

    auto expr = binary(op, std::move(a), std::move(b));
    return evaluate_expr(*expr, tuple, schema, bound);
}

/// Evaluate `a <op> b` for two plain FLOAT64 (or INT) values -- regression
/// check that the DECIMAL fast path does not interfere with non-DECIMAL
/// comparisons.
Result<Value> eval_float_vs_float(BinaryOp op, double a, double b) {
    auto schema = make_schema({{"a", TypeId::FLOAT64}, {"b", TypeId::FLOAT64}});
    auto tuple = make_tuple({Value(a), Value(b)});
    auto bound = empty_bound();
    auto expr = binary(op, col_ref("a"), col_ref("b"));
    return evaluate_expr(*expr, tuple, schema, bound);
}

Result<Value> eval_int_vs_int(BinaryOp op, int32_t a, int32_t b) {
    auto schema = make_schema({{"a", TypeId::INT32}, {"b", TypeId::INT32}});
    auto tuple = make_tuple({Value(a), Value(b)});
    auto bound = empty_bound();
    auto expr = binary(op, col_ref("a"), col_ref("b"));
    return evaluate_expr(*expr, tuple, schema, bound);
}

} // namespace

// =============================================================================
// 1. Exact repro + all six operators, both operand orders.
// =============================================================================

TEST(QA_GDB1287, ExactReproOnlyHigherValueMatches) {
    // amount=5.00 > 10.00 -> false; amount=10.00 > 10.00 -> false;
    // amount=15.00 > 10.00 -> true. Pre-fix bug matched ALL rows regardless
    // of magnitude because the DECIMAL was compared via its *unscaled*
    // coefficient against the double literal.
    auto r5 = eval_decimal_vs_literal(BinaryOp::GREATER, 500, 2, "10.00", LiteralKind::FLOAT, false);
    auto r10 =
        eval_decimal_vs_literal(BinaryOp::GREATER, 1000, 2, "10.00", LiteralKind::FLOAT, false);
    auto r15 =
        eval_decimal_vs_literal(BinaryOp::GREATER, 1500, 2, "10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r5.has_value()) << r5.error().message;
    ASSERT_TRUE(r10.has_value()) << r10.error().message;
    ASSERT_TRUE(r15.has_value()) << r15.error().message;
    EXPECT_FALSE(r5->as_bool());
    EXPECT_FALSE(r10->as_bool());
    EXPECT_TRUE(r15->as_bool());
}

TEST(QA_GDB1287, AllSixOperatorsColumnLiteralOrder) {
    // amount = 10.00 (coeff 1000, scale 2) vs literal 10.00.
    auto eq = eval_decimal_vs_literal(BinaryOp::EQUAL, 1000, 2, "10.00", LiteralKind::FLOAT, false);
    auto ne =
        eval_decimal_vs_literal(BinaryOp::NOT_EQUAL, 1000, 2, "10.00", LiteralKind::FLOAT, false);
    auto lt = eval_decimal_vs_literal(BinaryOp::LESS, 1000, 2, "10.00", LiteralKind::FLOAT, false);
    auto le =
        eval_decimal_vs_literal(BinaryOp::LESS_EQUAL, 1000, 2, "10.00", LiteralKind::FLOAT, false);
    auto gt = eval_decimal_vs_literal(BinaryOp::GREATER, 1000, 2, "10.00", LiteralKind::FLOAT, false);
    auto ge = eval_decimal_vs_literal(
        BinaryOp::GREATER_EQUAL, 1000, 2, "10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(eq.has_value()) << eq.error().message;
    ASSERT_TRUE(ne.has_value()) << ne.error().message;
    ASSERT_TRUE(lt.has_value()) << lt.error().message;
    ASSERT_TRUE(le.has_value()) << le.error().message;
    ASSERT_TRUE(gt.has_value()) << gt.error().message;
    ASSERT_TRUE(ge.has_value()) << ge.error().message;
    EXPECT_TRUE(eq->as_bool());
    EXPECT_FALSE(ne->as_bool());
    EXPECT_FALSE(lt->as_bool());
    EXPECT_TRUE(le->as_bool());
    EXPECT_FALSE(gt->as_bool());
    EXPECT_TRUE(ge->as_bool());
}

TEST(QA_GDB1287, AllSixOperatorsLiteralColumnOrder) {
    // Reversed operand order: `10.00 <op> amount`. amount=15.00 (coeff 1500).
    auto eq = eval_decimal_vs_literal(BinaryOp::EQUAL, 1500, 2, "10.00", LiteralKind::FLOAT, true);
    auto ne =
        eval_decimal_vs_literal(BinaryOp::NOT_EQUAL, 1500, 2, "10.00", LiteralKind::FLOAT, true);
    // 10.00 < amount(15.00) -> true.
    auto lt = eval_decimal_vs_literal(BinaryOp::LESS, 1500, 2, "10.00", LiteralKind::FLOAT, true);
    // 10.00 > amount(15.00) -> false.
    auto gt = eval_decimal_vs_literal(BinaryOp::GREATER, 1500, 2, "10.00", LiteralKind::FLOAT, true);
    ASSERT_TRUE(eq.has_value()) << eq.error().message;
    ASSERT_TRUE(ne.has_value()) << ne.error().message;
    ASSERT_TRUE(lt.has_value()) << lt.error().message;
    ASSERT_TRUE(gt.has_value()) << gt.error().message;
    EXPECT_FALSE(eq->as_bool());
    EXPECT_TRUE(ne->as_bool());
    EXPECT_TRUE(lt->as_bool());
    EXPECT_FALSE(gt->as_bool());
}

// =============================================================================
// 2. THE SUBTLE ONE: finer-precision literal vs column scale (rounding).
// =============================================================================

// DECIMAL(10,2) column value 10.00 (coeff 1000) vs literal 9.995. If
// fit_to_storage rounds 9.995 to scale 2 using round-half-away-from-zero:
// 9.995 * 100 = 999.5 -> llround -> 1000 -> represents 10.00. So
// amount(10.00) > 9.995-rounded-to-10.00 evaluates FALSE even though the true
// mathematical comparison (10.00 > 9.995) is TRUE. This documents a genuine
// correctness gap introduced by rounding the literal to the column's scale
// *before* comparing, rather than comparing at full precision.
TEST(QA_GDB1287, FinerPrecisionLiteral_9995_RoundsUpToColumnScale) {
    auto r_gt =
        eval_decimal_vs_literal(BinaryOp::GREATER, 1000, 2, "9.995", LiteralKind::FLOAT, false);
    auto r_lt = eval_decimal_vs_literal(BinaryOp::LESS, 1000, 2, "9.995", LiteralKind::FLOAT, false);
    auto r_eq =
        eval_decimal_vs_literal(BinaryOp::EQUAL, 1000, 2, "9.995", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r_gt.has_value()) << r_gt.error().message;
    ASSERT_TRUE(r_lt.has_value()) << r_lt.error().message;
    ASSERT_TRUE(r_eq.has_value()) << r_eq.error().message;

    // Mathematically, 10.00 > 9.995 is TRUE. If the implementation rounds the
    // literal to scale 2 BEFORE comparing (9.995 -> 10.00), this assertion
    // will FAIL, revealing a wrong-result bug worth filing as High severity.
    EXPECT_TRUE(r_gt->as_bool())
        << "amount(10.00) > 9.995 should be TRUE mathematically -- if this "
           "fails, the literal was rounded to 10.00 at the column's scale "
           "before comparison, producing a false negative.";
    EXPECT_FALSE(r_lt->as_bool()) << "amount(10.00) < 9.995 should be FALSE.";
    EXPECT_FALSE(r_eq->as_bool())
        << "amount(10.00) = 9.995 should be FALSE (10.00 != 9.995); if the "
           "literal rounds to 10.00 this wrongly reports equality.";
}

// 10.001 vs column value 10.00 (coeff 1000): 10.001 rounds to scale 2 via
// round-half-away-from-zero on 1000.1 -> 1000 -> represents 10.00. True
// comparison: 10.00 < 10.001 (true). If the literal rounds down to 10.00,
// `amount < 10.001` wrongly becomes `amount < 10.00` (false).
TEST(QA_GDB1287, FinerPrecisionLiteral_10_001_RoundsDownToColumnScale) {
    auto r_lt =
        eval_decimal_vs_literal(BinaryOp::LESS, 1000, 2, "10.001", LiteralKind::FLOAT, false);
    auto r_eq =
        eval_decimal_vs_literal(BinaryOp::EQUAL, 1000, 2, "10.001", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r_lt.has_value()) << r_lt.error().message;
    ASSERT_TRUE(r_eq.has_value()) << r_eq.error().message;

    EXPECT_TRUE(r_lt->as_bool())
        << "amount(10.00) < 10.001 should be TRUE mathematically -- if this "
           "fails, literal 10.001 was rounded to 10.00 before comparison.";
    EXPECT_FALSE(r_eq->as_bool())
        << "amount(10.00) = 10.001 should be FALSE; a rounded-to-10.00 "
           "literal would wrongly match.";
}

// 10.005 is the round-half boundary at scale 2 (round-half-away-from-zero ->
// 10.01). True comparison: 10.00 < 10.005 (true).
TEST(QA_GDB1287, FinerPrecisionLiteral_10_005_HalfwayBoundary) {
    auto r_lt =
        eval_decimal_vs_literal(BinaryOp::LESS, 1000, 2, "10.005", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r_lt.has_value()) << r_lt.error().message;
    EXPECT_TRUE(r_lt->as_bool()) << "amount(10.00) < 10.005 should be TRUE mathematically.";
}

// =============================================================================
// 3. DECIMAL vs INT literal; negative decimals; zero.
// =============================================================================

TEST(QA_GDB1287, DecimalVsIntLiteral) {
    // amount = 15.00 (coeff 1500, scale 2) > 10 (INT literal) -> true.
    auto r_gt = eval_decimal_vs_literal(BinaryOp::GREATER, 1500, 2, "10", LiteralKind::INTEGER, false);
    // amount = 10.00 (coeff 1000, scale 2) = 10 (INT literal) -> true.
    auto r_eq = eval_decimal_vs_literal(BinaryOp::EQUAL, 1000, 2, "10", LiteralKind::INTEGER, false);
    ASSERT_TRUE(r_gt.has_value()) << r_gt.error().message;
    ASSERT_TRUE(r_eq.has_value()) << r_eq.error().message;
    EXPECT_TRUE(r_gt->as_bool());
    EXPECT_TRUE(r_eq->as_bool());
}

TEST(QA_GDB1287, NegativeDecimalsCompareCorrectly) {
    // amount = -10.00 (coeff -1000, scale 2) < -5.00 -> true.
    auto r_lt =
        eval_decimal_vs_literal(BinaryOp::LESS, -1000, 2, "-5.00", LiteralKind::FLOAT, false);
    // amount = -5.00 (coeff -500) > -10.00 -> true.
    auto r_gt =
        eval_decimal_vs_literal(BinaryOp::GREATER, -500, 2, "-10.00", LiteralKind::FLOAT, false);
    // amount = -10.00 (coeff -1000) >= -10.00 -> true.
    auto r_ge =
        eval_decimal_vs_literal(BinaryOp::GREATER_EQUAL, -1000, 2, "-10.00", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r_lt.has_value()) << r_lt.error().message;
    ASSERT_TRUE(r_gt.has_value()) << r_gt.error().message;
    ASSERT_TRUE(r_ge.has_value()) << r_ge.error().message;
    EXPECT_TRUE(r_lt->as_bool());
    EXPECT_TRUE(r_gt->as_bool());
    EXPECT_TRUE(r_ge->as_bool());
}

TEST(QA_GDB1287, ZeroDecimalVsZeroLiteral) {
    // amount = 0.00 (coeff 0) = 0 -> true.
    auto r_eq = eval_decimal_vs_literal(BinaryOp::EQUAL, 0, 2, "0", LiteralKind::INTEGER, false);
    // amount = 0.01 (coeff 1) > 0 -> true.
    auto r_gt = eval_decimal_vs_literal(BinaryOp::GREATER, 1, 2, "0", LiteralKind::INTEGER, false);
    // amount = -0.01 (coeff -1) < 0 -> true.
    auto r_lt = eval_decimal_vs_literal(BinaryOp::LESS, -1, 2, "0", LiteralKind::INTEGER, false);
    ASSERT_TRUE(r_eq.has_value()) << r_eq.error().message;
    ASSERT_TRUE(r_gt.has_value()) << r_gt.error().message;
    ASSERT_TRUE(r_lt.has_value()) << r_lt.error().message;
    EXPECT_TRUE(r_eq->as_bool());
    EXPECT_TRUE(r_gt->as_bool());
    EXPECT_TRUE(r_lt->as_bool());
}

// =============================================================================
// 4. Large scale/precision DECIMAL(38,x); overflow.
// =============================================================================

TEST(QA_GDB1287, LargeScalePrecisionDecimal38) {
    // DECIMAL(38,10): amount = 1.0000000001 (coeff 10000000001) > 1.0 -> true.
    auto r = eval_decimal_vs_literal(BinaryOp::GREATER, 10000000001LL, 10, "1.0", LiteralKind::FLOAT,
                                     false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

// A literal with more magnitude than fits comfortably: fit_to_storage must
// either error cleanly or produce a well-defined coefficient -- must NOT
// silently wrap/truncate to a wrong small value that produces a bogus match.
TEST(QA_GDB1287, OverflowLiteralExceedingScaleDoesNotSilentlyWrapSmall) {
    // amount = 100.00 (coeff 10000, scale 2) compared against a huge literal
    // that vastly exceeds any DECIMAL(5,2) representable range.
    auto r = eval_decimal_vs_literal(BinaryOp::GREATER, 10000, 2, "100000000000000000000.0",
                                     LiteralKind::FLOAT, false);
    if (r.has_value()) {
        // If it "succeeds", 100.00 must NOT be reported greater than 1e20.
        EXPECT_FALSE(r->as_bool()) << "amount(100.00) should never be > 1e20; silent "
                                       "overflow wraparound would falsely report otherwise.";
    }
    // A clean StatusCode::TYPE_ERROR is also an acceptable outcome (see
    // fit_to_storage's overflow guard in coercion.cpp); only a wrongly-TRUE
    // result here would indicate silent-wrong-result overflow.
}

// =============================================================================
// 5. NULL DECIMAL vs literal.
// =============================================================================

TEST(QA_GDB1287, NullDecimalVsLiteralIsUnknownForAllOperators) {
    // NULL amount compared against 10.00 via all six operators must yield
    // NULL (three-valued logic: unknown), never TRUE.
    for (BinaryOp op : {BinaryOp::EQUAL, BinaryOp::NOT_EQUAL, BinaryOp::LESS, BinaryOp::LESS_EQUAL,
                       BinaryOp::GREATER, BinaryOp::GREATER_EQUAL}) {
        auto r = eval_decimal_vs_literal(op, 0, 2, "10.00", LiteralKind::FLOAT, false,
                                         /*is_null_col=*/true);
        ASSERT_TRUE(r.has_value()) << r.error().message;
        EXPECT_TRUE(r->is_null()) << "NULL amount compared via operator " << static_cast<int>(op)
                                   << " must yield NULL/unknown, not a boolean.";
    }
}

// =============================================================================
// 6. Regression: DECIMAL-vs-DECIMAL, FLOAT-vs-FLOAT, INT-vs-INT unaffected.
// =============================================================================

TEST(QA_GDB1287, DecimalVsDecimalDifferentScalesStillCorrect) {
    // a = 10.00 (scale 2), b = 10.0000 (scale 4) -> equal.
    auto r_eq = eval_decimal_vs_decimal(BinaryOp::EQUAL, 1000, 2, 100000, 4);
    // a = 10.00 (scale 2), b = 10.0001 (scale 4) -> a < b.
    auto r_lt = eval_decimal_vs_decimal(BinaryOp::LESS, 1000, 2, 100001, 4);
    ASSERT_TRUE(r_eq.has_value()) << r_eq.error().message;
    ASSERT_TRUE(r_lt.has_value()) << r_lt.error().message;
    EXPECT_TRUE(r_eq->as_bool());
    EXPECT_TRUE(r_lt->as_bool());
}

TEST(QA_GDB1287, FloatVsFloatUnaffected) {
    auto r = eval_float_vs_float(BinaryOp::GREATER, 10.5, 10.0);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

TEST(QA_GDB1287, IntVsIntUnaffected) {
    auto r = eval_int_vs_int(BinaryOp::GREATER, 15, 10);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool());
}

// =============================================================================
// 7. Documented-but-unfixed: FLOAT64 literal-binding IEEE-754 error.
// =============================================================================

// 0.3 cannot be represented exactly in FLOAT64. bind_literal binds the
// literal's TEXT as FLOAT64 before fit_to_storage ever sees it, inheriting
// IEEE-754 representation error. At scale 4, llround(0.3-as-double * 10000)
// should still land on exactly 3000 in nearly all realistic cases (the
// representation error is far below the rounding threshold at this scale),
// but this test documents actual behavior rather than assuming it.
TEST(QA_GDB1287, DocumentedFloat64BindingIEEEErrorProbe) {
    // amount = 0.3000 (coeff 3000, scale 4) = 0.3 literal.
    auto r = eval_decimal_vs_literal(BinaryOp::EQUAL, 3000, 4, "0.3", LiteralKind::FLOAT, false);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->as_bool())
        << "amount(0.3000) = 0.3 literal should match at scale 4 despite FLOAT64 "
           "binding of the literal -- if this fails, the documented IEEE-754 "
           "binding error (bind_literal binding decimal literal TEXT as FLOAT64) "
           "is user-visible here and should be escalated to High severity.";
}
