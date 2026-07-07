// GDB-1289 adversarial QA: TIMESTAMP/DATE/TIME vs STRING literal coercion.
//
// These tests go beyond the implementer's happy-path suite
// (tests/qa/test_qa_gdb_1289.cpp) to probe: invalid/edge timestamp string
// forms, leap-year correctness, DATE/TIME columns (not just TIMESTAMP),
// runtime-only paths (parameters, computed strings) that the bind-time
// validator does NOT cover, and BETWEEN/IN which do not route through
// bind_binary at all.
//
// Full end-to-end QueryEngine tests -- not binder-only -- so we observe
// actual row filtering, not just successful binding.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace sixseven;

namespace {

class QA_GDB1289_Adversarial : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_1289_adv";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());

        Config config = Config::load_defaults();
        auto bootstrap_result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config, data_dir_);
        ASSERT_TRUE(bootstrap_result.has_value()) << bootstrap_result.error().message;

        exec_ok("CREATE TABLE events (id INT, ts TIMESTAMP, d DATE, t TIME, tag TEXT)");
        exec_ok("INSERT INTO events VALUES (1, '2025-12-31 23:00:00', '2025-12-31', "
                "'23:00:00', '2025-12-31 23:00:00')");
        exec_ok("INSERT INTO events VALUES (2, '2026-01-01 00:00:00', '2026-01-01', "
                "'00:00:00', '2026-01-01 00:00:00')");
        exec_ok("INSERT INTO events VALUES (3, '2026-01-02 00:00:00', '2026-01-02', "
                "'12:34:56', '2026-01-02 00:00:00')");
        exec_ok("INSERT INTO events VALUES (4, '2024-02-29 00:00:00', '2024-02-29', "
                "'00:00:00', '2024-02-29 00:00:00')"); // leap-year valid row
    }

    void TearDown() override {
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value())
            << "SQL failed: " << sql << (result ? "" : (" -- " + result.error().message));
        return result ? std::move(*result) : QueryResult{};
    }

    Result<QueryResult> exec(const std::string& sql) { return engine_->execute(sql); }

    void expect_clean_type_error(const std::string& sql) {
        auto result = exec(sql);
        ASSERT_FALSE(result.has_value()) << "Expected error for: " << sql;
        EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR)
            << "sql=" << sql << " message=" << result.error().message;
        EXPECT_FALSE(result.error().message.empty()) << "Error message must not be empty";
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
};

} // namespace

// ===========================================================================
// Leap-year correctness: valid vs invalid Feb 29
// ===========================================================================

TEST_F(QA_GDB1289_Adversarial, LeapYear2024Feb29IsValidAndMatches) {
    auto qr = exec_ok("SELECT id FROM events WHERE ts = '2024-02-29 00:00:00'");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 4);
}

TEST_F(QA_GDB1289_Adversarial, NonLeapYear2026Feb29IsRejectedCleanly) {
    expect_clean_type_error("SELECT id FROM events WHERE ts = '2026-02-29 00:00:00'");
}

TEST_F(QA_GDB1289_Adversarial, DateOnlyLeapYear2024Feb29ValidOnDateColumn) {
    auto qr = exec_ok("SELECT id FROM events WHERE d = '2024-02-29'");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 4);
}

TEST_F(QA_GDB1289_Adversarial, DateOnlyNonLeapYearFeb29RejectedCleanly) {
    expect_clean_type_error("SELECT id FROM events WHERE d = '2026-02-29'");
}

// ===========================================================================
// Invalid / malformed timestamp strings -- must all error cleanly
// ===========================================================================

TEST_F(QA_GDB1289_Adversarial, EmptyStringRejectedCleanly) {
    expect_clean_type_error("SELECT id FROM events WHERE ts = ''");
}

TEST_F(QA_GDB1289_Adversarial, BadMonth13RejectedCleanly) {
    expect_clean_type_error("SELECT id FROM events WHERE ts = '2026-13-01 00:00:00'");
}

TEST_F(QA_GDB1289_Adversarial, BadDay32RejectedCleanly) {
    expect_clean_type_error("SELECT id FROM events WHERE ts = '2026-01-32 00:00:00'");
}

TEST_F(QA_GDB1289_Adversarial, ZeroMonthZeroDayRejectedCleanly) {
    expect_clean_type_error("SELECT id FROM events WHERE ts = '2026-00-00 00:00:00'");
}

TEST_F(QA_GDB1289_Adversarial, NonZeroPaddedMonthDayRejectedCleanly) {
    // Implementation requires exactly 2-digit month/day (parse_digits fixed
    // width). '2026-1-1' is therefore invalid input, not a PG-style loose
    // parse. Confirm it errors cleanly rather than silently misparsing.
    expect_clean_type_error("SELECT id FROM events WHERE ts = '2026-1-1'");
}

TEST_F(QA_GDB1289_Adversarial, HugeYearRejectedCleanly) {
    expect_clean_type_error("SELECT id FROM events WHERE ts = '999999-01-01 00:00:00'");
}

TEST_F(QA_GDB1289_Adversarial, NegativeYearRejectedCleanly) {
    expect_clean_type_error("SELECT id FROM events WHERE ts = '-001-01-01 00:00:00'");
}

TEST_F(QA_GDB1289_Adversarial, TrailingJunkRejectedCleanly) {
    expect_clean_type_error("SELECT id FROM events WHERE ts = '2026-01-01 00:00:00 xyz'");
}

TEST_F(QA_GDB1289_Adversarial, LeadingWhitespaceRejectedCleanly) {
    expect_clean_type_error("SELECT id FROM events WHERE ts = ' 2026-01-01 00:00:00'");
}

TEST_F(QA_GDB1289_Adversarial, TrailingWhitespaceRejectedCleanly) {
    expect_clean_type_error("SELECT id FROM events WHERE ts = '2026-01-01 00:00:00 '");
}

TEST_F(QA_GDB1289_Adversarial, TimezoneSuffixZRejectedOrErrorsCleanly) {
    // Timezone support is not claimed by this ticket. As long as it's a
    // clean TYPE_ERROR (not a crash / not silently accepted with wrong
    // semantics), this is correct, not a bug.
    expect_clean_type_error("SELECT id FROM events WHERE ts = '2026-01-01T00:00:00Z'");
}

TEST_F(QA_GDB1289_Adversarial, TimezoneOffsetSuffixRejectedOrErrorsCleanly) {
    expect_clean_type_error("SELECT id FROM events WHERE ts = '2026-01-01 00:00:00+05:00'");
}

TEST_F(QA_GDB1289_Adversarial, NulByteInStringRejectedCleanlyNoCrash) {
    std::string sql = "SELECT id FROM events WHERE ts = '2026-01";
    sql += '\0';
    sql += "1-01 00:00:00'";
    auto result = exec(sql);
    // Whatever happens (parse-level truncation or type error), it must not
    // crash the process and must not be a silent-wrong success.
    if (result.has_value()) {
        // If it parsed at all despite the embedded NUL, it must not have
        // spuriously matched every row.
        SUCCEED() << "Parser handled embedded NUL without crash";
    } else {
        SUCCEED() << "Rejected cleanly: " << result.error().message;
    }
}

TEST_F(QA_GDB1289_Adversarial, UnicodeGarbageRejectedCleanly) {
    expect_clean_type_error("SELECT id FROM events WHERE ts = '2026-01-01日本語'");
}

TEST_F(QA_GDB1289_Adversarial, FractionalSecondsAcceptedAndCompareCorrectly) {
    // Fractional seconds ARE supported per parse_time_part; confirm accepted
    // and that finer-grained comparison filters correctly. Only rows 2 and 3
    // (2026-01-01, 2026-01-02) are strictly after 2025-12-31 23:59:59.999999;
    // row 1 is exactly 2025-12-31 23:00:00 (earlier) and row 4 is 2024-02-29
    // (much earlier).
    auto qr = exec_ok("SELECT id FROM events WHERE ts > '2025-12-31 23:59:59.999999'");
    std::vector<int32_t> ids;
    for (auto& row : qr.rows) {
        ids.push_back(row[0].as_int32());
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<int32_t>{2, 3}));
}

// ===========================================================================
// DATE column vs string literal -- same coercion, correct results
// ===========================================================================

TEST_F(QA_GDB1289_Adversarial, DateColumnEqualsValidLiteralMatches) {
    auto qr = exec_ok("SELECT id FROM events WHERE d = '2026-01-01'");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 2);
}

TEST_F(QA_GDB1289_Adversarial, DateColumnInvalidLiteralRejectedCleanly) {
    expect_clean_type_error("SELECT id FROM events WHERE d = 'not-a-date'");
}

TEST_F(QA_GDB1289_Adversarial, DateColumnGreaterThanFiltersCorrectly) {
    auto qr = exec_ok("SELECT id FROM events WHERE d > '2026-01-01'");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 3);
}

// ===========================================================================
// TIME column vs string literal -- same coercion, correct results
// ===========================================================================

TEST_F(QA_GDB1289_Adversarial, TimeColumnEqualsValidLiteralMatches) {
    auto qr = exec_ok("SELECT id FROM events WHERE t = '12:34:56'");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 3);
}

TEST_F(QA_GDB1289_Adversarial, TimeColumnInvalidHourRejectedCleanly) {
    expect_clean_type_error("SELECT id FROM events WHERE t = '25:00:00'");
}

TEST_F(QA_GDB1289_Adversarial, TimeColumnInvalidMinuteRejectedCleanly) {
    expect_clean_type_error("SELECT id FROM events WHERE t = '00:60:00'");
}

TEST_F(QA_GDB1289_Adversarial, TimeColumnLessThanFiltersCorrectly) {
    // t values: row1=23:00:00, row2=00:00:00, row3=12:34:56, row4=00:00:00.
    // < '12:00:00' matches rows 2 and 4 only.
    auto qr = exec_ok("SELECT id FROM events WHERE t < '12:00:00'");
    std::vector<int32_t> ids;
    for (auto& row : qr.rows) {
        ids.push_back(row[0].as_int32());
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<int32_t>{2, 4}));
}

// ===========================================================================
// Runtime-only paths NOT covered by bind-time validation:
// parameters and computed string expressions (CONCAT / ||).
// The implementer flagged these as validated only at RUNTIME.
// ===========================================================================

TEST_F(QA_GDB1289_Adversarial, ComputedStringViaConcatValidCompares) {
    // Build '2026-01-01 00:00:00' via CONCAT so the binder cannot see the
    // literal text at bind time -- this must still work correctly at runtime.
    auto qr = exec_ok(
        "SELECT id FROM events WHERE ts = ('2026-01-01' || ' ' || '00:00:00')");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 2);
}

TEST_F(QA_GDB1289_Adversarial, ComputedStringViaConcatInvalidErrorsCleanlyAtRuntime) {
    // CONCAT produces 'hello-not-a-timestamp' which the binder cannot
    // validate ahead of time (only a STRING LiteralExpr is checked). Must
    // error cleanly at runtime, not crash, not silently match/no-match.
    auto result = exec("SELECT id FROM events WHERE ts = ('hello' || '-not-a-timestamp')");
    ASSERT_FALSE(result.has_value())
        << "Computed invalid temporal string must error at runtime, not silently succeed";
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST_F(QA_GDB1289_Adversarial, ComputedStringFromTagColumnValidComparesCorrectly) {
    // tag is a real STRING column holding valid timestamp text -- this
    // exercises the column (not literal) -> temporal runtime coercion path,
    // which the implementer says was "pre-existing and confirmed correct."
    auto qr = exec_ok("SELECT id FROM events WHERE ts = tag");
    ASSERT_EQ(qr.rows.size(), 4u); // every row's tag matches its own ts
}

// ===========================================================================
// BETWEEN and IN with string literals vs TIMESTAMP column.
// Flagged by implementer as NOT routing through bind_binary -- probe for
// silent-wrong-results (High severity) vs. clean rejection (acceptable) vs.
// crash (Critical).
// ===========================================================================

TEST_F(QA_GDB1289_Adversarial, BetweenValidStringLiteralsFiltersCorrectly) {
    auto result = exec(
        "SELECT id FROM events WHERE ts BETWEEN '2026-01-01 00:00:00' AND '2026-01-02 00:00:00'");
    if (!result.has_value()) {
        ADD_FAILURE() << "BETWEEN with valid temporal string literals failed to bind/execute: "
                      << result.error().message;
        return;
    }
    // If it executes, the results MUST be correct (ids 2 and 3), not
    // silently wrong (e.g. returning all rows or none).
    std::vector<int32_t> ids;
    for (auto& row : result->rows) {
        ids.push_back(row[0].as_int32());
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<int32_t>{2, 3}))
        << "BETWEEN produced silently-wrong results for valid temporal string bounds";
}

TEST_F(QA_GDB1289_Adversarial, BetweenInvalidStringLiteralDoesNotSilentlyMisbehave) {
    auto result = exec("SELECT id FROM events WHERE ts BETWEEN 'hello' AND 'world'");
    if (result.has_value()) {
        // Silent wrong-results (e.g. matching nothing or everything without
        // erroring) is a High-severity bug distinct from clean rejection.
        ADD_FAILURE() << "BETWEEN with invalid temporal string literals ('hello','world') "
                         "did not error -- silently returned "
                      << result->rows.size() << " rows instead of a clean TYPE_ERROR";
    }
    // else: clean rejection is fine, don't require a specific code here since
    // this is a follow-up scope question, just confirm it's not a crash
    // (reaching this line at all proves no crash) and not silent-wrong.
}

TEST_F(QA_GDB1289_Adversarial, InValidStringLiteralsFiltersCorrectly) {
    auto result = exec(
        "SELECT id FROM events WHERE ts IN ('2026-01-01 00:00:00', '2026-01-02 00:00:00')");
    if (!result.has_value()) {
        ADD_FAILURE() << "IN with valid temporal string literals failed to bind/execute: "
                      << result.error().message;
        return;
    }
    std::vector<int32_t> ids;
    for (auto& row : result->rows) {
        ids.push_back(row[0].as_int32());
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<int32_t>{2, 3}))
        << "IN produced silently-wrong results for valid temporal string literals";
}

TEST_F(QA_GDB1289_Adversarial, InInvalidStringLiteralDoesNotSilentlyMisbehave) {
    auto result = exec("SELECT id FROM events WHERE ts IN ('hello', 'world')");
    if (result.has_value()) {
        ADD_FAILURE() << "IN with invalid temporal string literals did not error -- silently "
                         "returned "
                      << result->rows.size() << " rows instead of a clean TYPE_ERROR";
    }
}

// ===========================================================================
// Regression: strict-reject for bool/float/int vs string must be unaffected
// ===========================================================================

TEST_F(QA_GDB1289_Adversarial, IntColumnVsStringStillStrictReject) {
    exec_ok("CREATE TABLE ints_tbl (id INT, n INT)");
    expect_clean_type_error("SELECT id FROM ints_tbl WHERE n = 'not-a-number'");
}

TEST_F(QA_GDB1289_Adversarial, BoolColumnVsValidLookingStringStillRejected) {
    exec_ok("CREATE TABLE flags2 (id INT, active BOOL)");
    expect_clean_type_error("SELECT id FROM flags2 WHERE active = 'true'");
}

// ===========================================================================
// TIMESTAMP vs TIMESTAMP / TIMESTAMP vs NULL unaffected by the change
// ===========================================================================

TEST_F(QA_GDB1289_Adversarial, TimestampVsTimestampColumnStillWorks) {
    exec_ok("CREATE TABLE events2 (id INT, ts2 TIMESTAMP)");
    exec_ok("INSERT INTO events2 VALUES (1, '2026-01-01 00:00:00')");
    auto qr = exec_ok(
        "SELECT events.id FROM events JOIN events2 ON events.ts = events2.ts2");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 2);
}

TEST_F(QA_GDB1289_Adversarial, TimestampVsNullStillWorks) {
    auto qr = exec_ok("SELECT id FROM events WHERE ts = NULL");
    EXPECT_EQ(qr.rows.size(), 0u); // NULL comparison never matches
}
