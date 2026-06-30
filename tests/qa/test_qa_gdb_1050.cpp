// GDB-1050: Integer arithmetic wrapped silently on overflow. eval_arithmetic's
// integer fast path computed l+r / l-r / l*r as int64 with no overflow check, so
// e.g. INT64_MAX + 1 silently wrapped to INT64_MIN (and signed overflow is UB).
//
// The fix adds portable checked add/sub/mul and returns TYPE_ERROR on overflow
// rather than wrapping, mirroring the no-silent-wrap contract from GDB-1045
// (integer narrowing) and GDB-1048 (decimal overflow).
//
// MUTATION GUARD: under the old code each overflowing expression returned a wrapped
// integer Value (has_value() == true with a garbage result), so the ASSERT_FALSE /
// error-code assertions below fail. The in-range cases pin that valid arithmetic is
// unaffected.

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

Result<Value> eval(BinaryOp op, const std::string& a, const std::string& b) {
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};
    BoundStatement bound{};
    auto expr = binary(op, lit_int(a), lit_int(b));
    return evaluate_expr(*expr, tuple, schema, bound);
}

constexpr const char* kMax = "9223372036854775807";  // INT64_MAX
constexpr const char* kMin = "-9223372036854775808"; // INT64_MIN

} // namespace

TEST(QA_GDB1050, AddOverflowErrorsNotWraps) {
    auto r = eval(BinaryOp::ADD, kMax, "1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB1050, AddNegativeOverflowErrors) {
    auto r = eval(BinaryOp::ADD, kMin, "-1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB1050, SubtractUnderflowErrorsNotWraps) {
    auto r = eval(BinaryOp::SUBTRACT, kMin, "1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB1050, SubtractPositiveOverflowErrors) {
    // INT64_MAX - (-1) overflows.
    auto r = eval(BinaryOp::SUBTRACT, kMax, "-1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB1050, MultiplyOverflowErrorsNotWraps) {
    auto r = eval(BinaryOp::MULTIPLY, kMax, "2");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB1050, MultiplyIntMinByMinusOneOverflows) {
    auto r = eval(BinaryOp::MULTIPLY, kMin, "-1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB1050, InRangeAddStillWorks) {
    auto r = eval(BinaryOp::ADD, "2", "2");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int64(), 4);
}

TEST(QA_GDB1050, InRangeSubtractStillWorks) {
    auto r = eval(BinaryOp::SUBTRACT, "10", "3");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int64(), 7);
}

TEST(QA_GDB1050, InRangeMultiplyStillWorks) {
    auto r = eval(BinaryOp::MULTIPLY, "3", "4");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int64(), 12);
}

TEST(QA_GDB1050, BoundaryMaxTimesOneIsExactNotOverflow) {
    // INT64_MAX * 1 is exactly representable and must NOT be flagged as overflow.
    auto r = eval(BinaryOp::MULTIPLY, kMax, "1");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int64(), INT64_MAX);
}

TEST(QA_GDB1050, BoundaryIntMinTimesOneIsExact) {
    auto r = eval(BinaryOp::MULTIPLY, kMin, "1");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int64(), INT64_MIN);
}

TEST(QA_GDB1050, NegativeTimesNegativeInRange) {
    auto r = eval(BinaryOp::MULTIPLY, "-6", "-7");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_int64(), 42);
}
