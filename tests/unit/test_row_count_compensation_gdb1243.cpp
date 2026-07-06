/// Regression tests for GDB-1243.
///
/// PART A: implicit (autocommit) transaction aborts must compensate the
/// per-heap live row_count_ counter for whatever partial progress the failed
/// statement made, exactly like the explicit ROLLBACK path already does.
/// Without the fix, a mid-statement autocommit DML error leaves row_count_
/// (and therefore the COUNT(*) fast path) permanently drifted:
///   - a failed multi-row DELETE undercounts (rows deleted before the error
///     never get re-added back to the counter),
///   - a failed multi-row INSERT...SELECT overcounts,
///   - a failed UPDATE (new version inserted, old version's mark_deleted
///     fails) leaks +1 per affected row.
///
/// PART B: active_txn_row_deltas_ must not hold a use-after-free-prone raw
/// TableHeap* across a DROP TABLE inside an explicit transaction followed by
/// ROLLBACK.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/update.h"
#include "sixseven/index/rid.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

class RowCountCompensationTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_row_count_gdb1243";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

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
        EXPECT_TRUE(result.has_value())
            << "exec failed for: " << sql
            << " :: " << (result ? std::string{} : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    /// Execute SQL expected to fail; returns the error status code.
    StatusCode exec_error(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "expected failure for: " << sql;
        return result ? StatusCode::OK : result.error().code;
    }

    int64_t count_star(const std::string& table) {
        auto qr = exec_ok("SELECT COUNT(*) FROM " + table);
        EXPECT_EQ(qr.rows.size(), 1u);
        if (qr.rows.empty() || qr.rows[0].empty()) {
            return -1;
        }
        return qr.rows[0][0].as_int64();
    }

    /// Read TableHeap::row_count() directly (bypassing the query engine and
    /// its planner-level COUNT(*) fast-path routing, which -- for MVCC heaps
    /// with an active read view -- always recomputes the count via a
    /// visibility-filtered scan rather than trusting the live counter; see
    /// CountScanOperator::do_next). This is the actual counter that GDB-1243's
    /// compensation logic (active_txn_row_deltas_ / adjust_row_count)
    /// mutates, so asserting on it directly is what makes these tests fail
    /// deterministically before the fix and pass after it.
    int64_t heap_row_count(const std::string& table) {
        auto schema = catalog_.get_table(current_db_, table);
        EXPECT_TRUE(schema.has_value()) << "table not found: " << table;
        if (!schema) {
            return -1;
        }
        auto ts = storage_->get_table_storage(schema->table_id);
        EXPECT_TRUE(ts.has_value()) << "table storage not found: " << table;
        if (!ts || *ts == nullptr || (*ts)->heap == nullptr) {
            return -1;
        }
        return static_cast<int64_t>((*ts)->heap->row_count());
    }

    database_id_t current_db_ = default_database_id;

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

} // namespace

// =============================================================================
// AC1: mid-statement autocommit DELETE error -> COUNT(*) is not undercounted.
// =============================================================================

TEST_F(RowCountCompensationTest, AutocommitDeleteMidScanErrorDoesNotUndercount) {
    exec_ok("CREATE TABLE t (id INT, name VARCHAR)");
    exec_ok("INSERT INTO t VALUES (1, 'a')");
    exec_ok("INSERT INTO t VALUES (2, 'b')");
    exec_ok("INSERT INTO t VALUES (3, 'c')");
    ASSERT_EQ(count_star("t"), 3);

    // Autocommit (no explicit BEGIN) DELETE with a per-row WHERE predicate
    // that divides by zero on the second scanned row (id = 2): row id=1 is
    // deleted (row_count_ decremented) before the predicate errors on id=2,
    // aborting the whole statement. Without the fix, row_count_ stays at 2
    // even though the implicit transaction's abort makes id=1's deletion
    // invisible again.
    auto code = exec_error("DELETE FROM t WHERE 10 / (id - 2) > -1000");
    EXPECT_EQ(code, StatusCode::INVALID_ARGUMENT);

    // All three rows must still be visible and both the visibility-scan
    // COUNT(*) and the raw row_count_ counter must match the true count.
    auto rows = exec_ok("SELECT id FROM t");
    EXPECT_EQ(rows.rows.size(), 3u) << "aborted DELETE must leave every row visible";
    EXPECT_EQ(count_star("t"), 3) << "COUNT(*) must not be undercounted after a "
                                     "mid-statement autocommit DELETE error";
    EXPECT_EQ(heap_row_count("t"), 3)
        << "row_count_ must be restored to 3 after the implicit txn abort compensates "
           "the one row that was deleted before the mid-scan error";
}

// =============================================================================
// AC2: UPDATE partial failure -> counters correct (no +1 leak).
//
// UpdateOperator materializes every target row's new SET-applied values
// *before* mutating the heap (Halloween-problem avoidance; see update.cpp),
// so a SET-expression error can only ever be hit during materialization,
// before any row_count_ mutation has happened. The actual leak window this
// AC targets -- an old version's mark_deleted() failing *after* its paired
// new version's insert_tuple() already succeeded -- can only be forced with
// a heap-level fault, since UPDATE's SQL-visible failure points (SET
// expression errors, WHERE errors) all occur upstream of any mutation. This
// test drives UpdateOperator directly (white-box) with a real MVCC heap and
// a fake two-row child iterator whose second row's RID targets a page that
// does not exist, so its insert (new version, +1) succeeds but its paired
// mark_deleted (old version, -1) fails with a real TableHeap error --
// reproducing exactly the leak scenario the ticket describes.
// =============================================================================

namespace {

/// A trivial fixed-list child iterator: returns each queued tuple in order,
/// matching the id/name storage schema used by UpdateOperatorLeakTest.
class FixedRowsIterator : public Iterator {
public:
    explicit FixedRowsIterator(std::vector<Tuple> rows, OutputSchema schema)
        : rows_(std::move(rows)), schema_(std::move(schema)) {}

    const OutputSchema& output_schema() const override { return schema_; }

protected:
    Result<void> do_open() override {
        idx_ = 0;
        return ok();
    }

    Result<std::optional<Tuple>> do_next() override {
        if (idx_ >= rows_.size()) {
            return ok(std::optional<Tuple>(std::nullopt));
        }
        return ok(std::optional<Tuple>(rows_[idx_++]));
    }

    void do_close() override {}

private:
    std::vector<Tuple> rows_;
    OutputSchema schema_;
    size_t idx_ = 0;
};

} // namespace

class UpdateOperatorLeakTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_test_update_leak_gdb1243.db";
        std::filesystem::remove(path_);
        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;
        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);

        storage_schema_ = Schema({{"id", TypeId::INT32}, {"name", TypeId::STRING}});
        output_schema_ = OutputSchema({
            OutputColumn{"", "id", TypeId::INT32, false, 0},
            OutputColumn{"", "name", TypeId::STRING, true, 0},
        });

        // MVCC-enabled heap so UpdateOperator takes the insert_tuple +
        // mark_deleted path (the one that can leak) rather than the legacy
        // in-place update_tuple path.
        TableHeapOptions opts;
        opts.mvcc_headers = true;
        heap_ = std::make_unique<TableHeap>(*bpm_, dm_, file_id_, opts);
    }

    void TearDown() override {
        heap_.reset();
        bpm_.reset();
        auto close = dm_.close_file(file_id_);
        (void)close;
        std::filesystem::remove(path_);
    }

    RID insert_row(int32_t id, const std::string& name) {
        std::vector<Value> vals = {Value(id), Value(name)};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        EXPECT_TRUE(bytes.has_value()) << bytes.error().message;
        auto rid = heap_->insert_tuple(*bytes, frozen_txn_id);
        EXPECT_TRUE(rid.has_value()) << rid.error().message;
        return rid ? *rid : RID::invalid();
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<TableHeap> heap_;
    Schema storage_schema_;
    OutputSchema output_schema_;
};

TEST_F(UpdateOperatorLeakTest, MarkDeletedFailureAfterInsertSucceedsLeavesNoLeak) {
    RID good_rid = insert_row(1, "alice");
    ASSERT_TRUE(heap_->row_count() == 1);

    // Row 2's tuple carries an RID that does not exist in this heap's file
    // (page 9999 was never allocated), so its old-version mark_deleted() will
    // fail at the fetch_page step -- after its paired new-version
    // insert_tuple() has already succeeded and incremented row_count_.
    RID bogus_rid{9999, 0};

    Tuple t1;
    t1.values = {Value(1), Value(std::string("alice"))};
    t1.rid = good_rid;

    Tuple t2;
    t2.values = {Value(2), Value(std::string("ghost"))};
    t2.rid = bogus_rid;

    std::vector<Tuple> rows;
    rows.push_back(std::move(t1));
    rows.push_back(std::move(t2));

    auto child = std::make_unique<FixedRowsIterator>(std::move(rows), output_schema_);

    BoundStatement bound;
    std::vector<UpdateAssignment> assignments;
    // SET name = 'updated' for every row (column index 1 in storage schema).
    static const auto new_name_expr = [] {
        auto e = std::make_unique<LiteralExpr>();
        e->kind = LiteralKind::STRING;
        e->value = "updated";
        return e;
    }();
    assignments.push_back(UpdateAssignment{1, new_name_expr.get()});

    UpdateOperator update_op(
        *heap_, storage_schema_, std::move(child), std::move(assignments), bound);

    auto open_result = update_op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    auto result = update_op.next();
    ASSERT_FALSE(result.has_value())
        << "expected mark_deleted on the bogus RID to fail the statement";
    update_op.close();

    // Row 1 (good_rid) was fully updated: net-zero (+1 insert, -1
    // mark_deleted). Row 2's new version was inserted (+1) but its paired
    // mark_deleted never completed, so without compensation row_count_ would
    // be left at 2 (1 real live row + 1 leaked phantom) instead of 1.
    //
    // row_delta_so_far() must reflect exactly this leaked state so the
    // QueryEngine-level compensation (GDB-1243) can reverse it precisely.
    EXPECT_EQ(update_op.row_delta_so_far(), 1)
        << "row_delta_so_far() must report the +1 leak from row 2's orphaned "
           "new version (insert succeeded, paired mark_deleted failed)";

    // Directly reproduce what compensate_row_deltas_and_clear() would do: an
    // abort must reverse row_delta_so_far() exactly to restore row_count_ to
    // its pre-statement value (1 -- only good_rid's original row, still
    // uncompensated for its own net-zero update since that nets to nothing).
    heap_->adjust_row_count(-update_op.row_delta_so_far());
    EXPECT_EQ(heap_->row_count(), 1u)
        << "after compensating the reported partial delta, row_count_ must "
           "match the true number of live rows (id=1's updated version)";
}

// =============================================================================
// AC3: failed autocommit INSERT...SELECT -> COUNT(*) not overcounted.
// =============================================================================

TEST_F(RowCountCompensationTest, AutocommitInsertSelectMidScanErrorDoesNotOvercount) {
    exec_ok("CREATE TABLE src (id INT)");
    exec_ok("INSERT INTO src VALUES (1)");
    exec_ok("INSERT INTO src VALUES (2)");
    exec_ok("INSERT INTO src VALUES (300)"); // overflows the TINYINT target column.

    exec_ok("CREATE TABLE dst (id TINYINT)");
    ASSERT_EQ(count_star("dst"), 0);

    // INSERT ... SELECT copies src.id into dst.id (TINYINT, range
    // [-128,127]). Rows 1 and 2 fit and are inserted (row_count_ incremented
    // for each); the third row (300) overflows TINYINT's range and errors,
    // aborting the whole statement after two rows were already committed to
    // row_count_.
    auto code = exec_error("INSERT INTO dst SELECT id FROM src");
    EXPECT_EQ(code, StatusCode::TYPE_ERROR);

    EXPECT_EQ(count_star("dst"), 0) << "COUNT(*) must not be overcounted after a "
                                       "mid-statement autocommit INSERT...SELECT error";
    EXPECT_EQ(heap_row_count("dst"), 0)
        << "row_count_ must be compensated back to 0 -- the two rows inserted "
           "before the third row's overflow error must not leak into the counter";
    auto rows = exec_ok("SELECT * FROM dst");
    EXPECT_TRUE(rows.rows.empty()) << "no row inserted by the aborted statement should "
                                      "be visible";
}

// =============================================================================
// AC4: DROP TABLE inside an explicit transaction + ROLLBACK -> no crash/UAF,
// and other tables' counters are still correct.
// =============================================================================

TEST_F(RowCountCompensationTest, DropTableInExplicitTxnThenRollbackNoUseAfterFree) {
    exec_ok("CREATE TABLE t (id INT)");
    exec_ok("INSERT INTO t VALUES (1)");
    exec_ok("INSERT INTO t VALUES (2)");

    exec_ok("CREATE TABLE survivor (id INT)");
    exec_ok("INSERT INTO survivor VALUES (10)");
    ASSERT_EQ(count_star("survivor"), 1);

    exec_ok("BEGIN");
    // Accumulate a live-row-count delta on 'survivor' inside the explicit
    // transaction, keyed by table_id (GDB-1243) so it survives 't' being
    // dropped in the same transaction.
    exec_ok("INSERT INTO survivor VALUES (11)");
    // Drop 't' inside the transaction: its TableHeap is freed here (storage_
    // .drop_table_storage flushes + closes + deletes the file).
    exec_ok("DROP TABLE t");
    // Rolling back must not dereference the freed heap for 't', and must
    // still correctly compensate 'survivor's counter.
    exec_ok("ROLLBACK");

    // 't' was dropped for real (DDL is not transactional here) -- dropping it
    // is not undone by ROLLBACK; the point of this test is that resolving
    // any stale table_id entry for it at compensation time is a safe no-op
    // rather than a use-after-free.
    auto dropped = engine_->execute("SELECT * FROM t");
    EXPECT_FALSE(dropped.has_value());

    // 'survivor' must be compensated back to its pre-transaction count.
    EXPECT_EQ(count_star("survivor"), 1)
        << "surviving table's counter must still be compensated correctly "
           "after a DROP TABLE + ROLLBACK in the same explicit transaction";
    EXPECT_EQ(heap_row_count("survivor"), 1)
        << "row_count_ for the surviving table must be compensated correctly";
}
