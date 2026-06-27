#pragma once

#include "sixseven/catalog/catalog.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/binder.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace sixseven {

/// Base fixture for binder tests. Holds the catalog and binder, and exposes
/// the three shared helpers: parse, bind_ok, bind_error.
///
/// Derived fixtures must implement SetUp() to construct `binder` (and
/// optionally register tables/algorithms into `catalog`). No SetUp() is
/// defined here so each fixture can pass different arguments to Binder.
class BinderTestBase : public ::testing::Test {
protected:
    Catalog catalog;
    std::unique_ptr<Binder> binder;

    /// Parse a SQL string into a Stmt, assert success.
    StmtPtr parse(const std::string& sql) {
        Lexer lexer(sql);
        auto tokens = lexer.tokenize();
        if (!tokens.has_value()) {
            ADD_FAILURE() << "Lex failed: " << tokens.error().message;
            return nullptr;
        }
        Parser parser(std::move(*tokens));
        auto result = parser.parse();
        if (!result.has_value()) {
            ADD_FAILURE() << "Parse failed: " << result.error().message;
            return nullptr;
        }
        return std::move(*result);
    }

    /// Parse and bind, assert success. Returns default-constructed BoundStatement on failure.
    BoundStatement bind_ok(const std::string& sql) {
        auto stmt = parse(sql);
        if (!stmt) {
            return {};
        }
        auto result = binder->bind(*stmt);
        if (!result.has_value()) {
            ADD_FAILURE() << "Bind failed: " << result.error().message;
            return {};
        }
        return std::move(*result);
    }

    /// Parse and bind, assert failure with expected StatusCode.
    void bind_error(const std::string& sql, StatusCode expected) {
        auto stmt = parse(sql);
        if (!stmt) {
            ADD_FAILURE() << "Parse failed unexpectedly";
            return;
        }
        auto result = binder->bind(*stmt);
        EXPECT_FALSE(result.has_value()) << "Expected bind error but succeeded";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected)
                << "Expected " << status_code_name(expected) << " but got "
                << status_code_name(result.error().code) << ": " << result.error().message;
        }
    }
};

} // namespace sixseven
