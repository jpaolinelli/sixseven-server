#include "giodb/common/result.h"
#include "giodb/common/types.h"
#include "giodb/parser/lexer.h"
#include "giodb/parser/parser.h"

#include <gtest/gtest.h>

#include <string>

namespace giodb {
namespace {

// ============================================================================
// QA tests for GDB-257: Parser EMBEDDING named-parameter syntax
// ============================================================================

static StmtPtr parse_one(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    EXPECT_TRUE(tokens.has_value()) << tokens.error().message;
    if (!tokens)
        return nullptr;
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_TRUE(stmts.has_value()) << sql << ": " << stmts.error().message;
    if (!stmts || stmts->empty())
        return nullptr;
    return std::move((*stmts)[0]);
}

static void parse_error(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return; // lexer error is acceptable
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_FALSE(stmts.has_value()) << "Expected parse error for: " << sql;
}

// -- Named-parameter syntax: source='...', provider='...' --------------------

TEST(QA_GDB257, NamedParameterSourceThenProvider) {
    auto stmt = parse_one("CREATE TABLE t (id INT PRIMARY KEY, "
                          "vec EMBEDDING(384, source='title', provider='openai'))");
    ASSERT_NE(stmt, nullptr);
    auto* create = dynamic_cast<const CreateTableStmt*>(stmt.get());
    ASSERT_NE(create, nullptr);
    ASSERT_GE(create->columns.size(), 2u);
    EXPECT_EQ(create->columns[1].type.name, "EMBEDDING");
    EXPECT_EQ(create->columns[1].type.param1, 384);
    EXPECT_EQ(create->columns[1].type.source, "title");
    EXPECT_EQ(create->columns[1].type.provider, "openai");
}

TEST(QA_GDB257, NamedParameterProviderThenSource) {
    auto stmt = parse_one("CREATE TABLE t (id INT PRIMARY KEY, "
                          "vec EMBEDDING(384, provider='builtin/384', source='body'))");
    ASSERT_NE(stmt, nullptr);
    auto* create = dynamic_cast<const CreateTableStmt*>(stmt.get());
    ASSERT_NE(create, nullptr);
    ASSERT_GE(create->columns.size(), 2u);
    EXPECT_EQ(create->columns[1].type.source, "body");
    EXPECT_EQ(create->columns[1].type.provider, "builtin/384");
}

// -- Positional syntax still works -------------------------------------------

TEST(QA_GDB257, PositionalSyntaxStillWorks) {
    auto stmt = parse_one("CREATE TABLE t (id INT PRIMARY KEY, "
                          "vec EMBEDDING(384, description, 'openai'))");
    ASSERT_NE(stmt, nullptr);
    auto* create = dynamic_cast<const CreateTableStmt*>(stmt.get());
    ASSERT_NE(create, nullptr);
    ASSERT_GE(create->columns.size(), 2u);
    EXPECT_EQ(create->columns[1].type.source, "description");
    EXPECT_EQ(create->columns[1].type.provider, "openai");
}

// -- Error cases -------------------------------------------------------------

TEST(QA_GDB257, DuplicateSourceParameter) {
    parse_error("CREATE TABLE t (id INT PRIMARY KEY, "
                "vec EMBEDDING(384, source='a', source='b'))");
}

TEST(QA_GDB257, UnknownNamedParameter) {
    parse_error("CREATE TABLE t (id INT PRIMARY KEY, "
                "vec EMBEDDING(384, source='a', model='gpt'))");
}

TEST(QA_GDB257, MissingSourceParameter) {
    parse_error("CREATE TABLE t (id INT PRIMARY KEY, "
                "vec EMBEDDING(384, provider='openai'))");
}

TEST(QA_GDB257, MissingProviderParameter) {
    parse_error("CREATE TABLE t (id INT PRIMARY KEY, "
                "vec EMBEDDING(384, source='title'))");
}

} // namespace
} // namespace giodb
