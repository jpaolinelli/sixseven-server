// QA regression tests for GDB-1198.
//
// GDB-1198 removed VarLenMatchTest.PathLengthFunction (tests/unit/test_variable_length_match.cpp)
// as a pure duplicate-test cleanup: it only exercised Path::length() on hand-built structs,
// which is already covered by PathValueTest.PathLength/PathInValue/EmptyPath, and it never
// invoked the SQL path_length() despite the name, which is covered by the PathLengthSqlFunction
// suite (ReturnsCorrectLength / NullArgReturnsNull / WrongArgCountFails).
//
// This QA suite independently re-verifies both layers with adversarial edge cases beyond what
// the deleted test or the surviving suites already assert, to close the following gap:
// the deleted test's p3 case exercised a 3-hop path, and no surviving test exceeds 2 hops.
// Path::length() is trivial arithmetic (steps.size() - 1, special-cased only for empty), so a
// higher hop count adds no meaningfully different code path -- but we lock it in explicitly here
// so the coverage claim is verifiable rather than asserted by inspection alone.

#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

using namespace sixseven;

namespace {

Path make_path(int64_t n_steps) {
    Path p;
    for (int64_t i = 0; i < n_steps; ++i) {
        int64_t edge_id = (i + 1 < n_steps) ? (100 + i) : -1;
        p.steps.push_back({i + 1, edge_id});
    }
    return p;
}

}  // namespace

// -- Path::length() struct method: coverage beyond 0/1/2 hops ----------------

TEST(QA_GDB1198_PathStructLength, ThreeHopPath) {
    Path p = make_path(4);  // 4 steps -> 3 hops
    EXPECT_EQ(p.length(), 3);
}

TEST(QA_GDB1198_PathStructLength, SingleNodeNoHops) {
    Path p = make_path(1);  // single node, zero hops
    EXPECT_EQ(p.length(), 0);
}

TEST(QA_GDB1198_PathStructLength, LongPathManyHops) {
    Path p = make_path(101);  // 100 hops
    EXPECT_EQ(p.length(), 100);
}

// -- SQL path_length() function: additional adversarial coverage ------------

TEST(QA_GDB1198_PathLengthSqlFunction, ThreeHopPathReturnsThree) {
    Path path = make_path(4);

    Tuple tuple;
    tuple.values.push_back(Value(path));

    std::vector<OutputColumn> cols;
    cols.push_back({"", "p", TypeId::PATH, true, 0});
    OutputSchema schema(std::move(cols));

    auto col_ref = std::make_unique<ColumnRefExpr>();
    col_ref->column = "p";

    FunctionCallExpr fn_expr;
    fn_expr.name = "path_length";
    fn_expr.args.push_back(std::move(col_ref));

    BoundStatement bound;
    auto result = evaluate_expr(fn_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->is_null());
    EXPECT_EQ(result->as_int64(), 3);
}

TEST(QA_GDB1198_PathLengthSqlFunction, SingleNodePathReturnsZero) {
    Path path = make_path(1);

    Tuple tuple;
    tuple.values.push_back(Value(path));

    std::vector<OutputColumn> cols;
    cols.push_back({"", "p", TypeId::PATH, true, 0});
    OutputSchema schema(std::move(cols));

    auto col_ref = std::make_unique<ColumnRefExpr>();
    col_ref->column = "p";

    FunctionCallExpr fn_expr;
    fn_expr.name = "path_length";
    fn_expr.args.push_back(std::move(col_ref));

    BoundStatement bound;
    auto result = evaluate_expr(fn_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->is_null());
    EXPECT_EQ(result->as_int64(), 0);
}

TEST(QA_GDB1198_PathLengthSqlFunction, EmptyPathReturnsZero) {
    Path path;  // no steps at all

    Tuple tuple;
    tuple.values.push_back(Value(path));

    std::vector<OutputColumn> cols;
    cols.push_back({"", "p", TypeId::PATH, true, 0});
    OutputSchema schema(std::move(cols));

    auto col_ref = std::make_unique<ColumnRefExpr>();
    col_ref->column = "p";

    FunctionCallExpr fn_expr;
    fn_expr.name = "path_length";
    fn_expr.args.push_back(std::move(col_ref));

    BoundStatement bound;
    auto result = evaluate_expr(fn_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->is_null());
    EXPECT_EQ(result->as_int64(), 0);
}

TEST(QA_GDB1198_PathLengthSqlFunction, WrongTypeArgFails) {
    // path_length() called on a non-PATH column should fail cleanly, not crash.
    Tuple tuple;
    tuple.values.push_back(Value(static_cast<int64_t>(42)));

    std::vector<OutputColumn> cols;
    cols.push_back({"", "n", TypeId::INT64, true, 0});
    OutputSchema schema(std::move(cols));

    auto col_ref = std::make_unique<ColumnRefExpr>();
    col_ref->column = "n";

    FunctionCallExpr fn_expr;
    fn_expr.name = "path_length";
    fn_expr.args.push_back(std::move(col_ref));

    BoundStatement bound;
    auto result = evaluate_expr(fn_expr, tuple, schema, bound);
    // Either a clean error or an is_null() result is acceptable; a crash or silent
    // int64 result would not be.
    if (result.has_value()) {
        EXPECT_TRUE(result->is_null());
    } else {
        EXPECT_FALSE(result.error().message.empty());
    }
}

TEST(QA_GDB1198_PathLengthSqlFunction, TooManyArgsFails) {
    Path path = make_path(2);

    Tuple tuple;
    tuple.values.push_back(Value(path));
    tuple.values.push_back(Value(path));

    std::vector<OutputColumn> cols;
    cols.push_back({"", "p1", TypeId::PATH, true, 0});
    cols.push_back({"", "p2", TypeId::PATH, true, 1});
    OutputSchema schema(std::move(cols));

    auto col_ref1 = std::make_unique<ColumnRefExpr>();
    col_ref1->column = "p1";
    auto col_ref2 = std::make_unique<ColumnRefExpr>();
    col_ref2->column = "p2";

    FunctionCallExpr fn_expr;
    fn_expr.name = "path_length";
    fn_expr.args.push_back(std::move(col_ref1));
    fn_expr.args.push_back(std::move(col_ref2));

    BoundStatement bound;
    auto result = evaluate_expr(fn_expr, tuple, schema, bound);
    ASSERT_FALSE(result.has_value());
    EXPECT_FALSE(result.error().message.empty());
}
