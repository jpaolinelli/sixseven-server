#pragma once
/// @file parser_qa_helpers.h
/// Shared parser test helpers for QA suites GDB-103/104/105/106.
///
/// Provides inline free-function helpers so each translation unit gets its own
/// copy without ODR violations.

#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"

#include <gtest/gtest.h>

#include <vector>

using namespace sixseven;

inline std::vector<StmtPtr> parse_ok(std::string_view sql) {
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

inline StmtPtr parse_one(std::string_view sql) {
    auto stmts = parse_ok(sql);
    EXPECT_EQ(stmts.size(), 1u);
    if (stmts.size() != 1)
        return nullptr;
    return std::move(stmts[0]);
}

inline void expect_parse_error(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return; // lexer error is also acceptable
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_FALSE(stmts.has_value()) << "expected parse error for: " << sql;
}

inline Result<std::vector<StmtPtr>> parse_result(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return tl::unexpected(tokens.error());
    Parser parser(std::move(*tokens));
    return parser.parse_all();
}
