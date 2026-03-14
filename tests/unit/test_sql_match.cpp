#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"

#include <gtest/gtest.h>

using namespace sixseven;

// -- Helpers ------------------------------------------------------------------

static std::vector<StmtPtr> parse_ok(std::string_view sql) {
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

static StmtPtr parse_one(std::string_view sql) {
    auto stmts = parse_ok(sql);
    EXPECT_EQ(stmts.size(), 1u);
    if (stmts.size() != 1)
        return nullptr;
    return std::move(stmts[0]);
}

// -- SELECT ... FROM MATCH parser tests ---------------------------------------

TEST(SqlMatch, SelectFromMatchBasic) {
    auto stmt = parse_one("SELECT a.name, b.name FROM MATCH (a:users)-[e:follows]->(b:users)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 2u);
    ASSERT_EQ(sel->from.size(), 1u);
    ASSERT_NE(sel->from[0].match_source, nullptr);

    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 2u);
    EXPECT_EQ(m->pattern[0].node.variable, "a");
    EXPECT_EQ(m->pattern[0].node.label, "users");
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->variable, "e");
    EXPECT_EQ(m->pattern[0].outgoing_edge->edge_type, "follows");
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::OUT);
    EXPECT_EQ(m->pattern[1].node.variable, "b");
    EXPECT_EQ(m->pattern[1].node.label, "users");
    EXPECT_FALSE(m->pattern[1].outgoing_edge.has_value());
}

TEST(SqlMatch, SelectFromMatchWithWhere) {
    auto stmt = parse_one("SELECT b.name FROM MATCH (a:users)-[e:follows]->(b:users) "
                          "WHERE a.age > 18");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    ASSERT_NE(sel->from[0].match_source, nullptr);
    EXPECT_NE(sel->where_expr, nullptr);
}

TEST(SqlMatch, SelectFromMatchIncoming) {
    auto stmt = parse_one("SELECT a.name FROM MATCH (a:users)<-[e:follows]-(b:users)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::IN);
}

TEST(SqlMatch, SelectFromMatchUndirected) {
    auto stmt = parse_one("SELECT a.name FROM MATCH (a:users)-[e:knows]-(b:users)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    EXPECT_EQ(m->pattern[0].outgoing_edge->direction, TraverseDirection::BOTH);
}

TEST(SqlMatch, SelectFromMatchMultiHop) {
    auto stmt = parse_one("SELECT a.name, c.name "
                          "FROM MATCH (a:users)-[e1:follows]->(b:users)-[e2:follows]->(c:users)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 3u);
    ASSERT_TRUE(m->pattern[0].outgoing_edge.has_value());
    ASSERT_TRUE(m->pattern[1].outgoing_edge.has_value());
    EXPECT_FALSE(m->pattern[2].outgoing_edge.has_value());
}

// -- Composability: ORDER BY, LIMIT ------------------------------------------

TEST(SqlMatch, SelectFromMatchOrderBy) {
    auto stmt = parse_one("SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) "
                          "ORDER BY a.name");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_NE(sel->from[0].match_source, nullptr);
    ASSERT_EQ(sel->order_by.size(), 1u);
}

TEST(SqlMatch, SelectFromMatchLimit) {
    auto stmt = parse_one("SELECT a.name FROM MATCH (a:users)-[e:follows]->(b:users) LIMIT 10");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_NE(sel->from[0].match_source, nullptr);
    EXPECT_NE(sel->limit, nullptr);
}

TEST(SqlMatch, SelectFromMatchOrderByLimit) {
    auto stmt = parse_one("SELECT a.name, b.name "
                          "FROM MATCH (a:users)-[e:follows]->(b:users) "
                          "WHERE a.id > 0 "
                          "ORDER BY a.name DESC "
                          "LIMIT 5 OFFSET 2");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_NE(sel->from[0].match_source, nullptr);
    EXPECT_NE(sel->where_expr, nullptr);
    ASSERT_EQ(sel->order_by.size(), 1u);
    EXPECT_EQ(sel->order_by[0].direction, SortDirection::DESC);
    EXPECT_NE(sel->limit, nullptr);
    EXPECT_NE(sel->offset, nullptr);
}

// -- Backward compatibility: MATCH ... RETURN still works ---------------------

TEST(SqlMatch, StandaloneMatchReturnStillWorks) {
    auto stmt = parse_one("MATCH (a:users)-[e:follows]->(b:users) RETURN a.name, b.name");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->pattern.size(), 2u);
    ASSERT_EQ(m->return_items.size(), 2u);
}

TEST(SqlMatch, StandaloneMatchReturnWithWhere) {
    auto stmt = parse_one("MATCH (a:users)-[e:follows]->(b:users) "
                          "WHERE a.age > 18 RETURN b.name");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_NE(m->where_expr, nullptr);
    ASSERT_EQ(m->return_items.size(), 1u);
}

// -- Edge cases ---------------------------------------------------------------

TEST(SqlMatch, SelectStarFromMatch) {
    auto stmt = parse_one("SELECT * FROM MATCH (a:users)-[e:follows]->(b:users)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    EXPECT_TRUE(sel->items[0].is_star);
    ASSERT_NE(sel->from[0].match_source, nullptr);
}

TEST(SqlMatch, SelectFromMatchNoVariable) {
    auto stmt = parse_one("SELECT * FROM MATCH (:users)-[:follows]->(:users)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    EXPECT_TRUE(m->pattern[0].node.variable.empty());
    EXPECT_EQ(m->pattern[0].node.label, "users");
    EXPECT_TRUE(m->pattern[0].outgoing_edge->variable.empty());
    EXPECT_EQ(m->pattern[0].outgoing_edge->edge_type, "follows");
}

TEST(SqlMatch, SelectFromMatchDistinct) {
    auto stmt = parse_one("SELECT DISTINCT a.name FROM MATCH (a:users)-[e:follows]->(b:users)");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_TRUE(sel->distinct);
    ASSERT_NE(sel->from[0].match_source, nullptr);
}
