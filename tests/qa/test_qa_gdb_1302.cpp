// QA regression / adversarial tests for GDB-1302: durable fsync of the
// autoincrement high-water-mark in StorageManager::write_autoincrement.
//
// Focus areas (per QA handoff):
//   - read-after-write/restart correctness across extreme sequences
//     (many rapid writes, boundary values, concurrent writers)
//   - no functional regression from the added fsync
//   - the sync_file error path surfaces as a real error, not a silent no-op

#include "sixseven/catalog/schema.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <thread>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

class QA_GDB1302 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_1302";
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

}  // namespace

// -----------------------------------------------------------------------------
// Boundary values for the HWM itself.
// -----------------------------------------------------------------------------

TEST_F(QA_GDB1302, WriteAutoincrementZeroPersistsAcrossReopen) {
    auto schema = make_schema(10, "users");
    {
        StorageManager sm(dm_, data_dir_);
        ASSERT_TRUE(sm.create_table_storage(default_database_id, 10, schema).has_value());
        ASSERT_TRUE(sm.write_autoincrement(10, 0).has_value());
    }
    DiskManager dm2;
    StorageManager sm2(dm2, data_dir_);
    ASSERT_TRUE(sm2.open_table_storage(default_database_id, 10, schema).has_value());
    auto read_result = sm2.read_autoincrement(10);
    ASSERT_TRUE(read_result.has_value());
    EXPECT_EQ(*read_result, 0);
}

TEST_F(QA_GDB1302, WriteAutoincrementOnePersistsAcrossReopen) {
    auto schema = make_schema(10, "users");
    {
        StorageManager sm(dm_, data_dir_);
        ASSERT_TRUE(sm.create_table_storage(default_database_id, 10, schema).has_value());
        ASSERT_TRUE(sm.write_autoincrement(10, 1).has_value());
    }
    DiskManager dm2;
    StorageManager sm2(dm2, data_dir_);
    ASSERT_TRUE(sm2.open_table_storage(default_database_id, 10, schema).has_value());
    auto read_result = sm2.read_autoincrement(10);
    ASSERT_TRUE(read_result.has_value());
    EXPECT_EQ(*read_result, 1);
}

TEST_F(QA_GDB1302, WriteAutoincrementNearInt64MaxPersistsAcrossReopen) {
    // Value is stored as uint64_t on disk but the public API is int64_t;
    // exercise a value near INT64_MAX to catch truncation/sign-extension bugs
    // in the persistence path.
    const int64_t huge = std::numeric_limits<int64_t>::max() - 1;
    auto schema = make_schema(10, "users");
    {
        StorageManager sm(dm_, data_dir_);
        ASSERT_TRUE(sm.create_table_storage(default_database_id, 10, schema).has_value());
        ASSERT_TRUE(sm.write_autoincrement(10, huge).has_value());
    }
    DiskManager dm2;
    StorageManager sm2(dm2, data_dir_);
    ASSERT_TRUE(sm2.open_table_storage(default_database_id, 10, schema).has_value());
    auto read_result = sm2.read_autoincrement(10);
    ASSERT_TRUE(read_result.has_value());
    EXPECT_EQ(*read_result, huge);
}

// -----------------------------------------------------------------------------
// Many rapid successive writes, then a close/reopen -- only the last value
// must survive, and every intermediate fsync must have actually landed
// (otherwise a crash mid-sequence could regress the HWM).
// -----------------------------------------------------------------------------

TEST_F(QA_GDB1302, ManyRapidWritesThenReopenObservesFinalValue) {
    auto schema = make_schema(10, "users");
    constexpr int64_t kIterations = 2000;
    {
        StorageManager sm(dm_, data_dir_);
        ASSERT_TRUE(sm.create_table_storage(default_database_id, 10, schema).has_value());
        for (int64_t i = 1; i <= kIterations; ++i) {
            auto write_result = sm.write_autoincrement(10, i);
            ASSERT_TRUE(write_result.has_value()) << write_result.error().message;
            // Verify read-your-own-write within the same session at every step,
            // not just at the end -- catches partial/incorrect intermediate
            // writes that a final-value-only check would miss.
            auto read_result = sm.read_autoincrement(10);
            ASSERT_TRUE(read_result.has_value());
            ASSERT_EQ(*read_result, i) << "mismatch at iteration " << i;
        }
    }

    DiskManager dm2;
    StorageManager sm2(dm2, data_dir_);
    ASSERT_TRUE(sm2.open_table_storage(default_database_id, 10, schema).has_value());
    auto read_result = sm2.read_autoincrement(10);
    ASSERT_TRUE(read_result.has_value());
    EXPECT_EQ(*read_result, kIterations);
}

// -----------------------------------------------------------------------------
// Monotonic decrease is technically allowed by the API (it just persists
// whatever value is given); confirm the durability path doesn't special-case
// increasing values only.
// -----------------------------------------------------------------------------

TEST_F(QA_GDB1302, WriteAutoincrementNonMonotonicSequencePersistsLastWrite) {
    auto schema = make_schema(10, "users");
    {
        StorageManager sm(dm_, data_dir_);
        ASSERT_TRUE(sm.create_table_storage(default_database_id, 10, schema).has_value());
        ASSERT_TRUE(sm.write_autoincrement(10, 100).has_value());
        ASSERT_TRUE(sm.write_autoincrement(10, 5).has_value());
        ASSERT_TRUE(sm.write_autoincrement(10, 50).has_value());
    }
    DiskManager dm2;
    StorageManager sm2(dm2, data_dir_);
    ASSERT_TRUE(sm2.open_table_storage(default_database_id, 10, schema).has_value());
    auto read_result = sm2.read_autoincrement(10);
    ASSERT_TRUE(read_result.has_value());
    EXPECT_EQ(*read_result, 50);
}

// -----------------------------------------------------------------------------
// Concurrent writers on the same table: StorageManager::write_autoincrement
// takes an internal lock (mu_), so concurrent calls must not corrupt the
// header or crash; the final persisted value must be one of the values
// actually written (no torn writes).
// -----------------------------------------------------------------------------

TEST_F(QA_GDB1302, ConcurrentWritersDoNotCorruptHeader) {
    auto schema = make_schema(10, "users");
    StorageManager sm(dm_, data_dir_);
    ASSERT_TRUE(sm.create_table_storage(default_database_id, 10, schema).has_value());

    constexpr int kThreads = 8;
    constexpr int64_t kWritesPerThread = 100;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&sm, t, &failures]() {
            for (int64_t i = 1; i <= kWritesPerThread; ++i) {
                int64_t value = static_cast<int64_t>(t) * kWritesPerThread + i;
                auto write_result = sm.write_autoincrement(10, value);
                if (!write_result.has_value()) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(failures.load(), 0);

    // Header must not be corrupted: read must succeed and return a value that
    // is at least a value some thread actually wrote (not garbage/torn data).
    auto read_result = sm.read_autoincrement(10);
    ASSERT_TRUE(read_result.has_value());
    EXPECT_GE(*read_result, 1);
    EXPECT_LE(*read_result, static_cast<int64_t>(kThreads) * kWritesPerThread);
}

// -----------------------------------------------------------------------------
// Fault injection: close the underlying file out from under StorageManager
// (simulating an I/O failure / already-closed fd) and confirm the write
// surfaces a real error rather than silently reporting success while nothing
// was persisted. This directly probes the "does sync_file error propagate"
// concern from the handoff, since we cannot portably force ENOSPC/EIO in a
// unit test.
// -----------------------------------------------------------------------------

TEST_F(QA_GDB1302, WriteAutoincrementAfterUnderlyingFileClosedSurfacesError) {
    auto schema = make_schema(10, "users");
    StorageManager sm(dm_, data_dir_);
    ASSERT_TRUE(sm.create_table_storage(default_database_id, 10, schema).has_value());
    ASSERT_TRUE(sm.write_autoincrement(10, 1).has_value());

    auto storage_result = sm.get_table_storage(10);
    ASSERT_TRUE(storage_result.has_value());
    FileId file_id = (*storage_result)->file_id;

    // Close the fd directly via DiskManager, bypassing StorageManager's own
    // bookkeeping, to simulate the fd having gone bad underneath the
    // StorageManager (e.g. an external I/O fault). StorageManager still
    // believes the table is open and will attempt to write/sync a closed fd.
    ASSERT_TRUE(dm_.close_file(file_id).has_value());

    auto write_result = sm.write_autoincrement(10, 2);
    // The write must NOT silently report success: either the pwrite step or
    // the subsequent sync_file step must fail against the closed fd, and that
    // failure must propagate to the caller as a non-ok Result.
    EXPECT_FALSE(write_result.has_value())
        << "write_autoincrement reported success against a closed/invalid fd; "
           "the fsync error path is not propagating correctly";
}
