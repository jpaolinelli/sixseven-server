#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace sixseven;

// -- Test helpers -------------------------------------------------------------

namespace {

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

/// Build a Tuple from values.
Tuple make_tuple(std::vector<Value> vals) {
    return Tuple{std::move(vals), std::nullopt};
}

/// Make a LiteralExpr.
ExprPtr lit_int(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::INTEGER;
    e->value = v;
    return e;
}

ExprPtr lit_float(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::FLOAT;
    e->value = v;
    return e;
}

ExprPtr lit_string(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::STRING;
    e->value = v;
    return e;
}

ExprPtr lit_bool(bool v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::BOOLEAN;
    e->value = v ? "true" : "false";
    return e;
}

ExprPtr lit_null() {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::NULL_LITERAL;
    e->value = "";
    return e;
}

ExprPtr col_ref(const std::string& name) {
    auto e = std::make_unique<ColumnRefExpr>();
    e->column = name;
    return e;
}

ExprPtr col_ref(const std::string& table, const std::string& name) {
    auto e = std::make_unique<ColumnRefExpr>();
    e->table = table;
    e->column = name;
    return e;
}

ExprPtr fn_call(const std::string& name) {
    auto e = std::make_unique<FunctionCallExpr>();
    e->name = name;
    return e;
}

ExprPtr binary(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
    auto e = std::make_unique<BinaryExpr>();
    e->op = op;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    return e;
}

ExprPtr unary(UnaryOp op, ExprPtr operand) {
    auto e = std::make_unique<UnaryExpr>();
    e->op = op;
    e->operand = std::move(operand);
    return e;
}

} // namespace

// -- Empty BoundStatement used by all tests -----------------------------------

static BoundStatement empty_bound() {
    return BoundStatement{};
}

// =============================================================================
// Literal evaluation
// =============================================================================

TEST(ExprEvaluator, LiteralInteger) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*lit_int("42"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->is_null());
    EXPECT_EQ(result->as_int64(), 42);
}

TEST(ExprEvaluator, LiteralFloat) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*lit_float("3.14"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_DOUBLE_EQ(result->as_float64(), 3.14);
}

TEST(ExprEvaluator, LiteralString) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*lit_string("hello"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_string(), "hello");
}

TEST(ExprEvaluator, LiteralBoolTrue) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*lit_bool(true), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

TEST(ExprEvaluator, LiteralBoolFalse) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*lit_bool(false), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->as_bool());
}

TEST(ExprEvaluator, LiteralNull) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*lit_null(), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->is_null());
}

// =============================================================================
// Column reference
// =============================================================================

TEST(ExprEvaluator, ColumnRefByName) {
    auto schema = make_schema({{"id", TypeId::INT32}, {"name", TypeId::STRING}});
    auto tuple = make_tuple({Value(int32_t(7)), Value(std::string("alice"))});
    auto bound = empty_bound();

    auto result = evaluate_expr(*col_ref("name"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_string(), "alice");
}

TEST(ExprEvaluator, ColumnRefQualified) {
    OutputColumn c1{"users", "id", TypeId::INT32, false, 1};
    OutputColumn c2{"users", "name", TypeId::STRING, true, 1};
    OutputSchema schema({c1, c2});
    auto tuple = make_tuple({Value(int32_t(7)), Value(std::string("alice"))});
    auto bound = empty_bound();

    auto result = evaluate_expr(*col_ref("users", "name"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_string(), "alice");
}

TEST(ExprEvaluator, ColumnRefNotFound) {
    auto schema = make_schema({{"id", TypeId::INT32}});
    auto tuple = make_tuple({Value(int32_t(1))});
    auto bound = empty_bound();

    auto result = evaluate_expr(*col_ref("nonexistent"), tuple, schema, bound);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// =============================================================================
// Arithmetic
// =============================================================================

TEST(ExprEvaluator, AddIntegers) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::ADD, lit_int("10"), lit_int("20"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_int64(), 30);
}

TEST(ExprEvaluator, SubtractIntegers) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::SUBTRACT, lit_int("50"), lit_int("20"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_int64(), 30);
}

TEST(ExprEvaluator, MultiplyIntegers) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::MULTIPLY, lit_int("6"), lit_int("7"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_int64(), 42);
}

TEST(ExprEvaluator, DivideIntegersTruncatesTowardZero) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    // GDB-1049: integer / integer stays integer (PostgreSQL semantics), not a float.
    auto expr = binary(BinaryOp::DIVIDE, lit_int("10"), lit_int("3"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_int64(), 3);
}

TEST(ExprEvaluator, ModuloIntegers) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::MODULO, lit_int("10"), lit_int("3"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_int64(), 1);
}

TEST(ExprEvaluator, AddFloats) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::ADD, lit_float("1.5"), lit_float("2.5"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_DOUBLE_EQ(result->as_float64(), 4.0);
}

TEST(ExprEvaluator, MixedIntFloat) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    // int + float → float
    auto expr = binary(BinaryOp::ADD, lit_int("3"), lit_float("0.14"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_NEAR(result->as_float64(), 3.14, 0.001);
}

TEST(ExprEvaluator, DivisionByZero) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::DIVIDE, lit_int("10"), lit_int("0"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(ExprEvaluator, ArithmeticWithNull) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::ADD, lit_int("10"), lit_null());
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->is_null());
}

// =============================================================================
// Comparison
// =============================================================================

TEST(ExprEvaluator, EqualTrue) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::EQUAL, lit_int("42"), lit_int("42"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

TEST(ExprEvaluator, EqualFalse) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::EQUAL, lit_int("1"), lit_int("2"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->as_bool());
}

TEST(ExprEvaluator, LessThan) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::LESS, lit_int("1"), lit_int("2"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

TEST(ExprEvaluator, GreaterEqual) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::GREATER_EQUAL, lit_int("5"), lit_int("5"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

TEST(ExprEvaluator, ComparisonWithNull) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::EQUAL, lit_int("1"), lit_null());
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->is_null()); // NULL = anything → NULL
}

// =============================================================================
// Logical operators
// =============================================================================

TEST(ExprEvaluator, AndTrueTrue) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::AND, lit_bool(true), lit_bool(true));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

TEST(ExprEvaluator, AndTrueFalse) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::AND, lit_bool(true), lit_bool(false));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->as_bool());
}

TEST(ExprEvaluator, AndFalseShortCircuit) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    // FALSE AND anything = FALSE (short-circuit).
    auto expr = binary(BinaryOp::AND, lit_bool(false), lit_null());
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->as_bool());
}

TEST(ExprEvaluator, OrTrueFalse) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::OR, lit_bool(false), lit_bool(true));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

TEST(ExprEvaluator, OrTrueShortCircuit) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    // TRUE OR anything = TRUE (short-circuit).
    auto expr = binary(BinaryOp::OR, lit_bool(true), lit_null());
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

TEST(ExprEvaluator, NotTrue) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = unary(UnaryOp::NOT, lit_bool(true));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->as_bool());
}

TEST(ExprEvaluator, NotFalse) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = unary(UnaryOp::NOT, lit_bool(false));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

TEST(ExprEvaluator, NullAndTrue) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::AND, lit_null(), lit_bool(true));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->is_null());
}

// =============================================================================
// Unary
// =============================================================================

TEST(ExprEvaluator, NegateInteger) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = unary(UnaryOp::NEGATE, lit_int("42"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_int64(), -42);
}

TEST(ExprEvaluator, NegateFloat) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = unary(UnaryOp::NEGATE, lit_float("3.14"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_DOUBLE_EQ(result->as_float64(), -3.14);
}

TEST(ExprEvaluator, NegateNull) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = unary(UnaryOp::NEGATE, lit_null());
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->is_null());
}

// =============================================================================
// IS NULL / IS NOT NULL
// =============================================================================

TEST(ExprEvaluator, IsNullTrue) {
    auto bound = empty_bound();
    auto schema = make_schema({{"x", TypeId::INT32}});
    auto tuple = make_tuple({Value::make_null()});

    auto is_null_expr = std::make_unique<IsNullExpr>();
    is_null_expr->expr = col_ref("x");
    is_null_expr->negated = false;

    auto result = evaluate_expr(*is_null_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

TEST(ExprEvaluator, IsNullFalse) {
    auto bound = empty_bound();
    auto schema = make_schema({{"x", TypeId::INT32}});
    auto tuple = make_tuple({Value(int32_t(5))});

    auto is_null_expr = std::make_unique<IsNullExpr>();
    is_null_expr->expr = col_ref("x");
    is_null_expr->negated = false;

    auto result = evaluate_expr(*is_null_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->as_bool());
}

TEST(ExprEvaluator, IsNotNull) {
    auto bound = empty_bound();
    auto schema = make_schema({{"x", TypeId::INT32}});
    auto tuple = make_tuple({Value(int32_t(5))});

    auto is_null_expr = std::make_unique<IsNullExpr>();
    is_null_expr->expr = col_ref("x");
    is_null_expr->negated = true;

    auto result = evaluate_expr(*is_null_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

// =============================================================================
// BETWEEN
// =============================================================================

TEST(ExprEvaluator, BetweenTrue) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto between_expr = std::make_unique<BetweenExpr>();
    between_expr->expr = lit_int("5");
    between_expr->low = lit_int("1");
    between_expr->high = lit_int("10");
    between_expr->negated = false;

    auto result = evaluate_expr(*between_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

TEST(ExprEvaluator, BetweenFalse) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto between_expr = std::make_unique<BetweenExpr>();
    between_expr->expr = lit_int("15");
    between_expr->low = lit_int("1");
    between_expr->high = lit_int("10");
    between_expr->negated = false;

    auto result = evaluate_expr(*between_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->as_bool());
}

TEST(ExprEvaluator, NotBetween) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto between_expr = std::make_unique<BetweenExpr>();
    between_expr->expr = lit_int("15");
    between_expr->low = lit_int("1");
    between_expr->high = lit_int("10");
    between_expr->negated = true;

    auto result = evaluate_expr(*between_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool()); // 15 NOT BETWEEN 1 AND 10 → true
}

// =============================================================================
// IN
// =============================================================================

TEST(ExprEvaluator, InListFound) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto in_expr = std::make_unique<InExpr>();
    in_expr->expr = lit_int("3");
    in_expr->values.push_back(lit_int("1"));
    in_expr->values.push_back(lit_int("2"));
    in_expr->values.push_back(lit_int("3"));
    in_expr->negated = false;

    auto result = evaluate_expr(*in_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

TEST(ExprEvaluator, InListNotFound) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto in_expr = std::make_unique<InExpr>();
    in_expr->expr = lit_int("5");
    in_expr->values.push_back(lit_int("1"));
    in_expr->values.push_back(lit_int("2"));
    in_expr->values.push_back(lit_int("3"));
    in_expr->negated = false;

    auto result = evaluate_expr(*in_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->as_bool());
}

TEST(ExprEvaluator, NotInList) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto in_expr = std::make_unique<InExpr>();
    in_expr->expr = lit_int("5");
    in_expr->values.push_back(lit_int("1"));
    in_expr->values.push_back(lit_int("2"));
    in_expr->negated = true;

    auto result = evaluate_expr(*in_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

// =============================================================================
// LIKE
// =============================================================================

TEST(ExprEvaluator, LikePercent) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto like_expr = std::make_unique<LikeExpr>();
    like_expr->expr = lit_string("hello world");
    like_expr->pattern = lit_string("hello%");
    like_expr->negated = false;

    auto result = evaluate_expr(*like_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

TEST(ExprEvaluator, LikeUnderscore) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto like_expr = std::make_unique<LikeExpr>();
    like_expr->expr = lit_string("cat");
    like_expr->pattern = lit_string("c_t");
    like_expr->negated = false;

    auto result = evaluate_expr(*like_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

TEST(ExprEvaluator, LikeNoMatch) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto like_expr = std::make_unique<LikeExpr>();
    like_expr->expr = lit_string("dog");
    like_expr->pattern = lit_string("c_t");
    like_expr->negated = false;

    auto result = evaluate_expr(*like_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->as_bool());
}

TEST(ExprEvaluator, NotLike) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto like_expr = std::make_unique<LikeExpr>();
    like_expr->expr = lit_string("dog");
    like_expr->pattern = lit_string("c%");
    like_expr->negated = true;

    auto result = evaluate_expr(*like_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->as_bool());
}

// =============================================================================
// CASE expression
// =============================================================================

TEST(ExprEvaluator, SearchedCaseMatch) {
    auto bound = empty_bound();
    auto schema = make_schema({{"x", TypeId::INT32}});
    auto tuple = make_tuple({Value(int32_t(10))});

    // CASE WHEN x > 5 THEN 'big' ELSE 'small' END
    auto case_expr = std::make_unique<CaseExpr>();
    CaseWhen when;
    when.condition = binary(BinaryOp::GREATER, col_ref("x"), lit_int("5"));
    when.result = lit_string("big");
    case_expr->whens.push_back(std::move(when));
    case_expr->else_expr = lit_string("small");

    auto result = evaluate_expr(*case_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_string(), "big");
}

TEST(ExprEvaluator, SearchedCaseElse) {
    auto bound = empty_bound();
    auto schema = make_schema({{"x", TypeId::INT32}});
    auto tuple = make_tuple({Value(int32_t(2))});

    auto case_expr = std::make_unique<CaseExpr>();
    CaseWhen when;
    when.condition = binary(BinaryOp::GREATER, col_ref("x"), lit_int("5"));
    when.result = lit_string("big");
    case_expr->whens.push_back(std::move(when));
    case_expr->else_expr = lit_string("small");

    auto result = evaluate_expr(*case_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_string(), "small");
}

TEST(ExprEvaluator, CaseNoMatchNoElse) {
    auto bound = empty_bound();
    auto schema = make_schema({{"x", TypeId::INT32}});
    auto tuple = make_tuple({Value(int32_t(2))});

    auto case_expr = std::make_unique<CaseExpr>();
    CaseWhen when;
    when.condition = binary(BinaryOp::GREATER, col_ref("x"), lit_int("5"));
    when.result = lit_string("big");
    case_expr->whens.push_back(std::move(when));
    // No else_expr.

    auto result = evaluate_expr(*case_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->is_null());
}

// =============================================================================
// evaluate_predicate
// =============================================================================

TEST(ExprEvaluator, PredicateTrue) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_predicate(*lit_bool(true), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(*result);
}

TEST(ExprEvaluator, PredicateFalse) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_predicate(*lit_bool(false), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(*result);
}

TEST(ExprEvaluator, PredicateNullIsFalse) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_predicate(*lit_null(), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(*result); // NULL → false for predicates
}

// =============================================================================
// CONCAT
// =============================================================================

TEST(ExprEvaluator, ConcatStrings) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::CONCAT, lit_string("hello"), lit_string(" world"));
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->as_string(), "hello world");
}

TEST(ExprEvaluator, ConcatWithNull) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto expr = binary(BinaryOp::CONCAT, lit_string("hello"), lit_null());
    auto result = evaluate_expr(*expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->is_null());
}

// =============================================================================
// gen_uuid() function
// =============================================================================

TEST(ExprEvaluator, GenUuidReturnsUuidType) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*fn_call("gen_uuid"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->is_null());
    EXPECT_EQ(result->type_id(), TypeId::UUID);
}

TEST(ExprEvaluator, GenUuidVersion4Format) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*fn_call("gen_uuid"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto& uuid = result->as_uuid();
    // Version 4: byte 6 high nibble should be 0x4_.
    EXPECT_EQ((uuid[6] >> 4) & 0x0F, 4);
    // Variant 1: byte 8 high bits should be 10xx (value 2).
    EXPECT_EQ((uuid[8] >> 6) & 0x03, 2);
}

TEST(ExprEvaluator, GenUuidUniqueness) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto r1 = evaluate_expr(*fn_call("gen_uuid"), tuple, schema, bound);
    auto r2 = evaluate_expr(*fn_call("gen_uuid"), tuple, schema, bound);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    ASSERT_TRUE(r2.has_value()) << r2.error().message;

    // Two generated UUIDs should be different.
    EXPECT_NE(r1->as_uuid(), r2->as_uuid());
}

TEST(ExprEvaluator, GenRandomUuidAlias) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*fn_call("gen_random_uuid"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->type_id(), TypeId::UUID);
}

TEST(ExprEvaluator, UuidGenerateV4Alias) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*fn_call("uuid_generate_v4"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->type_id(), TypeId::UUID);
}

TEST(ExprEvaluator, GenUuidCaseInsensitive) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*fn_call("GEN_UUID"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->type_id(), TypeId::UUID);
}

// =============================================================================
// CURRENT_TIMESTAMP / NOW()
// =============================================================================

TEST(ExprEvaluator, NowReturnsTimestamp) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*fn_call("now"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->is_null());
    EXPECT_EQ(result->type_id(), TypeId::TIMESTAMP);

    // The timestamp should be close to the current wall-clock time.
    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    auto diff = std::abs(result->as_timestamp().microseconds - now_us);
    EXPECT_LT(diff, 1000000); // Within 1 second.
}

TEST(ExprEvaluator, CurrentTimestampAsFunction) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*fn_call("CURRENT_TIMESTAMP"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->type_id(), TypeId::TIMESTAMP);
}

TEST(ExprEvaluator, CurrentTimestampAsColumnRef) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    // CURRENT_TIMESTAMP parsed as bare identifier (ColumnRefExpr).
    auto result = evaluate_expr(*col_ref("CURRENT_TIMESTAMP"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->type_id(), TypeId::TIMESTAMP);

    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    auto diff = std::abs(result->as_timestamp().microseconds - now_us);
    EXPECT_LT(diff, 1000000);
}

// =============================================================================
// CURRENT_DATE
// =============================================================================

TEST(ExprEvaluator, CurrentDateAsFunction) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*fn_call("CURRENT_DATE"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->is_null());
    EXPECT_EQ(result->type_id(), TypeId::DATE);

    // The date should be close to today.
    auto now_days = std::chrono::duration_cast<std::chrono::days>(
                        std::chrono::floor<std::chrono::days>(
                            std::chrono::system_clock::now().time_since_epoch()))
                        .count();
    auto diff = std::abs(static_cast<int64_t>(result->as_date().days_since_epoch) -
                         static_cast<int64_t>(now_days));
    EXPECT_LE(diff, 1); // Within 1 day (handles midnight edge).
}

TEST(ExprEvaluator, CurrentDateAsColumnRef) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*col_ref("CURRENT_DATE"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->type_id(), TypeId::DATE);
}

// =============================================================================
// CURRENT_TIME
// =============================================================================

TEST(ExprEvaluator, CurrentTimeAsFunction) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*fn_call("CURRENT_TIME"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->is_null());
    EXPECT_EQ(result->type_id(), TypeId::TIME);

    // Microseconds since midnight must be in valid range [0, 86400000000).
    auto us = result->as_time().microseconds;
    EXPECT_GE(us, 0);
    EXPECT_LT(us, 86400000000LL);
}

TEST(ExprEvaluator, CurrentTimeAsColumnRef) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*col_ref("current_time"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->type_id(), TypeId::TIME);
}

TEST(ExprEvaluator, CurrentTimeCaseInsensitiveColumnRef) {
    auto bound = empty_bound();
    OutputSchema schema;
    Tuple tuple{{}, std::nullopt};

    auto result = evaluate_expr(*col_ref("Current_Timestamp"), tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->type_id(), TypeId::TIMESTAMP);
}
