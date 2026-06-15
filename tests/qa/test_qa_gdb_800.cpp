// QA regression tests for GDB-800:
// ExternalSortOperator constructor must clamp max_merge_width <= 1 to 2,
// preventing an infinite loop in reduce_runs().
//
// Attack surface:
//  - merge_width = 0 (zero — inner cursor stuck, no progress)
//  - merge_width = 1 (one — each group is 1 run, reduce_runs never converges)
//  - merge_width = SIZE_MAX (overflow in std::min / i+max_merge_width_)
//  - merge_width = 2 (minimum valid: must work correctly, not be further clamped)
//  - Large merge_width (> run count: only one final-merge pass needed)
//  - Single run produced (reduce_runs loop not entered — clamp irrelevant path)
//  - Exactly N runs == merge_width (boundary: loop body entered once, outputs 1 run)
//  - Correctness under very high run counts with small merge widths
//  - Silent clamp: clamped value must produce identical sorted output to explicit
//    width-2 invocation on the same data

#include "sixseven/executor/external_sort.h"
#include "sixseven/executor/iterator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace sixseven;

// =============================================================================
// Local helpers (keep QA file self-contained)
// =============================================================================

namespace {

class VecSource : public Iterator {
public:
    VecSource(std::vector<Tuple> rows, OutputSchema schema)
        : rows_(std::move(rows)), schema_(std::move(schema)) {}

    const OutputSchema& output_schema() const override { return schema_; }

protected:
    Result<void> do_open() override {
        cursor_ = 0;
        return ok();
    }
    Result<std::optional<Tuple>> do_next() override {
        if (cursor_ >= rows_.size()) {
            return ok(std::optional<Tuple>(std::nullopt));
        }
        return ok(std::optional<Tuple>(rows_[cursor_++]));
    }
    void do_close() override { cursor_ = 0; }

private:
    std::vector<Tuple> rows_;
    OutputSchema schema_;
    size_t cursor_ = 0;
};

ExprPtr col_ref(const std::string& name) {
    auto e = std::make_unique<ColumnRefExpr>();
    e->column = name;
    return e;
}

Tuple row_int(int32_t v) {
    return Tuple{{Value(v)}, std::nullopt};
}

// Drain all rows from an already-constructed ExternalSortOperator.
// Returns empty vector on error (ASSERT fires).
std::vector<Tuple> drain_all(ExternalSortOperator& op) {
    auto open = op.open();
    if (!open) {
        ADD_FAILURE() << "open() failed: " << open.error().message;
        return {};
    }
    std::vector<Tuple> out;
    while (true) {
        auto r = op.next();
        if (!r) {
            ADD_FAILURE() << "next() failed: " << r.error().message;
            return out;
        }
        if (!r->has_value()) break;
        out.push_back(std::move(r->value()));
    }
    op.close();
    return out;
}

// Generate N integers descending (N-1, N-2, …, 0) so external sort is needed
// (tiny work_mem) and the output must be strictly ascending if sorting works.
std::vector<Tuple> descending_ints(int32_t n) {
    std::vector<Tuple> tuples;
    tuples.reserve(static_cast<size_t>(n));
    for (int32_t i = n - 1; i >= 0; --i) {
        tuples.push_back(row_int(i));
    }
    return tuples;
}

OutputSchema int_schema() {
    return OutputSchema{{{{"", "v", TypeId::INT32, false, 0}}}};
}

// Verify ascending order.
void expect_ascending(const std::vector<Tuple>& rows, int32_t expected_count) {
    ASSERT_EQ(static_cast<int32_t>(rows.size()), expected_count);
    for (size_t i = 1; i < rows.size(); ++i) {
        EXPECT_LE(rows[i - 1].values[0].as_int32(), rows[i].values[0].as_int32())
            << "Order violation at index " << i;
    }
}

// Tiny work_mem (1 byte) guarantees every tuple becomes its own run, giving
// us full control over run count vs merge width.
constexpr size_t FORCE_SPILL = 1u;

} // namespace

// =============================================================================
// QA fixture
// =============================================================================

class QA_GDB800 : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb800";
        std::filesystem::remove_all(temp_dir_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    std::filesystem::path temp_dir_;
};

// =============================================================================
// AC1: merge_width=0 clamped — does not hang or crash, output is correct
// =============================================================================

TEST_F(QA_GDB800, MergeWidth0NeverHangs) {
    // 10 tuples, 1-byte work_mem → 10 runs. merge_width=0 would make
    // reduce_runs advance i by 0 every pass and loop forever without the clamp.
    auto tuples = descending_ints(10);
    auto source = std::make_unique<VecSource>(std::move(tuples), int_schema());

    BoundStatement bound;
    auto expr = col_ref("v");
    std::vector<SortKey> keys = {{expr.get(), SortDirection::ASC}};

    ExternalSortOperator op(std::move(source), std::move(keys), bound, FORCE_SPILL, 0, temp_dir_);
    auto results = drain_all(op);
    expect_ascending(results, 10);
}

// =============================================================================
// AC2: merge_width=1 clamped — does not hang or crash, output is correct
// =============================================================================

TEST_F(QA_GDB800, MergeWidth1NeverHangs) {
    // 15 tuples, many runs. merge_width=1 gives a group of exactly 1 run per
    // iteration — no merge happens, run count stays the same, infinite loop.
    auto tuples = descending_ints(15);
    auto source = std::make_unique<VecSource>(std::move(tuples), int_schema());

    BoundStatement bound;
    auto expr = col_ref("v");
    std::vector<SortKey> keys = {{expr.get(), SortDirection::ASC}};

    ExternalSortOperator op(std::move(source), std::move(keys), bound, FORCE_SPILL, 1, temp_dir_);
    auto results = drain_all(op);
    expect_ascending(results, 15);
}

// =============================================================================
// AC3: merge_width=2 (explicit minimum valid) — must work and NOT be clamped
//       further (i.e. sorting still completes correctly)
// =============================================================================

TEST_F(QA_GDB800, MergeWidth2WorksCorrectly) {
    auto tuples = descending_ints(20);
    auto source = std::make_unique<VecSource>(std::move(tuples), int_schema());

    BoundStatement bound;
    auto expr = col_ref("v");
    std::vector<SortKey> keys = {{expr.get(), SortDirection::ASC}};

    ExternalSortOperator op(std::move(source), std::move(keys), bound, FORCE_SPILL, 2, temp_dir_);
    auto results = drain_all(op);
    expect_ascending(results, 20);
}

// =============================================================================
// AC4: clamp is silent — output from width=0 and width=1 must be identical
//       to output from explicit width=2
// =============================================================================

TEST_F(QA_GDB800, ClampedOutputMatchesExplicitWidth2) {
    // Run the same data three times (different temp dirs to avoid conflicts)
    // and confirm all produce identical sorted sequences.
    auto base_tuples = descending_ints(12);

    BoundStatement bound;
    auto expr0 = col_ref("v");
    auto expr1 = col_ref("v");
    auto expr2 = col_ref("v");

    std::vector<SortKey> keys0 = {{expr0.get(), SortDirection::ASC}};
    std::vector<SortKey> keys1 = {{expr1.get(), SortDirection::ASC}};
    std::vector<SortKey> keys2 = {{expr2.get(), SortDirection::ASC}};

    auto td0 = temp_dir_ / "w0";
    auto td1 = temp_dir_ / "w1";
    auto td2 = temp_dir_ / "w2";

    ExternalSortOperator op0(
        std::make_unique<VecSource>(base_tuples, int_schema()),
        std::move(keys0), bound, FORCE_SPILL, 0, td0);
    ExternalSortOperator op1(
        std::make_unique<VecSource>(base_tuples, int_schema()),
        std::move(keys1), bound, FORCE_SPILL, 1, td1);
    ExternalSortOperator op2(
        std::make_unique<VecSource>(base_tuples, int_schema()),
        std::move(keys2), bound, FORCE_SPILL, 2, td2);

    auto r0 = drain_all(op0);
    auto r1 = drain_all(op1);
    auto r2 = drain_all(op2);

    ASSERT_EQ(r0.size(), 12u);
    ASSERT_EQ(r1.size(), 12u);
    ASSERT_EQ(r2.size(), 12u);

    for (size_t i = 0; i < 12u; ++i) {
        EXPECT_EQ(r0[i].values[0].as_int32(), r2[i].values[0].as_int32())
            << "width=0 vs width=2 mismatch at index " << i;
        EXPECT_EQ(r1[i].values[0].as_int32(), r2[i].values[0].as_int32())
            << "width=1 vs width=2 mismatch at index " << i;
    }
}

// =============================================================================
// AC5: merge_width = SIZE_MAX — must not overflow i + max_merge_width_ in
//       reduce_runs; std::min saturates safely, so loop exits in one pass
// =============================================================================

TEST_F(QA_GDB800, MergeWidthSizeMaxNoOverflow) {
    // With merge_width > run_count, reduce_runs loop condition is false from
    // the start → no loop, falls straight through to setup_final_merge.
    // If std::min overflows, i+max_merge_width_ wraps to < run_files_.size()
    // and the loop spins incorrectly — this test catches that.
    auto tuples = descending_ints(8);
    auto source = std::make_unique<VecSource>(std::move(tuples), int_schema());

    BoundStatement bound;
    auto expr = col_ref("v");
    std::vector<SortKey> keys = {{expr.get(), SortDirection::ASC}};

    ExternalSortOperator op(std::move(source), std::move(keys), bound, FORCE_SPILL,
                            std::numeric_limits<size_t>::max(), temp_dir_);
    auto results = drain_all(op);
    expect_ascending(results, 8);
}

// =============================================================================
// AC6: exactly merge_width runs (boundary: loop not entered, one merge pass)
// =============================================================================

TEST_F(QA_GDB800, ExactlyMergeWidthRunsProducesSingleFinalMerge) {
    // 4 tuples, each forced to its own run (1-byte work_mem), merge_width=4.
    // run_files_.size() == max_merge_width_ → reduce_runs loop body never runs.
    // setup_final_merge must open all 4 readers and produce sorted output.
    auto tuples = descending_ints(4);
    auto source = std::make_unique<VecSource>(std::move(tuples), int_schema());

    BoundStatement bound;
    auto expr = col_ref("v");
    std::vector<SortKey> keys = {{expr.get(), SortDirection::ASC}};

    ExternalSortOperator op(std::move(source), std::move(keys), bound, FORCE_SPILL, 4, temp_dir_);
    auto results = drain_all(op);
    expect_ascending(results, 4);
}

// =============================================================================
// AC7: single run produced (reduce_runs not entered regardless of clamp)
// =============================================================================

TEST_F(QA_GDB800, SingleRunBypassesReduceRunsLoop) {
    // 1 tuple with work_mem_bytes_ = 1. generate_runs will accumulate 1 tuple,
    // mem_used >= 1 is true, but buffer.size() == 1 so the flush guard
    // ("buffer.size() > 1") suppresses the mid-stream flush. The remaining
    // buffer is flushed after the loop, producing exactly 1 run file.
    // reduce_runs loop condition (run_files_.size() > max_merge_width_) is
    // false (1 > 2 is false), so no loop — but the clamp must not have broken
    // anything, and the single row must come back correctly.
    auto tuples = std::vector<Tuple>{row_int(42)};
    auto source = std::make_unique<VecSource>(std::move(tuples), int_schema());

    BoundStatement bound;
    auto expr = col_ref("v");
    std::vector<SortKey> keys = {{expr.get(), SortDirection::ASC}};

    ExternalSortOperator op(std::move(source), std::move(keys), bound, FORCE_SPILL, 1, temp_dir_);
    auto results = drain_all(op);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].values[0].as_int32(), 42);
}

// =============================================================================
// AC8: high run count (100 runs) + small merge_width=3 → many reduce passes
//       Tests that clamped OR real width=3 correctly converges across passes
// =============================================================================

TEST_F(QA_GDB800, HighRunCountWithSmallMergeWidthConverges) {
    // 100 tuples, each its own run, merge_width=3.
    // ceil(100/3)=34 runs after pass 1, ceil(34/3)=12, ceil(12/3)=4, 4<=3 → no.
    // ceil(4/3)=2, 2<=3 → stop.  Total 4 reduce passes before final merge.
    auto tuples = descending_ints(100);
    auto source = std::make_unique<VecSource>(std::move(tuples), int_schema());

    BoundStatement bound;
    auto expr = col_ref("v");
    std::vector<SortKey> keys = {{expr.get(), SortDirection::ASC}};

    ExternalSortOperator op(std::move(source), std::move(keys), bound, FORCE_SPILL, 3, temp_dir_);
    auto results = drain_all(op);
    expect_ascending(results, 100);
}

// =============================================================================
// AC9: width=1 with exactly 2 tuples (minimum useful dataset for clamp to matter)
// =============================================================================

TEST_F(QA_GDB800, MergeWidth1ExactlyTwoTuplesDoesNotHang) {
    // 2 tuples, each its own run → run_files_.size()==2, max_merge_width_==2
    // after clamp. reduce_runs condition: 2 > 2 is false, loop not entered.
    // Final merge reads 2 runs. Must not hang.
    auto tuples = std::vector<Tuple>{row_int(99), row_int(1)};
    auto source = std::make_unique<VecSource>(std::move(tuples), int_schema());

    BoundStatement bound;
    auto expr = col_ref("v");
    std::vector<SortKey> keys = {{expr.get(), SortDirection::ASC}};

    ExternalSortOperator op(std::move(source), std::move(keys), bound, FORCE_SPILL, 1, temp_dir_);
    auto results = drain_all(op);

    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 99);
}

// =============================================================================
// AC10: width=0 with empty input — must not crash or loop
// =============================================================================

TEST_F(QA_GDB800, MergeWidth0EmptyInputDoesNotCrash) {
    auto source = std::make_unique<VecSource>(std::vector<Tuple>{}, int_schema());

    BoundStatement bound;
    auto expr = col_ref("v");
    std::vector<SortKey> keys = {{expr.get(), SortDirection::ASC}};

    ExternalSortOperator op(std::move(source), std::move(keys), bound, FORCE_SPILL, 0, temp_dir_);
    auto results = drain_all(op);
    EXPECT_TRUE(results.empty());
}

// =============================================================================
// AC11: width=0 repeated open/close (operator re-use)
// =============================================================================

TEST_F(QA_GDB800, MergeWidth0CanBeOpenedAndClosedTwice) {
    auto tuples = descending_ints(5);
    auto source = std::make_unique<VecSource>(tuples, int_schema());

    BoundStatement bound;
    auto expr = col_ref("v");
    std::vector<SortKey> keys = {{expr.get(), SortDirection::ASC}};

    ExternalSortOperator op(std::move(source), std::move(keys), bound, FORCE_SPILL, 0, temp_dir_);

    // First drain.
    auto r1 = drain_all(op);
    expect_ascending(r1, 5);

    // Second open/drain (re-use).
    auto r2 = drain_all(op);
    expect_ascending(r2, 5);
}

// =============================================================================
// AC12: DESC sort with merge_width=1 (clamp + direction)
// =============================================================================

TEST_F(QA_GDB800, MergeWidth1DescSortCorrect) {
    // Ascending input, sort DESC, merge_width=1 → must clamp and sort correctly.
    std::vector<Tuple> tuples;
    for (int32_t i = 0; i < 10; ++i) tuples.push_back(row_int(i));
    auto source = std::make_unique<VecSource>(std::move(tuples), int_schema());

    BoundStatement bound;
    auto expr = col_ref("v");
    std::vector<SortKey> keys = {{expr.get(), SortDirection::DESC}};

    ExternalSortOperator op(std::move(source), std::move(keys), bound, FORCE_SPILL, 1, temp_dir_);
    auto results = drain_all(op);

    ASSERT_EQ(results.size(), 10u);
    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_GE(results[i - 1].values[0].as_int32(), results[i].values[0].as_int32())
            << "DESC order violation at index " << i;
    }
}
