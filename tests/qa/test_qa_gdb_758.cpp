#include "sixseven/catalog/catalog.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "test_qa_helpers.h"

using namespace sixseven;

// =============================================================================
// GDB-758: string DEFAULT values with embedded single quotes
// =============================================================================
//
// Root cause: expr_to_sql() serialized STRING literals as
//   "'" + lit->value + "'"
// without escaping embedded single quotes. When a stored default like
// 'it''s a test' was read back and re-parsed at INSERT time, the lexer
// saw an unterminated string and failed with PARSE_ERROR.
//
// Fix: single quotes in lit->value are doubled before wrapping.

class QA_GDB758 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_qa_gdb758";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        bootstrap_qa_catalog(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (!result.has_value()) {
            ADD_FAILURE() << "exec_ok failed for: " << sql
                          << "\n  error: " << result.error().message;
            return QueryResult{};
        }
        return std::move(*result);
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// ---------------------------------------------------------------------------
// AC1: CREATE TABLE with DEFAULT 'it''s a test', INSERT omitting the column,
//      SELECT verifies exact value.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB758, CreateTableDefaultWithSingleQuote) {
    exec_ok("CREATE TABLE t1 ("
            "  id INT,"
            "  label TEXT DEFAULT 'it''s a test'"
            ")");

    exec_ok("INSERT INTO t1 (id) VALUES (1)");

    auto qr = exec_ok("SELECT id, label FROM t1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "it's a test");
}

// ---------------------------------------------------------------------------
// AC2: ALTER TABLE ADD COLUMN ... DEFAULT with embedded single quote.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB758, AlterTableAddColumnDefaultWithSingleQuote) {
    exec_ok("CREATE TABLE t2 (id INT)");
    exec_ok("INSERT INTO t2 (id) VALUES (1)");

    exec_ok("ALTER TABLE t2 ADD COLUMN note TEXT DEFAULT 'can''t stop'");

    // Existing row should pick up the new default when selected (if supported),
    // or at minimum a new INSERT should use the default.
    exec_ok("INSERT INTO t2 (id) VALUES (2)");

    auto qr = exec_ok("SELECT id, note FROM t2 WHERE id = 2");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "can't stop");
}

// ---------------------------------------------------------------------------
// AC3: Default that is exactly a single quote character ('''').
// ---------------------------------------------------------------------------

TEST_F(QA_GDB758, DefaultThatIsJustASingleQuote) {
    exec_ok("CREATE TABLE t3 ("
            "  id INT,"
            "  ch TEXT DEFAULT ''''"
            ")");

    exec_ok("INSERT INTO t3 (id) VALUES (42)");

    auto qr = exec_ok("SELECT id, ch FROM t3");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "'");
}

// ---------------------------------------------------------------------------
// AC4: Default with multiple embedded quotes ('a''b''c').
// ---------------------------------------------------------------------------

TEST_F(QA_GDB758, DefaultWithMultipleEmbeddedQuotes) {
    exec_ok("CREATE TABLE t4 ("
            "  id INT,"
            "  tag TEXT DEFAULT 'a''b''c'"
            ")");

    exec_ok("INSERT INTO t4 (id) VALUES (1)");

    auto qr = exec_ok("SELECT tag FROM t4");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "a'b'c");
}

// ---------------------------------------------------------------------------
// AC6: Plain string default without quotes is unaffected.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB758, PlainStringDefaultUnaffected) {
    exec_ok("CREATE TABLE t6 ("
            "  id INT,"
            "  name TEXT DEFAULT 'hello'"
            ")");

    exec_ok("INSERT INTO t6 (id) VALUES (1)");

    auto qr = exec_ok("SELECT name FROM t6");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "hello");
}

// ---------------------------------------------------------------------------
// AC7: Multiple rows all receive the correct quoted default.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB758, MultipleRowsUseQuotedDefault) {
    exec_ok("CREATE TABLE t7 ("
            "  id INT,"
            "  msg TEXT DEFAULT 'it''s fine'"
            ")");

    exec_ok("INSERT INTO t7 (id) VALUES (1)");
    exec_ok("INSERT INTO t7 (id) VALUES (2)");
    exec_ok("INSERT INTO t7 (id) VALUES (3)");

    auto qr = exec_ok("SELECT id, msg FROM t7 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 3u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(qr.rows[i][1].as_string(), "it's fine") << "row " << i;
    }
}

// ---------------------------------------------------------------------------
// AC8: Explicit value overrides the quoted default.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB758, ExplicitValueOverridesQuotedDefault) {
    exec_ok("CREATE TABLE t8 ("
            "  id INT,"
            "  label TEXT DEFAULT 'don''t use me'"
            ")");

    exec_ok("INSERT INTO t8 VALUES (1, 'override')");

    auto qr = exec_ok("SELECT label FROM t8");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "override");
}