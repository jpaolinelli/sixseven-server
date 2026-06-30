// GDB-1049: Integer division returned FLOAT64 (SELECT 7/2 = 3.5), diverging from
// PostgreSQL, which defines integer / integer = integer truncated toward zero.
//
// Root cause: eval_arithmetic()'s integer fast path explicitly excluded DIVIDE
// (`both_integer(lhs, rhs) && op != BinaryOp::DIVIDE`), so int/int division fell
// through to the float path and produced a FLOAT64. The fix lets DIVIDE stay in the
// int64 path with C++ truncate-toward-zero semantics, preserving the existing
// division-by-zero error and rejecting the INT64_MIN / -1 overflow.
//
// MUTATION GUARD: every value assertion below reads as_int64(); under the old
// float-routing these expressions returned a FLOAT64 (e.g. 7/2 -> 3.5), so the
// result would not be an integer 3 and the assertions fail.

#include "sixseven/common/value.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

using namespace sixseven;

namespace {

ExprPtr lit_int(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::INTEGER;
    e->value = v;
    return e;
}

ExprPtr binary(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
    auto e = std::make_unique<BinaryExpr>();
    e->op = op;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    return e;
}

Result<Value> eval_div(const std::string& a, const std::string& b) {
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};
    BoundStatement bound{};
    auto expr = binary(BinaryOp::DIVIDE, lit_int(a), lit_int(b));
    return evaluate_expr(*expr, tuple, schema, bound);
}

} // namespace

TEST(QA_GDB1049, SevenDivTwoIsIntegerThree) {
    auto r = eval_div("7", "2");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->type_id(), TypeId::INT64);
    EXPECT_EQ(r->as_int64(), 3);
}

TEST(QA_GDB1049, NegativeDividendTruncatesTowardZeroNotFloor) {
    // -7 / 2 == -3 (truncate toward zero), NOT -4 (floor).
    auto r = eval_div("-7", "2");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int64(), -3);
}

TEST(QA_GDB1049, NegativeDivisorTruncatesTowardZero) {
    // 7 / -2 == -3 (truncate toward zero).
    auto r = eval_div("7", "-2");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int64(), -3);
}

TEST(QA_GDB1049, ExactIntegerDivision) {
    auto r = eval_div("6", "2");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int64(), 3);
}

TEST(QA_GDB1049, ZeroDividend) {
    auto r = eval_div("0", "5");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int64(), 0);
}

TEST(QA_GDB1049, DivisionByZeroStillErrors) {
    auto r = eval_div("5", "0");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB1049, Int64MinDivByMinusOneOverflowErrors) {
    auto r = eval_div("-9223372036854775808", "-1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}
