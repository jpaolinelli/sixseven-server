#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"

#include <gtest/gtest.h>

#include <climits>
#include <string>

using namespace sixseven;

// =============================================================================
// Helpers
// =============================================================================

static Result<StmtPtr> try_parse(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return tl::unexpected(tokens.error());
    Parser parser(std::move(*tokens));
    return parser.parse();
}

static StmtPtr parse_one(std::string_view sql) {
    auto result = try_parse(sql);
    EXPECT_TRUE(result.has_value()) << result.error().message;
    return result ? std::move(*result) : nullptr;
}

// =============================================================================
// GDB-216: safe_stoi overflow — verify error Result (not crash)
// =============================================================================

// -- VARCHAR overflow ----------------------------------------------------------

TEST(QA_GDB216_Overflow, VarcharOverflowReturnsError) {
    auto r = try_parse("CREATE TABLE t (c VARCHAR(99999999999999999))");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB216_Overflow, VarcharIntMaxPlusOneReturnsError) {
    auto r = try_parse("CREATE TABLE t (c VARCHAR(2147483648))");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB216_Overflow, VarcharIntMaxParsesOk) {
    auto r = try_parse("CREATE TABLE t (c VARCHAR(2147483647))");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto* ct = dynamic_cast<CreateTableStmt*>(r->get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 1u);
    ASSERT_TRUE(ct->columns[0].type.param1.has_value());
    EXPECT_EQ(*ct->columns[0].type.param1, INT32_MAX);
}

TEST(QA_GDB216_Overflow, VarcharZeroParsesOk) {
    auto r = try_parse("CREATE TABLE t (c VARCHAR(0))");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto* ct = dynamic_cast<CreateTableStmt*>(r->get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 1u);
    ASSERT_TRUE(ct->columns[0].type.param1.has_value());
    EXPECT_EQ(*ct->columns[0].type.param1, 0);
}

TEST(QA_GDB216_Overflow, CharOverflowReturnsError) {
    auto r = try_parse("CREATE TABLE t (c CHAR(99999999999999999))");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// -- DECIMAL overflow ---------------------------------------------------------

TEST(QA_GDB216_Overflow, DecimalPrecisionOverflowReturnsError) {
    auto r = try_parse("CREATE TABLE t (c DECIMAL(99999999999999999, 2))");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB216_Overflow, DecimalScaleOverflowReturnsError) {
    auto r = try_parse("CREATE TABLE t (c DECIMAL(10, 99999999999999999))");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB216_Overflow, DecimalBothOverflowReturnsError) {
    auto r = try_parse("CREATE TABLE t (c DECIMAL(99999999999999999, 99999999999999999))");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB216_Overflow, DecimalPrecisionIntMaxPlusOneError) {
    auto r = try_parse("CREATE TABLE t (c DECIMAL(2147483648, 2))");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB216_Overflow, DecimalScaleIntMaxPlusOneError) {
    auto r = try_parse("CREATE TABLE t (c DECIMAL(10, 2147483648))");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB216_Overflow, DecimalIntMaxParsesOk) {
    auto r = try_parse("CREATE TABLE t (c DECIMAL(2147483647, 2147483647))");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto* ct = dynamic_cast<CreateTableStmt*>(r->get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 1u);
    ASSERT_TRUE(ct->columns[0].type.param1.has_value());
    ASSERT_TRUE(ct->columns[0].type.param2.has_value());
    EXPECT_EQ(*ct->columns[0].type.param1, INT32_MAX);
    EXPECT_EQ(*ct->columns[0].type.param2, INT32_MAX);
}

// -- EMBEDDING overflow -------------------------------------------------------

TEST(QA_GDB216_Overflow, EmbeddingDimensionOverflowReturnsError) {
    auto r = try_parse("CREATE TABLE t (e EMBEDDING(99999999999999999, body, 'openai'))");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB216_Overflow, EmbeddingDimIntMaxPlusOneError) {
    auto r = try_parse("CREATE TABLE t (e EMBEDDING(2147483648, body, 'openai'))");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB216_Overflow, EmbeddingDimIntMaxParsesOk) {
    auto r = try_parse("CREATE TABLE t (e EMBEDDING(2147483647, body, 'openai'))");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto* ct = dynamic_cast<CreateTableStmt*>(r->get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->columns.size(), 1u);
    ASSERT_TRUE(ct->columns[0].type.param1.has_value());
    EXPECT_EQ(*ct->columns[0].type.param1, INT32_MAX);
}

// -- TRAVERSE MAX_DEPTH overflow ----------------------------------------------

TEST(QA_GDB216_Overflow, TraverseMaxDepthOverflowReturnsError) {
    auto r = try_parse("TRAVERSE follows FROM users(1) MAX_DEPTH 99999999999999999");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB216_Overflow, TraverseMaxDepthIntMaxPlusOneError) {
    auto r = try_parse("TRAVERSE follows FROM users(1) MAX_DEPTH 2147483648");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB216_Overflow, TraverseMaxDepthIntMaxParsesOk) {
    auto r = try_parse("TRAVERSE follows FROM users(1) MAX_DEPTH 2147483647");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto* stmt = dynamic_cast<TraverseStmt*>(r->get());
    ASSERT_NE(stmt, nullptr);
    ASSERT_TRUE(stmt->max_depth.has_value());
    EXPECT_EQ(*stmt->max_depth, INT32_MAX);
}

TEST(QA_GDB216_Overflow, TraverseMaxDepthZeroParsesOk) {
    auto r = try_parse("TRAVERSE follows FROM users(1) MAX_DEPTH 0");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto* stmt = dynamic_cast<TraverseStmt*>(r->get());
    ASSERT_NE(stmt, nullptr);
    ASSERT_TRUE(stmt->max_depth.has_value());
    EXPECT_EQ(*stmt->max_depth, 0);
}

// -- NEAREST WITHIN TRAVERSE MAX_DEPTH overflow -------------------------------

TEST(QA_GDB216_Overflow, NearestTraverseMaxDepthOverflowReturnsError) {
    auto r = try_parse("NEAREST 5 FROM t.col TO [1,2,3] "
                       "WITHIN TRAVERSE follows FROM users(1) MAX_DEPTH 99999999999999999");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB216_Overflow, NearestTraverseMaxDepthIntMaxPlusOneError) {
    auto r = try_parse("NEAREST 5 FROM t.col TO [1,2,3] "
                       "WITHIN TRAVERSE follows FROM users(1) MAX_DEPTH 2147483648");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// -- SHORTEST PATH MAX_DEPTH overflow -----------------------------------------

TEST(QA_GDB216_Overflow, ShortestPathMaxDepthOverflowReturnsError) {
    auto r = try_parse("SHORTEST PATH FROM a(1) TO b(2) VIA edge MAX_DEPTH 99999999999999999");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB216_Overflow, ShortestPathMaxDepthIntMaxPlusOneError) {
    auto r = try_parse("SHORTEST PATH FROM a(1) TO b(2) VIA edge MAX_DEPTH 2147483648");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB216_Overflow, ShortestPathMaxDepthIntMaxParsesOk) {
    auto r = try_parse("SHORTEST PATH FROM a(1) TO b(2) VIA edge MAX_DEPTH 2147483647");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto* stmt = dynamic_cast<ShortestPathStmt*>(r->get());
    ASSERT_NE(stmt, nullptr);
    ASSERT_TRUE(stmt->max_depth.has_value());
    EXPECT_EQ(*stmt->max_depth, INT32_MAX);
}

// -- Extreme overflow values --------------------------------------------------

TEST(QA_GDB216_Overflow, VarcharExtremelyLargeValueDoesNotCrash) {
    // 100-digit number — way beyond any integer type.
    std::string huge(100, '9');
    auto sql = "CREATE TABLE t (c VARCHAR(" + huge + "))";
    auto r = try_parse(sql);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_GDB216_Overflow, TraverseUint64MaxOverflow) {
    // UINT64_MAX = 18446744073709551615 — overflows int32.
    auto r = try_parse("TRAVERSE follows FROM users(1) MAX_DEPTH 18446744073709551615");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

// -- Error message content verification ---------------------------------------

TEST(QA_GDB216_Overflow, ErrorMessageContainsOutOfRange) {
    auto r = try_parse("CREATE TABLE t (c VARCHAR(99999999999999999))");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("out of range"), std::string::npos)
        << "Error message: " << r.error().message;
}

// =============================================================================
// GDB-217: Password unquoting
// =============================================================================

// -- CREATE USER password unquoting -------------------------------------------

TEST(QA_GDB217_Password, CreateUserSimplePassword) {
    auto stmt = parse_one("CREATE USER admin WITH PASSWORD 'secret123'");
    auto* cu = dynamic_cast<CreateUserStmt*>(stmt.get());
    ASSERT_NE(cu, nullptr);
    EXPECT_EQ(cu->username, "admin");
    EXPECT_EQ(cu->password, "secret123");
}

TEST(QA_GDB217_Password, CreateUserPasswordNoQuotes) {
    // Verify the password does NOT start or end with a quote.
    auto stmt = parse_one("CREATE USER bob WITH PASSWORD 'pass'");
    auto* cu = dynamic_cast<CreateUserStmt*>(stmt.get());
    ASSERT_NE(cu, nullptr);
    EXPECT_FALSE(cu->password.empty());
    EXPECT_NE(cu->password.front(), '\'');
    EXPECT_NE(cu->password.back(), '\'');
    EXPECT_EQ(cu->password, "pass");
}

TEST(QA_GDB217_Password, CreateUserEscapedSingleQuote) {
    // SQL: 'it''s a secret' should become: it's a secret
    auto stmt = parse_one("CREATE USER admin WITH PASSWORD 'it''s a secret'");
    auto* cu = dynamic_cast<CreateUserStmt*>(stmt.get());
    ASSERT_NE(cu, nullptr);
    EXPECT_EQ(cu->password, "it's a secret");
}

TEST(QA_GDB217_Password, CreateUserMultipleEscapedQuotes) {
    // SQL: 'a''b''c' should become: a'b'c
    auto stmt = parse_one("CREATE USER admin WITH PASSWORD 'a''b''c'");
    auto* cu = dynamic_cast<CreateUserStmt*>(stmt.get());
    ASSERT_NE(cu, nullptr);
    EXPECT_EQ(cu->password, "a'b'c");
}

TEST(QA_GDB217_Password, CreateUserEmptyPassword) {
    // SQL: '' should become empty string
    auto stmt = parse_one("CREATE USER admin WITH PASSWORD ''");
    auto* cu = dynamic_cast<CreateUserStmt*>(stmt.get());
    ASSERT_NE(cu, nullptr);
    EXPECT_EQ(cu->password, "");
}

TEST(QA_GDB217_Password, CreateUserPasswordAllQuotes) {
    // SQL: '''''''' is 4 pairs of quotes inside outer quotes = '''
    // Wait: '''''''' has 8 single-quotes. As a SQL literal:
    //   Outer quotes: first ' and last '
    //   Inner: '''''' which is 3 escaped pairs = '''
    auto stmt = parse_one("CREATE USER admin WITH PASSWORD ''''''''");
    auto* cu = dynamic_cast<CreateUserStmt*>(stmt.get());
    ASSERT_NE(cu, nullptr);
    EXPECT_EQ(cu->password, "'''");
}

TEST(QA_GDB217_Password, CreateUserLongPassword) {
    // Password with 200 characters.
    std::string long_pass(200, 'x');
    auto sql = "CREATE USER admin WITH PASSWORD '" + long_pass + "'";
    auto stmt = parse_one(sql);
    auto* cu = dynamic_cast<CreateUserStmt*>(stmt.get());
    ASSERT_NE(cu, nullptr);
    EXPECT_EQ(cu->password, long_pass);
}

TEST(QA_GDB217_Password, CreateUserPasswordWithSpaces) {
    auto stmt = parse_one("CREATE USER admin WITH PASSWORD 'my secret password'");
    auto* cu = dynamic_cast<CreateUserStmt*>(stmt.get());
    ASSERT_NE(cu, nullptr);
    EXPECT_EQ(cu->password, "my secret password");
}

// -- ALTER USER password unquoting --------------------------------------------

TEST(QA_GDB217_Password, AlterUserSimplePassword) {
    auto stmt = parse_one("ALTER USER admin WITH PASSWORD 'newpass'");
    auto* au = dynamic_cast<AlterUserStmt*>(stmt.get());
    ASSERT_NE(au, nullptr);
    EXPECT_EQ(au->username, "admin");
    EXPECT_EQ(au->password, "newpass");
}

TEST(QA_GDB217_Password, AlterUserPasswordNoQuotes) {
    auto stmt = parse_one("ALTER USER bob WITH PASSWORD 'changed'");
    auto* au = dynamic_cast<AlterUserStmt*>(stmt.get());
    ASSERT_NE(au, nullptr);
    EXPECT_NE(au->password.front(), '\'');
    EXPECT_NE(au->password.back(), '\'');
    EXPECT_EQ(au->password, "changed");
}

TEST(QA_GDB217_Password, AlterUserEscapedQuote) {
    auto stmt = parse_one("ALTER USER admin WITH PASSWORD 'can''t guess'");
    auto* au = dynamic_cast<AlterUserStmt*>(stmt.get());
    ASSERT_NE(au, nullptr);
    EXPECT_EQ(au->password, "can't guess");
}

TEST(QA_GDB217_Password, AlterUserEmptyPassword) {
    auto stmt = parse_one("ALTER USER admin WITH PASSWORD ''");
    auto* au = dynamic_cast<AlterUserStmt*>(stmt.get());
    ASSERT_NE(au, nullptr);
    EXPECT_EQ(au->password, "");
}

TEST(QA_GDB217_Password, AlterUserMultipleEscapedQuotes) {
    auto stmt = parse_one("ALTER USER admin WITH PASSWORD 'x''y''z'");
    auto* au = dynamic_cast<AlterUserStmt*>(stmt.get());
    ASSERT_NE(au, nullptr);
    EXPECT_EQ(au->password, "x'y'z");
}
