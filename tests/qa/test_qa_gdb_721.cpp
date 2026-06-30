/// @file test_qa_gdb_721.cpp
/// QA regression tests for GDB-721: per-session SET variables were accepted
/// and SHOWn but never honored by the engine.
///
/// The fix makes statement_timeout HONORED: the protocol layer arms a
/// thread-local deadline (StatementDeadlineGuard) from the session value
/// before each query executor call, and QueryEngine's root drain loop cancels
/// the statement with QUERY_CANCELED (SQLSTATE 57014) once the deadline
/// passes. Granularity: checked between tuple pulls at the plan root.
///
/// All other session variables remain accepted-but-inert for PostgreSQL
/// driver compatibility; the matrix is documented at
/// Session::DEFAULT_VARIABLES in src/server/session.cpp and pinned here.

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
#include <memory>
#include <string>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

class QA721StatementTimeoutTest : public ::testing::Test {
protected:
    void SetUp() override {
        StatementDeadline::clear();
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa721_timeout";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        StatementDeadline::clear();
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        ASSERT_TRUE(result.has_value()) << sql << ": " << result.error().message;
    }

    /// Create a table with `n` integer rows (batched inserts).
    void make_rows_table(int n) {
        exec_ok("CREATE TABLE nums (a INT)");
        std::string values;
        int in_batch = 0;
        for (int i = 0; i < n; ++i) {
            values += (in_batch == 0 ? "(" : ", (") + std::to_string(i) + ")";
            if (++in_batch == 100 || i == n - 1) {
                exec_ok("INSERT INTO nums VALUES " + values);
                values.clear();
                in_batch = 0;
            }
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// -----------------------------------------------------------------------------
// AC1: regression — statement_timeout must actually cancel a slow query.
// Pre-fix, the SET was swallowed by the session layer and the query ran to
// completion; this test failed.
// -----------------------------------------------------------------------------

TEST_F(QA721StatementTimeoutTest, StatementTimeoutCancelsSlowQuery) {
    make_rows_table(600);

    // Mirror the protocol path: SET goes through the session, then the
    // session's timeout is armed around the engine call.
    Session session(1);
    auto set_result = session.try_handle_command("SET statement_timeout = 10");
    ASSERT_TRUE(set_result.has_value());
    ASSERT_TRUE(set_result->has_value()) << set_result->error().message;
    ASSERT_EQ(session.statement_timeout_ms(), 10);

    // 600 x 600 = 360k-row cross join: bounded, but far slower than 10ms.
    StatementDeadlineGuard guard(session.statement_timeout_ms());
    auto result = engine_->execute("SELECT * FROM nums a CROSS JOIN nums b");

    ASSERT_FALSE(result.has_value()) << "slow query was not cancelled — "
                                        "statement_timeout is still inert";
    EXPECT_EQ(result.error().code, StatusCode::QUERY_CANCELED);
    EXPECT_NE(result.error().message.find("statement timeout"), std::string::npos)
        << result.error().message;
}

TEST_F(QA721StatementTimeoutTest, ExpiredDeadlineCancelsDeterministically) {
    make_rows_table(5);

    // Deterministic pin: a deadline already in the past cancels any query
    // that yields at least one root-level tuple pull.
    StatementDeadline::arm(StatementDeadline::Clock::now() - std::chrono::milliseconds(1));
    auto result = engine_->execute("SELECT * FROM nums");
    StatementDeadline::clear();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::QUERY_CANCELED);
}

TEST_F(QA721StatementTimeoutTest, ZeroTimeoutMeansDisabled) {
    make_rows_table(50);

    Session session(1);
    EXPECT_EQ(session.statement_timeout_ms(), 0); // PostgreSQL default.

    StatementDeadlineGuard guard(session.statement_timeout_ms());
    auto result = engine_->execute("SELECT * FROM nums a CROSS JOIN nums b");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 2500u);
}

TEST_F(QA721StatementTimeoutTest, TimeoutDoesNotLeakIntoNextStatement) {
    make_rows_table(600);

    Session session(1);
    ASSERT_TRUE(session.set_variable("statement_timeout", "10").has_value());

    {
        StatementDeadlineGuard guard(session.statement_timeout_ms());
        auto cancelled = engine_->execute("SELECT * FROM nums a CROSS JOIN nums b");
        ASSERT_FALSE(cancelled.has_value());
    }

    // Guard destroyed: the next statement (e.g. after RESET, or simply a
    // fast one issued without an armed deadline) is unaffected.
    auto result = engine_->execute("SELECT * FROM nums");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 600u);
}

// -----------------------------------------------------------------------------
// SET / SHOW round-trips and value validation for the honored variable.
// -----------------------------------------------------------------------------

TEST(QA721SessionVariables, SetShowRoundTripStatementTimeout) {
    Session s(1);
    auto set = s.try_handle_command("SET statement_timeout = 5000");
    ASSERT_TRUE(set.has_value());
    ASSERT_TRUE(set->has_value()) << set->error().message;

    auto show = s.try_handle_command("SHOW statement_timeout");
    ASSERT_TRUE(show.has_value());
    ASSERT_TRUE(show->has_value());
    ASSERT_EQ((*show)->rows.size(), 1u);
    EXPECT_EQ((*show)->rows[0][0].as_string(), "5000");
    EXPECT_EQ(s.statement_timeout_ms(), 5000);
}

TEST(QA721SessionVariables, TimeoutUnitsAreParsed) {
    Session s(1);
    ASSERT_TRUE(s.set_variable("statement_timeout", "250ms").has_value());
    EXPECT_EQ(s.statement_timeout_ms(), 250);
    ASSERT_TRUE(s.set_variable("statement_timeout", "5s").has_value());
    EXPECT_EQ(s.statement_timeout_ms(), 5000);
    ASSERT_TRUE(s.set_variable("statement_timeout", "2min").has_value());
    EXPECT_EQ(s.statement_timeout_ms(), 120000);
}

TEST(QA721SessionVariables, InvalidTimeoutValueRejectedAtSetTime) {
    Session s(1);
    EXPECT_FALSE(s.set_variable("statement_timeout", "abc").has_value());
    EXPECT_FALSE(s.set_variable("statement_timeout", "-5").has_value());
    EXPECT_FALSE(s.set_variable("statement_timeout", "10 parsecs").has_value());
    EXPECT_FALSE(s.set_variable("statement_timeout", "").has_value());
    // Value untouched after rejected SETs.
    EXPECT_EQ(s.statement_timeout_ms(), 0);

    auto cmd = s.try_handle_command("SET statement_timeout = 'abc'");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_FALSE(cmd->has_value());
}

TEST(QA721SessionVariables, ResetRestoresDisabledTimeout) {
    Session s(1);
    ASSERT_TRUE(s.set_variable("statement_timeout", "5000").has_value());
    ASSERT_TRUE(s.reset_variable("statement_timeout").has_value());
    EXPECT_EQ(s.statement_timeout_ms(), 0);
}

// -----------------------------------------------------------------------------
// Enforcement matrix pin: every other variable is accepted-but-inert for
// PostgreSQL driver compatibility (psqlODBC/psql/JDBC issue these at connect
// time; erroring would break clients). Documented at Session::DEFAULT_VARIABLES.
// -----------------------------------------------------------------------------

TEST(QA721SessionVariables, InertVariablesStillAcceptedAndEchoed) {
    Session s(1);
    // Note: default_transaction_isolation is NOT in this list. It started as
    // inert here but was promoted to HONORED + validated-at-SET-time by GDB-978
    // (engine consumes it via QueryEngine::set_session_isolation), so setting it
    // to a bogus value like "qa721" is now correctly rejected. Its honored
    // behavior is covered by test_qa_gdb_978.cpp / test_isolation_levels.cpp.
    const char* inert_vars[] = {"work_mem",
                                "search_path",
                                "embedding_provider_url",
                                "embedding_api_key",
                                "datestyle",
                                "client_encoding",
                                "extra_float_digits",
                                "application_name",
                                "timezone",
                                "standard_conforming_strings",
                                "bytea_output"};
    for (const char* name : inert_vars) {
        auto set = s.try_handle_command(std::string("SET ") + name + " = 'qa721'");
        ASSERT_TRUE(set.has_value()) << name;
        ASSERT_TRUE(set->has_value()) << name << ": " << set->error().message;
        auto val = s.get_variable(name);
        ASSERT_TRUE(val.has_value()) << name;
        EXPECT_EQ(*val, "qa721") << name;
    }
}

} // namespace
