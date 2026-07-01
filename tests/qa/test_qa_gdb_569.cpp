/// QA adversarial tests for GDB-569: psqlODBC Integration Testing.
/// Tests ParamRef parsing, SELECT-without-FROM, unqualified pg_catalog
/// table resolution, new session variables, and OID 0 parameter formatting.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/logging.h"
#include "sixseven/common/result.h"
#include "sixseven/executor/pg_catalog_tables.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/server/session.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace sixseven;

// =============================================================================
// Helpers
// =============================================================================

static void init_test_catalog(Catalog& catalog) {
    auto r = catalog.restore_database(default_database_id, "demo");
    (void)r;
}

// =============================================================================
// Query Engine fixture (reused across multiple test groups)
// =============================================================================

class QA_GDB569_Engine : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        register_pg_catalog_tables(catalog_);
        data_dir_ = fs::temp_directory_path() / "sixseven_qa_gdb569";
        fs::remove_all(data_dir_);
        fs::create_directories(data_dir_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        fs::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value());
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected);
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    fs::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// 1. ParamRef lexer edge cases
// =============================================================================

TEST(QA_GDB569_Lexer, ParamRefBasic) {
    Lexer lexer("$1");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_GE(tokens->size(), 1u);
    EXPECT_EQ((*tokens)[0].type, TokenType::PARAM_REF);
    EXPECT_EQ((*tokens)[0].lexeme, "$1");
}

TEST(QA_GDB569_Lexer, ParamRefLargeIndex) {
    Lexer lexer("$999");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_GE(tokens->size(), 1u);
    EXPECT_EQ((*tokens)[0].type, TokenType::PARAM_REF);
    EXPECT_EQ((*tokens)[0].lexeme, "$999");
}

TEST(QA_GDB569_Lexer, ParamRefZero) {
    Lexer lexer("$0");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_GE(tokens->size(), 1u);
    EXPECT_EQ((*tokens)[0].type, TokenType::PARAM_REF);
    EXPECT_EQ((*tokens)[0].lexeme, "$0");
}

TEST(QA_GDB569_Lexer, DollarSignAloneIsNotParamRef) {
    Lexer lexer("$ foo");
    auto tokens = lexer.tokenize();
    // Should either error or not produce PARAM_REF.
    if (tokens.has_value() && !tokens->empty()) {
        EXPECT_NE((*tokens)[0].type, TokenType::PARAM_REF);
    }
}

TEST(QA_GDB569_Lexer, ParamRefFollowedByIdentifier) {
    Lexer lexer("$1abc");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_GE(tokens->size(), 2u);
    EXPECT_EQ((*tokens)[0].type, TokenType::PARAM_REF);
    EXPECT_EQ((*tokens)[0].lexeme, "$1");
    EXPECT_EQ((*tokens)[1].type, TokenType::IDENTIFIER);
}

TEST(QA_GDB569_Lexer, MultipleParamRefs) {
    Lexer lexer("$1 $2 $3");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    ASSERT_GE(tokens->size(), 3u);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ((*tokens)[i].type, TokenType::PARAM_REF);
    }
    EXPECT_EQ((*tokens)[0].lexeme, "$1");
    EXPECT_EQ((*tokens)[1].lexeme, "$2");
    EXPECT_EQ((*tokens)[2].lexeme, "$3");
}

// =============================================================================
// 2. ParamRef parser edge cases
// =============================================================================

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

TEST(QA_GDB569_Parser, ParseParamRefInWhere) {
    auto stmt = parse_one("SELECT * FROM t WHERE x = $1");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(sel->where_expr.get());
    ASSERT_NE(bin, nullptr);
    auto* param = dynamic_cast<ParamRefExpr*>(bin->rhs.get());
    ASSERT_NE(param, nullptr);
    EXPECT_EQ(param->index, 1);
}

TEST(QA_GDB569_Parser, ParseParamRefInSelectList) {
    auto stmt = parse_one("SELECT $1 AS val");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->items.size(), 1u);
    auto* param = dynamic_cast<ParamRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(param, nullptr);
    EXPECT_EQ(param->index, 1);
}

TEST(QA_GDB569_Parser, ParseParamRefMultipleInWhereClause) {
    auto stmts = parse_ok("SELECT * FROM t WHERE a = $1 AND b > $2 AND c < $3");
    ASSERT_EQ(stmts.size(), 1u);
}

TEST(QA_GDB569_Parser, ParseParamRefIndex0) {
    auto stmt = parse_one("SELECT $0");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    auto* param = dynamic_cast<ParamRefExpr*>(sel->items[0].expr.get());
    ASSERT_NE(param, nullptr);
    EXPECT_EQ(param->index, 0);
}

// =============================================================================
// 3. SELECT without FROM — query engine execution
// =============================================================================

TEST_F(QA_GDB569_Engine, SelectIntegerLiteral) {
    auto qr = exec_ok("SELECT 1 AS val");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
}

TEST_F(QA_GDB569_Engine, SelectStringLiteral) {
    auto qr = exec_ok("SELECT 'hello world' AS msg");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "hello world");
}

TEST_F(QA_GDB569_Engine, SelectBoolLiteralTrue) {
    auto qr = exec_ok("SELECT TRUE AS flag");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_TRUE(qr.rows[0][0].as_bool());
}

TEST_F(QA_GDB569_Engine, SelectBoolLiteralFalse) {
    auto qr = exec_ok("SELECT FALSE AS flag");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_FALSE(qr.rows[0][0].as_bool());
}

TEST_F(QA_GDB569_Engine, SelectNullLiteral) {
    auto qr = exec_ok("SELECT NULL AS nothing");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_TRUE(qr.rows[0][0].is_null());
}

TEST_F(QA_GDB569_Engine, SelectMultipleLiterals) {
    auto qr = exec_ok("SELECT 42 AS num, 'text' AS str, TRUE AS b");
    ASSERT_EQ(qr.rows.size(), 1u);
    ASSERT_EQ(qr.column_names.size(), 3u);
    EXPECT_EQ(qr.column_names[0], "num");
    EXPECT_EQ(qr.column_names[1], "str");
    EXPECT_EQ(qr.column_names[2], "b");
    EXPECT_EQ(qr.rows[0][0].as_int32(), 42);
    EXPECT_EQ(qr.rows[0][1].as_string(), "text");
    EXPECT_TRUE(qr.rows[0][2].as_bool());
}

TEST_F(QA_GDB569_Engine, SelectFloatLiteral) {
    auto qr = exec_ok("SELECT 3.14 AS pi");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_NEAR(qr.rows[0][0].as_float64(), 3.14, 0.001);
}

TEST_F(QA_GDB569_Engine, SelectLiteralWithoutAlias) {
    auto qr = exec_ok("SELECT 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "1");
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
}

TEST_F(QA_GDB569_Engine, SelectNegativeInteger) {
    // GDB-661 fixed the unary-minus fast path; SELECT -5 must now succeed.
    auto qr = exec_ok("SELECT -5 AS neg");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), -5);
}

TEST_F(QA_GDB569_Engine, SelectEmptyString) {
    auto qr = exec_ok("SELECT '' AS empty");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "");
}

// =============================================================================
// 4. Unqualified pg_catalog table resolution
// =============================================================================

TEST_F(QA_GDB569_Engine, SelectFromPgTypeUnqualified) {
    auto qr = exec_ok("SELECT typname FROM pg_type");
    EXPECT_GE(qr.rows.size(), 1u);
}

TEST_F(QA_GDB569_Engine, SelectFromPgClassUnqualified) {
    // Verify unqualified pg_class resolves without error.
    auto result = engine_->execute("SELECT relname FROM pg_class");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // pg_class may return 0 rows if no user tables are registered in the
    // virtual table's captured catalog state — that's acceptable here.
}

TEST_F(QA_GDB569_Engine, SelectFromPgCatalogQualifiedStillWorks) {
    auto qr = exec_ok("SELECT typname FROM pg_catalog.pg_type");
    EXPECT_GE(qr.rows.size(), 1u);
}

TEST_F(QA_GDB569_Engine, SelectFromNonExistentTableStillErrors) {
    auto result = engine_->execute("SELECT * FROM no_such_table_xyz");
    EXPECT_FALSE(result.has_value());
}

TEST_F(QA_GDB569_Engine, SelectFromPgAttributeUnqualified) {
    // pg_attribute is registered by register_pg_catalog_tables() in SetUp, so
    // an unqualified reference must resolve and the query must succeed -- the
    // same contract the sibling SelectFromPgClassUnqualified test asserts. The
    // previous version discarded the result and only checked "no crash", which
    // passed even if unqualified pg_catalog resolution regressed.
    auto result = engine_->execute("SELECT attname FROM pg_attribute");
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

// =============================================================================
// 5. Session variables — psqlODBC compatibility
// =============================================================================

TEST(QA_GDB569_Session, NewSessionVariablesExist) {
    Session session(1);
    std::vector<std::string> expected = {
        "datestyle",
        "client_encoding",
        "extra_float_digits",
        "application_name",
        "lc_messages",
        "lc_monetary",
        "lc_numeric",
        "lc_time",
        "timezone",
        "intervalstyle",
        "standard_conforming_strings",
        "bytea_output",
    };
    for (const auto& var : expected) {
        auto val = session.get_variable(var);
        EXPECT_TRUE(val.has_value()) << "Missing session variable: " << var;
    }
}

TEST(QA_GDB569_Session, DateStyleDefault) {
    Session session(1);
    auto val = session.get_variable("datestyle");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "ISO, MDY");
}

TEST(QA_GDB569_Session, ClientEncodingDefault) {
    Session session(1);
    auto val = session.get_variable("client_encoding");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "UTF8");
}

TEST(QA_GDB569_Session, SetNewSessionVariable) {
    Session session(1);
    auto r = session.set_variable("timezone", "America/New_York");
    EXPECT_TRUE(r.has_value());
    auto val = session.get_variable("timezone");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "America/New_York");
}

TEST(QA_GDB569_Session, ResetNewSessionVariable) {
    Session session(1);
    (void)session.set_variable("timezone", "America/New_York");
    auto r = session.reset_variable("timezone");
    EXPECT_TRUE(r.has_value());
    auto val = session.get_variable("timezone");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "UTC");
}

TEST(QA_GDB569_Session, TotalVariableCount) {
    Session session(1);
    auto vars = session.get_all_variables();
    EXPECT_EQ(vars.size(), 18u);
}

TEST(QA_GDB569_Session, GetAllVariablesAreSorted) {
    Session session(1);
    auto vars = session.get_all_variables();
    for (size_t i = 1; i < vars.size(); ++i) {
        EXPECT_LE(vars[i - 1].first, vars[i].first)
            << "Variables not sorted at index " << i;
    }
}

TEST(QA_GDB569_Session, StandardConformingStringsDefault) {
    Session session(1);
    auto val = session.get_variable("standard_conforming_strings");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "on");
}

TEST(QA_GDB569_Session, IntervalStyleDefault) {
    Session session(1);
    auto val = session.get_variable("intervalstyle");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "postgres");
}

TEST(QA_GDB569_Session, ExtraFloatDigitsDefault) {
    Session session(1);
    auto val = session.get_variable("extra_float_digits");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "3");
}

// =============================================================================
// 6. SELECT without FROM — column type verification
// =============================================================================

TEST_F(QA_GDB569_Engine, SelectLiteralColumnTypes) {
    auto qr = exec_ok("SELECT 42 AS num, 'text' AS str, TRUE AS b, NULL AS n");
    ASSERT_EQ(qr.column_types.size(), 4u);
    EXPECT_EQ(qr.column_types[0], TypeId::INT32);
    EXPECT_EQ(qr.column_types[1], TypeId::STRING);
    EXPECT_EQ(qr.column_types[2], TypeId::BOOL);
    EXPECT_EQ(qr.column_types[3], TypeId::STRING);
}

TEST_F(QA_GDB569_Engine, SelectFloatLiteralType) {
    auto qr = exec_ok("SELECT 1.5 AS f");
    ASSERT_EQ(qr.column_types.size(), 1u);
    EXPECT_EQ(qr.column_types[0], TypeId::FLOAT64);
}

// =============================================================================
// 7. Stress: rapid successive SELECT-without-FROM queries
// =============================================================================

TEST_F(QA_GDB569_Engine, StressSelectLiterals) {
    for (int i = 0; i < 100; ++i) {
        auto qr = exec_ok("SELECT " + std::to_string(i) + " AS val");
        ASSERT_EQ(qr.rows.size(), 1u);
        EXPECT_EQ(qr.rows[0][0].as_int32(), i);
    }
}

// =============================================================================
// 8. Unqualified pg_catalog with CREATE TABLE — no name collision
// =============================================================================

TEST_F(QA_GDB569_Engine, UserTableShadowsPgCatalog) {
    exec_ok("CREATE TABLE pg_type (id INT, custom_col VARCHAR)");
    exec_ok("INSERT INTO pg_type VALUES (999, 'user_data')");
    auto qr = exec_ok("SELECT id FROM pg_type WHERE id = 999");
    // The user table should shadow the virtual pg_catalog table.
    bool found_user_data = false;
    for (const auto& row : qr.rows) {
        if (row[0].as_int32() == 999) {
            found_user_data = true;
        }
    }
    EXPECT_TRUE(found_user_data) << "User table should be accessible as pg_type";
}

// =============================================================================
// 9. Describe with ParamRef
// =============================================================================

TEST_F(QA_GDB569_Engine, DescribeWithParamRef) {
    exec_ok("CREATE TABLE users (id INT, name VARCHAR)");
    auto result = engine_->describe("SELECT name FROM users WHERE id = $1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_GE(result->size(), 1u);
}
