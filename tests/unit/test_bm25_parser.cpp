#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"

#include <gtest/gtest.h>

using namespace sixseven;

namespace {

StmtPtr parse_one(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    EXPECT_TRUE(tokens.has_value()) << (tokens ? "" : tokens.error().message);
    if (!tokens) {
        return nullptr;
    }
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_TRUE(stmts.has_value()) << (stmts ? "" : stmts.error().message);
    if (!stmts || stmts->size() != 1) {
        return nullptr;
    }
    return std::move((*stmts)[0]);
}

// Find a MatchExpr anywhere in a WHERE expression tree.
const MatchExpr* find_match(const Expr* e) {
    if (e == nullptr) {
        return nullptr;
    }
    if (auto* m = dynamic_cast<const MatchExpr*>(e)) {
        return m;
    }
    if (auto* b = dynamic_cast<const BinaryExpr*>(e)) {
        if (auto* l = find_match(b->lhs.get())) {
            return l;
        }
        return find_match(b->rhs.get());
    }
    if (auto* u = dynamic_cast<const UnaryExpr*>(e)) {
        return find_match(u->operand.get());
    }
    return nullptr;
}

} // namespace

TEST(Bm25Parser, ParsesMatchTo) {
    auto stmt = parse_one("SELECT id FROM articles WHERE MATCH(body) TO 'machine learning'");
    ASSERT_NE(stmt, nullptr);
    auto* sel = dynamic_cast<const SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_NE(sel->where_expr, nullptr);

    auto* match = find_match(sel->where_expr.get());
    ASSERT_NE(match, nullptr);

    auto* col = dynamic_cast<const ColumnRefExpr*>(match->column.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column, "body");

    auto* query = dynamic_cast<const LiteralExpr*>(match->query.get());
    ASSERT_NE(query, nullptr);
    EXPECT_EQ(query->value, "machine learning");
}

TEST(Bm25Parser, MatchComposesWithAnd) {
    auto stmt = parse_one("SELECT id FROM articles WHERE MATCH(body) TO 'db' AND id > 5");
    ASSERT_NE(stmt, nullptr);
    auto* sel = dynamic_cast<const SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_NE(find_match(sel->where_expr.get()), nullptr);
}

TEST(Bm25Parser, SelectScoreColumn) {
    // _score is just an ordinary identifier at parse time.
    auto stmt =
        parse_one("SELECT id, _score FROM articles WHERE MATCH(body) TO 'x' ORDER BY _score DESC");
    ASSERT_NE(stmt, nullptr);
}

TEST(Bm25Parser, MissingToIsError) {
    Lexer lexer("SELECT id FROM articles WHERE MATCH(body)");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_FALSE(stmts.has_value());
}

TEST(Bm25Parser, GraphMatchStatementStillParses) {
    // Ensure the expression-context MATCH did not break the graph MATCH statement.
    auto stmt = parse_one("MATCH (a)-[:knows]->(b) RETURN a, b");
    ASSERT_NE(stmt, nullptr);
}
