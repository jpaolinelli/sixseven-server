/// @file test_qa_gdb_615.cpp
/// @brief Adversarial QA tests for GDB-615: COUNT(*) fast path without tuple
///        deserialization.
///
/// Tests cover: edge cases (empty, single row, all-deleted), boundary conditions
/// (multi-page, large counts), insert/delete interleaving, idempotency,
/// slot reuse correctness, and agreement with SeqScan reference counts.

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

class GDB615_CountScan : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb615.db";
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
// Edge cases
// ============================================================================

TEST_F(GDB615_CountScan, EmptyTableReturnsZero) {
    TableHeap heap(*bpm_, dm_, file_id_);
    EXPECT_EQ(run_count_scan(heap), 0);
    EXPECT_EQ(run_count_scan(heap), count_via_seq_scan(heap));
}

TEST_F(GDB615_CountScan, AllRowsDeletedReturnsZero) {
    TableHeap heap(*bpm_, dm_, file_id_);
    std::vector<RID> rids;
    for (int i = 0; i < 10; ++i) {
        rids.push_back(insert_row(heap, i, "row_" + std::to_string(i)));
    }
    EXPECT_EQ(run_count_scan(heap), 10);

    // Delete every row.
    for (auto& rid : rids) {
        auto del = heap.delete_tuple(rid);
        ASSERT_TRUE(del.has_value()) << del.error().message;
    }

    EXPECT_EQ(run_count_scan(heap), 0);
    EXPECT_EQ(run_count_scan(heap), count_via_seq_scan(heap));
}

TEST_F(GDB615_CountScan, DeleteFirstSlotOnly) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto r0 = insert_row(heap, 0, "first");
    insert_row(heap, 1, "second");
    insert_row(heap, 2, "third");

    auto del = heap.delete_tuple(r0);
    ASSERT_TRUE(del.has_value());

    EXPECT_EQ(run_count_scan(heap), 2);
    EXPECT_EQ(run_count_scan(heap), count_via_seq_scan(heap));
}

TEST_F(GDB615_CountScan, DeleteLastSlotOnly) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 0, "first");
    insert_row(heap, 1, "second");
    auto r2 = insert_row(heap, 2, "third");

    auto del = heap.delete_tuple(r2);
    ASSERT_TRUE(del.has_value());

    EXPECT_EQ(run_count_scan(heap), 2);
    EXPECT_EQ(run_count_scan(heap), count_via_seq_scan(heap));
}

// ============================================================================
// Slot reuse after delete + insert
// ============================================================================

TEST_F(GDB615_CountScan, SlotReuseAfterDeleteAndInsert) {
    TableHeap heap(*bpm_, dm_, file_id_);
    auto r0 = insert_row(heap, 0, "original");
    insert_row(heap, 1, "kept");

    // Delete first, then insert a new row that may reuse slot 0.
    auto del = heap.delete_tuple(r0);
    ASSERT_TRUE(del.has_value());
    insert_row(heap, 99, "reused");

    // Should be 2 live rows.
    EXPECT_EQ(run_count_scan(heap), 2);
    EXPECT_EQ(run_count_scan(heap), count_via_seq_scan(heap));
}

TEST_F(GDB615_CountScan, RepeatedDeleteAndReinsert) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Cycle: insert 5, delete 3, insert 2, repeatedly.
    std::vector<RID> rids;
    for (int cycle = 0; cycle < 5; ++cycle) {
        for (int i = 0; i < 5; ++i) {
            rids.push_back(insert_row(heap, cycle * 100 + i, "c" + std::to_string(cycle)));
        }
        // Delete the first 3 inserted this cycle.
        for (int d = 0; d < 3 && !rids.empty(); ++d) {
            auto del = heap.delete_tuple(rids.back());
            ASSERT_TRUE(del.has_value()) << del.error().message;
            rids.pop_back();
        }
    }

    // Both paths must agree.
    EXPECT_EQ(run_count_scan(heap), count_via_seq_scan(heap));
}

// ============================================================================
// Multi-page / boundary conditions
// ============================================================================

TEST_F(GDB615_CountScan, MultiPageAgreesWithSeqScan) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert enough rows to span multiple pages. With a ~4KB page and small
    // tuples (~20 bytes), ~500 rows should span multiple pages.
    constexpr int num_rows = 2000;
    for (int i = 0; i < num_rows; ++i) {
        insert_row(heap, i, "multipage_" + std::to_string(i));
    }

    int64_t fast = run_count_scan(heap);
    int64_t ref = count_via_seq_scan(heap);
    EXPECT_EQ(fast, num_rows);
    EXPECT_EQ(fast, ref);
}

TEST_F(GDB615_CountScan, MultiPageWithDeletionsAgreesWithSeqScan) {
    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr int num_rows = 1000;
    std::vector<RID> rids;
    rids.reserve(num_rows);
    for (int i = 0; i < num_rows; ++i) {
        rids.push_back(insert_row(heap, i, "mp_del_" + std::to_string(i)));
    }

    // Delete every 3rd row.
    int deleted = 0;
    for (int i = 0; i < num_rows; i += 3) {
        auto del = heap.delete_tuple(rids[i]);
        ASSERT_TRUE(del.has_value()) << del.error().message;
        ++deleted;
    }

    int64_t expected = num_rows - deleted;
    EXPECT_EQ(run_count_scan(heap), expected);
    EXPECT_EQ(run_count_scan(heap), count_via_seq_scan(heap));
}

// ============================================================================
// Idempotency — multiple open/next/close cycles
// ============================================================================

TEST_F(GDB615_CountScan, RepeatedOpenCloseProducesSameResult) {
    TableHeap heap(*bpm_, dm_, file_id_);
    for (int i = 0; i < 50; ++i) {
        insert_row(heap, i, "idem_" + std::to_string(i));
    }

    int64_t first = run_count_scan(heap);
    for (int trial = 0; trial < 5; ++trial) {
        EXPECT_EQ(run_count_scan(heap), first)
            << "Mismatch on trial " << trial;
    }
}

// ============================================================================
// Large string values (wider tuples, fewer per page)
// ============================================================================

TEST_F(GDB615_CountScan, WideRowsAcrossPages) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // 500-byte string per row → ~8 tuples per 4KB page.
    std::string wide(500, 'X');
    constexpr int num_rows = 200;
    for (int i = 0; i < num_rows; ++i) {
        insert_row(heap, i, wide);
    }

    EXPECT_EQ(run_count_scan(heap), num_rows);
    EXPECT_EQ(run_count_scan(heap), count_via_seq_scan(heap));
}

// ============================================================================
// Single row edge case
// ============================================================================

TEST_F(GDB615_CountScan, SingleRowInsertDeleteInsert) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto r = insert_row(heap, 1, "only");
    EXPECT_EQ(run_count_scan(heap), 1);

    auto del = heap.delete_tuple(r);
    ASSERT_TRUE(del.has_value());
    EXPECT_EQ(run_count_scan(heap), 0);

    insert_row(heap, 2, "replacement");
    EXPECT_EQ(run_count_scan(heap), 1);
    EXPECT_EQ(run_count_scan(heap), count_via_seq_scan(heap));
}

// ============================================================================
// Output schema correctness
// ============================================================================

TEST_F(GDB615_CountScan, OutputTupleHasExactlyOneInt64Value) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "a");
    insert_row(heap, 2, "b");

    CountScanOperator scan(heap, count_output_schema());
    auto open = scan.open();
    ASSERT_TRUE(open.has_value());

    auto row = scan.next();
    ASSERT_TRUE(row.has_value());
    ASSERT_TRUE(row->has_value());

    const auto& tuple = row->value();
    ASSERT_EQ(tuple.values.size(), 1u);
    // Must be exactly INT64 with value 2.
    EXPECT_EQ(tuple.values[0].as_int64(), 2);

    scan.close();
}

TEST_F(GDB615_CountScan, PlanNodeDetailIncludesTableName) {
    TableHeap heap(*bpm_, dm_, file_id_);
    // Schema with table_name set.
    auto schema = OutputSchema({{"my_table", "__agg_0", TypeId::INT64, false, 0}});
    CountScanOperator scan(heap, std::move(schema));
    EXPECT_EQ(scan.plan_node_detail(), "on my_table");
}

TEST_F(GDB615_CountScan, PlanNodeDetailEmptyWhenNoTableName) {
    TableHeap heap(*bpm_, dm_, file_id_);
    CountScanOperator scan(heap, count_output_schema());
    // Default count_output_schema has empty table_name.
    EXPECT_EQ(scan.plan_node_detail(), "");
}

// ============================================================================
// Stress: high row count
// ============================================================================

TEST_F(GDB615_CountScan, StressFiveThousandRows) {
    TableHeap heap(*bpm_, dm_, file_id_);

    constexpr int N = 5000;
    for (int i = 0; i < N; ++i) {
        insert_row(heap, i, "s" + std::to_string(i));
    }

    EXPECT_EQ(run_count_scan(heap), N);
    EXPECT_EQ(run_count_scan(heap), count_via_seq_scan(heap));
}

// ============================================================================
// Interleaved operations: insert, count, delete, count, insert, count
// ============================================================================

TEST_F(GDB615_CountScan, InterleavedInsertDeleteCount) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Start empty.
    EXPECT_EQ(run_count_scan(heap), 0);

    // Insert 10 rows.
    std::vector<RID> rids;
    for (int i = 0; i < 10; ++i) {
        rids.push_back(insert_row(heap, i, "v" + std::to_string(i)));
    }
    EXPECT_EQ(run_count_scan(heap), 10);

    // Delete 5 (even IDs).
    for (int i = 0; i < 10; i += 2) {
        auto del = heap.delete_tuple(rids[i]);
        ASSERT_TRUE(del.has_value());
    }
    EXPECT_EQ(run_count_scan(heap), 5);

    // Insert 3 more.
    for (int i = 100; i < 103; ++i) {
        insert_row(heap, i, "new_" + std::to_string(i));
    }
    EXPECT_EQ(run_count_scan(heap), 8);

    // Always agree with reference.
    EXPECT_EQ(run_count_scan(heap), count_via_seq_scan(heap));
}
