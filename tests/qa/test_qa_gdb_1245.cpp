// QA regression tests for GDB-1245: empty-string DEFAULT crashed INSERT with a
// span-subscript OOB (zero-length variable field). Fixed by GDB-1224
// (well-defined zero-length var-field reads via read_var_value) plus the
// NULL-bitmap-based get_field logic. These tests verify that empty string is
// distinct from NULL end-to-end: via DEFAULT '', via explicit INSERT ... '',
// and that NULL rows remain NULL -- with `= ''` and `IS NULL` correctly
// discriminating between them, and values surviving a re-read.

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

class QA_GDB1245 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_qa_gdb1245";
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

// =============================================================================
// Core AC: DEFAULT '' inserts and round-trips as empty string, not NULL, and
// does not crash.
// =============================================================================

TEST_F(QA_GDB1245, DefaultEmptyStringDoesNotCrashAndRoundTrips) {
    exec_ok("CREATE TABLE t1245_default (id INT, tag TEXT DEFAULT '')");
    exec_ok("INSERT INTO t1245_default (id) VALUES (1)");

    auto qr = exec_ok("SELECT tag FROM t1245_default WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_FALSE(qr.rows[0][0].is_null());
    EXPECT_EQ(qr.rows[0][0].as_string(), "");
}

// =============================================================================
// Explicit empty string insert (distinct code path from DEFAULT re-lex).
// =============================================================================

TEST_F(QA_GDB1245, ExplicitEmptyStringDoesNotCrashAndRoundTrips) {
    exec_ok("CREATE TABLE t1245_explicit (id INT, tag TEXT)");
    exec_ok("INSERT INTO t1245_explicit VALUES (2, '')");

    auto qr = exec_ok("SELECT tag FROM t1245_explicit WHERE id = 2");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_FALSE(qr.rows[0][0].is_null());
    EXPECT_EQ(qr.rows[0][0].as_string(), "");
}

// =============================================================================
// NULL remains distinct from empty string: a row with no default and no
// explicit value must read back as NULL, not "".
// =============================================================================

TEST_F(QA_GDB1245, NullRemainsDistinctFromEmptyString) {
    exec_ok("CREATE TABLE t1245_null (id INT, tag TEXT)");
    exec_ok("INSERT INTO t1245_null (id) VALUES (3)");

    auto qr = exec_ok("SELECT tag FROM t1245_null WHERE id = 3");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_TRUE(qr.rows[0][0].is_null());
}

// =============================================================================
// `= ''` matches empty strings but not NULL; `IS NULL` matches NULL but not
// empty strings. Mix DEFAULT '', explicit '', and NULL rows in one table.
// =============================================================================

TEST_F(QA_GDB1245, EqualsEmptyAndIsNullDiscriminateCorrectly) {
    exec_ok("CREATE TABLE t1245_mixed (id INT, tag TEXT DEFAULT '')");

    // Row 1: uses DEFAULT '' -> tag = ""
    exec_ok("INSERT INTO t1245_mixed (id) VALUES (1)");
    // Row 2: explicit '' -> tag = ""
    exec_ok("INSERT INTO t1245_mixed VALUES (2, '')");
    // Row 3: explicit NULL -> tag IS NULL
    exec_ok("INSERT INTO t1245_mixed VALUES (3, NULL)");
    // Row 4: explicit non-empty value, sanity control.
    exec_ok("INSERT INTO t1245_mixed VALUES (4, 'hello')");

    auto empties = exec_ok("SELECT id FROM t1245_mixed WHERE tag = '' ORDER BY id");
    ASSERT_EQ(empties.rows.size(), 2u);
    EXPECT_EQ(empties.rows[0][0].as_int32(), 1);
    EXPECT_EQ(empties.rows[1][0].as_int32(), 2);

    auto nulls = exec_ok("SELECT id FROM t1245_mixed WHERE tag IS NULL ORDER BY id");
    ASSERT_EQ(nulls.rows.size(), 1u);
    EXPECT_EQ(nulls.rows[0][0].as_int32(), 3);

    // IS NOT NULL must include both empty-string rows and the non-empty row,
    // but exclude the NULL row.
    auto not_nulls = exec_ok("SELECT id FROM t1245_mixed WHERE tag IS NOT NULL ORDER BY id");
    ASSERT_EQ(not_nulls.rows.size(), 3u);
    EXPECT_EQ(not_nulls.rows[0][0].as_int32(), 1);
    EXPECT_EQ(not_nulls.rows[1][0].as_int32(), 2);
    EXPECT_EQ(not_nulls.rows[2][0].as_int32(), 4);

    // Non-empty row must not spuriously match `= ''`.
    auto non_empty_excluded = exec_ok("SELECT id FROM t1245_mixed WHERE tag = '' AND id = 4");
    EXPECT_EQ(non_empty_excluded.rows.size(), 0u);
}

// =============================================================================
// Full round trip across all three rows read back together, verifying every
// value (including re-reading tag as a plain projected column) is stable.
// =============================================================================

TEST_F(QA_GDB1245, MixedRowsRoundTripStableOnReRead) {
    exec_ok("CREATE TABLE t1245_reread (id INT, tag TEXT DEFAULT '')");
    exec_ok("INSERT INTO t1245_reread (id) VALUES (1)");  // default ''
    exec_ok("INSERT INTO t1245_reread VALUES (2, '')");   // explicit ''
    exec_ok("INSERT INTO t1245_reread VALUES (3, NULL)"); // explicit NULL

    // First read.
    auto qr1 = exec_ok("SELECT id, tag FROM t1245_reread ORDER BY id");
    ASSERT_EQ(qr1.rows.size(), 3u);
    EXPECT_FALSE(qr1.rows[0][1].is_null());
    EXPECT_EQ(qr1.rows[0][1].as_string(), "");
    EXPECT_FALSE(qr1.rows[1][1].is_null());
    EXPECT_EQ(qr1.rows[1][1].as_string(), "");
    EXPECT_TRUE(qr1.rows[2][1].is_null());

    // Second read (re-fetch from storage) must be identical -- no drift, no
    // crash, no corruption from a prior read.
    auto qr2 = exec_ok("SELECT id, tag FROM t1245_reread ORDER BY id");
    ASSERT_EQ(qr2.rows.size(), 3u);
    EXPECT_FALSE(qr2.rows[0][1].is_null());
    EXPECT_EQ(qr2.rows[0][1].as_string(), "");
    EXPECT_FALSE(qr2.rows[1][1].is_null());
    EXPECT_EQ(qr2.rows[1][1].as_string(), "");
    EXPECT_TRUE(qr2.rows[2][1].is_null());
}
