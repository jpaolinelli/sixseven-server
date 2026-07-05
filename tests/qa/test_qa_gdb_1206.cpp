#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace sixseven;

// =============================================================================
// GDB-1206 adversarial QA
// Double-quoted (delimited) identifier support, exercised through the FULL
// pipeline: lex -> parse -> bind -> plan -> execute -> catalog round-trip.
// =============================================================================

class QA_GDB1206 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_qa_gdb_1206";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        init_stack();
        run_bootstrap();
    }

    void TearDown() override {
        teardown_stack();
        std::filesystem::remove_all(data_dir_);
    }

    void init_stack() {
        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        config_ = Config::load_defaults();
    }

    void teardown_stack() {
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
    }

    void run_bootstrap() {
        auto result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (!result.has_value()) {
            ADD_FAILURE() << "exec_ok failed: " << sql << "\n  error: " << result.error().message;
            return QueryResult{};
        }
        return std::move(*result);
    }

    Result<QueryResult> exec(const std::string& sql) { return engine_->execute(sql); }

    void exec_should_fail(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "expected error but got success for: " << sql;
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    Config config_;
};

// -----------------------------------------------------------------------------
// 1) End-to-end DDL/DML/SELECT round-trip with spaces & mixed case.
// -----------------------------------------------------------------------------

TEST_F(QA_GDB1206, WeirdTableAndColumnNamesRoundTrip) {
    exec_ok(R"(CREATE TABLE "Weird Table" ("Col A" INT);)");
    exec_ok(R"(INSERT INTO "Weird Table" ("Col A") VALUES (42);)");

    auto qr = exec_ok(R"(SELECT "Col A" FROM "Weird Table";)");
    ASSERT_EQ(qr.column_names.size(), 1u);
    // Column name must NOT contain leaked literal quote characters.
    EXPECT_EQ(qr.column_names[0], "Col A");
    EXPECT_EQ(qr.column_names[0].find('"'), std::string::npos);
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 42);
}

TEST_F(QA_GDB1206, QuotedTableNameNoLeakedQuotesInCatalog) {
    exec_ok(R"(CREATE TABLE "Quoted Name" (id INT);)");
    // The stored table name must be exactly `Quoted Name`, not `"Quoted Name"`.
    // Verified indirectly through SQL: a query against the unquoted, unescaped
    // name must succeed, while a name containing literal quote characters
    // (which would only happen if the parser leaked quotes into the stored
    // name) must fail to resolve.
    auto ok_ref = exec(R"(SELECT * FROM "Quoted Name";)");
    ASSERT_TRUE(ok_ref.has_value()) << "table should be reachable by its unquoted, unescaped name: "
                                    << ok_ref.error().message;

    auto leaked_ref = exec("SELECT * FROM \"\"\"Quoted Name\"\"\";");
    EXPECT_FALSE(leaked_ref.has_value())
        << "table must not accidentally be stored under a name containing literal quote chars";
}

// -----------------------------------------------------------------------------
// 2) Case sensitivity: quoted vs unquoted must be CONSISTENT with each other.
// -----------------------------------------------------------------------------

TEST_F(QA_GDB1206, UnquotedAndQuotedSameCaseAreConsistent) {
    exec_ok("CREATE TABLE users (id INT);");
    exec_ok("INSERT INTO users (id) VALUES (1);");

    // Unquoted lower-case reference must succeed (table was created unquoted lower-case).
    auto unquoted = exec("SELECT * FROM users;");
    ASSERT_TRUE(unquoted.has_value()) << unquoted.error().message;

    // Quoted with the SAME case used at creation time must also succeed.
    auto quoted_same_case = exec(R"(SELECT * FROM "users";)");
    ASSERT_TRUE(quoted_same_case.has_value())
        << "quoted identifier with matching case must resolve to the same table";
}

TEST_F(QA_GDB1206, QuotedDifferentCaseDoesNotSilentlyMatch) {
    exec_ok("CREATE TABLE users (id INT);");

    // "USERS" (quoted, different case) must behave consistently with how the
    // engine treats unquoted different-case references. Since this engine's
    // identifiers are documented as case-preserving/case-sensitive throughout
    // (see identifier_text() in parser.cpp), an unquoted `USERS` must ALSO
    // fail to resolve to the `users` table -- verifying the two paths agree.
    auto quoted_upper = exec(R"(SELECT * FROM "USERS";)");
    auto unquoted_upper = exec("SELECT * FROM USERS;");

    // Both must produce the SAME outcome (both fail, or both succeed) --
    // a mismatch here is the case-sensitivity inconsistency bug we're hunting.
    EXPECT_EQ(quoted_upper.has_value(), unquoted_upper.has_value())
        << "quoted \"USERS\" and unquoted USERS disagree on whether they "
           "resolve to table `users` -- case-sensitivity inconsistency";

    // And per the engine's documented case-sensitive model, neither should
    // resolve to the lower-case `users` table.
    EXPECT_FALSE(quoted_upper.has_value())
        << "case-sensitive engine: \"USERS\" must not match table `users`";
}

TEST_F(QA_GDB1206, CreateTwoTablesDifferingOnlyByQuotedCase) {
    // If the engine is truly case-sensitive end-to-end, these are two
    // distinct tables and both creates should succeed without collision.
    exec_ok(R"(CREATE TABLE "Users" (id INT);)");
    auto second = exec(R"(CREATE TABLE "users" (id INT);)");
    ASSERT_TRUE(second.has_value())
        << "\"Users\" and \"users\" must be distinct tables under a "
           "case-sensitive identifier model: " << second.error().message;

    exec_ok(R"(INSERT INTO "Users" (id) VALUES (1);)");
    exec_ok(R"(INSERT INTO "users" (id) VALUES (2);)");

    auto r1 = exec_ok(R"(SELECT id FROM "Users";)");
    auto r2 = exec_ok(R"(SELECT id FROM "users";)");
    ASSERT_EQ(r1.rows.size(), 1u);
    ASSERT_EQ(r2.rows.size(), 1u);
    EXPECT_EQ(r1.rows[0][0].as_int32(), 1);
    EXPECT_EQ(r2.rows[0][0].as_int32(), 2);
}

// -----------------------------------------------------------------------------
// 3) Escaping: "" -> " ; quoted keyword as column name.
// -----------------------------------------------------------------------------

TEST_F(QA_GDB1206, EscapedDoubleQuoteInIdentifier) {
    // "a""""b" -> identifier `a""b` (two escaped-quote pairs = two literal quotes).
    exec_ok(R"(CREATE TABLE t_escape ("a""""b" INT);)");
    exec_ok(R"(INSERT INTO t_escape ("a""""b") VALUES (7);)");
    auto qr = exec_ok(R"(SELECT "a""""b" FROM t_escape;)");
    ASSERT_EQ(qr.column_names.size(), 1u);
    EXPECT_EQ(qr.column_names[0], "a\"\"b");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 7);
}

TEST_F(QA_GDB1206, QuotedKeywordAsColumnUsableInSelectWhereOrderBy) {
    // `select` and `order` are keywords; quoting must let them be column names.
    exec_ok(R"(CREATE TABLE t_kw ("select" INT, "order" INT);)");
    exec_ok(R"(INSERT INTO t_kw ("select", "order") VALUES (1, 3);)");
    exec_ok(R"(INSERT INTO t_kw ("select", "order") VALUES (2, 1);)");

    auto qr = exec_ok(R"(SELECT "select" FROM t_kw WHERE "order" = 1;)");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 2);

    auto ordered = exec_ok(R"(SELECT "select" FROM t_kw ORDER BY "order";)");
    ASSERT_EQ(ordered.rows.size(), 2u);
    EXPECT_EQ(ordered.rows[0][0].as_int32(), 2);
    EXPECT_EQ(ordered.rows[1][0].as_int32(), 1);
}

// -----------------------------------------------------------------------------
// 4) Qualified quoted names: "table"."column".
// -----------------------------------------------------------------------------

TEST_F(QA_GDB1206, QualifiedQuotedTableDotColumn) {
    exec_ok(R"(CREATE TABLE "My Table" ("My Col" INT);)");
    exec_ok(R"(INSERT INTO "My Table" ("My Col") VALUES (99);)");

    auto qr = exec(R"(SELECT "My Table"."My Col" FROM "My Table";)");
    ASSERT_TRUE(qr.has_value()) << "qualified quoted column reference should parse and execute: "
                                << qr.error().message;
    ASSERT_EQ(qr->rows.size(), 1u);
    EXPECT_EQ(qr->rows[0][0].as_int32(), 99);
}

// -----------------------------------------------------------------------------
// 5) Degenerate / malformed quoted identifiers -- must PARSE_ERROR, not crash.
// -----------------------------------------------------------------------------

TEST_F(QA_GDB1206, UnterminatedQuotedIdentifierIsCleanParseError) {
    auto r = exec(R"(SELECT "abc FROM t;)");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(QA_GDB1206, EmptyQuotedIdentifierIsCleanParseError) {
    auto r = exec(R"(SELECT "" FROM t;)");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(QA_GDB1206, LoneQuoteAtEofIsCleanParseError) {
    auto r = exec("SELECT \"");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(QA_GDB1206, LoneQuoteAtEofDirectLexer) {
    // Drive the lexer directly to make sure it never hangs/crashes on a
    // truncated quoted-identifier stream, independent of the parser.
    Lexer lexer("\"");
    auto tokens = lexer.tokenize();
    ASSERT_FALSE(tokens.has_value());
    EXPECT_EQ(tokens.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(QA_GDB1206, UnterminatedWithEscapesDoesNotHang) {
    // Trailing dangling escape-looking sequence: `"abc""` (odd number of
    // quotes after content) followed by EOF -- must still terminate cleanly.
    Lexer lexer("\"abc\"\"");
    auto tokens = lexer.tokenize();
    ASSERT_FALSE(tokens.has_value());
    EXPECT_EQ(tokens.error().code, StatusCode::PARSE_ERROR);
}

// -----------------------------------------------------------------------------
// 6) Interaction: quoted identifier as function name / GROUP BY / alias / JOIN ON.
// -----------------------------------------------------------------------------

TEST_F(QA_GDB1206, QuotedIdentifierAsAlias) {
    exec_ok("CREATE TABLE t_alias (v INT);");
    exec_ok("INSERT INTO t_alias (v) VALUES (5);");
    auto qr = exec(R"(SELECT v AS "My Alias" FROM t_alias;)");
    ASSERT_TRUE(qr.has_value()) << qr.error().message;
    ASSERT_EQ(qr->column_names.size(), 1u);
    EXPECT_EQ(qr->column_names[0], "My Alias");
}

TEST_F(QA_GDB1206, QuotedIdentifierAsTableAliasInJoinOn) {
    exec_ok("CREATE TABLE t_left (id INT, v INT);");
    exec_ok("CREATE TABLE t_right (id INT, w INT);");
    exec_ok("INSERT INTO t_left (id, v) VALUES (1, 10);");
    exec_ok("INSERT INTO t_right (id, w) VALUES (1, 20);");

    auto qr = exec(
        R"(SELECT "L".v, "R".w FROM t_left AS "L" JOIN t_right AS "R" ON "L".id = "R".id;)");
    ASSERT_TRUE(qr.has_value()) << qr.error().message;
    ASSERT_EQ(qr->rows.size(), 1u);
    EXPECT_EQ(qr->rows[0][0].as_int32(), 10);
    EXPECT_EQ(qr->rows[0][1].as_int32(), 20);
}

TEST_F(QA_GDB1206, QuotedIdentifierInGroupBy) {
    exec_ok(R"(CREATE TABLE t_group ("Grp" INT, v INT);)");
    exec_ok(R"(INSERT INTO t_group ("Grp", v) VALUES (1, 10);)");
    exec_ok(R"(INSERT INTO t_group ("Grp", v) VALUES (1, 20);)");
    exec_ok(R"(INSERT INTO t_group ("Grp", v) VALUES (2, 5);)");

    auto qr = exec(R"(SELECT "Grp", SUM(v) FROM t_group GROUP BY "Grp" ORDER BY "Grp";)");
    ASSERT_TRUE(qr.has_value()) << qr.error().message;
    ASSERT_EQ(qr->rows.size(), 2u);
    EXPECT_EQ(qr->rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr->rows[0][1].as_int64(), 30);
    EXPECT_EQ(qr->rows[1][0].as_int32(), 2);
    EXPECT_EQ(qr->rows[1][1].as_int64(), 5);
}

// Adversarial: a quoted identifier that looks like CAST(...) pseudo-syntax
// must NOT be misdetected as the CAST pseudo-function (per handoff, CAST
// detection is skipped for quoted identifiers -- verify that holds).
TEST_F(QA_GDB1206, QuotedCastLookalikeIsPlainColumnNotCastFunction) {
    exec_ok(R"(CREATE TABLE t_cast ("CAST" INT);)");
    exec_ok(R"(INSERT INTO t_cast ("CAST") VALUES (123);)");
    auto qr = exec(R"(SELECT "CAST" FROM t_cast;)");
    ASSERT_TRUE(qr.has_value()) << qr.error().message;
    ASSERT_EQ(qr->rows.size(), 1u);
    EXPECT_EQ(qr->rows[0][0].as_int32(), 123);
}

// Stress: many quoted identifiers with special characters, spaces, and
// escapes in a single statement.
TEST_F(QA_GDB1206, ManyQuotedColumnsWideRow) {
    std::string ddl = R"(CREATE TABLE "Wide Tbl" ()";
    std::string insert_cols;
    std::string insert_vals;
    for (int i = 0; i < 20; ++i) {
        std::string col = "\"Col " + std::to_string(i) + " X\"";
        if (i > 0) {
            ddl += ", ";
            insert_cols += ", ";
            insert_vals += ", ";
        }
        ddl += col + " INT";
        insert_cols += col;
        insert_vals += std::to_string(i);
    }
    ddl += ");";
    exec_ok(ddl);
    exec_ok(R"(INSERT INTO "Wide Tbl" ()" + insert_cols + ") VALUES (" + insert_vals + ");");

    auto qr = exec_ok(R"(SELECT * FROM "Wide Tbl";)");
    ASSERT_EQ(qr.column_names.size(), 20u);
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(qr.column_names[static_cast<size_t>(i)], "Col " + std::to_string(i) + " X");
    }
    ASSERT_EQ(qr.rows.size(), 1u);
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(qr.rows[0][static_cast<size_t>(i)].as_int32(), i);
    }
}
