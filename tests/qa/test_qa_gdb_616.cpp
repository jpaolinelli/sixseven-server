/// @file test_qa_gdb_616.cpp
/// @brief Adversarial QA tests for GDB-616: Live row count tracking in TableHeap.
///
/// Tests cover: persistence across reopen, counter accuracy under
/// insert/delete interleaving, double-delete protection, delete-all-then-reinsert,
/// failed-insert doesn't increment, multi-reopen cycles, large-scale stress,
/// CountScan agreement with SeqScan after persistence, and underflow guard.

#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/count_scan.h"
#include "sixseven/executor/seq_scan.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace sixseven;

// ============================================================================
// Test fixture
// ============================================================================

class GDB616_RowCount : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb616.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;

        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 128);

        storage_schema_ = Schema({
            {"id", TypeId::INT32},
            {"name", TypeId::STRING},
        });

        OutputColumn c1{"test_table", "id", TypeId::INT32, false, 0};
        OutputColumn c2{"test_table", "name", TypeId::STRING, true, 0};
        scan_output_schema_ = OutputSchema({c1, c2});
    }

    void TearDown() override {
        bpm_.reset();
        auto close = dm_.close_file(file_id_);
        (void)close;
        std::filesystem::remove(path_);
    }

    /// Flush buffer pool and create a fresh one (simulates crash/restart).
    void reopen_buffer_pool(size_t pool_size = 128) {
        bpm_.reset();
        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, pool_size);
    }

    RID insert_row(TableHeap& heap, int32_t id, const std::string& name) {
        std::vector<Value> vals = {Value(id), Value(name)};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        EXPECT_TRUE(bytes.has_value()) << bytes.error().message;
        if (!bytes.has_value()) {
            return RID{};
        }
        auto rid = heap.insert_tuple(*bytes);
        EXPECT_TRUE(rid.has_value()) << rid.error().message;
        return rid ? *rid : RID{};
    }

    RID insert_raw(TableHeap& heap, size_t len, uint8_t fill = 0xAA) {
        std::vector<uint8_t> data(len, fill);
        auto rid = heap.insert_tuple(data);
        EXPECT_TRUE(rid.has_value()) << rid.error().message;
        return rid ? *rid : RID{};
    }

    static OutputSchema count_output_schema() {
        return OutputSchema({{"", "__agg_0", TypeId::INT64, false, 0}});
    }

    int64_t run_count_scan(TableHeap& heap) {
        CountScanOperator scan(heap, count_output_schema());
        auto open = scan.open();
        EXPECT_TRUE(open.has_value()) << open.error().message;

        auto row = scan.next();
        EXPECT_TRUE(row.has_value()) << row.error().message;
        if (!row.has_value() || !row->has_value()) {
            ADD_FAILURE() << "count scan produced no row";
            return -1;
        }

        int64_t count = row->value().values[0].as_int64();

        auto row2 = scan.next();
        EXPECT_TRUE(row2.has_value()) << row2.error().message;
        if (row2.has_value()) {
            EXPECT_FALSE(row2->has_value());
        }

        scan.close();
        return count;
    }

    int64_t count_via_seq_scan(TableHeap& heap) {
        SeqScanOperator scan(heap, storage_schema_, scan_output_schema_);
        auto open = scan.open();
        EXPECT_TRUE(open.has_value()) << open.error().message;

        int64_t count = 0;
        while (true) {
            auto row = scan.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value()) {
                break;
            }
            ++count;
        }
        scan.close();
        return count;
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
    Schema storage_schema_;
    OutputSchema scan_output_schema_;
};

// ============================================================================
// AC: TableHeap maintains an accurate live row count
// ============================================================================

TEST_F(GDB616_RowCount, EmptyTableRowCountIsZero) {
    TableHeap heap(*bpm_, dm_, file_id_);
    EXPECT_EQ(heap.row_count(), 0u);
}

TEST_F(GDB616_RowCount, IncrementOnEveryInsert) {
    TableHeap heap(*bpm_, dm_, file_id_);
    for (int i = 0; i < 50; ++i) {
        insert_row(heap, i, "row_" + std::to_string(i));
        EXPECT_EQ(heap.row_count(), static_cast<uint64_t>(i + 1));
    }
}

TEST_F(GDB616_RowCount, DecrementOnEveryDelete) {
    TableHeap heap(*bpm_, dm_, file_id_);
    std::vector<RID> rids;
    for (int i = 0; i < 20; ++i) {
        rids.push_back(insert_row(heap, i, "row_" + std::to_string(i)));
    }

    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(heap.row_count(), static_cast<uint64_t>(20 - i));
        auto del = heap.delete_tuple(rids[i]);
        ASSERT_TRUE(del.has_value()) << del.error().message;
    }
    EXPECT_EQ(heap.row_count(), 0u);
}

// ============================================================================
// AC: Row count is correct after INSERT, DELETE, and mixed operations
// ============================================================================

TEST_F(GDB616_RowCount, DeleteAllThenReinsert) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert 10, delete all, verify 0, insert 5 more, verify 5.
    std::vector<RID> rids;
    for (int i = 0; i < 10; ++i) {
        rids.push_back(insert_row(heap, i, "v1_" + std::to_string(i)));
    }
    EXPECT_EQ(heap.row_count(), 10u);

    for (auto& rid : rids) {
        auto del = heap.delete_tuple(rid);
        ASSERT_TRUE(del.has_value()) << del.error().message;
    }
    EXPECT_EQ(heap.row_count(), 0u);

    for (int i = 100; i < 105; ++i) {
        insert_row(heap, i, "v2_" + std::to_string(i));
    }
    EXPECT_EQ(heap.row_count(), 5u);
}

TEST_F(GDB616_RowCount, InterleavedInsertDeleteManyRounds) {
    TableHeap heap(*bpm_, dm_, file_id_);
    int64_t expected = 0;

    for (int round = 0; round < 10; ++round) {
        // Insert 7 rows.
        std::vector<RID> rids;
        for (int i = 0; i < 7; ++i) {
            rids.push_back(insert_row(heap, round * 100 + i, "r" + std::to_string(round)));
            ++expected;
        }
        EXPECT_EQ(heap.row_count(), static_cast<uint64_t>(expected));

        // Delete 4 of them.
        for (int d = 0; d < 4; ++d) {
            auto del = heap.delete_tuple(rids[d]);
            ASSERT_TRUE(del.has_value()) << del.error().message;
            --expected;
        }
        EXPECT_EQ(heap.row_count(), static_cast<uint64_t>(expected));
    }

    // Final agreement with sequential scan.
    EXPECT_EQ(run_count_scan(heap), expected);
}

// ============================================================================
// AC: Row count is persisted in the file header page
// ============================================================================

TEST_F(GDB616_RowCount, PersistAfterInsertsAndReopen) {
    {
        TableHeap heap(*bpm_, dm_, file_id_);
        for (int i = 0; i < 30; ++i) {
            insert_row(heap, i, "persist_" + std::to_string(i));
        }
        EXPECT_EQ(heap.row_count(), 30u);
    }

    reopen_buffer_pool();
    TableHeap heap2(*bpm_, dm_, file_id_);
    EXPECT_EQ(heap2.row_count(), 30u);
}

TEST_F(GDB616_RowCount, PersistAfterDeletesAndReopen) {
    std::vector<RID> rids;
    {
        TableHeap heap(*bpm_, dm_, file_id_);
        for (int i = 0; i < 20; ++i) {
            rids.push_back(insert_row(heap, i, "pd_" + std::to_string(i)));
        }
        // Delete 8 rows.
        for (int i = 0; i < 8; ++i) {
            auto del = heap.delete_tuple(rids[i]);
            ASSERT_TRUE(del.has_value()) << del.error().message;
        }
        EXPECT_EQ(heap.row_count(), 12u);
    }

    reopen_buffer_pool();
    TableHeap heap2(*bpm_, dm_, file_id_);
    EXPECT_EQ(heap2.row_count(), 12u);
}

TEST_F(GDB616_RowCount, PersistDeleteAllAndReopen) {
    {
        TableHeap heap(*bpm_, dm_, file_id_);
        std::vector<RID> rids;
        for (int i = 0; i < 5; ++i) {
            rids.push_back(insert_row(heap, i, "da_" + std::to_string(i)));
        }
        for (auto& rid : rids) {
            auto del = heap.delete_tuple(rid);
            ASSERT_TRUE(del.has_value()) << del.error().message;
        }
        EXPECT_EQ(heap.row_count(), 0u);
    }

    reopen_buffer_pool();
    TableHeap heap2(*bpm_, dm_, file_id_);
    EXPECT_EQ(heap2.row_count(), 0u);
}

// ============================================================================
// AC: Row count is recovered correctly after crash/restart
// ============================================================================

TEST_F(GDB616_RowCount, MultipleReopenCyclesAccumulate) {
    // Simulate multiple restart cycles, each adding rows.
    for (int cycle = 0; cycle < 5; ++cycle) {
        {
            TableHeap heap(*bpm_, dm_, file_id_);
            EXPECT_EQ(heap.row_count(), static_cast<uint64_t>(cycle * 10))
                << "Wrong count at start of cycle " << cycle;

            for (int i = 0; i < 10; ++i) {
                insert_row(heap, cycle * 100 + i, "cyc" + std::to_string(cycle));
            }
            EXPECT_EQ(heap.row_count(), static_cast<uint64_t>((cycle + 1) * 10));
        }
        reopen_buffer_pool();
    }

    // Final verification.
    TableHeap heap(*bpm_, dm_, file_id_);
    EXPECT_EQ(heap.row_count(), 50u);
}

TEST_F(GDB616_RowCount, ReopenAfterMixedOpsMatchesSeqScan) {
    {
        TableHeap heap(*bpm_, dm_, file_id_);
        std::vector<RID> rids;
        for (int i = 0; i < 100; ++i) {
            rids.push_back(insert_row(heap, i, "mix_" + std::to_string(i)));
        }
        // Delete every 5th row.
        for (int i = 0; i < 100; i += 5) {
            auto del = heap.delete_tuple(rids[i]);
            ASSERT_TRUE(del.has_value()) << del.error().message;
        }
    }

    reopen_buffer_pool();
    TableHeap heap2(*bpm_, dm_, file_id_);
    // row_count should match the actual live tuple count from a seq scan.
    EXPECT_EQ(run_count_scan(heap2), count_via_seq_scan(heap2));
    EXPECT_EQ(heap2.row_count(), 80u);
}

// ============================================================================
// AC: Planner uses live row count for bare COUNT(*)
// ============================================================================

TEST_F(GDB616_RowCount, CountScanReturnsRowCountValue) {
    TableHeap heap(*bpm_, dm_, file_id_);
    for (int i = 0; i < 42; ++i) {
        insert_row(heap, i, "cs_" + std::to_string(i));
    }

    // CountScan should return the same value as row_count().
    int64_t scan_count = run_count_scan(heap);
    EXPECT_EQ(scan_count, static_cast<int64_t>(heap.row_count()));
    EXPECT_EQ(scan_count, 42);
}

TEST_F(GDB616_RowCount, CountScanAfterReopenMatchesPersistedCount) {
    {
        TableHeap heap(*bpm_, dm_, file_id_);
        for (int i = 0; i < 75; ++i) {
            insert_row(heap, i, "csr_" + std::to_string(i));
        }
    }

    reopen_buffer_pool();
    TableHeap heap2(*bpm_, dm_, file_id_);
    EXPECT_EQ(run_count_scan(heap2), 75);
    EXPECT_EQ(run_count_scan(heap2), count_via_seq_scan(heap2));
}

// ============================================================================
// Edge case: double delete — should not double-decrement
// ============================================================================

TEST_F(GDB616_RowCount, DoubleDeleteDoesNotDoubleDecrement) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto r0 = insert_row(heap, 1, "only");
    EXPECT_EQ(heap.row_count(), 1u);

    auto del1 = heap.delete_tuple(r0);
    ASSERT_TRUE(del1.has_value()) << del1.error().message;
    EXPECT_EQ(heap.row_count(), 0u);

    // Second delete on the same RID should fail (tuple already deleted).
    auto del2 = heap.delete_tuple(r0);
    EXPECT_FALSE(del2.has_value()) << "Expected double-delete to fail";

    // Row count must NOT have decremented again (underflow to UINT64_MAX).
    EXPECT_EQ(heap.row_count(), 0u);
}

// ============================================================================
// Edge case: failed insert should not increment counter
// ============================================================================

TEST_F(GDB616_RowCount, FailedInsertDoesNotIncrementCount) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Try to insert an empty tuple — should fail.
    std::vector<uint8_t> empty;
    auto result = heap.insert_tuple(empty);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(heap.row_count(), 0u);
}

TEST_F(GDB616_RowCount, OversizedTupleDoesNotIncrementCount) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // A tuple larger than a page should fail to insert.
    // page_size = 8192, but page has header overhead, so ~8000+ byte tuple should fail.
    std::vector<uint8_t> huge(8192, 0xFF);
    auto result = heap.insert_tuple(huge);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(heap.row_count(), 0u);
}

// ============================================================================
// Boundary: single row insert, delete, persist cycle
// ============================================================================

TEST_F(GDB616_RowCount, SingleRowInsertDeletePersistCycle) {
    {
        TableHeap heap(*bpm_, dm_, file_id_);
        auto rid = insert_row(heap, 1, "one");
        EXPECT_EQ(heap.row_count(), 1u);

        auto del = heap.delete_tuple(rid);
        ASSERT_TRUE(del.has_value());
        EXPECT_EQ(heap.row_count(), 0u);
    }

    reopen_buffer_pool();
    TableHeap heap2(*bpm_, dm_, file_id_);
    EXPECT_EQ(heap2.row_count(), 0u);

    // Insert again after reopen.
    insert_row(heap2, 2, "two");
    EXPECT_EQ(heap2.row_count(), 1u);
}

// ============================================================================
// Stress: large number of inserts with persistence verification
// ============================================================================

TEST_F(GDB616_RowCount, StressFiveThousandInsertsPersist) {
    constexpr int N = 5000;
    {
        TableHeap heap(*bpm_, dm_, file_id_);
        for (int i = 0; i < N; ++i) {
            insert_row(heap, i, "s" + std::to_string(i));
        }
        EXPECT_EQ(heap.row_count(), static_cast<uint64_t>(N));
    }

    reopen_buffer_pool();
    TableHeap heap2(*bpm_, dm_, file_id_);
    EXPECT_EQ(heap2.row_count(), static_cast<uint64_t>(N));
    EXPECT_EQ(run_count_scan(heap2), N);
}

TEST_F(GDB616_RowCount, StressMassDeleteHalfAndPersist) {
    constexpr int N = 2000;
    std::vector<RID> rids;

    {
        TableHeap heap(*bpm_, dm_, file_id_);
        rids.reserve(N);
        for (int i = 0; i < N; ++i) {
            rids.push_back(insert_row(heap, i, "sd_" + std::to_string(i)));
        }

        // Delete the first half.
        for (int i = 0; i < N / 2; ++i) {
            auto del = heap.delete_tuple(rids[i]);
            ASSERT_TRUE(del.has_value()) << del.error().message;
        }
        EXPECT_EQ(heap.row_count(), static_cast<uint64_t>(N / 2));
    }

    reopen_buffer_pool();
    TableHeap heap2(*bpm_, dm_, file_id_);
    EXPECT_EQ(heap2.row_count(), static_cast<uint64_t>(N / 2));
    EXPECT_EQ(run_count_scan(heap2), count_via_seq_scan(heap2));
}

// ============================================================================
// Multi-page: row count correct across page boundaries
// ============================================================================

TEST_F(GDB616_RowCount, MultiPageRowCountMatchesSeqScan) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Use wide tuples to force many pages.
    std::string wide(500, 'W');
    constexpr int N = 300;
    std::vector<RID> rids;
    for (int i = 0; i < N; ++i) {
        rids.push_back(insert_row(heap, i, wide));
    }
    EXPECT_EQ(heap.row_count(), static_cast<uint64_t>(N));

    // Delete every 3rd row.
    int deleted = 0;
    for (int i = 0; i < N; i += 3) {
        auto del = heap.delete_tuple(rids[i]);
        ASSERT_TRUE(del.has_value()) << del.error().message;
        ++deleted;
    }

    uint64_t expected = N - deleted;
    EXPECT_EQ(heap.row_count(), expected);
    EXPECT_EQ(run_count_scan(heap), static_cast<int64_t>(expected));
    EXPECT_EQ(run_count_scan(heap), count_via_seq_scan(heap));
}

// ============================================================================
// Raw tuple operations: row count tracks raw byte inserts
// ============================================================================

TEST_F(GDB616_RowCount, RawTupleInsertDeleteTracked) {
    TableHeap heap(*bpm_, dm_, file_id_);

    insert_raw(heap, 100, 0x11);
    auto r2 = insert_raw(heap, 200, 0x22);
    insert_raw(heap, 300, 0x33);
    EXPECT_EQ(heap.row_count(), 3u);

    auto del = heap.delete_tuple(r2);
    ASSERT_TRUE(del.has_value());
    EXPECT_EQ(heap.row_count(), 2u);
}

// ============================================================================
// Small buffer pool: persistence works under eviction pressure
// ============================================================================

TEST_F(GDB616_RowCount, SmallBufferPoolPersistsCorrectly) {
    // Use a very small buffer pool to force frequent evictions.
    bpm_.reset();
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 4);

    {
        TableHeap heap(*bpm_, dm_, file_id_);
        // Insert enough rows to span many pages with only 4 buffer frames.
        for (int i = 0; i < 200; ++i) {
            auto data = std::vector<uint8_t>(100, static_cast<uint8_t>(i & 0xFF));
            auto rid = heap.insert_tuple(data);
            ASSERT_TRUE(rid.has_value()) << rid.error().message;
        }
        EXPECT_EQ(heap.row_count(), 200u);
    }

    // Reopen with small pool.
    bpm_.reset();
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 4);
    TableHeap heap2(*bpm_, dm_, file_id_);
    EXPECT_EQ(heap2.row_count(), 200u);
}

// ============================================================================
// Idempotency: opening the same heap twice reads same count
// ============================================================================

TEST_F(GDB616_RowCount, TwoHeapInstancesReadSameCount) {
    {
        TableHeap heap(*bpm_, dm_, file_id_);
        for (int i = 0; i < 15; ++i) {
            insert_row(heap, i, "dup_" + std::to_string(i));
        }
    }

    reopen_buffer_pool();

    // Two heap instances on the same file should read the same persisted count.
    TableHeap h1(*bpm_, dm_, file_id_);
    TableHeap h2(*bpm_, dm_, file_id_);
    EXPECT_EQ(h1.row_count(), h2.row_count());
    EXPECT_EQ(h1.row_count(), 15u);
}

// ============================================================================
// AC: Unit tests for counter accuracy (verify dev tests exist and pass)
// This test exercises the same patterns as dev tests but with different data
// to catch hard-coded or coincidental passes.
// ============================================================================

TEST_F(GDB616_RowCount, CounterAccuracyPrimeNumbers) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert 37 rows (prime number).
    std::vector<RID> rids;
    for (int i = 0; i < 37; ++i) {
        rids.push_back(insert_row(heap, i, "prime_" + std::to_string(i)));
    }
    EXPECT_EQ(heap.row_count(), 37u);

    // Delete 13 rows (prime number).
    for (int i = 0; i < 13; ++i) {
        auto del = heap.delete_tuple(rids[i]);
        ASSERT_TRUE(del.has_value()) << del.error().message;
    }
    EXPECT_EQ(heap.row_count(), 24u);

    // Insert 7 more.
    for (int i = 0; i < 7; ++i) {
        insert_row(heap, 1000 + i, "extra_" + std::to_string(i));
    }
    EXPECT_EQ(heap.row_count(), 31u);
    EXPECT_EQ(run_count_scan(heap), 31);
}
