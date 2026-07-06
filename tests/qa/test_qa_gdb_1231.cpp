// QA regression / adversarial tests for GDB-1231:
// StorageManager::drop_database_storage must close open table/index files
// (flush + dm_.close_file + erase) before remove_all'ing the database
// directory, so DROP DATABASE doesn't leak fds or fail to remove files on
// Windows.
//
// Adversarial focus:
//  1. Multiple open tables AND indexes in one db -- all closed, dir removed.
//  2. Double-drop safety: drop_table_storage then drop_database_storage.
//  3. Multi-db isolation: dropping db A must not touch db B's open tables.
//  4. Reopen path: open_table_storage (not create) sets db_id too.
//  5. Empty db drop; drop of a never-existed db directory (IO path).

#include "sixseven/catalog/schema.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

class QA_GDB1231_StorageManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1231";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
    }

    void TearDown() override { std::filesystem::remove_all(data_dir_); }

    TableSchema make_schema(table_id_t tid, const std::string& name) {
        TableSchema ts;
        ts.table_id = tid;
        ts.name = name;
        CatalogColumnDef col;
        col.ordinal = 0;
        col.name = "id";
        col.type_id = TypeId::INT32;
        ts.columns.push_back(col);
        return ts;
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
};

// -----------------------------------------------------------------------
// 1. Multiple open tables AND indexes in the dropped db.
// -----------------------------------------------------------------------

TEST_F(QA_GDB1231_StorageManagerTest, DropDatabaseWithMultipleTablesAndIndexesRemovesEverything) {
    StorageManager sm(dm_, data_dir_);

    ASSERT_TRUE(sm.create_database_storage(9).has_value());

    auto s1 = make_schema(200, "t1");
    auto s2 = make_schema(201, "t2");
    auto s3 = make_schema(202, "t3");
    ASSERT_TRUE(sm.create_table_storage(9, 200, s1).has_value());
    ASSERT_TRUE(sm.create_table_storage(9, 201, s2).has_value());
    ASSERT_TRUE(sm.create_table_storage(9, 202, s3).has_value());

    ASSERT_TRUE(sm.create_index_storage(9, 900).has_value());
    ASSERT_TRUE(sm.create_index_storage(9, 901).has_value());

    auto db_dir = data_dir_ / "databases" / "9";
    ASSERT_TRUE(std::filesystem::exists(db_dir / "tables" / "table_200.db"));
    ASSERT_TRUE(std::filesystem::exists(db_dir / "indexes" / "index_900.db"));

    auto drop = sm.drop_database_storage(9);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // Directory must be fully gone -- on Windows this fails if any fd is
    // still open against a file under db_dir.
    EXPECT_FALSE(std::filesystem::exists(db_dir));

    // No storage remains registered for any of the dropped tables/indexes.
    EXPECT_FALSE(sm.get_table_storage(200).has_value());
    EXPECT_FALSE(sm.get_table_storage(201).has_value());
    EXPECT_FALSE(sm.get_table_storage(202).has_value());
    EXPECT_FALSE(sm.get_index_storage(900).has_value());
    EXPECT_FALSE(sm.get_index_storage(901).has_value());
}

// -----------------------------------------------------------------------
// 2. Double-drop safety.
// -----------------------------------------------------------------------

TEST_F(QA_GDB1231_StorageManagerTest, DropTableThenDropDatabaseDoesNotDoubleCloseOrCrash) {
    StorageManager sm(dm_, data_dir_);

    ASSERT_TRUE(sm.create_database_storage(11).has_value());
    auto s1 = make_schema(300, "t1");
    auto s2 = make_schema(301, "t2");
    ASSERT_TRUE(sm.create_table_storage(11, 300, s1).has_value());
    ASSERT_TRUE(sm.create_table_storage(11, 301, s2).has_value());

    // Individually drop one table first (mirrors DROP TABLE before DROP
    // DATABASE, or the catalog dropping members table-by-table).
    ASSERT_TRUE(sm.drop_table_storage(11, 300).has_value());
    EXPECT_FALSE(std::filesystem::exists(data_dir_ / "databases" / "11" / "tables" / "table_300.db"));

    // Dropping the database must not attempt to close table 300 again (it
    // is no longer in tables_) and must still close + remove table 301.
    auto drop_db = sm.drop_database_storage(11);
    ASSERT_TRUE(drop_db.has_value()) << drop_db.error().message;

    EXPECT_FALSE(std::filesystem::exists(data_dir_ / "databases" / "11"));
    EXPECT_FALSE(sm.get_table_storage(301).has_value());
}

TEST_F(QA_GDB1231_StorageManagerTest, DropDatabaseWithTablesNeverOpenedSucceeds) {
    StorageManager sm(dm_, data_dir_);

    ASSERT_TRUE(sm.create_database_storage(12).has_value());
    auto s1 = make_schema(310, "t1");
    ASSERT_TRUE(sm.create_table_storage(12, 310, s1).has_value());

    // Simulate the table file existing on disk but *not* registered in
    // tables_ (e.g. after a restart where it was never reopened): drop the
    // in-memory entry manually via drop_table_storage's file-removal path,
    // then recreate the file directly without an open registration.
    ASSERT_TRUE(sm.drop_table_storage(12, 310).has_value());
    auto table_path = data_dir_ / "databases" / "12" / "tables" / "table_999.db";
    std::filesystem::create_directories(table_path.parent_path());
    { std::ofstream(table_path.string()) << "stray bytes"; }
    ASSERT_TRUE(std::filesystem::exists(table_path));

    // drop_database_storage has nothing registered for db 12 -- it must
    // still succeed and remove the stray file via remove_all.
    auto drop_db = sm.drop_database_storage(12);
    ASSERT_TRUE(drop_db.has_value()) << drop_db.error().message;
    EXPECT_FALSE(std::filesystem::exists(data_dir_ / "databases" / "12"));
}

// -----------------------------------------------------------------------
// 3. Multi-db isolation.
// -----------------------------------------------------------------------

TEST_F(QA_GDB1231_StorageManagerTest, DropDatabaseOnlyAffectsItsOwnTablesAndIndexes) {
    StorageManager sm(dm_, data_dir_);

    ASSERT_TRUE(sm.create_database_storage(20).has_value());
    ASSERT_TRUE(sm.create_database_storage(21).has_value());

    auto sa = make_schema(400, "a");
    auto sb = make_schema(401, "b");
    ASSERT_TRUE(sm.create_table_storage(20, 400, sa).has_value());
    ASSERT_TRUE(sm.create_table_storage(21, 401, sb).has_value());
    ASSERT_TRUE(sm.create_index_storage(20, 950).has_value());
    ASSERT_TRUE(sm.create_index_storage(21, 951).has_value());

    auto drop = sm.drop_database_storage(20);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // db 20 fully removed.
    EXPECT_FALSE(std::filesystem::exists(data_dir_ / "databases" / "20"));
    EXPECT_FALSE(sm.get_table_storage(400).has_value());
    EXPECT_FALSE(sm.get_index_storage(950).has_value());

    // db 21 untouched: still open, still usable, file still present.
    auto g = sm.get_table_storage(401);
    ASSERT_TRUE(g.has_value());
    EXPECT_TRUE(std::filesystem::exists(data_dir_ / "databases" / "21" / "tables" / "table_401.db"));
    EXPECT_TRUE(sm.get_index_storage(951).has_value());

    // db 21's table must still be writable/usable post-sibling-drop.
    auto* storage = *g;
    ASSERT_NE(storage, nullptr);
    ASSERT_NE(storage->heap, nullptr);
}

// -----------------------------------------------------------------------
// 4. Reopen path sets db_id too.
// -----------------------------------------------------------------------

TEST_F(QA_GDB1231_StorageManagerTest, DropDatabaseClosesTableOpenedViaOpenTableStorage) {
    auto sa = make_schema(500, "a");

    {
        // First "process": create db + table, then destroy the manager
        // (simulating a clean shutdown) so only the file remains on disk.
        StorageManager sm(dm_, data_dir_);
        ASSERT_TRUE(sm.create_database_storage(30).has_value());
        ASSERT_TRUE(sm.create_table_storage(30, 500, sa).has_value());
    }

    // Second "process" (post-restart): reopen the table via
    // open_table_storage (not create_table_storage) -- this is the path
    // that must also stamp db_id so drop_database_storage can find it.
    StorageManager sm2(dm_, data_dir_);
    ASSERT_TRUE(sm2.open_table_storage(30, 500, sa).has_value());
    ASSERT_TRUE(sm2.get_table_storage(500).has_value());

    auto drop = sm2.drop_database_storage(30);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    EXPECT_FALSE(std::filesystem::exists(data_dir_ / "databases" / "30"));
    EXPECT_FALSE(sm2.get_table_storage(500).has_value());
}

// -----------------------------------------------------------------------
// 5. Empty / nonexistent database drop (IO edge cases).
// -----------------------------------------------------------------------

TEST_F(QA_GDB1231_StorageManagerTest, DropEmptyDatabaseWithNoTablesSucceeds) {
    StorageManager sm(dm_, data_dir_);
    ASSERT_TRUE(sm.create_database_storage(40).has_value());
    ASSERT_TRUE(std::filesystem::exists(data_dir_ / "databases" / "40"));

    auto drop = sm.drop_database_storage(40);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;
    EXPECT_FALSE(std::filesystem::exists(data_dir_ / "databases" / "40"));
}

TEST_F(QA_GDB1231_StorageManagerTest, DropDatabaseDirectoryThatWasNeverCreatedSucceeds) {
    StorageManager sm(dm_, data_dir_);
    // No create_database_storage(41) call at all -- directory never existed.
    auto drop = sm.drop_database_storage(41);
    // remove_all on a nonexistent path is not an error (ec stays clear),
    // matching DropNonExistentDatabaseStorageSucceeds in dev tests.
    ASSERT_TRUE(drop.has_value()) << drop.error().message;
}

// -----------------------------------------------------------------------
// Bonus: repeated drop_database_storage on the same db_id (idempotency).
// -----------------------------------------------------------------------

TEST_F(QA_GDB1231_StorageManagerTest, DropDatabaseStorageTwiceInARowSucceeds) {
    StorageManager sm(dm_, data_dir_);
    ASSERT_TRUE(sm.create_database_storage(50).has_value());
    auto s1 = make_schema(600, "t1");
    ASSERT_TRUE(sm.create_table_storage(50, 600, s1).has_value());

    ASSERT_TRUE(sm.drop_database_storage(50).has_value());
    // Second call: nothing registered, directory already gone -- must still
    // return ok(), not crash or double-close.
    auto drop2 = sm.drop_database_storage(50);
    EXPECT_TRUE(drop2.has_value()) << drop2.error().message;
}

} // namespace
