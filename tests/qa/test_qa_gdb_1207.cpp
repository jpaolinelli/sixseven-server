// QA regression tests for GDB-1207.
//
// GDB-1207 removed a provably-vacuous conjunct from the LINK bulk-form
// disambiguation in Parser::parse_link():
//
//   Old: if (match_ident_ci(peek(), "TO") && !check(TokenType::LPAREN))
//   New: if (match_ident_ci(peek(), "TO"))
//
// The claim under test: match_ident_ci(tok, "TO") can only be true when
// tok.type == TokenType::IDENTIFIER (with lexeme "TO", case-insensitive),
// and check(TokenType::LPAREN) can only be true when tok.type ==
// TokenType::LPAREN. Both predicates inspect the *same* current token
// (peek()), and a single token cannot simultaneously have type IDENTIFIER
// and type LPAREN. Therefore whenever the first conjunct held, the second
// was already guaranteed to hold, and removing it cannot change behavior
// for ANY input.
//
// This file adversarially probes the disambiguation boundary: every token
// type that can legally (or illegally) follow the LINK source-table name,
// keyword-vs-identifier edge cases for "TO", malformed statements, and
// end-to-end execution sanity so that if a real divergence exists, it
// shows up as a parse-shape or crash difference.

#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"

#include <gtest/gtest.h>

using namespace sixseven;

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

// Returns true if parsing failed (either at the lexer or parser stage).
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

// -- Core disambiguation matrix ------------------------------------------------

TEST(QA_GDB1207, BulkFormTableToTableParsesAsBulkLinkStmt) {
    auto stmt = parse_one("LINK accounts TO accounts VIA linked_to VALUES ('x', 'y')");
    auto* blk = dynamic_cast<BulkLinkStmt*>(stmt.get());
    ASSERT_NE(blk, nullptr) << "LINK table TO ... must parse as BulkLinkStmt";
    EXPECT_EQ(blk->source_table, "accounts");
    EXPECT_EQ(blk->target_table, "accounts");
    EXPECT_EQ(blk->edge_type, "linked_to");
}

TEST(QA_GDB1207, SingleFormTableParenKeyParensesAsLinkStmt) {
    auto stmt = parse_one("LINK accounts(1) TO accounts(2) VIA linked_to");
    auto* lnk = dynamic_cast<LinkStmt*>(stmt.get());
    ASSERT_NE(lnk, nullptr) << "LINK table(key) TO ... must parse as LinkStmt";
    EXPECT_EQ(lnk->source_table, "accounts");
    EXPECT_EQ(lnk->target_table, "accounts");
}

// The old guarded conjunct `!check(TokenType::LPAREN)` would only ever have
// had a chance to matter if the token immediately after the source table
// name could be simultaneously an IDENTIFIER("TO") and an LPAREN. That is
// impossible by the lexer's token model, but adversarially probe every
// token kind that can appear right after the source table name to make
// sure the disambiguation still routes correctly in all of them.

TEST(QA_GDB1207, TokenAfterSourceTableIsLparenRoutesToSingleForm) {
    // '(' immediately after source table name -- must NOT be treated as bulk.
    auto stmt = parse_one("LINK accounts(1) TO accounts(2) VIA follows");
    EXPECT_NE(dynamic_cast<LinkStmt*>(stmt.get()), nullptr);
    EXPECT_EQ(dynamic_cast<BulkLinkStmt*>(stmt.get()), nullptr);
}

TEST(QA_GDB1207, TokenAfterSourceTableIsToIdentifierRoutesToBulkForm) {
    // Bare identifier "TO" immediately after source table name -- must be bulk.
    auto stmt = parse_one("LINK accounts TO accounts VIA follows VALUES (1, 2)");
    EXPECT_NE(dynamic_cast<BulkLinkStmt*>(stmt.get()), nullptr);
    EXPECT_EQ(dynamic_cast<LinkStmt*>(stmt.get()), nullptr);
}

TEST(QA_GDB1207, ToIsCaseInsensitiveInBulkForm) {
    for (auto kw : {"to", "To", "tO", "TO"}) {
        std::string sql = std::string("LINK accounts ") + kw +
                           " accounts VIA follows VALUES (1, 2)";
        auto stmt = parse_one(sql);
        auto* blk = dynamic_cast<BulkLinkStmt*>(stmt.get());
        ASSERT_NE(blk, nullptr) << "case-insensitive TO failed for: " << kw;
    }
}

TEST(QA_GDB1207, ToIsCaseInsensitiveInSingleForm) {
    for (auto kw : {"to", "To", "tO", "TO"}) {
        std::string sql = std::string("LINK accounts(1) ") + kw + " accounts(2) VIA follows";
        auto stmt = parse_one(sql);
        auto* lnk = dynamic_cast<LinkStmt*>(stmt.get());
        ASSERT_NE(lnk, nullptr) << "case-insensitive TO failed for: " << kw;
    }
}

// -- Malformed / adjacent-token adversarial cases ------------------------------

TEST(QA_GDB1207, MissingToAfterSourceTableIsParseError) {
    // Neither IDENTIFIER("TO") nor LPAREN follows -- must error, not crash.
    EXPECT_TRUE(parse_fails("LINK accounts VIA follows VALUES (1, 2)"));
}

TEST(QA_GDB1207, EmptyParensAfterSourceTableIsParseError) {
    // LPAREN routes to single-form key parsing; empty key expr should error
    // cleanly rather than crash.
    EXPECT_TRUE(parse_fails("LINK accounts() TO accounts(2) VIA follows"));
}

TEST(QA_GDB1207, ToLookingIdentifierWithTrailingCharsIsNotBulkKeyword) {
    // "TOO" is a different identifier lexeme; must not be mistaken for the
    // bulk-form TO. Expect a parse error (TOO is not a valid table-name
    // continuation in this grammar position), not a mis-route.
    EXPECT_TRUE(parse_fails("LINK accounts TOO accounts VIA follows VALUES (1, 2)"));
}

TEST(QA_GDB1207, DanglingLinkStatementDoesNotCrash) {
    EXPECT_TRUE(parse_fails("LINK"));
    EXPECT_TRUE(parse_fails("LINK accounts"));
    EXPECT_TRUE(parse_fails("LINK accounts TO"));
    EXPECT_TRUE(parse_fails("LINK accounts("));
    EXPECT_TRUE(parse_fails("LINK accounts TO accounts"));
    EXPECT_TRUE(parse_fails("LINK accounts TO accounts VIA"));
}

TEST(QA_GDB1207, BulkFormWithMissingViaIsParseError) {
    EXPECT_TRUE(parse_fails("LINK accounts TO accounts VALUES (1, 2)"));
}

TEST(QA_GDB1207, SingleFormWithMissingViaIsParseError) {
    EXPECT_TRUE(parse_fails("LINK accounts(1) TO accounts(2)"));
}

// -- Properties / VALUES permutations (unaffected surface area) ---------------

TEST(QA_GDB1207, SingleFormWithPropertiesStillParses) {
    auto stmt = parse_one(
        "LINK accounts(1) TO accounts(2) VIA linked_to (weight = 0.5, note = 'x')");
    auto* lnk = dynamic_cast<LinkStmt*>(stmt.get());
    ASSERT_NE(lnk, nullptr);
    ASSERT_EQ(lnk->properties.size(), 2u);
}

TEST(QA_GDB1207, BulkFormWithMultipleRowsStillParses) {
    auto stmt = parse_one(
        "LINK accounts TO accounts VIA linked_to VALUES (1, 2), (3, 4), (5, 6)");
    auto* blk = dynamic_cast<BulkLinkStmt*>(stmt.get());
    ASSERT_NE(blk, nullptr);
    ASSERT_EQ(blk->rows.size(), 3u);
}

TEST(QA_GDB1207, BulkFormDifferentSourceAndTargetTables) {
    auto stmt = parse_one("LINK users TO posts VIA authored VALUES ('alice', 1)");
    auto* blk = dynamic_cast<BulkLinkStmt*>(stmt.get());
    ASSERT_NE(blk, nullptr);
    EXPECT_EQ(blk->source_table, "users");
    EXPECT_EQ(blk->target_table, "posts");
}

// -- Stress: many sequential LINK statements of both forms ---------------------

TEST(QA_GDB1207, ManyMixedLinkStatementsAllRouteCorrectly) {
    std::string sql;
    for (int i = 0; i < 200; ++i) {
        if (i % 2 == 0) {
            sql += "LINK accounts TO accounts VIA follows VALUES (" + std::to_string(i) + ", " +
                   std::to_string(i + 1) + ");\n";
        } else {
            sql += "LINK accounts(" + std::to_string(i) + ") TO accounts(" +
                   std::to_string(i + 1) + ") VIA follows;\n";
        }
    }
    auto stmts = parse_ok(sql);
    ASSERT_EQ(stmts.size(), 200u);
    for (int i = 0; i < 200; ++i) {
        if (i % 2 == 0) {
            EXPECT_NE(dynamic_cast<BulkLinkStmt*>(stmts[static_cast<size_t>(i)].get()), nullptr)
                << "index " << i;
        } else {
            EXPECT_NE(dynamic_cast<LinkStmt*>(stmts[static_cast<size_t>(i)].get()), nullptr)
                << "index " << i;
        }
    }
}
