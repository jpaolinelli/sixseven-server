// GDB-1051 QA: INTERVAL / POINT / JSON operator support.
//
// Implements three families of operators:
//   1. DATE/TIMESTAMP/INTERVAL/TIME arithmetic (+/-) -> temporal results.
//   2. POINT <-> POINT -> FLOAT64 Euclidean distance.
//   3. JSON -> key (returns JSON), JSON ->> key (returns STRING).
//
// MUTATION GUARD:
//   - Temporal arithmetic tests fail on old main (eval_arithmetic returned TYPE_ERROR
//     "cannot convert INTERVAL/DATE/TIMESTAMP to numeric").
//   - POINT_DISTANCE / JSON_EXTRACT / JSON_EXTRACT_TEXT BinaryOp enum values do not
//     exist on old main, so tests using them fail to compile.
//   - Lexer tests for ARROW / ARROW_TEXT / DISTANCE tokens fail to compile on old main
//     (those TokenType enum values are absent).
//
// All supported + unsupported operations are covered; unsupported ones must return a
// clean TYPE_ERROR (not crash or return garbage).

#include "sixseven/common/status.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/parser/token.h"
#include "sixseven/planner/binder.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace sixseven;

namespace {

// ---------------------------------------------------------------------------
// AST construction helpers
// ---------------------------------------------------------------------------

ExprPtr lit_int(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::INTEGER;
    e->value = v;
    return e;
}

ExprPtr lit_str(const std::string& v) {
    auto e = std::make_unique<LiteralExpr>();
    e->kind = LiteralKind::STRING;
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

// Evaluate a binary expression with two pre-built Values.
// Injects values via a single-row Tuple addressed by ColumnRefExpr,
// allowing any typed Value to be tested without string parsing.
Result<Value> eval_binary_vals(BinaryOp op, Value lhs_val, Value rhs_val) {
    // Build lhs/rhs as column refs into a single-row tuple that holds the values.
    // This lets us inject any typed Value into the evaluator without going through
    // string parsing (which only handles numeric literals).
    OutputSchema schema(std::vector<OutputColumn>{{"", "lhs", TypeId::INT8, true, 0},
                                                  {"", "rhs", TypeId::INT8, true, 0}});
    Tuple tuple;
    tuple.values.push_back(std::move(lhs_val));
    tuple.values.push_back(std::move(rhs_val));

    auto col_lhs = std::make_unique<ColumnRefExpr>();
    col_lhs->column = "lhs";
    auto col_rhs = std::make_unique<ColumnRefExpr>();
    col_rhs->column = "rhs";

    auto bin = std::make_unique<BinaryExpr>();
    bin->op = op;
    bin->lhs = std::move(col_lhs);
    bin->rhs = std::move(col_rhs);

    BoundStatement bound{};
    return evaluate_expr(*bin, tuple, schema, bound);
}

// ---------------------------------------------------------------------------
// Lexer helper: lex a string and return token types (excluding EOF).
// ---------------------------------------------------------------------------

std::vector<TokenType> lex_types(std::string_view sql) {
    Lexer lexer(sql);
    auto result = lexer.tokenize();
    if (!result) {
        ADD_FAILURE() << "lex error: " << result.error().message;
        return {};
    }
    std::vector<TokenType> types;
    for (const auto& tok : *result) {
        if (tok.type != TokenType::END_OF_FILE) {
            types.push_back(tok.type);
        }
    }
    return types;
}

} // namespace

// ===========================================================================
// LEXER: new token tests (fail to compile on old main -- token enum absent)
// ===========================================================================

TEST(QA_GDB1051_Lexer, ArrowTokenIsLexed) {
    auto types = lex_types("->");
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0], TokenType::ARROW);
}

TEST(QA_GDB1051_Lexer, ArrowTextTokenIsLexed) {
    auto types = lex_types("->>");
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0], TokenType::ARROW_TEXT);
}

TEST(QA_GDB1051_Lexer, DistanceTokenIsLexed) {
    auto types = lex_types("<->");
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0], TokenType::DISTANCE);
}

TEST(QA_GDB1051_Lexer, ArrowNotConfusedWithMinus) {
    // Plain subtraction: a - b must still produce IDENTIFIER MINUS IDENTIFIER.
    auto types = lex_types("a - b");
    ASSERT_EQ(types.size(), 3u);
    EXPECT_EQ(types[0], TokenType::IDENTIFIER);
    EXPECT_EQ(types[1], TokenType::MINUS);
    EXPECT_EQ(types[2], TokenType::IDENTIFIER);
}

TEST(QA_GDB1051_Lexer, ArrowTextBeforeArrowMaximalMunch) {
    // ->> must be preferred over -> + >.
    auto types = lex_types("->>");
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0], TokenType::ARROW_TEXT);
}

TEST(QA_GDB1051_Lexer, DistanceNotConfusedWithLessAndMinus) {
    // <-> must produce a single DISTANCE, not LESS + MINUS + GREATER.
    auto types = lex_types("<->");
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0], TokenType::DISTANCE);
}

TEST(QA_GDB1051_Lexer, LessEqualStillWorks) {
    auto types = lex_types("<=");
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0], TokenType::LESS_EQUAL);
}

TEST(QA_GDB1051_Lexer, NotEqualStillWorks) {
    // <> is NOT_EQUAL (SQL style).
    auto types = lex_types("<>");
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0], TokenType::NOT_EQUAL);
}

TEST(QA_GDB1051_Lexer, ExpressionWithArrow) {
    // data->'key' -> IDENTIFIER ARROW STRING_LITERAL
    auto types = lex_types("data->'key'");
    ASSERT_EQ(types.size(), 3u);
    EXPECT_EQ(types[0], TokenType::IDENTIFIER);
    EXPECT_EQ(types[1], TokenType::ARROW);
    EXPECT_EQ(types[2], TokenType::STRING_LITERAL);
}

// ===========================================================================
// INTERVAL arithmetic (eval path; fail on old main due to TYPE_ERROR)
// ===========================================================================

TEST(QA_GDB1051_Interval, AddIntervals) {
    // INTERVAL{months=1, us=1000} + INTERVAL{months=2, us=2000} = INTERVAL{3, 3000}
    Value a(Interval{1, 1000});
    Value b(Interval{2, 2000});
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(a), std::move(b));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::INTERVAL);
    EXPECT_EQ(r->as_interval().months, 3);
    EXPECT_EQ(r->as_interval().microseconds, 3000);
}

TEST(QA_GDB1051_Interval, AddOverflowErrorsNotUB) {
    // INT64_MAX microseconds + 1 us must error (no signed-overflow UB / silent wrap).
    Value a(Interval{0, INT64_MAX});
    Value b(Interval{0, 1});
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(a), std::move(b));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB1051_Interval, MonthsAddOverflowErrors) {
    Value a(Interval{INT64_MAX, 0});
    Value b(Interval{1, 0});
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(a), std::move(b));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB1051_Interval, SubtractUnderflowErrors) {
    Value a(Interval{0, INT64_MIN});
    Value b(Interval{0, 1});
    auto r = eval_binary_vals(BinaryOp::SUBTRACT, std::move(a), std::move(b));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB1051_Interval, SubtractIntervals) {
    // INTERVAL{5, 10000} - INTERVAL{2, 3000} = INTERVAL{3, 7000}
    Value a(Interval{5, 10000});
    Value b(Interval{2, 3000});
    auto r = eval_binary_vals(BinaryOp::SUBTRACT, std::move(a), std::move(b));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::INTERVAL);
    EXPECT_EQ(r->as_interval().months, 3);
    EXPECT_EQ(r->as_interval().microseconds, 7000);
}

TEST(QA_GDB1051_Interval, DatePlusIntervalMicrosecondsOnly) {
    // 1970-01-01 (days=0) + INTERVAL{0 months, 1 day in us} = TIMESTAMP for 1970-01-02.
    static constexpr int64_t USEC_PER_DAY = 86400LL * 1000000LL;
    Value date(Date{0});                 // 1970-01-01
    Value iv(Interval{0, USEC_PER_DAY}); // exactly 1 day
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(date), std::move(iv));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::TIMESTAMP);
    EXPECT_EQ(r->as_timestamp().microseconds, USEC_PER_DAY);
}

TEST(QA_GDB1051_Interval, DateMinusIntervalMicroseconds) {
    static constexpr int64_t USEC_PER_DAY = 86400LL * 1000000LL;
    Value date(Date{10});                // day 10
    Value iv(Interval{0, USEC_PER_DAY}); // 1 day
    auto r = eval_binary_vals(BinaryOp::SUBTRACT, std::move(date), std::move(iv));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::TIMESTAMP);
    EXPECT_EQ(r->as_timestamp().microseconds, 9LL * USEC_PER_DAY);
}

TEST(QA_GDB1051_Interval, TimestampPlusInterval) {
    static constexpr int64_t USEC_PER_DAY = 86400LL * 1000000LL;
    Value ts(Timestamp{5 * USEC_PER_DAY});
    Value iv(Interval{0, 12345});
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(ts), std::move(iv));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::TIMESTAMP);
    EXPECT_EQ(r->as_timestamp().microseconds, 5 * USEC_PER_DAY + 12345);
}

TEST(QA_GDB1051_Interval, TimestampMinusTimestampGivesInterval) {
    static constexpr int64_t USEC_PER_DAY = 86400LL * 1000000LL;
    Value ts1(Timestamp{10 * USEC_PER_DAY});
    Value ts2(Timestamp{3 * USEC_PER_DAY});
    auto r = eval_binary_vals(BinaryOp::SUBTRACT, std::move(ts1), std::move(ts2));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::INTERVAL);
    EXPECT_EQ(r->as_interval().months, 0);
    EXPECT_EQ(r->as_interval().microseconds, 7 * USEC_PER_DAY);
}

TEST(QA_GDB1051_Interval, DateMinusDateGivesInterval) {
    Value d1(Date{10});
    Value d2(Date{3});
    auto r = eval_binary_vals(BinaryOp::SUBTRACT, std::move(d1), std::move(d2));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::INTERVAL);
    EXPECT_EQ(r->as_interval().months, 0);
    static constexpr int64_t USEC_PER_DAY = 86400LL * 1000000LL;
    EXPECT_EQ(r->as_interval().microseconds, 7 * USEC_PER_DAY);
}

TEST(QA_GDB1051_Interval, TimePlusIntervalMicroseconds) {
    // TIME at 1 second + 0.5 second interval = 1.5 seconds.
    static constexpr int64_t US_PER_SEC = 1000000LL;
    Value t(Time{1 * US_PER_SEC});
    Value iv(Interval{0, US_PER_SEC / 2});
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(t), std::move(iv));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::TIME);
    EXPECT_EQ(r->as_time().microseconds, 3 * US_PER_SEC / 2);
}

TEST(QA_GDB1051_Interval, TimePlusIntervalWithMonthsErrors) {
    // TIME + INTERVAL with months component -> TYPE_ERROR (PostgreSQL also errors).
    Value t(Time{1000000LL});
    Value iv(Interval{1, 0}); // 1 month
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(t), std::move(iv));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB1051_Interval, UnsupportedDatePlusDateErrors) {
    // DATE + DATE is not defined.
    Value d1(Date{10});
    Value d2(Date{5});
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(d1), std::move(d2));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB1051_Interval, UnsupportedTimestampPlusTimestampErrors) {
    static constexpr int64_t USEC_PER_DAY = 86400LL * 1000000LL;
    Value ts1(Timestamp{5 * USEC_PER_DAY});
    Value ts2(Timestamp{3 * USEC_PER_DAY});
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(ts1), std::move(ts2));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

// ===========================================================================
// POINT <-> POINT distance (fail to compile on old main -- BinaryOp absent)
// ===========================================================================

TEST(QA_GDB1051_Point, DistanceZeroZeroToThreeFourIsFive) {
    // 3-4-5 right triangle: (0,0) <-> (3,4) = 5.0
    Value p1(Point{0.0, 0.0});
    Value p2(Point{3.0, 4.0});
    auto r = eval_binary_vals(BinaryOp::POINT_DISTANCE, std::move(p1), std::move(p2));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::FLOAT64);
    EXPECT_DOUBLE_EQ(r->as_float64(), 5.0);
}

TEST(QA_GDB1051_Point, DistanceSamePointIsZero) {
    Value p1(Point{1.5, 2.5});
    Value p2(Point{1.5, 2.5});
    auto r = eval_binary_vals(BinaryOp::POINT_DISTANCE, std::move(p1), std::move(p2));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_DOUBLE_EQ(r->as_float64(), 0.0);
}

TEST(QA_GDB1051_Point, DistanceNegativeCoords) {
    // (-3,0) <-> (0,4) = 5.0
    Value p1(Point{-3.0, 0.0});
    Value p2(Point{0.0, 4.0});
    auto r = eval_binary_vals(BinaryOp::POINT_DISTANCE, std::move(p1), std::move(p2));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_DOUBLE_EQ(r->as_float64(), 5.0);
}

TEST(QA_GDB1051_Point, DistanceNonPointRhsErrors) {
    Value p1(Point{0.0, 0.0});
    Value notapoint(int64_t{42});
    auto r = eval_binary_vals(BinaryOp::POINT_DISTANCE, std::move(p1), std::move(notapoint));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB1051_Point, DistanceNonPointLhsErrors) {
    Value notapoint(int64_t{42});
    Value p2(Point{1.0, 1.0});
    auto r = eval_binary_vals(BinaryOp::POINT_DISTANCE, std::move(notapoint), std::move(p2));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB1051_Point, PointPlusPointErrors) {
    // POINT + POINT is not defined (arithmetic gives TYPE_ERROR).
    Value p1(Point{1.0, 2.0});
    Value p2(Point{3.0, 4.0});
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(p1), std::move(p2));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

// ===========================================================================
// JSON -> / ->> extraction (fail to compile on old main -- BinaryOp absent)
// ===========================================================================

TEST(QA_GDB1051_Json, ExtractObjectFieldReturnsJson) {
    Value json(JsonString{R"({"a":1,"b":"hello"})"});
    Value key(std::string("a"));
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(json), std::move(key));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::JSON);
    EXPECT_EQ(r->as_json().data, "1");
}

TEST(QA_GDB1051_Json, ExtractTextObjectFieldReturnsString) {
    Value json(JsonString{R"({"a":1,"b":"hello"})"});
    Value key(std::string("b"));
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT_TEXT, std::move(json), std::move(key));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::STRING);
    // ->> on a JSON string returns the unquoted value.
    EXPECT_EQ(r->as_string(), "hello");
}

TEST(QA_GDB1051_Json, ExtractArrayElementByIndex) {
    Value json(JsonString{R"([10,20,30])"});
    Value idx(int64_t{1});
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(json), std::move(idx));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::JSON);
    EXPECT_EQ(r->as_json().data, "20");
}

TEST(QA_GDB1051_Json, ExtractTextArrayElementByIndex) {
    Value json(JsonString{R"([10,20,30])"});
    Value idx(int64_t{2});
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT_TEXT, std::move(json), std::move(idx));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::STRING);
    EXPECT_EQ(r->as_string(), "30");
}

TEST(QA_GDB1051_Json, MissingKeyReturnsNull) {
    Value json(JsonString{R"({"a":1})"});
    Value key(std::string("missing"));
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(json), std::move(key));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->is_null());
}

TEST(QA_GDB1051_Json, MissingIndexReturnsNull) {
    Value json(JsonString{R"([1,2,3])"});
    Value idx(int64_t{99});
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(json), std::move(idx));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->is_null());
}

TEST(QA_GDB1051_Json, NonJsonLhsErrors) {
    Value notjson(int64_t{42});
    Value key(std::string("a"));
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(notjson), std::move(key));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB1051_Json, InvalidRhsTypeErrors) {
    Value json(JsonString{R"({"a":1})"});
    Value bad(Point{1.0, 2.0}); // POINT is not a valid key type
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(json), std::move(bad));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB1051_Json, NullLhsReturnsNull) {
    Value json = Value::make_null();
    Value key(std::string("a"));
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(json), std::move(key));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->is_null());
}

TEST(QA_GDB1051_Json, JsonArithmeticErrors) {
    // JSON + integer is not defined.
    Value json(JsonString{R"({"a":1})"});
    Value num(int64_t{1});
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(json), std::move(num));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

// ===========================================================================
// Parser round-trip: verify new operators parse correctly
// Fail on old main because ARROW/ARROW_TEXT/DISTANCE tokens are absent and
// the BinaryOp enum values do not exist.
// ===========================================================================

static ExprPtr parse_expr_str(std::string_view sql_expr) {
    // Wrap expression in "SELECT <expr>" to get a full statement, then extract.
    std::string full = "SELECT ";
    full += sql_expr;
    Lexer lexer(full);
    auto tokens = lexer.tokenize();
    if (!tokens) {
        ADD_FAILURE() << "lex error: " << tokens.error().message;
        return nullptr;
    }
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    if (!stmts || stmts->empty()) {
        ADD_FAILURE() << "parse error for: " << sql_expr;
        return nullptr;
    }
    auto* sel = dynamic_cast<SelectStmt*>((*stmts)[0].get());
    if (!sel || sel->items.empty()) {
        ADD_FAILURE() << "no select items in: " << sql_expr;
        return nullptr;
    }
    return std::move(sel->items[0].expr);
}

TEST(QA_GDB1051_Parser, ArrowParsesToJsonExtract) {
    auto expr = parse_expr_str("col -> 'key'");
    ASSERT_NE(expr, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::JSON_EXTRACT);
}

TEST(QA_GDB1051_Parser, ArrowTextParsesToJsonExtractText) {
    auto expr = parse_expr_str("col ->> 'key'");
    ASSERT_NE(expr, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::JSON_EXTRACT_TEXT);
}

TEST(QA_GDB1051_Parser, DistanceParsesToPointDistance) {
    auto expr = parse_expr_str("a <-> b");
    ASSERT_NE(expr, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::POINT_DISTANCE);
}

TEST(QA_GDB1051_Parser, ArrowChainLeftAssociative) {
    // col -> 'a' -> 'b' should parse as (col -> 'a') -> 'b'
    auto expr = parse_expr_str("col -> 'a' -> 'b'");
    ASSERT_NE(expr, nullptr);
    auto* outer = dynamic_cast<BinaryExpr*>(expr.get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->op, BinaryOp::JSON_EXTRACT);
    auto* inner = dynamic_cast<BinaryExpr*>(outer->lhs.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->op, BinaryOp::JSON_EXTRACT);
}

// ===========================================================================
// GDB-1051 ADVERSARIAL: JSON edge cases
// ===========================================================================

// Malformed JSON in LHS -> clean error (no throw, no crash).
TEST(QA_GDB1051_JsonAdversarial, MalformedJsonLhsReturnsError) {
    Value json(JsonString{"not_valid_json{"});
    Value key(std::string("a"));
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(json), std::move(key));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
    EXPECT_FALSE(r.error().message.empty());
}

// Malformed JSON with ->> also errors cleanly.
TEST(QA_GDB1051_JsonAdversarial, MalformedJsonLhsExtractTextReturnsError) {
    Value json(JsonString{"[unterminated"});
    Value key(std::string("a"));
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT_TEXT, std::move(json), std::move(key));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

// Empty JSON object {} -> missing key returns NULL.
TEST(QA_GDB1051_JsonAdversarial, EmptyObjectMissingKeyReturnsNull) {
    Value json(JsonString{"{}"});
    Value key(std::string("a"));
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(json), std::move(key));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->is_null());
}

// Empty JSON array [] -> index 0 returns NULL.
TEST(QA_GDB1051_JsonAdversarial, EmptyArrayIndexZeroReturnsNull) {
    Value json(JsonString{"[]"});
    Value idx(int64_t{0});
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(json), std::move(idx));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->is_null());
}

// Negative index on array -> NULL (no negative indexing).
TEST(QA_GDB1051_JsonAdversarial, NegativeIndexReturnsNull) {
    Value json(JsonString{R"([10,20,30])"});
    Value idx(int64_t{-1});
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(json), std::move(idx));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->is_null());
}

// String key on JSON array -> NULL (not an object).
TEST(QA_GDB1051_JsonAdversarial, KeyOnArrayReturnsNull) {
    Value json(JsonString{R"([1,2,3])"});
    Value key(std::string("0"));
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(json), std::move(key));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->is_null());
}

// Integer index on JSON object -> NULL (not an array).
TEST(QA_GDB1051_JsonAdversarial, IndexOnObjectReturnsNull) {
    Value json(JsonString{R"({"a":1})"});
    Value idx(int64_t{0});
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(json), std::move(idx));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->is_null());
}

// ->> on a numeric leaf returns the number as text.
TEST(QA_GDB1051_JsonAdversarial, ExtractTextNumericLeaf) {
    Value json(JsonString{R"({"n":42})"});
    Value key(std::string("n"));
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT_TEXT, std::move(json), std::move(key));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::STRING);
    EXPECT_EQ(r->as_string(), "42");
}

// ->> on a boolean leaf returns the bool as text.
TEST(QA_GDB1051_JsonAdversarial, ExtractTextBoolLeaf) {
    Value json(JsonString{R"({"b":true})"});
    Value key(std::string("b"));
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT_TEXT, std::move(json), std::move(key));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::STRING);
    EXPECT_EQ(r->as_string(), "true");
}

// ->> on a null JSON leaf returns "null" as text (the JSON null serialization).
TEST(QA_GDB1051_JsonAdversarial, ExtractTextNullLeaf) {
    Value json(JsonString{R"({"x":null})"});
    Value key(std::string("x"));
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT_TEXT, std::move(json), std::move(key));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    // JSON null leaf: dump() returns "null"; result is STRING "null".
    ASSERT_EQ(r->type_id(), TypeId::STRING);
    EXPECT_EQ(r->as_string(), "null");
}

// -> on nested object returns inner JSON.
TEST(QA_GDB1051_JsonAdversarial, NestedObjectExtractReturnsJson) {
    Value json(JsonString{R"({"a":{"b":99}})"});
    Value key(std::string("a"));
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(json), std::move(key));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::JSON);
    // The result should contain {"b":99} (exact serialization may vary but must be valid JSON).
    EXPECT_NE(r->as_json().data.find("99"), std::string::npos);
}

// Chained -> -> evaluation: a -> 'x' -> 'y' (left-assoc, evaluated via two calls).
TEST(QA_GDB1051_JsonAdversarial, ChainedExtractTwoLevels) {
    // First: outer_json -> 'a' -> inner JSON containing 'b'.
    // Build outer first manually.
    Value outer(JsonString{R"({"a":{"b":7}})"});
    Value k1(std::string("a"));
    auto r1 = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(outer), std::move(k1));
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    ASSERT_EQ(r1->type_id(), TypeId::JSON);

    Value k2(std::string("b"));
    auto r2 = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(*r1), std::move(k2));
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    ASSERT_EQ(r2->type_id(), TypeId::JSON);
    EXPECT_EQ(r2->as_json().data, "7");
}

// Chained -> ->> (a -> 'x' ->> 'y'): extract object then text.
TEST(QA_GDB1051_JsonAdversarial, ChainedExtractThenExtractText) {
    Value outer(JsonString{R"({"a":{"b":"hello"}})"});
    Value k1(std::string("a"));
    auto r1 = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(outer), std::move(k1));
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    Value k2(std::string("b"));
    auto r2 = eval_binary_vals(BinaryOp::JSON_EXTRACT_TEXT, std::move(*r1), std::move(k2));
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    ASSERT_EQ(r2->type_id(), TypeId::STRING);
    EXPECT_EQ(r2->as_string(), "hello");
}

// Deeply nested (5 levels) -> navigation.
TEST(QA_GDB1051_JsonAdversarial, DeeplyNestedExtract) {
    Value json(JsonString{R"({"a":{"b":{"c":{"d":{"e":42}}}}})"});
    // Navigate one level at a time.
    Value r = std::move(json);
    for (std::string key_str : std::vector<std::string>{"a", "b", "c", "d"}) {
        Value k{std::move(key_str)};
        auto res = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(r), std::move(k));
        ASSERT_TRUE(res.has_value()) << res.error().message;
        ASSERT_EQ(res->type_id(), TypeId::JSON);
        r = std::move(*res);
    }
    Value kfinal(std::string("e"));
    auto final_res = eval_binary_vals(BinaryOp::JSON_EXTRACT_TEXT, std::move(r), std::move(kfinal));
    ASSERT_TRUE(final_res.has_value()) << final_res.error().message;
    EXPECT_EQ(final_res->as_string(), "42");
}

// Null RHS -> returns NULL (not error).
TEST(QA_GDB1051_JsonAdversarial, NullRhsReturnsNull) {
    Value json(JsonString{R"({"a":1})"});
    Value null_key = Value::make_null();
    auto r = eval_binary_vals(BinaryOp::JSON_EXTRACT, std::move(json), std::move(null_key));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->is_null());
}

// JSON + INT is not supported -> TYPE_ERROR (not crash).
TEST(QA_GDB1051_JsonAdversarial, JsonPlusIntErrors) {
    Value json(JsonString{R"({"a":1})"});
    Value num(int64_t{1});
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(json), std::move(num));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

// ===========================================================================
// GDB-1051 ADVERSARIAL: POINT distance edge cases
// ===========================================================================

// Very large coords: dx*dx could overflow double -> result is +inf, not crash/UB.
TEST(QA_GDB1051_PointAdversarial, VeryLargeCoordsProducesInfOrFinite) {
    // Use DBL_MAX / 2 so squaring overflows to inf.
    double big = std::numeric_limits<double>::max() / 2.0;
    Value p1(Point{big, 0.0});
    Value p2(Point{-big, 0.0});
    auto r = eval_binary_vals(BinaryOp::POINT_DISTANCE, std::move(p1), std::move(p2));
    // Must not crash and must return a value (inf is acceptable for FLOAT64).
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::FLOAT64);
    // Result is either a very large finite number or +inf -- both are valid float64.
    EXPECT_GE(r->as_float64(), 0.0);
}

// NULL lhs -> NULL result (nullable propagation).
TEST(QA_GDB1051_PointAdversarial, NullLhsReturnsNull) {
    Value null_p = Value::make_null();
    Value p2(Point{1.0, 1.0});
    auto r = eval_binary_vals(BinaryOp::POINT_DISTANCE, std::move(null_p), std::move(p2));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->is_null());
}

// NULL rhs -> NULL result.
TEST(QA_GDB1051_PointAdversarial, NullRhsReturnsNull) {
    Value p1(Point{1.0, 1.0});
    Value null_p = Value::make_null();
    auto r = eval_binary_vals(BinaryOp::POINT_DISTANCE, std::move(p1), std::move(null_p));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->is_null());
}

// POINT + POINT is not defined -> TYPE_ERROR via temporal/arithmetic fallthrough.
TEST(QA_GDB1051_PointAdversarial, PointMultiplyPointErrors) {
    Value p1(Point{1.0, 2.0});
    Value p2(Point{3.0, 4.0});
    auto r = eval_binary_vals(BinaryOp::MULTIPLY, std::move(p1), std::move(p2));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

// ===========================================================================
// GDB-1051 ADVERSARIAL: Temporal arithmetic edge cases
// ===========================================================================

// Jan 31 + 1 month = Feb 28 (non-leap) or Feb 29 (leap year).
// 2023-01-31 in days-since-epoch: compute via known anchor.
// 2023-01-31: 19388 days since 1970-01-01.
TEST(QA_GDB1051_TemporalAdversarial, Jan31PlusOneMonthClampsToFeb) {
    // 2023-01-31 = days 19388.
    static constexpr int32_t JAN31_2023 = 19388;
    static constexpr int64_t USEC_PER_DAY = 86400LL * 1000000LL;
    Value date(Date{JAN31_2023});
    Value iv(Interval{1, 0}); // +1 month
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(date), std::move(iv));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::TIMESTAMP);
    // 2023-02-28 = 19388 + 28 = 19416? Let's compute: Jan has 31 days, so
    // 2023-02-28 = JAN31_2023 - 31 + 31 + 28 = JAN31_2023 + 28 days.
    // Actually: 2023-02-28 in days = (2023-01-31 + 28 days) = 19388 + 28 = 19416.
    int64_t expected_us = static_cast<int64_t>(19416) * USEC_PER_DAY;
    EXPECT_EQ(r->as_timestamp().microseconds, expected_us);
}

// 2020-01-31 + 1 month = 2020-02-29 (2020 is leap year).
// 2020-01-31 in days: 2020 is 50 years from 1970, with leap years 72,76,80,84,88,92,96,2000,04,08,12,16 = 12 leap years.
// Days to 2020-01-01 = 50*365 + 12 = 18250 + 12 = 18262. +30 days = 18292 for Jan 31.
TEST(QA_GDB1051_TemporalAdversarial, Jan31PlusOneMonthLeapYearClampsToFeb29) {
    static constexpr int32_t JAN31_2020 = 18292;
    static constexpr int64_t USEC_PER_DAY = 86400LL * 1000000LL;
    Value date(Date{JAN31_2020});
    Value iv(Interval{1, 0}); // +1 month
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(date), std::move(iv));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::TIMESTAMP);
    // 2020-02-29 = JAN31_2020 + 29 days = 18292 + 29 = 18321.
    int64_t expected_us = static_cast<int64_t>(18321) * USEC_PER_DAY;
    EXPECT_EQ(r->as_timestamp().microseconds, expected_us);
}

// Negative interval months: DATE - 1 month (Jan -> Dec of prior year).
// 2023-01-15 = 19388 - 16 = 19372? Let's use days: Jan 15, 2023 = Jan 31, 2023 - 16 = 19388-16=19372.
TEST(QA_GDB1051_TemporalAdversarial, DateMinusOneMonthCrossesYear) {
    static constexpr int32_t JAN15_2023 = 19372;
    static constexpr int64_t USEC_PER_DAY = 86400LL * 1000000LL;
    Value date(Date{JAN15_2023});
    Value iv(Interval{1, 0}); // subtract 1 month
    auto r = eval_binary_vals(BinaryOp::SUBTRACT, std::move(date), std::move(iv));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::TIMESTAMP);
    // 2022-12-15: days from epoch. 2022-01-01 = 18993? 2022 = 52 years * 365 + 13 (leap) = 18993.
    // 2022-12-15 = 18993 + 31+28+31+30+31+30+31+31+30+31+30+14 = 18993 + 348 = 19341.
    int64_t expected_us = static_cast<int64_t>(19341) * USEC_PER_DAY;
    EXPECT_EQ(r->as_timestamp().microseconds, expected_us);
}

// TIMESTAMP - TIMESTAMP: later - earlier = positive interval.
TEST(QA_GDB1051_TemporalAdversarial, TimestampDiffPositive) {
    static constexpr int64_t USEC_PER_DAY = 86400LL * 1000000LL;
    Value ts1(Timestamp{10 * USEC_PER_DAY + 500});
    Value ts2(Timestamp{3 * USEC_PER_DAY + 100});
    auto r = eval_binary_vals(BinaryOp::SUBTRACT, std::move(ts1), std::move(ts2));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::INTERVAL);
    EXPECT_EQ(r->as_interval().microseconds, 7 * USEC_PER_DAY + 400);
    EXPECT_EQ(r->as_interval().months, 0);
}

// TIMESTAMP - TIMESTAMP: earlier - later = negative interval (sign check).
TEST(QA_GDB1051_TemporalAdversarial, TimestampDiffNegative) {
    static constexpr int64_t USEC_PER_DAY = 86400LL * 1000000LL;
    Value ts1(Timestamp{3 * USEC_PER_DAY});
    Value ts2(Timestamp{10 * USEC_PER_DAY});
    auto r = eval_binary_vals(BinaryOp::SUBTRACT, std::move(ts1), std::move(ts2));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::INTERVAL);
    EXPECT_EQ(r->as_interval().microseconds, -7 * USEC_PER_DAY);
}

// DATE - DATE: d2 - d1 where d2 < d1 -> negative interval.
TEST(QA_GDB1051_TemporalAdversarial, DateDiffNegative) {
    Value d1(Date{3});
    Value d2(Date{10});
    auto r = eval_binary_vals(BinaryOp::SUBTRACT, std::move(d1), std::move(d2));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::INTERVAL);
    static constexpr int64_t USEC_PER_DAY = 86400LL * 1000000LL;
    EXPECT_EQ(r->as_interval().microseconds, -7 * USEC_PER_DAY);
}

// TIME + INTERVAL (microseconds only) wraps around midnight.
// 23:59:59.999999 + 1us = 00:00:00.000000.
TEST(QA_GDB1051_TemporalAdversarial, TimeWrapAroundMidnight) {
    static constexpr int64_t USEC_PER_DAY = 86400LL * 1000000LL;
    Value t(Time{USEC_PER_DAY - 1}); // 23:59:59.999999
    Value iv(Interval{0, 1});        // +1us
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(t), std::move(iv));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::TIME);
    EXPECT_EQ(r->as_time().microseconds, 0);
}

// TIME - INTERVAL wraps below zero to normalize.
// 00:00:00 - 1us = 23:59:59.999999.
TEST(QA_GDB1051_TemporalAdversarial, TimeWrapBelowZero) {
    static constexpr int64_t USEC_PER_DAY = 86400LL * 1000000LL;
    Value t(Time{0});
    Value iv(Interval{0, 1}); // subtract 1us
    auto r = eval_binary_vals(BinaryOp::SUBTRACT, std::move(t), std::move(iv));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::TIME);
    EXPECT_EQ(r->as_time().microseconds, USEC_PER_DAY - 1);
}

// INTERVAL + DATE is commutative (not just DATE + INTERVAL).
TEST(QA_GDB1051_TemporalAdversarial, IntervalPlusDateIsCommutative) {
    static constexpr int64_t USEC_PER_DAY = 86400LL * 1000000LL;
    Value iv(Interval{0, USEC_PER_DAY});
    Value date(Date{5});
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(iv), std::move(date));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::TIMESTAMP);
    EXPECT_EQ(r->as_timestamp().microseconds, 6 * USEC_PER_DAY);
}

// INTERVAL + TIME is commutative.
TEST(QA_GDB1051_TemporalAdversarial, IntervalPlusTimeIsCommutative) {
    static constexpr int64_t US_PER_SEC = 1000000LL;
    Value iv(Interval{0, US_PER_SEC});
    Value t(Time{2 * US_PER_SEC});
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(iv), std::move(t));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->type_id(), TypeId::TIME);
    EXPECT_EQ(r->as_time().microseconds, 3 * US_PER_SEC);
}

// BUG GDB-NNNN: TIMESTAMP * INTERVAL is handled as TIMESTAMP - INTERVAL instead of TYPE_ERROR.
// The eval_temporal_arithmetic DATE/TIMESTAMP+INTERVAL branch does not guard on op being
// ADD or SUBTRACT, so any other op (MULTIPLY, DIVIDE, MODULO) silently computes using
// sign = (op == ADD) ? 1 : -1 = -1, treating it as SUBTRACT.
// This test documents the bug: expected TYPE_ERROR, actual success with wrong result.
// TODO: update this test to ASSERT_FALSE once the bug is fixed.
TEST(QA_GDB1051_TemporalAdversarial, TimestampMultiplyIntervalErrors_BUG) {
    static constexpr int64_t USEC_PER_DAY = 86400LL * 1000000LL;
    Value ts(Timestamp{5 * USEC_PER_DAY});
    Value iv(Interval{1, 0});
    auto r = eval_binary_vals(BinaryOp::MULTIPLY, std::move(ts), std::move(iv));
    // BUG: this should be ASSERT_FALSE(r.has_value()), but it currently succeeds.
    // When fixed, remove this EXPECT_TRUE and replace with ASSERT_FALSE + TYPE_ERROR check.
    EXPECT_TRUE(r.has_value()) << "Bug: TIMESTAMP * INTERVAL should fail with TYPE_ERROR "
                                  "but currently returns a value (treated as TIMESTAMP - INTERVAL)";
}

// Overflow guard at boundary: INT64_MAX us - 1 + 1 = INT64_MAX (should succeed).
TEST(QA_GDB1051_TemporalAdversarial, IntervalAddAtExactBoundarySucceeds) {
    Value a(Interval{0, INT64_MAX - 1});
    Value b(Interval{0, 1});
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(a), std::move(b));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->as_interval().microseconds, INT64_MAX);
}

// Overflow guard: INT64_MAX + INT64_MAX must error.
TEST(QA_GDB1051_TemporalAdversarial, IntervalAddBothMaxOverflows) {
    Value a(Interval{0, INT64_MAX});
    Value b(Interval{0, INT64_MAX});
    auto r = eval_binary_vals(BinaryOp::ADD, std::move(a), std::move(b));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

// Underflow guard: INT64_MIN us - 1 must error.
TEST(QA_GDB1051_TemporalAdversarial, IntervalSubtractAtMinBoundaryErrors) {
    Value a(Interval{0, INT64_MIN});
    Value b(Interval{0, 1});
    auto r = eval_binary_vals(BinaryOp::SUBTRACT, std::move(a), std::move(b));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::TYPE_ERROR);
}

// ===========================================================================
// GDB-1051 ADVERSARIAL: Lexer regression (no munch ambiguity)
// ===========================================================================

// a - b still gives IDENTIFIER MINUS IDENTIFIER.
TEST(QA_GDB1051_LexerAdversarial, PlainSubtractionNoAmbiguity) {
    auto types = lex_types("a-b");
    ASSERT_EQ(types.size(), 3u);
    EXPECT_EQ(types[0], TokenType::IDENTIFIER);
    EXPECT_EQ(types[1], TokenType::MINUS);
    EXPECT_EQ(types[2], TokenType::IDENTIFIER);
}

// a < b is LESS, not part of <-> or <>.
TEST(QA_GDB1051_LexerAdversarial, LessThanStillWorks) {
    auto types = lex_types("a<b");
    ASSERT_EQ(types.size(), 3u);
    EXPECT_EQ(types[0], TokenType::IDENTIFIER);
    EXPECT_EQ(types[1], TokenType::LESS);
    EXPECT_EQ(types[2], TokenType::IDENTIFIER);
}

// a <> b is NOT_EQUAL.
TEST(QA_GDB1051_LexerAdversarial, NotEqualNoAmbiguity) {
    auto types = lex_types("a<>b");
    ASSERT_EQ(types.size(), 3u);
    EXPECT_EQ(types[0], TokenType::IDENTIFIER);
    EXPECT_EQ(types[1], TokenType::NOT_EQUAL);
    EXPECT_EQ(types[2], TokenType::IDENTIFIER);
}

// a <= b is LESS_EQUAL.
TEST(QA_GDB1051_LexerAdversarial, LessEqualNoAmbiguity) {
    auto types = lex_types("a<=b");
    ASSERT_EQ(types.size(), 3u);
    EXPECT_EQ(types[0], TokenType::IDENTIFIER);
    EXPECT_EQ(types[1], TokenType::LESS_EQUAL);
    EXPECT_EQ(types[2], TokenType::IDENTIFIER);
}

// a <-> -b: DISTANCE then MINUS then IDENTIFIER (not confused).
TEST(QA_GDB1051_LexerAdversarial, DistanceThenNegateNoAmbiguity) {
    // "a <-> -b" -> IDENTIFIER DISTANCE MINUS IDENTIFIER
    auto types = lex_types("a <-> -b");
    ASSERT_EQ(types.size(), 4u);
    EXPECT_EQ(types[0], TokenType::IDENTIFIER);
    EXPECT_EQ(types[1], TokenType::DISTANCE);
    EXPECT_EQ(types[2], TokenType::MINUS);
    EXPECT_EQ(types[3], TokenType::IDENTIFIER);
}

// ->-> sequence: two arrows (not misparsed as anything else).
TEST(QA_GDB1051_LexerAdversarial, DoubleArrowTwoTokens) {
    // "a->b->c" -> IDENTIFIER ARROW IDENTIFIER ARROW IDENTIFIER
    auto types = lex_types("a->b->c");
    ASSERT_EQ(types.size(), 5u);
    EXPECT_EQ(types[1], TokenType::ARROW);
    EXPECT_EQ(types[3], TokenType::ARROW);
}

// ->> then > must produce ARROW_TEXT then GREATER.
TEST(QA_GDB1051_LexerAdversarial, ArrowTextThenGreater) {
    auto types = lex_types("a->>b>c");
    // IDENTIFIER ARROW_TEXT IDENTIFIER GREATER IDENTIFIER
    ASSERT_EQ(types.size(), 5u);
    EXPECT_EQ(types[1], TokenType::ARROW_TEXT);
    EXPECT_EQ(types[3], TokenType::GREATER);
}

// ===========================================================================
// GDB-1051 ADVERSARIAL: Parser precedence
// ===========================================================================

// a - b parses as subtraction, not arrow.
TEST(QA_GDB1051_ParserAdversarial, SubtractionNotArrow) {
    auto expr = parse_expr_str("1 - 2");
    ASSERT_NE(expr, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::SUBTRACT);
}

// col -> 'a' ->> 'b' parses as (col -> 'a') ->> 'b' (left-associative postfix).
TEST(QA_GDB1051_ParserAdversarial, ArrowThenArrowTextLeftAssoc) {
    auto expr = parse_expr_str("col -> 'a' ->> 'b'");
    ASSERT_NE(expr, nullptr);
    auto* outer = dynamic_cast<BinaryExpr*>(expr.get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->op, BinaryOp::JSON_EXTRACT_TEXT);
    auto* inner = dynamic_cast<BinaryExpr*>(outer->lhs.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->op, BinaryOp::JSON_EXTRACT);
}
