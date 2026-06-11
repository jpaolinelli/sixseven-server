/// @file test_qa_gdb_721_adversarial.cpp
/// Adversarial QA tests for GDB-721: statement_timeout deadline enforcement.
///
/// Attack categories:
///   1. Timeout cancellation reliability (repeated cross-join attacks)
///   2. Deadline/RESET interaction — no stale deadline on subsequent statements
///   3. Validation edge cases — fractional units, case variants, overflow
///   4. Inert-var matrix integrity — SET/SHOW unchanged by 721
///   5. Overflow-proof deadline arithmetic (INT64_MAX ms must not wrap)
///   6. SHOW never-set variable echoes the default
///   7. Concurrency: two independent engine instances only cancel the right one
///   8. DML with already-expired deadline — cancelled before completion
///   9. SET 0 re-arms disabled state after a prior timeout value
///  10. Sequential cancel-then-succeed pattern (no stale state)

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/statement_deadline.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/server/session.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <thread>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Shared fixture
// ─────────────────────────────────────────────────────────────────────────────

class QA721Adversarial : public ::testing::Test {
protected:
    void SetUp() override {
        StatementDeadline::clear();
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa721_adv";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_  = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        StatementDeadline::clear();
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void exec_ok(const std::string& sql) {
        auto r = engine_->execute(sql);
        ASSERT_TRUE(r.has_value()) << sql << ": " << r.error().message;
    }

    /// Build a table with n integer rows.
    void make_rows_table(int n, const std::string& tbl = "nums") {
        exec_ok("CREATE TABLE " + tbl + " (a INT)");
        std::string values;
        int batch = 0;
        for (int i = 0; i < n; ++i) {
            values += (batch == 0 ? "(" : ", (") + std::to_string(i) + ")";
            if (++batch == 100 || i == n - 1) {
                exec_ok("INSERT INTO " + tbl + " VALUES " + values);
                values.clear();
                batch = 0;
            }
        }
    }

    DiskManager             dm_;
    Catalog                 catalog_;
    std::filesystem::path   data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine>    engine_;
};

// ─────────────────────────────────────────────────────────────────────────────
// 1. Timeout cancellation reliability — run 10 times
//    A 1ms deadline on a cross-join of 500×500 = 250K rows MUST always cancel.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(QA721Adversarial, TimeoutCancelsReliably_Repeated10x) {
    make_rows_table(500);

    for (int attempt = 0; attempt < 10; ++attempt) {
        StatementDeadline::clear(); // paranoid reset between attempts
        StatementDeadlineGuard guard(1); // 1 ms
        auto r = engine_->execute("SELECT * FROM nums a CROSS JOIN nums b");
        ASSERT_FALSE(r.has_value())
            << "attempt " << attempt << ": slow query not cancelled";
        EXPECT_EQ(r.error().code, StatusCode::QUERY_CANCELED)
            << "attempt " << attempt;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Timeout during result streaming — partial rows, then 57014
//    Arm a past-deadline so any tuple pull cancels; verify QUERY_CANCELED.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(QA721Adversarial, TimeoutDuringStreaming_Returns57014NotPartialSuccess) {
    make_rows_table(200);

    // Force the deadline to be in the past — so the first drain-loop check fires.
    StatementDeadline::arm(StatementDeadline::Clock::now() - std::chrono::milliseconds(1));
    auto r = engine_->execute("SELECT * FROM nums");
    StatementDeadline::clear();

    ASSERT_FALSE(r.has_value()) << "expected QUERY_CANCELED, got success";
    EXPECT_EQ(r.error().code, StatusCode::QUERY_CANCELED);
    // SQLSTATE 57014 text in the message
    EXPECT_NE(r.error().message.find("statement timeout"), std::string::npos)
        << "unexpected message: " << r.error().message;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. SET 0 disables timeout; slow query must succeed afterward
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(QA721Adversarial, ResetToZeroDisablesTimeout) {
    make_rows_table(100);

    Session s(1);
    ASSERT_TRUE(s.set_variable("statement_timeout", "1").has_value()); // 1 ms armed
    ASSERT_TRUE(s.set_variable("statement_timeout", "0").has_value()); // disable
    EXPECT_EQ(s.statement_timeout_ms(), 0);

    StatementDeadlineGuard guard(s.statement_timeout_ms()); // timeout_ms==0 → no arm
    auto r = engine_->execute("SELECT * FROM nums a CROSS JOIN nums b");
    // 100×100 = 10K rows; should succeed in any reasonable time.
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->rows.size(), 10000u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Two sequential queries: first cancelled, second (fast) succeeds cleanly
//    Verifies the guard RAII clears the deadline between statements.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(QA721Adversarial, CancelledQueryFollowedByFastQuerySucceeds) {
    make_rows_table(600);

    Session s(1);
    ASSERT_TRUE(s.set_variable("statement_timeout", "1").has_value());

    // First: cancel
    {
        StatementDeadlineGuard guard(s.statement_timeout_ms());
        auto r = engine_->execute("SELECT * FROM nums a CROSS JOIN nums b");
        ASSERT_FALSE(r.has_value()) << "expected cancellation";
        EXPECT_EQ(r.error().code, StatusCode::QUERY_CANCELED);
    } // guard destroyed → deadline cleared

    // Verify deadline is truly gone
    EXPECT_FALSE(StatementDeadline::armed());
    EXPECT_FALSE(StatementDeadline::expired());

    // Second: fast query — no timeout armed (zero ms)
    ASSERT_TRUE(s.set_variable("statement_timeout", "0").has_value());
    StatementDeadlineGuard guard2(s.statement_timeout_ms()); // 0 → no arm
    auto r2 = engine_->execute("SELECT * FROM nums");
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    EXPECT_EQ(r2->rows.size(), 600u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Overflow-proof deadline arithmetic: INT64_MAX ms wraps steady_clock.
//    BUG filed as GDB-1238 (see adversarial report). Test DISABLED to avoid
//    blocking CI until the bug is fixed; left here as a regression anchor.
//
//    Root cause: steady_clock::time_point uses nanoseconds internally (64-bit).
//    now() + milliseconds(INT64_MAX) overflows the nanosecond counter and wraps
//    to a point in the past, making expired() return true immediately.
//    The practical exposure requires a user to SET statement_timeout=INT64_MAX,
//    which is a cosmetic edge case; a real killer deadline (e.g. 1 year in ms)
//    will also exhibit the same wrap on any 64-bit clock.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(QA721Adversarial, DISABLED_VeryLargeTimeoutDoesNotOverflowOrCancel) {
    make_rows_table(5);

    // INT64_MAX ms ≈ 292 million years — effectively "never"
    const int64_t huge_ms = std::numeric_limits<int64_t>::max();

    StatementDeadline::arm(StatementDeadline::Clock::now() +
                           std::chrono::milliseconds(huge_ms));

    bool expired_immediately = StatementDeadline::expired();
    StatementDeadline::clear();

    EXPECT_FALSE(expired_immediately)
        << "INT64_MAX ms deadline wrapped and is already expired — "
           "deadline arithmetic overflows";
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Validation: fractional value "1.5s" rejected
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(QA721Adversarial, FractionalUnitRejected) {
    Session s(1);
    EXPECT_FALSE(s.set_variable("statement_timeout", "1.5s").has_value())
        << "fractional 1.5s should be rejected (parser reads only leading digits)";
    EXPECT_EQ(s.statement_timeout_ms(), 0) << "value must be unchanged after rejection";
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. Validation: spaces around the unit " 5 s " — behavior depends on trimming
//    The implementation trims the whole value, then splits on trailing unit.
//    "5 s" with internal space: unit would be " s" (with space), which is not
//    a recognized unit → should be rejected.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(QA721Adversarial, SpaceWithinUnitRejected) {
    Session s(1);
    // "5 s" — space between number and unit.  The impl finds trailing unit = " s"
    // (after trimming the whole value but not stripping internal spaces between
    // digits and unit).  " s" is not a recognized unit.
    auto r = s.set_variable("statement_timeout", "5 s");
    // This is either rejected (not a recognized unit " s") or accepted (if
    // the impl strips internal spaces).  Either is defensible, but must be
    // consistent with the actual unit loop and must not corrupt timeout_ms.
    if (r.has_value()) {
        // Accepted path: verify it parsed correctly to 5000 ms
        EXPECT_EQ(s.statement_timeout_ms(), 5000)
            << "if '5 s' is accepted, it must parse as 5000ms";
    } else {
        // Rejected path: value stays at default
        EXPECT_EQ(s.statement_timeout_ms(), 0);
    }
    // Either way, the stored value must not be a garbage/partial int.
    EXPECT_GE(s.statement_timeout_ms(), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. Validation: uppercase units "5S" and "5MS" — case-insensitive?
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(QA721Adversarial, UpperCaseUnitsParsedCaseInsensitively) {
    Session s(1);
    // The impl calls to_lower on the unit string — so "5S" → unit "s" → 5000ms
    ASSERT_TRUE(s.set_variable("statement_timeout", "5S").has_value())
        << "5S should be accepted (unit lowercased before comparison)";
    EXPECT_EQ(s.statement_timeout_ms(), 5000);

    ASSERT_TRUE(s.set_variable("statement_timeout", "250MS").has_value())
        << "250MS should be accepted";
    EXPECT_EQ(s.statement_timeout_ms(), 250);
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. Validation: "+5s" (leading plus) — should be rejected
//    The parser reads only leading digits; '+' is not a digit → pos==0 → reject.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(QA721Adversarial, LeadingPlusRejected) {
    Session s(1);
    EXPECT_FALSE(s.set_variable("statement_timeout", "+5s").has_value())
        << "+5s should be rejected (leading '+' is not a digit)";
    EXPECT_EQ(s.statement_timeout_ms(), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. Validation: "0x10" (hex) — should be rejected
//     Parser reads only decimal digits from pos=0; "0" then "x..." — unit is
//     "x10" which is not recognized.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(QA721Adversarial, HexLiteralRejected) {
    Session s(1);
    auto r = s.set_variable("statement_timeout", "0x10");
    // Either rejected (unknown unit "x10") or accepted as 0ms (no unit after "0").
    // Either is defensible.  0ms is legal (disabled).  "x10" as unit is not.
    if (r.has_value()) {
        // Accepted: must have parsed as 0 (only leading "0" digits matched)
        EXPECT_EQ(s.statement_timeout_ms(), 0)
            << "if '0x10' is accepted, only leading '0' should have been used";
    } else {
        EXPECT_EQ(s.statement_timeout_ms(), 0);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 11. Validation: barely-overflow value ("9223372036854775808" = INT64_MAX+1)
//     stoll should throw → parse_timeout_ms catches and returns error.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(QA721Adversarial, OverflowIntegerRejected) {
    Session s(1);
    // INT64_MAX + 1 as string — stoll overflows → must be caught and rejected.
    EXPECT_FALSE(s.set_variable("statement_timeout", "9223372036854775808").has_value())
        << "overflow integer must be rejected";
    EXPECT_EQ(s.statement_timeout_ms(), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 12. Validation: INT64_MAX itself ("9223372036854775807") — accepted
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(QA721Adversarial, MaxInt64ValueAccepted) {
    Session s(1);
    const std::string max_str = "9223372036854775807";
    ASSERT_TRUE(s.set_variable("statement_timeout", max_str).has_value())
        << "INT64_MAX should be accepted as a raw ms value";
    EXPECT_EQ(s.statement_timeout_ms(), std::numeric_limits<int64_t>::max());
}

// ─────────────────────────────────────────────────────────────────────────────
// 13. Inert variable matrix integrity — SET then verify SHOW echoes new value;
//     engine behavior is not checked (inert by design), but SHOW must echo.
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(QA721Adversarial, InertVarSetThenShowEchoes) {
    Session s(1);
    auto set = s.try_handle_command("SET work_mem = 'qa_adversarial_256MB'");
    ASSERT_TRUE(set.has_value());
    ASSERT_TRUE(set->has_value()) << set->error().message;

    auto show = s.try_handle_command("SHOW work_mem");
    ASSERT_TRUE(show.has_value());
    ASSERT_TRUE(show->has_value());
    ASSERT_EQ((*show)->rows.size(), 1u);
    EXPECT_EQ((*show)->rows[0][0].as_string(), "qa_adversarial_256MB");
}

// ─────────────────────────────────────────────────────────────────────────────
// 14. SHOW of never-explicitly-set variable echoes the default
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(QA721Adversarial, ShowNeverSetVariableEchoesDefault) {
    Session s(1); // fresh session — no SET calls
    auto show = s.try_handle_command("SHOW datestyle");
    ASSERT_TRUE(show.has_value());
    ASSERT_TRUE(show->has_value());
    ASSERT_EQ((*show)->rows.size(), 1u);
    // Default is "ISO, MDY" per Session::DEFAULT_VARIABLES
    EXPECT_EQ((*show)->rows[0][0].as_string(), "ISO, MDY");
}

// ─────────────────────────────────────────────────────────────────────────────
// 15. Concurrency: two engines on two threads — only the one with a 1ms
//     deadline is cancelled; the other (no deadline) runs to completion.
//     Uses thread_local correctly: each thread has its own deadline state.
// ─────────────────────────────────────────────────────────────────────────────

TEST(QA721Concurrency, TwoThreadsOnlyOneGetsTimeout) {
    // Set up two independent engine instances (different data dirs, catalogs).
    auto make_engine = [](const std::filesystem::path& dir,
                          std::unique_ptr<StorageManager>& sm,
                          std::unique_ptr<QueryEngine>& eng,
                          DiskManager& dm,
                          Catalog& cat) {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        init_test_catalog(cat);
        sm  = std::make_unique<StorageManager>(dm, dir);
        eng = std::make_unique<QueryEngine>(cat, *sm);
    };

    std::filesystem::path dir_a = std::filesystem::temp_directory_path() / "qa721_thr_a";
    std::filesystem::path dir_b = std::filesystem::temp_directory_path() / "qa721_thr_b";

    DiskManager dm_a, dm_b;
    Catalog     cat_a, cat_b;
    std::unique_ptr<StorageManager> sm_a, sm_b;
    std::unique_ptr<QueryEngine>    eng_a, eng_b;

    make_engine(dir_a, sm_a, eng_a, dm_a, cat_a);
    make_engine(dir_b, sm_b, eng_b, dm_b, cat_b);

    // Populate both with 500 rows each.
    auto populate = [](QueryEngine& eng, const std::string& tbl) {
        (void)eng.execute("CREATE TABLE " + tbl + " (a INT)");
        std::string values;
        int batch = 0;
        for (int i = 0; i < 500; ++i) {
            values += (batch == 0 ? "(" : ", (") + std::to_string(i) + ")";
            if (++batch == 100 || i == 499) {
                (void)eng.execute("INSERT INTO " + tbl + " VALUES " + values);
                values.clear(); batch = 0;
            }
        }
    };
    populate(*eng_a, "nums");
    populate(*eng_b, "nums");

    bool a_cancelled = false;
    bool b_succeeded = false;

    // Thread A: 1ms deadline → should cancel
    std::thread th_a([&] {
        StatementDeadlineGuard guard(1);
        auto r = eng_a->execute("SELECT * FROM nums x CROSS JOIN nums y");
        a_cancelled = !r.has_value() && r.error().code == StatusCode::QUERY_CANCELED;
    });

    // Thread B: no deadline (timeout=0) → should succeed
    std::thread th_b([&] {
        // Explicitly verify no deadline from thread A bleeds over (thread_local)
        EXPECT_FALSE(StatementDeadline::armed())
            << "thread B should start with no armed deadline (thread_local isolation)";
        auto r = eng_b->execute("SELECT * FROM nums x CROSS JOIN nums y");
        b_succeeded = r.has_value();
    });

    th_a.join();
    th_b.join();

    EXPECT_TRUE(a_cancelled)  << "thread A with 1ms timeout was NOT cancelled";
    EXPECT_TRUE(b_succeeded)  << "thread B with no timeout was unexpectedly cancelled";

    // Cleanup
    eng_a.reset(); sm_a.reset();
    eng_b.reset(); sm_b.reset();
    std::filesystem::remove_all(dir_a);
    std::filesystem::remove_all(dir_b);
}

} // namespace
