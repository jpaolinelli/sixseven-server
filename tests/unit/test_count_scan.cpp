#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/count_scan.h"
#include "sixseven/executor/hash_aggregate.h"
#include "sixseven/executor/seq_scan.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
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

// -- Test fixture -------------------------------------------------------------

class CountScanTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_test_count_scan.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;

        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);

        // Schema: id (INT32), name (STRING)
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

    /// Insert a row into the heap.
    void insert_row(TableHeap& heap, int32_t id, const std::string& name) {
        std::vector<Value> vals = {Value(id), Value(name)};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        ASSERT_TRUE(bytes.has_value()) << bytes.error().message;
        auto rid = heap.insert_tuple(*bytes);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    /// Build the single-column output schema for COUNT(*) result.
    static OutputSchema count_output_schema() {
        return OutputSchema({{"", "__agg_0", TypeId::INT64, false, 0}});
    }

    /// Run CountScanOperator and return the count.
    int64_t run_count_scan(TableHeap& heap) {
        CountScanOperator scan(heap, *bpm_, count_output_schema());
        auto open = scan.open();
        EXPECT_TRUE(open.has_value()) << open.error().message;

        auto row = scan.next();
        EXPECT_TRUE(row.has_value()) << row.error().message;
        EXPECT_TRUE(row->has_value());

        int64_t count = row->value().values[0].as_int64();

        // Should be exhausted after one tuple.
        auto row2 = scan.next();
        EXPECT_TRUE(row2.has_value()) << row2.error().message;
        EXPECT_FALSE(row2->has_value());

        scan.close();
        return count;
    }

    /// Run a full SeqScan and count tuples manually (reference count).
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

// -- GDB-630: COUNT(*) fast path correctness ----------------------------------

TEST_F(CountScanTest, EmptyTableReturnsZero) {
    TableHeap heap(*bpm_, dm_, file_id_);
    EXPECT_EQ(run_count_scan(heap), 0);
}

TEST_F(CountScanTest, SingleRowReturnsOne) {
    TableHeap heap(*bpm_, dm_, file_id_);
    insert_row(heap, 1, "alice");
    EXPECT_EQ(run_count_scan(heap), 1);
}

TEST_F(CountScanTest, ManyRowsReturnsCorrectCount) {
    TableHeap heap(*bpm_, dm_, file_id_);
    constexpr int num_rows = 1500;
    for (int i = 0; i < num_rows; ++i) {
        insert_row(heap, i, "user_" + std::to_string(i));
    }
    EXPECT_EQ(run_count_scan(heap), num_rows);
}

TEST_F(CountScanTest, MatchesSeqScanCount) {
    TableHeap heap(*bpm_, dm_, file_id_);
    for (int i = 0; i < 100; ++i) {
        insert_row(heap, i, "row_" + std::to_string(i));
    }
    EXPECT_EQ(run_count_scan(heap), count_via_seq_scan(heap));
}

TEST_F(CountScanTest, CountAfterDeleteReturnsUpdatedCount) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert 5 rows, tracking RIDs.
    std::vector<RID> rids;
    for (int i = 0; i < 5; ++i) {
        std::vector<Value> vals = {Value(i), Value("row_" + std::to_string(i))};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        ASSERT_TRUE(bytes.has_value());
        auto rid = heap.insert_tuple(*bytes);
        ASSERT_TRUE(rid.has_value());
        rids.push_back(*rid);
    }

    EXPECT_EQ(run_count_scan(heap), 5);

    // Delete 2 rows.
    auto del1 = heap.delete_tuple(rids[1]);
    ASSERT_TRUE(del1.has_value()) << del1.error().message;
    auto del2 = heap.delete_tuple(rids[3]);
    ASSERT_TRUE(del2.has_value()) << del2.error().message;

    EXPECT_EQ(run_count_scan(heap), 3);
    EXPECT_EQ(run_count_scan(heap), count_via_seq_scan(heap));
}

TEST_F(CountScanTest, PlanNodeNameIsCountScan) {
    TableHeap heap(*bpm_, dm_, file_id_);
    CountScanOperator scan(heap, *bpm_, count_output_schema());
    EXPECT_EQ(scan.plan_node_name(), "Count Scan");
}

TEST_F(CountScanTest, OutputSchemaHasSingleInt64Column) {
    TableHeap heap(*bpm_, dm_, file_id_);
    CountScanOperator scan(heap, *bpm_, count_output_schema());
    EXPECT_EQ(scan.output_schema().column_count(), 1u);
    EXPECT_EQ(scan.output_schema().column(0).type_id, TypeId::INT64);
}
