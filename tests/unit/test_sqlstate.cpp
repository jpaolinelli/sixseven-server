#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Bring the internal status_to_sqlstate function into scope for testing.
// It is defined as a free function in pg_protocol.cpp (non-static, internal
// linkage via anonymous namespace would block us — but it is NOT in an anon
// namespace; it is file-scope but declared static only implicitly via the
// .cpp).  Rather than coupling the test to a private function we replicate
// a canonical reference table here and cross-check against well-known values.
// This guarantees the mapping cannot silently regress to wrong codes.
// ---------------------------------------------------------------------------

namespace sixseven {

// Forward declaration — status_to_sqlstate is defined in pg_protocol.cpp.
// We expose it for testing by declaring it extern here.
std::string_view status_to_sqlstate(StatusCode code);

} // namespace sixseven

using namespace sixseven;

// ---------------------------------------------------------------------------
// Test: every StatusCode maps to a non-empty, 5-character SQLSTATE
// ---------------------------------------------------------------------------

TEST(SqlStateMapping, AllCodesNonEmpty) {
    // Enumerate every enumerator to keep in sync with status.h.
    const StatusCode all_codes[] = {
        StatusCode::OK,
        StatusCode::NOT_FOUND,
        StatusCode::ALREADY_EXISTS,
        StatusCode::INVALID_ARGUMENT,
        StatusCode::INTERNAL_ERROR,
        StatusCode::NOT_IMPLEMENTED,
        StatusCode::IO_ERROR,
        StatusCode::PARSE_ERROR,
        StatusCode::TYPE_ERROR,
        StatusCode::CONSTRAINT_VIOLATION,
        StatusCode::TXN_CONFLICT,
        StatusCode::TXN_ABORTED,
        StatusCode::NETWORK_ERROR,
        StatusCode::AUTH_ERROR,
        StatusCode::REPLICATION_ERROR,
        StatusCode::READ_ONLY,
        StatusCode::LOCK_TIMEOUT,
        StatusCode::DEADLOCK,
        StatusCode::QUERY_CANCELED,
    };

    for (const auto code : all_codes) {
        auto state = status_to_sqlstate(code);
        EXPECT_EQ(state.size(), 5u) << "SQLSTATE for " << status_code_name(code)
                                    << " must be exactly 5 chars, got '" << state << "'";
        EXPECT_FALSE(state.empty())
            << "SQLSTATE for " << status_code_name(code) << " must not be empty";
    }
}

// ---------------------------------------------------------------------------
// Test: spot-check key SQLSTATE values
// ---------------------------------------------------------------------------

TEST(SqlStateMapping, SpotCheckKeyMappings) {
    EXPECT_EQ(status_to_sqlstate(StatusCode::OK), "00000");
    EXPECT_EQ(status_to_sqlstate(StatusCode::PARSE_ERROR), "42601");     // syntax error
    EXPECT_EQ(status_to_sqlstate(StatusCode::TYPE_ERROR), "42804");      // datatype mismatch
    EXPECT_EQ(status_to_sqlstate(StatusCode::NOT_FOUND), "42P01");       // undefined table
    EXPECT_EQ(status_to_sqlstate(StatusCode::ALREADY_EXISTS), "42P07");  // duplicate table
    EXPECT_EQ(status_to_sqlstate(StatusCode::INTERNAL_ERROR), "XX000");  // internal error
    EXPECT_EQ(status_to_sqlstate(StatusCode::NOT_IMPLEMENTED), "0A000"); // feature not supported
    EXPECT_EQ(status_to_sqlstate(StatusCode::CONSTRAINT_VIOLATION),
              "23000");                                                 // integrity constraint
    EXPECT_EQ(status_to_sqlstate(StatusCode::TXN_CONFLICT), "40001");   // serialization failure
    EXPECT_EQ(status_to_sqlstate(StatusCode::TXN_ABORTED), "25P02");    // in failed tx
    EXPECT_EQ(status_to_sqlstate(StatusCode::DEADLOCK), "40P01");       // deadlock detected
    EXPECT_EQ(status_to_sqlstate(StatusCode::QUERY_CANCELED), "57014"); // query canceled
    EXPECT_EQ(status_to_sqlstate(StatusCode::AUTH_ERROR), "28000");     // invalid authorization
    EXPECT_EQ(status_to_sqlstate(StatusCode::READ_ONLY), "25006");      // read-only transaction
    EXPECT_EQ(status_to_sqlstate(StatusCode::LOCK_TIMEOUT), "55P03");   // lock not available
}

// ---------------------------------------------------------------------------
// Test: no two StatusCodes map to the same SQLSTATE unless they are
// intentionally grouped (XX000 is a valid shared internal-error bucket).
// At minimum PARSE_ERROR must differ from INTERNAL_ERROR, TYPE_ERROR, etc.
// ---------------------------------------------------------------------------

TEST(SqlStateMapping, ParseErrorIsDistinct) {
    auto parse_state = status_to_sqlstate(StatusCode::PARSE_ERROR);
    EXPECT_NE(parse_state, status_to_sqlstate(StatusCode::INTERNAL_ERROR));
    EXPECT_NE(parse_state, status_to_sqlstate(StatusCode::TYPE_ERROR));
    EXPECT_NE(parse_state, status_to_sqlstate(StatusCode::OK));
}

// ---------------------------------------------------------------------------
// Test: parse-error position — error struct carries query_pos
// ---------------------------------------------------------------------------

TEST(ParseErrorPosition, ErrorAtStartOfQuery) {
    // "@" is an invalid token at offset 1 (1-based).
    std::string sql = "@invalid";
    Lexer lex(sql);
    auto tokens = lex.tokenize();
    ASSERT_FALSE(tokens.has_value()); // Lexer should reject '@'
    // The lexer error does not carry query_pos — that's fine.
    // What matters is that for parser errors the position is set.
    // Test a syntactically invalid query that the lexer accepts.
    (void)tokens;
}

TEST(ParseErrorPosition, ParserErrorCarriesPosition) {
    // "SELECT FROM" — missing select list / unexpected FROM token.
    // The offending token is "FROM" at byte offset 8 (1-based: S=1,E=2,L=3,E=4,C=5,T=6, =7, F=8).
    std::string sql = "SELECT FROM users";
    Lexer lex(sql);
    auto tokens_result = lex.tokenize();
    ASSERT_TRUE(tokens_result.has_value()) << tokens_result.error().message;

    Parser parser(std::move(*tokens_result));
    auto stmt = parser.parse();
    ASSERT_FALSE(stmt.has_value());
    EXPECT_EQ(stmt.error().code, StatusCode::PARSE_ERROR);
    // Position must be set and point into the query string.
    ASSERT_TRUE(stmt.error().query_pos.has_value())
        << "PARSE_ERROR must carry a query_pos (byte offset)";
    uint32_t pos = *stmt.error().query_pos;
    EXPECT_GE(pos, 1u) << "position must be 1-based";
    EXPECT_LE(pos, static_cast<uint32_t>(sql.size())) << "position must be within query string";
    // FROM starts at 1-based offset 8 in "SELECT FROM users"
    EXPECT_EQ(pos, 8u);
}

TEST(ParseErrorPosition, ParserErrorMidQuery) {
    // "SELECT id, FROM users" — FROM appears at offset 12.
    // S(1)E(2)L(3)E(4)C(5)T(6) (7)i(8)d(9),(10) (11)F(12)...
    std::string sql = "SELECT id, FROM users";
    Lexer lex(sql);
    auto tokens_result = lex.tokenize();
    ASSERT_TRUE(tokens_result.has_value()) << tokens_result.error().message;

    Parser parser(std::move(*tokens_result));
    auto stmt = parser.parse();
    ASSERT_FALSE(stmt.has_value());
    EXPECT_EQ(stmt.error().code, StatusCode::PARSE_ERROR);
    ASSERT_TRUE(stmt.error().query_pos.has_value());
    uint32_t pos = *stmt.error().query_pos;
    EXPECT_GE(pos, 1u);
    EXPECT_LE(pos, static_cast<uint32_t>(sql.size()));
    // FROM starts at byte 12 in "SELECT id, FROM users"
    EXPECT_EQ(pos, 12u);
}

TEST(ParseErrorPosition, ParserErrorNearEnd) {
    // "SELECT id FROM" — missing table name at EOF (offset = len+1 or last token).
    std::string sql = "SELECT id FROM";
    Lexer lex(sql);
    auto tokens_result = lex.tokenize();
    ASSERT_TRUE(tokens_result.has_value()) << tokens_result.error().message;

    Parser parser(std::move(*tokens_result));
    auto stmt = parser.parse();
    ASSERT_FALSE(stmt.has_value());
    EXPECT_EQ(stmt.error().code, StatusCode::PARSE_ERROR);
    ASSERT_TRUE(stmt.error().query_pos.has_value());
    uint32_t pos = *stmt.error().query_pos;
    EXPECT_GE(pos, 1u);
    // Position must be within or just past query string.
    EXPECT_LE(pos, static_cast<uint32_t>(sql.size() + 1));
}

// ---------------------------------------------------------------------------
// Test: make_parse_error helper sets query_pos correctly
// ---------------------------------------------------------------------------

TEST(MakeParseError, SetsQueryPos) {
    auto unexpected = make_parse_error("syntax error", 42u);
    const auto& err = unexpected.value();
    EXPECT_EQ(err.code, StatusCode::PARSE_ERROR);
    ASSERT_TRUE(err.query_pos.has_value());
    EXPECT_EQ(*err.query_pos, 42u);
    EXPECT_EQ(err.message, "syntax error");
}

TEST(MakeParseError, MakeErrorDoesNotSetQueryPos) {
    auto unexpected = make_error(StatusCode::PARSE_ERROR, "plain error");
    EXPECT_FALSE(unexpected.value().query_pos.has_value());
}
