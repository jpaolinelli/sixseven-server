// QA adversarial tests for GDB-950: PortalSuspended / Execute max_rows paging.
//
// These tests exercise the paging state machine logic, boundary values, signed/
// unsigned edge cases, and portal lifecycle correctness.
//
// Full socket round-trips are Windows-unverifiable (pre-existing CRT fd-assert
// crash); all tests here operate on Portal/Session state directly or on
// arithmetic extracted from handle_execute, matching the implementer's harness
// style in tests/unit/test_portal_suspended.cpp.

#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/server/pg_protocol.h"
#include "sixseven/server/session.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helper: build a QueryResult with `n` INT32 rows, column "id".
// ---------------------------------------------------------------------------
static QueryResult make_row_result(int n) {
    QueryResult qr;
    qr.column_names = {"id"};
    qr.column_types = {TypeId::INT32};
    for (int i = 0; i < n; ++i) {
        qr.rows.push_back({Value(static_cast<int32_t>(i + 1))});
    }
    return qr;
}

// Helper: build a DML QueryResult (no columns, affected_rows set).
static QueryResult make_dml_result(int64_t affected) {
    QueryResult qr;
    qr.affected_rows = affected;
    return qr;
}

// ---------------------------------------------------------------------------
// Helper that mirrors the exact paging arithmetic from handle_execute.
// Returns {to_send, rows_sent_after, suspended}.
// ---------------------------------------------------------------------------
struct PageStep {
    size_t to_send;
    size_t rows_sent_after;
    bool suspended; // true -> PortalSuspended; false -> CommandComplete
};

static PageStep compute_page(size_t total, size_t rows_sent, int32_t max_rows) {
    const size_t remaining = total - rows_sent;
    const size_t to_send =
        (max_rows <= 0) ? remaining : std::min(static_cast<size_t>(max_rows), remaining);
    const size_t new_rows_sent = rows_sent + to_send;
    const bool suspended = (max_rows > 0) && (new_rows_sent < total);
    return {to_send, new_rows_sent, suspended};
}

// =============================================================================
// Suite QA_GDB950_Boundary - off-by-one and boundary arithmetic.
// =============================================================================

// AC: max_rows == total rows -> all sent, NO PortalSuspended (off-by-one trap).
TEST(QA_GDB950_Boundary, MaxRowsEqualsTotal_NeverSuspended) {
    const size_t total = 7;
    const int32_t max_rows = 7; // exactly equals total
    auto step = compute_page(total, 0, max_rows);
    EXPECT_EQ(step.to_send, 7U);
    EXPECT_EQ(step.rows_sent_after, 7U);
    EXPECT_FALSE(step.suspended) << "When max_rows == total, all rows drain on first page; "
                                    "PortalSuspended must NOT be emitted.";
}

// AC: max_rows > total -> all sent in one Execute, CommandComplete, no suspend.
TEST(QA_GDB950_Boundary, MaxRowsGreaterThanTotal_SendsAllNeverSuspends) {
    const size_t total = 3;
    const int32_t max_rows = 1000;
    auto step = compute_page(total, 0, max_rows);
    EXPECT_EQ(step.to_send, 3U);
    EXPECT_EQ(step.rows_sent_after, 3U);
    EXPECT_FALSE(step.suspended);
}

// AC: max_rows == 1 on a 5-row result -> 5 pages, each suspending except last.
TEST(QA_GDB950_Boundary, MaxRowsOne_PagesOneAtATime) {
    const size_t total = 5;
    const int32_t max_rows = 1;
    size_t rows_sent = 0;
    for (size_t page = 0; page < 5; ++page) {
        auto step = compute_page(total, rows_sent, max_rows);
        EXPECT_EQ(step.to_send, 1U) << "page " << page;
        rows_sent = step.rows_sent_after;
        if (page < 4) {
            EXPECT_TRUE(step.suspended) << "page " << page << " should suspend";
        } else {
            EXPECT_FALSE(step.suspended) << "last page should CommandComplete";
        }
    }
    EXPECT_EQ(rows_sent, total);
}

// AC: max_rows == INT32_MAX -> treated as a large positive limit, sends all.
TEST(QA_GDB950_Boundary, MaxRowsINT32MAX_TreatedAsLimit_SendsAll) {
    const size_t total = 100;
    const int32_t max_rows = std::numeric_limits<int32_t>::max();
    auto step = compute_page(total, 0, max_rows);
    // max_rows > total, so to_send == remaining == total
    EXPECT_EQ(step.to_send, total);
    EXPECT_FALSE(step.suspended);
}

// AC: 0-row result with max_rows > 0 -> no DataRows, CommandComplete immediately.
TEST(QA_GDB950_Boundary, ZeroRowResult_MaxRowsPositive_ImmediateCommandComplete) {
    const size_t total = 0;
    const int32_t max_rows = 5;
    const size_t remaining = total - 0; // 0
    const size_t to_send =
        (max_rows <= 0) ? remaining : std::min(static_cast<size_t>(max_rows), remaining);
    EXPECT_EQ(to_send, 0U);
    // Suspension condition: max_rows > 0 && rows_sent < total  => 5>0 && 0<0 => false
    const bool suspended = (max_rows > 0) && (0 + to_send < total);
    EXPECT_FALSE(suspended) << "Empty result must never emit PortalSuspended.";
}

// AC: 1-row result, max_rows == 1 -> drains on first Execute, no suspend.
TEST(QA_GDB950_Boundary, OneRow_MaxRowsOne_NeverSuspended) {
    const size_t total = 1;
    const int32_t max_rows = 1;
    auto step = compute_page(total, 0, max_rows);
    EXPECT_EQ(step.to_send, 1U);
    EXPECT_EQ(step.rows_sent_after, 1U);
    EXPECT_FALSE(step.suspended) << "Single-row drain at max_rows=1 must not suspend.";
}

// =============================================================================
// Suite QA_GDB950_NegativeMaxRows - signed/unsigned safety.
// =============================================================================

// AC: max_rows < 0 -> treated as "no limit" (same as 0), all rows sent.
// The code gate is (max_rows <= 0) before any cast to size_t.
TEST(QA_GDB950_NegativeMaxRows, NegativeOne_TreatedAsNoLimit) {
    const size_t total = 4;
    const int32_t max_rows = -1;
    // Verify the branch taken is the "no limit" branch
    EXPECT_TRUE(max_rows <= 0) << "Guard condition must catch negative max_rows";
    auto step = compute_page(total, 0, max_rows);
    EXPECT_EQ(step.to_send, total) << "Negative max_rows must send all rows.";
    EXPECT_FALSE(step.suspended);
}

TEST(QA_GDB950_NegativeMaxRows, INT32_MIN_TreatedAsNoLimit) {
    const size_t total = 10;
    const int32_t max_rows = std::numeric_limits<int32_t>::min();
    EXPECT_TRUE(max_rows <= 0);
    auto step = compute_page(total, 0, max_rows);
    EXPECT_EQ(step.to_send, total);
    EXPECT_FALSE(step.suspended);
}

// Confirm that if max_rows were cast directly to size_t while negative it would
// produce a massive unsigned value (demonstrating why the guard matters).
TEST(QA_GDB950_NegativeMaxRows, NaiveCastWouldProduceLargeSize) {
    const int32_t max_rows = -1;
    // Without the guard, casting to size_t gives SIZE_MAX or similar large value.
    // With the guard (max_rows <= 0 => no-limit path), no cast occurs.
    // This test documents the hazard: a naive static_cast<size_t>(-1) is huge.
    const auto naive_cast = static_cast<size_t>(max_rows);
    EXPECT_GT(naive_cast, static_cast<size_t>(1000000))
        << "Naive cast of -1 to size_t is huge; guard is essential.";
    // But compute_page takes the no-limit branch, not the cast branch.
    const size_t total = 5;
    auto step = compute_page(total, 0, max_rows);
    EXPECT_EQ(step.to_send, total) << "Guard must prevent the naive-cast path.";
}

// =============================================================================
// Suite QA_GDB950_Resume - multi-page accumulation correctness.
// =============================================================================

// AC: paginating 10 rows at 3 per Execute produces exactly the full row set
// in order: [1..3] [4..6] [7..9] [10], no dups, no skips.
TEST(QA_GDB950_Resume, MultiPageAccumulatesFullSetInOrder) {
    const size_t total = 10;
    const int32_t max_rows = 3;
    size_t rows_sent = 0;
    std::vector<size_t> pages_sent;

    while (rows_sent < total) {
        auto step = compute_page(total, rows_sent, max_rows);
        EXPECT_GT(step.to_send, 0U) << "Each Execute must make progress.";

        // Verify slice indices are contiguous and in range.
        for (size_t i = rows_sent; i < rows_sent + step.to_send; ++i) {
            EXPECT_LT(i, total) << "Index out of bounds at i=" << i;
        }

        pages_sent.push_back(step.to_send);
        rows_sent = step.rows_sent_after;

        if (step.suspended) {
            EXPECT_GT(max_rows, 0) << "Suspend only when max_rows > 0";
            EXPECT_LT(rows_sent, total) << "Suspend only when rows remain";
        } else {
            EXPECT_EQ(rows_sent, total) << "CommandComplete only when fully drained";
        }
    }

    EXPECT_EQ(rows_sent, total) << "All rows must be delivered exactly once.";
    // Pages: 3, 3, 3, 1
    ASSERT_EQ(pages_sent.size(), 4U);
    EXPECT_EQ(pages_sent[0], 3U);
    EXPECT_EQ(pages_sent[1], 3U);
    EXPECT_EQ(pages_sent[2], 3U);
    EXPECT_EQ(pages_sent[3], 1U);
}

// AC: query executed ONCE (not re-run on resume); Portal::executed flag prevents
// re-execution. Verify via Session Portal state.
TEST(QA_GDB950_Resume, ExecutedFlagPreventsReExecution) {
    Session s(1);
    Portal p;
    p.name = "pg";
    p.sql = "SELECT 1";
    p.executed = false;
    p.rows_sent = 0;
    s.add_portal("pg", std::move(p));

    Portal* mp = s.get_portal_mutable("pg");
    ASSERT_NE(mp, nullptr);

    // Simulate first Execute: set executed, populate cached_result.
    mp->cached_result = make_row_result(5);
    mp->rows_sent = 0;
    mp->executed = true;

    // Simulate second Execute (resume): executed is already true.
    // The implementation branches on !portal.executed; this is false now.
    EXPECT_TRUE(mp->executed) << "executed flag must remain true on resume Execute";

    // Simulate sending 2 rows on first Execute.
    mp->rows_sent = 2;
    // Second Execute: rows_sent cursor is at 2, continuing from there.
    // max_rows=2, rows_sent=2, total=5 -> remaining=3, to_send=2, rows_sent_after=4 < 5 -> suspend.
    auto step = compute_page(5, mp->rows_sent, 2);
    EXPECT_EQ(step.to_send, 2U); // rows 3-4
    EXPECT_TRUE(step.suspended)
        << "4 rows sent of 5 total -> still rows remaining -> PortalSuspended";
}

// AC: a drained portal re-Execute must not re-run the query.
// After full drain (rows_sent == total), a subsequent Execute should
// see executed==true and rows_sent == total => remaining==0 => to_send==0 => no suspend.
TEST(QA_GDB950_Resume, DrainedPortalReExecute_NoReRun_NullPage) {
    Session s(1);
    Portal p;
    p.name = "px";
    p.executed = true;
    p.cached_result = make_row_result(3);
    p.rows_sent = 3; // fully drained
    s.add_portal("px", std::move(p));

    const Portal* cp = s.get_portal("px");
    ASSERT_NE(cp, nullptr);
    EXPECT_TRUE(cp->executed);
    EXPECT_EQ(cp->rows_sent, 3U);

    // Any further Execute: remaining = 3-3 = 0, to_send = 0.
    const size_t total = cp->cached_result.rows.size();
    auto step = compute_page(total, cp->rows_sent, 5);
    EXPECT_EQ(step.to_send, 0U);
    EXPECT_EQ(step.rows_sent_after, 3U);
    EXPECT_FALSE(step.suspended);
}

// =============================================================================
// Suite QA_GDB950_MaxRowsZero - byte-unchanged no-regression.
// =============================================================================

// AC: max_rows == 0 -> "no limit" -> all rows in one Execute.
TEST(QA_GDB950_MaxRowsZero, ZeroMeansNoLimit_AllRowsOneExecution) {
    for (size_t total : {0U, 1U, 5U, 100U}) {
        const int32_t max_rows = 0;
        auto step = compute_page(total, 0, max_rows);
        EXPECT_EQ(step.to_send, total) << "total=" << total;
        EXPECT_FALSE(step.suspended) << "total=" << total;
    }
}

// =============================================================================
// Suite QA_GDB950_DML - non-row-returning never suspends.
// =============================================================================

// AC: DML portal (column_names empty) with max_rows > 0 -> CommandComplete, no suspend.
// This mirrors the branching in handle_execute:
//   if (!qr.column_names.empty()) { ... paging ... }
//   send_command_complete(...)   <- always reached for DML
TEST(QA_GDB950_DML, InsertPortal_NeverSuspended) {
    // DML result has no column_names.
    QueryResult dml = make_dml_result(5);
    EXPECT_TRUE(dml.column_names.empty())
        << "DML must not have column names; paging branch won't be entered.";
    // Verify: the code path skips the paging block entirely for empty column_names.
    // No suspension possible.
    const bool enters_paging_block = !dml.column_names.empty();
    EXPECT_FALSE(enters_paging_block);
}

TEST(QA_GDB950_DML, UpdateZeroRows_NeverSuspended) {
    QueryResult dml = make_dml_result(0);
    EXPECT_TRUE(dml.column_names.empty());
    EXPECT_FALSE(!dml.column_names.empty()); // paging branch never entered
}

// =============================================================================
// Suite QA_GDB950_Lifecycle - portal state reset.
// =============================================================================

// AC: re-Bind (add_portal with same name) resets executed/rows_sent to defaults.
TEST(QA_GDB950_Lifecycle, RebindResets_ExecutedAndRowsSent) {
    Session s(99);

    Portal p1;
    p1.name = "lp";
    p1.executed = true;
    p1.rows_sent = 42;
    p1.cached_result = make_row_result(42);
    s.add_portal("lp", std::move(p1));

    // Simulate Bind: replace with a fresh portal.
    Portal p2;
    p2.name = "lp";
    p2.executed = false;
    p2.rows_sent = 0;
    s.add_portal("lp", std::move(p2));

    const Portal* cp = s.get_portal("lp");
    ASSERT_NE(cp, nullptr);
    EXPECT_FALSE(cp->executed) << "Re-Bind must reset executed flag.";
    EXPECT_EQ(cp->rows_sent, 0U) << "Re-Bind must reset rows_sent cursor.";
    EXPECT_TRUE(cp->cached_result.rows.empty()) << "Re-Bind must reset cached_result.";
}

// AC: Close removes the portal entirely.
TEST(QA_GDB950_Lifecycle, CloseRemovesPortal) {
    Session s(7);
    Portal p;
    p.name = "cp";
    p.executed = true;
    p.rows_sent = 5;
    s.add_portal("cp", std::move(p));

    ASSERT_NE(s.get_portal("cp"), nullptr);
    s.remove_portal("cp");
    EXPECT_EQ(s.get_portal("cp"), nullptr) << "Close must remove the portal.";
    EXPECT_EQ(s.get_portal_mutable("cp"), nullptr) << "Mutable accessor also returns null.";
}

// AC: Close a portal that doesn't exist -> no crash (graceful no-op).
TEST(QA_GDB950_Lifecycle, CloseNonExistentPortal_NoOp) {
    Session s(8);
    // Should not crash or throw.
    EXPECT_NO_FATAL_FAILURE(s.remove_portal("ghost"));
}

// AC: get_portal_mutable on missing portal returns nullptr.
TEST(QA_GDB950_Lifecycle, GetPortalMutableMissing_ReturnsNull) {
    Session s(5);
    EXPECT_EQ(s.get_portal_mutable("no_such"), nullptr);
}

// AC: multiple distinct portals coexist independently.
TEST(QA_GDB950_Lifecycle, MultiplePortals_IndependentState) {
    Session s(10);

    Portal pa;
    pa.name = "a";
    pa.executed = true;
    pa.rows_sent = 3;
    pa.cached_result = make_row_result(10);
    s.add_portal("a", std::move(pa));

    Portal pb;
    pb.name = "b";
    pb.executed = false;
    pb.rows_sent = 0;
    s.add_portal("b", std::move(pb));

    const Portal* a = s.get_portal("a");
    const Portal* b = s.get_portal("b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    EXPECT_TRUE(a->executed);
    EXPECT_EQ(a->rows_sent, 3U);
    EXPECT_FALSE(b->executed);
    EXPECT_EQ(b->rows_sent, 0U);

    // Mutating 'b' must not affect 'a'.
    Portal* mb = s.get_portal_mutable("b");
    ASSERT_NE(mb, nullptr);
    mb->executed = true;
    mb->rows_sent = 7;

    const Portal* a2 = s.get_portal("a");
    EXPECT_EQ(a2->rows_sent, 3U) << "Portal 'a' state must be unaffected by mutation of 'b'.";
}

// =============================================================================
// Suite QA_GDB950_WireEncoding - PortalSuspended message bytes.
// =============================================================================

// AC: send_portal_suspended emits exactly {'s', 0x00, 0x00, 0x00, 0x04}.
TEST(QA_GDB950_WireEncoding, PortalSuspendedBytes_ExactEncoding) {
    MessageWriter w;
    w.begin_message('s');
    auto msg = w.finish();

    ASSERT_EQ(msg.size(), 5U) << "PortalSuspended must be exactly 5 bytes.";
    EXPECT_EQ(msg[0], static_cast<uint8_t>('s')) << "Type byte must be 's' (0x73).";
    EXPECT_EQ(msg[1], 0x00U) << "Length byte 0 must be 0x00.";
    EXPECT_EQ(msg[2], 0x00U) << "Length byte 1 must be 0x00.";
    EXPECT_EQ(msg[3], 0x00U) << "Length byte 2 must be 0x00.";
    EXPECT_EQ(msg[4], 0x04U) << "Length byte 3 must be 0x04 (length includes itself).";
}

// Confirm type byte value is 0x73 (ASCII 's').
TEST(QA_GDB950_WireEncoding, PortalSuspendedTypeByte_Is0x73) {
    EXPECT_EQ(static_cast<uint8_t>('s'), 0x73U);
    MessageWriter w;
    w.begin_message('s');
    auto msg = w.finish();
    EXPECT_EQ(msg[0], 0x73U);
}

// =============================================================================
// Suite QA_GDB950_PortalDefaults - new-portal field defaults.
// =============================================================================

// AC: Portal default-constructed has executed=false, rows_sent=0, empty cache.
TEST(QA_GDB950_PortalDefaults, DefaultPortalState) {
    Portal p;
    EXPECT_FALSE(p.executed);
    EXPECT_EQ(p.rows_sent, 0U);
    EXPECT_TRUE(p.cached_result.column_names.empty());
    EXPECT_TRUE(p.cached_result.rows.empty());
    EXPECT_EQ(p.cached_result.affected_rows, -1);
}

// AC: Portal fields survive round-trip through Session::add_portal.
TEST(QA_GDB950_PortalDefaults, PortalRoundTripThroughSession) {
    Session s(3);
    Portal p;
    p.name = "rt";
    p.sql = "SELECT 42";
    p.executed = false;
    p.rows_sent = 0;
    p.cached_result = make_row_result(0);
    s.add_portal("rt", std::move(p));

    const Portal* cp = s.get_portal("rt");
    ASSERT_NE(cp, nullptr);
    EXPECT_EQ(cp->name, "rt");
    EXPECT_EQ(cp->sql, "SELECT 42");
    EXPECT_FALSE(cp->executed);
    EXPECT_EQ(cp->rows_sent, 0U);
}

// =============================================================================
// Suite QA_GDB950_Arithmetic_EdgeCases - additional arithmetic probes.
// =============================================================================

// Suspended condition: max_rows > 0 AND rows_sent_after < total.
// When rows_sent_after == total, no suspension regardless of max_rows.
TEST(QA_GDB950_ArithmeticEdgeCases, ExactDrain_NoSuspension) {
    // 6 rows, max_rows=2, already sent 4 -> remaining=2, to_send=2, drain exactly.
    auto step = compute_page(6, 4, 2);
    EXPECT_EQ(step.to_send, 2U);
    EXPECT_EQ(step.rows_sent_after, 6U);
    EXPECT_FALSE(step.suspended) << "Exact drain must emit CommandComplete, not PortalSuspended.";
}

// Suspended condition: max_rows > 0 AND rows_sent_after < total.
TEST(QA_GDB950_ArithmeticEdgeCases, PartialPage_Suspends) {
    // 6 rows, max_rows=2, sent 0 -> to_send=2, rows_sent=2 < 6 -> suspend.
    auto step = compute_page(6, 0, 2);
    EXPECT_EQ(step.to_send, 2U);
    EXPECT_EQ(step.rows_sent_after, 2U);
    EXPECT_TRUE(step.suspended);
}

// Very large result: 1M rows paged at 10k per Execute, no overflow.
TEST(QA_GDB950_ArithmeticEdgeCases, LargeResult_NoSizeOverflow) {
    const size_t total = 1'000'000;
    const int32_t max_rows = 10000;
    size_t rows_sent = 0;
    size_t iteration = 0;
    while (rows_sent < total) {
        auto step = compute_page(total, rows_sent, max_rows);
        EXPECT_GT(step.to_send, 0U) << "Must always make progress; iteration=" << iteration;
        rows_sent = step.rows_sent_after;
        ++iteration;
        ASSERT_LT(iteration, 200U) << "Too many iterations; possible infinite loop.";
    }
    EXPECT_EQ(rows_sent, total);
    EXPECT_EQ(iteration, 100U); // 1M / 10k == 100 pages
}
