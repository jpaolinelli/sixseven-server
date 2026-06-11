/// QA adversarial tests for GDB-602: DROP DATABASE IF EXISTS errors when
/// database doesn't exist.
///
/// The bug: IF EXISTS after the database name (e.g., DROP DATABASE mydb IF
/// EXISTS) was not parsed, so the clause was silently ignored and the executor
/// raised NOT_FOUND instead of returning silently.
///
/// The fix: The parser now accepts IF EXISTS both before and after the database
/// name: DROP DATABASE [IF EXISTS] <name> [IF EXISTS] [CASCADE|RESTRICT].

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "test_qa_helpers.h"

using namespace sixseven;

// =============================================================================
// Parser-level helpers
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
    auto r = try_parse(sql);
    EXPECT_TRUE(r.has_value()) << r.error().message;
    return r ? std::move(*r) : nullptr;
}

static void expect_parse_error(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return; // lexer error is acceptable
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_FALSE(stmts.has_value()) << "expected parse error for: " << sql;
}

// =============================================================================
// Query-engine-level fixture (full pipeline: parse → bind → plan → execute)
// =============================================================================

class QA_GDB602 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_602";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        bootstrap_qa_catalog(catalog_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return std::move(*result);
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
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::filesystem::path data_dir_;
};

// =============================================================================
// AC 1: DROP DATABASE <name> IF EXISTS succeeds for nonexistent database
// =============================================================================

TEST_F(QA_GDB602, IfExistsAfterName_NonexistentDB_Succeeds) {
    auto qr = exec_ok("DROP DATABASE nonexistent_db IF EXISTS");
    EXPECT_EQ(qr.message, "DROP DATABASE");
}

// =============================================================================
// AC 2: DROP DATABASE <name> IF EXISTS CASCADE succeeds for nonexistent database
// =============================================================================

TEST_F(QA_GDB602, IfExistsAfterName_NonexistentDB_Cascade_Succeeds) {
    auto qr = exec_ok("DROP DATABASE nonexistent_db IF EXISTS CASCADE");
    EXPECT_EQ(qr.message, "DROP DATABASE");
}

// =============================================================================
// AC 3: The original IF EXISTS before name still works
// =============================================================================

TEST_F(QA_GDB602, IfExistsBeforeName_NonexistentDB_StillWorks) {
    auto qr = exec_ok("DROP DATABASE IF EXISTS nonexistent_db");
    EXPECT_EQ(qr.message, "DROP DATABASE");
}

TEST_F(QA_GDB602, IfExistsBeforeName_NonexistentDB_Cascade_StillWorks) {
    auto qr = exec_ok("DROP DATABASE IF EXISTS nonexistent_db CASCADE");
    EXPECT_EQ(qr.message, "DROP DATABASE");
}

// =============================================================================
// AC 4: DROP DATABASE without IF EXISTS still errors on nonexistent DB
// =============================================================================

TEST_F(QA_GDB602, NoIfExists_NonexistentDB_Errors) {
    exec_error("DROP DATABASE nonexistent_db", StatusCode::NOT_FOUND);
}

// =============================================================================
// Adversarial: IF EXISTS after name with RESTRICT
// =============================================================================

TEST_F(QA_GDB602, IfExistsAfterName_NonexistentDB_Restrict_Succeeds) {
    auto qr = exec_ok("DROP DATABASE nonexistent_db IF EXISTS RESTRICT");
    EXPECT_EQ(qr.message, "DROP DATABASE");
}

// =============================================================================
// Adversarial: IF EXISTS with database that DOES exist — should actually drop
// =============================================================================

TEST_F(QA_GDB602, IfExistsAfterName_ExistingDB_StillDrops) {
    exec_ok("CREATE DATABASE tempdb");
    auto qr = exec_ok("DROP DATABASE tempdb IF EXISTS");
    EXPECT_EQ(qr.message, "DROP DATABASE");
    // Verify the database is gone — creating it again should work.
    exec_ok("CREATE DATABASE tempdb");
}

TEST_F(QA_GDB602, IfExistsAfterName_ExistingDB_Cascade_StillDrops) {
    exec_ok("CREATE DATABASE tempdb");
    auto qr = exec_ok("DROP DATABASE tempdb IF EXISTS CASCADE");
    EXPECT_EQ(qr.message, "DROP DATABASE");
    exec_ok("CREATE DATABASE tempdb");
}

// =============================================================================
// Adversarial: Double IF EXISTS (before AND after name) — should not crash
// =============================================================================

TEST(QA_GDB602_Parser, DoubleIfExists_BeforeAndAfterName) {
    // The parser should only set if_exists once. The second IF EXISTS after
    // the name is guarded by !stmt->if_exists, so it should be skipped.
    // This means the second "IF" won't be consumed and will cause a parse
    // error (unexpected token). That's acceptable — just ensure no crash.
    auto r = try_parse("DROP DATABASE IF EXISTS mydb IF EXISTS");
    // Either it parses (treating IF EXISTS twice) or errors — no crash.
    SUCCEED();
}

// =============================================================================
// Adversarial: IF without EXISTS after name — should give parse error
// =============================================================================

TEST(QA_GDB602_Parser, IfWithoutExists_AfterName) {
    expect_parse_error("DROP DATABASE mydb IF");
}

TEST(QA_GDB602_Parser, IfWithoutExists_AfterName_Cascade) {
    expect_parse_error("DROP DATABASE mydb IF CASCADE");
}

// =============================================================================
// Adversarial: Parser correctly sets if_exists for after-name syntax
// =============================================================================

TEST(QA_GDB602_Parser, AfterNameSyntax_SetsIfExists) {
    auto stmt = parse_one("DROP DATABASE mydb IF EXISTS");
    auto* dd = dynamic_cast<DropDatabaseStmt*>(stmt.get());
    ASSERT_NE(dd, nullptr);
    EXPECT_EQ(dd->database_name, "mydb");
    EXPECT_TRUE(dd->if_exists);
    EXPECT_FALSE(dd->cascade);
}

TEST(QA_GDB602_Parser, AfterNameSyntax_CascadeSetsFlags) {
    auto stmt = parse_one("DROP DATABASE mydb IF EXISTS CASCADE");
    auto* dd = dynamic_cast<DropDatabaseStmt*>(stmt.get());
    ASSERT_NE(dd, nullptr);
    EXPECT_EQ(dd->database_name, "mydb");
    EXPECT_TRUE(dd->if_exists);
    EXPECT_TRUE(dd->cascade);
}

TEST(QA_GDB602_Parser, AfterNameSyntax_RestrictSetsFlags) {
    auto stmt = parse_one("DROP DATABASE mydb IF EXISTS RESTRICT");
    auto* dd = dynamic_cast<DropDatabaseStmt*>(stmt.get());
    ASSERT_NE(dd, nullptr);
    EXPECT_EQ(dd->database_name, "mydb");
    EXPECT_TRUE(dd->if_exists);
    EXPECT_FALSE(dd->cascade);
}

// =============================================================================
// Adversarial: Case sensitivity — IF EXISTS are keywords, should work in
// various casings
// =============================================================================

TEST(QA_GDB602_Parser, CaseInsensitive_IfExists) {
    auto stmt = parse_one("drop database mydb if exists");
    auto* dd = dynamic_cast<DropDatabaseStmt*>(stmt.get());
    ASSERT_NE(dd, nullptr);
    EXPECT_TRUE(dd->if_exists);
}

TEST(QA_GDB602_Parser, MixedCase_IfExists) {
    auto stmt = parse_one("DROP DATABASE mydb If Exists");
    auto* dd = dynamic_cast<DropDatabaseStmt*>(stmt.get());
    ASSERT_NE(dd, nullptr);
    EXPECT_TRUE(dd->if_exists);
}

// =============================================================================
// Adversarial: DROP DATABASE with name that is a keyword — make sure IF EXISTS
// after keyword-like names still parses correctly
// =============================================================================

TEST(QA_GDB602_Parser, DatabaseNamedIfStillParsesIfExists) {
    // A database named with a quoted identifier should still allow IF EXISTS.
    auto r = try_parse("DROP DATABASE \"if\" IF EXISTS");
    // If quoted identifiers are supported, this should parse.
    // If not, a parse error is acceptable — just no crash.
    SUCCEED();
}

// =============================================================================
// Adversarial: Multiple DROP DATABASE IF EXISTS in sequence — no state leak
// =============================================================================

TEST_F(QA_GDB602, MultipleDropIfExists_NoStateLeak) {
    auto qr1 = exec_ok("DROP DATABASE IF EXISTS nope1");
    EXPECT_EQ(qr1.message, "DROP DATABASE");
    auto qr2 = exec_ok("DROP DATABASE nope2 IF EXISTS");
    EXPECT_EQ(qr2.message, "DROP DATABASE");
    auto qr3 = exec_ok("DROP DATABASE IF EXISTS nope3");
    EXPECT_EQ(qr3.message, "DROP DATABASE");
    auto qr4 = exec_ok("DROP DATABASE nope4 IF EXISTS CASCADE");
    EXPECT_EQ(qr4.message, "DROP DATABASE");
}

// =============================================================================
// Adversarial: Cannot drop default database even with IF EXISTS after name
// =============================================================================

TEST_F(QA_GDB602, DropDefaultDB_IfExistsAfterName_StillFails) {
    exec_error("DROP DATABASE demo IF EXISTS", StatusCode::CONSTRAINT_VIOLATION);
}

TEST_F(QA_GDB602, DropDefaultDB_IfExistsBeforeName_StillFails) {
    exec_error("DROP DATABASE IF EXISTS demo", StatusCode::CONSTRAINT_VIOLATION);
}
