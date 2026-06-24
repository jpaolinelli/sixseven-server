/// @file test_qa_gdb_926.cpp
/// QA regression tests for GDB-926: bounds-checked integer parsing in the
/// parser (safe_stoll, safe_stou32, reuse of safe_stoi for hop quantifiers).
///
/// Verifies:
///   - Oversized literals now return PARSE_ERROR instead of throwing/crashing.
///   - Previously-truncating literals now return PARSE_ERROR.
///   - Valid in-range literals (including uint32 values > INT32_MAX) parse
///     with exactly the expected AST field values.

#include "sixseven/common/status.h"
#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace sixseven;

namespace {

// ============================================================================
// Parse helpers
// ============================================================================

/// Attempt to parse sql; return the result (may be error).
static Result<std::vector<StmtPtr>> try_parse(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens) {
        return tl::unexpected(tokens.error());
    }
    Parser parser(std::move(*tokens));
    return parser.parse_all();
}

/// Parse sql and assert it succeeds; return the single statement.
static StmtPtr parse_ok_one(std::string_view sql) {
    auto result = try_parse(sql);
    if (!result) {
        ADD_FAILURE() << "Expected parse success but got error: " << result.error().message
                      << " for SQL: " << sql;
        return nullptr;
    }
    if (result->size() != 1u) {
        ADD_FAILURE() << "Expected 1 statement, got " << result->size();
        return nullptr;
    }
    return std::move((*result)[0]);
}

/// Parse sql and assert it returns a PARSE_ERROR.
static void parse_expect_error(std::string_view sql) {
    auto result = try_parse(sql);
    // Lexer may reject it first (acceptable), otherwise parser must error.
    if (!result) {
        EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR)
            << "Expected PARSE_ERROR, got code " << static_cast<int>(result.error().code)
            << " message: " << result.error().message;
        return;
    }
    // If we got here, parse succeeded when it should have failed.
    ADD_FAILURE() << "Expected PARSE_ERROR but parse succeeded for SQL: " << sql;
}

/// Extract MatchStmt from a SELECT ... FROM MATCH ... statement.
static MatchStmt* match_from_select(const StmtPtr& stmt) {
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    if (!sel || sel->from.empty() || !sel->from[0].match_source) {
        return nullptr;
    }
    return dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
}

/// Extract BackfillStmt from a parsed statement.
static BackfillStmt* as_backfill(const StmtPtr& stmt) {
    return dynamic_cast<BackfillStmt*>(stmt.get());
}

/// Extract the WindowFunctionExpr from the first SELECT item of a SelectStmt.
static WindowFunctionExpr* window_func_from_select(const StmtPtr& stmt) {
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    if (!sel || sel->items.empty()) {
        return nullptr;
    }
    return dynamic_cast<WindowFunctionExpr*>(sel->items[0].expr.get());
}

} // namespace

// ============================================================================
// MATCH hop quantifier -- oversized literals must error (not crash/truncate)
// ============================================================================

TEST(QA_GDB926_HopQuantifier, OversizedMaxHops_ReturnsParseError) {
    // 99999999999999999999 overflows int64 -> std::out_of_range -> PARSE_ERROR.
    parse_expect_error("SELECT a.name FROM MATCH (a:p)-[r:e]->{1,99999999999999999999}(b:p)");
}

TEST(QA_GDB926_HopQuantifier, OversizedMinHops_ReturnsParseError) {
    parse_expect_error("SELECT a.name FROM MATCH (a:p)-[r:e]->{99999999999999999999,5}(b:p)");
}

TEST(QA_GDB926_HopQuantifier, TruncatedMaxHops_ReturnsParseError) {
    // 4294967297 = 2^32 + 1: fits int64 but not int32.
    // Previously: static_cast<int32_t>(std::stoll("4294967297")) == 1 (silent
    // truncation). Now: safe_stoi detects > INT32_MAX -> PARSE_ERROR.
    parse_expect_error("SELECT a.name FROM MATCH (a:p)-[r:e]->{1,4294967297}(b:p)");
}

TEST(QA_GDB926_HopQuantifier, TruncatedMinHops_ReturnsParseError) {
    // 2147483648 = INT32_MAX + 1: overflows int32.
    parse_expect_error("SELECT a.name FROM MATCH (a:p)-[r:e]->{2147483648,5}(b:p)");
}

// ============================================================================
// MATCH hop quantifier -- valid in-range literals parse with pinned values
// ============================================================================

TEST(QA_GDB926_HopQuantifier, ValidRange_OneToFive) {
    auto stmt = parse_ok_one("SELECT a.name FROM MATCH (a:p)-[r:e]->{1,5}(b:p)");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    ASSERT_TRUE(edge->min_hops.has_value());
    ASSERT_TRUE(edge->max_hops.has_value());
    EXPECT_EQ(*edge->min_hops, 1);
    EXPECT_EQ(*edge->max_hops, 5);
}

TEST(QA_GDB926_HopQuantifier, ValidExact_Two) {
    // {2} sets both min_hops and max_hops to 2.
    auto stmt = parse_ok_one("SELECT a.name FROM MATCH (a:p)-[r:e]->{2}(b:p)");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    ASSERT_TRUE(edge->min_hops.has_value());
    ASSERT_TRUE(edge->max_hops.has_value());
    EXPECT_EQ(*edge->min_hops, 2);
    EXPECT_EQ(*edge->max_hops, 2);
}

TEST(QA_GDB926_HopQuantifier, ValidRange_MaxInt32) {
    // INT32_MAX itself must still parse.
    auto stmt = parse_ok_one("SELECT a.name FROM MATCH (a:p)-[r:e]->{1,2147483647}(b:p)");
    auto* m = match_from_select(stmt);
    ASSERT_NE(m, nullptr);
    const auto& edge = m->pattern[0].outgoing_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(*edge->max_hops, 2147483647);
}

// ============================================================================
// BACKFILL BATCH -- oversized / truncating literals must error
// ============================================================================

TEST(QA_GDB926_Backfill, OversizedBatch_ReturnsParseError) {
    parse_expect_error("BACKFILL EMBEDDINGS ON t BATCH 99999999999999999999");
}

TEST(QA_GDB926_Backfill, TruncatedBatch_ReturnsParseError) {
    // 4294967296 = UINT32_MAX + 1: fits uint64 but not uint32.
    // Previously: std::stoul truncated/wrapped -> silent wrong value.
    // Now: safe_stou32 range-checks -> PARSE_ERROR.
    parse_expect_error("BACKFILL EMBEDDINGS ON t BATCH 4294967296");
}

TEST(QA_GDB926_Backfill, OversizedRateLimit_ReturnsParseError) {
    parse_expect_error("BACKFILL EMBEDDINGS ON t RATE_LIMIT 99999999999999999999");
}

TEST(QA_GDB926_Backfill, TruncatedRateLimit_ReturnsParseError) {
    parse_expect_error("BACKFILL EMBEDDINGS ON t RATE_LIMIT 4294967296");
}

// ============================================================================
// BACKFILL BATCH -- valid literals parse with pinned values
// ============================================================================

TEST(QA_GDB926_Backfill, ValidBatch_SmallValue) {
    auto stmt = parse_ok_one("BACKFILL EMBEDDINGS ON t BATCH 1000");
    auto* bf = as_backfill(stmt);
    ASSERT_NE(bf, nullptr);
    EXPECT_EQ(bf->batch_size, 1000u);
}

TEST(QA_GDB926_Backfill, ValidRateLimit_SmallValue) {
    auto stmt = parse_ok_one("BACKFILL EMBEDDINGS ON t RATE_LIMIT 500");
    auto* bf = as_backfill(stmt);
    ASSERT_NE(bf, nullptr);
    EXPECT_EQ(bf->rate_limit, 500u);
}

TEST(QA_GDB926_Backfill, ValidBatch_LargeUint32AboveInt32Max) {
    // 3000000000 > INT32_MAX but <= UINT32_MAX: must parse without error.
    // Guards against over-narrowing fix that coerces to int32.
    auto stmt = parse_ok_one("BACKFILL EMBEDDINGS ON t BATCH 3000000000");
    auto* bf = as_backfill(stmt);
    ASSERT_NE(bf, nullptr);
    EXPECT_EQ(bf->batch_size, 3000000000u);
}

TEST(QA_GDB926_Backfill, ValidBatch_Uint32Max) {
    // UINT32_MAX itself must parse successfully.
    auto stmt = parse_ok_one("BACKFILL EMBEDDINGS ON t BATCH 4294967295");
    auto* bf = as_backfill(stmt);
    ASSERT_NE(bf, nullptr);
    EXPECT_EQ(bf->batch_size, 4294967295u);
}

TEST(QA_GDB926_Backfill, ValidBatchAndRateLimit_Together) {
    auto stmt = parse_ok_one("BACKFILL EMBEDDINGS ON t BATCH 1000 RATE_LIMIT 500");
    auto* bf = as_backfill(stmt);
    ASSERT_NE(bf, nullptr);
    EXPECT_EQ(bf->batch_size, 1000u);
    EXPECT_EQ(bf->rate_limit, 500u);
}

// ============================================================================
// Window frame offset -- oversized literal must error
// ============================================================================

TEST(QA_GDB926_WindowFrame, OversizedOffset_ReturnsParseError) {
    parse_expect_error("SELECT SUM(v) OVER (ORDER BY id ROWS BETWEEN "
                       "99999999999999999999 PRECEDING AND CURRENT ROW) FROM t");
}

// ============================================================================
// Window frame offset -- valid literal parses with pinned value
// ============================================================================

TEST(QA_GDB926_WindowFrame, ValidOffset_FivePreceding) {
    // Verify start_offset is stored as exactly 5.
    auto stmt = parse_ok_one("SELECT SUM(v) OVER (ORDER BY id ROWS BETWEEN "
                             "5 PRECEDING AND CURRENT ROW) FROM t");
    auto* wf = window_func_from_select(stmt);
    ASSERT_NE(wf, nullptr);
    ASSERT_TRUE(wf->frame.has_value());
    EXPECT_EQ(wf->frame->start_bound, AstFrameBound::N_PRECEDING);
    EXPECT_EQ(wf->frame->start_offset, 5);
}

TEST(QA_GDB926_WindowFrame, ValidOffset_TwoFollowing) {
    auto stmt = parse_ok_one("SELECT SUM(v) OVER (ORDER BY id ROWS BETWEEN "
                             "CURRENT ROW AND 2 FOLLOWING) FROM t");
    auto* wf = window_func_from_select(stmt);
    ASSERT_NE(wf, nullptr);
    ASSERT_TRUE(wf->frame.has_value());
    EXPECT_EQ(wf->frame->end_bound, AstFrameBound::N_FOLLOWING);
    EXPECT_EQ(wf->frame->end_offset, 2);
}
