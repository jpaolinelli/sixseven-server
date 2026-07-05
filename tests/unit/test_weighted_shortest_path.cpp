#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/match_shortest_path.h"
#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"

#include <gtest/gtest.h>

#include <vector>

using namespace sixseven;

// ===========================================================================
// Parser tests for WEIGHT clause
//
// NOTE (GDB-1215): The executor-level tests for MatchShortestPathOperator
// that used to live in this file (WeightedGraphTestBase and its derived
// fixtures: WeightedShortestPathTest, LateCheaperArrivalTest,
// NegativeWeightTest) have moved to tests/unit/test_match_shortest_path.cpp
// to align the source->test naming convention with
// src/executor/match_shortest_path.cpp. Only parser and Path-struct-field
// tests remain here.
// ===========================================================================

namespace {

std::vector<StmtPtr> parse_ok(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    EXPECT_TRUE(tokens.has_value()) << tokens.error().message;
    if (!tokens)
        return {};

    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_TRUE(stmts.has_value()) << stmts.error().message;
    return stmts ? std::move(*stmts) : std::vector<StmtPtr>{};
}

StmtPtr parse_one(std::string_view sql) {
    auto stmts = parse_ok(sql);
    EXPECT_EQ(stmts.size(), 1u);
    if (stmts.size() != 1)
        return nullptr;
    return std::move(stmts[0]);
}

bool parse_fails(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return true;
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    return !stmts.has_value();
}

} // namespace

TEST(WeightParser, WeightClauseParsed) {
    auto stmt = parse_one("MATCH p = ANY SHORTEST (a:cities)-[r:road]->{1,20}(b:cities) "
                          "WEIGHT r.distance RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::ANY_SHORTEST);
    EXPECT_EQ(m->path_variable, "p");
    ASSERT_NE(m->weight_expr, nullptr);

    auto* col = dynamic_cast<ColumnRefExpr*>(m->weight_expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->table, "r");
    EXPECT_EQ(col->column, "distance");
}

TEST(WeightParser, WeightClauseWithAllShortest) {
    auto stmt = parse_one("MATCH p = ALL SHORTEST (a:cities)-[r:road]->{1,20}(b:cities) "
                          "WEIGHT r.distance RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::ALL_SHORTEST);
    ASSERT_NE(m->weight_expr, nullptr);
}

TEST(WeightParser, WeightClauseWithShortestK) {
    auto stmt = parse_one("MATCH p = SHORTEST 3 (a:cities)-[r:road]->{1,20}(b:cities) "
                          "WEIGHT r.distance RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::SHORTEST_K);
    EXPECT_EQ(m->shortest_k, 3);
    ASSERT_NE(m->weight_expr, nullptr);
}

TEST(WeightParser, WeightWithoutPathSelectorFails) {
    // WEIGHT without a path selector should be a parse error.
    EXPECT_TRUE(parse_fails("MATCH (a:cities)-[r:road]->(b:cities) WEIGHT r.distance RETURN a, b"));
}

TEST(WeightParser, NoWeightClauseIsNull) {
    auto stmt =
        parse_one("MATCH p = ANY SHORTEST (a:cities)-[r:road]->{1,10}(b:cities) RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->weight_expr, nullptr);
}

TEST(WeightParser, SelectFromMatchWeight) {
    auto stmt = parse_one("SELECT a.name, b.name, path_cost(p) AS total_distance "
                          "FROM MATCH p = ANY SHORTEST (a:cities)-[r:road]->{1,20}(b:cities) "
                          "WEIGHT r.distance "
                          "WHERE a.name = 'NYC' AND b.name = 'LA'");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    ASSERT_NE(sel->from[0].match_source, nullptr);

    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::ANY_SHORTEST);
    ASSERT_NE(m->weight_expr, nullptr);
}

// ===========================================================================
// Path struct total_weight field tests
// ===========================================================================

TEST(PathWeight, DefaultTotalWeightIsZero) {
    Path p;
    EXPECT_DOUBLE_EQ(p.total_weight, 0.0);
}

TEST(PathWeight, TotalWeightPreserved) {
    Path p;
    p.steps.push_back({1, 100});
    p.steps.push_back({2, -1});
    p.total_weight = 42.5;

    Value v(std::move(p));
    EXPECT_EQ(v.type_id(), TypeId::PATH);
    EXPECT_DOUBLE_EQ(v.as_path().total_weight, 42.5);
}

// ===========================================================================
// WEIGHT keyword is a valid identifier
// ===========================================================================

TEST(WeightParser, WeightAsColumnName) {
    auto stmt = parse_one("SELECT weight FROM cities");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->items.size(), 1u);
}
