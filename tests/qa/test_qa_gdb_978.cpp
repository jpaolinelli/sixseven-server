/// QA regression tests for GDB-978: Honor READ COMMITTED vs SNAPSHOT ISOLATION
/// at the QueryEngine.
///
/// Focus areas:
///   1. parse_isolation_level edge cases (whitespace, tabs, extra spaces).
///   2. set_isolation_change_callback fires and normalizes value correctly.
///   3. Mutation-detecting differential: verify that SI does NOT advance its
///      snapshot between statements within the SAME explicit transaction, while
///      RC DOES advance its snapshot on each statement.
///      This test directly exercises get_statement_snapshot() via the public
///      transaction_manager() accessor and verifies the snapshot xmax is frozen
///      under SI but advances under RC.
///   4. SET default_transaction_isolation validates known values and rejects
///      unknown values.
///   5. parse_isolation_level empty-string and whitespace-only inputs.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/server/session.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/txn/txn_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Helpers
// =============================================================================

namespace {

class GDB978Fixture : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb978";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        auto r = engine_->execute("CREATE TABLE t (id INT, v INT)");
        ASSERT_TRUE(r.has_value()) << r.error().message;
        r = engine_->execute("INSERT INTO t VALUES (1, 10)");
        ASSERT_TRUE(r.has_value()) << r.error().message;
        r = engine_->execute("INSERT INTO t VALUES (2, 20)");
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto r = engine_->execute(sql);
        EXPECT_TRUE(r.has_value())
            << "failed: " << sql << " :: " << (r ? std::string{} : r.error().message);
        return r ? std::move(*r) : QueryResult{};
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

} // namespace

// =============================================================================
// QA_GDB978: parse_isolation_level edge cases
// =============================================================================

// Leading whitespace should be stripped and parsed correctly.
TEST(QA_GDB978_Parse, LeadingWhitespaceReadCommitted) {
    auto r = QueryEngine::parse_isolation_level("  read committed");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, IsolationLevel::READ_COMMITTED);
}

// Trailing whitespace should be stripped.
TEST(QA_GDB978_Parse, TrailingWhitespaceReadCommitted) {
    auto r = QueryEngine::parse_isolation_level("read committed   ");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, IsolationLevel::READ_COMMITTED);
}

// Tab-separated words should normalize to a single space.
TEST(QA_GDB978_Parse, TabSeparatedSnapshotIsolation) {
    auto r = QueryEngine::parse_isolation_level("snapshot\tisolation");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, IsolationLevel::SNAPSHOT_ISOLATION);
}

// Multiple interior spaces should collapse to one.
TEST(QA_GDB978_Parse, MultipleSpacesRepeatableRead) {
    auto r = QueryEngine::parse_isolation_level("repeatable   read");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, IsolationLevel::SNAPSHOT_ISOLATION);
}

// Whitespace-only input should fail (not crash or return OK).
TEST(QA_GDB978_Parse, WhitespaceOnlyInput) {
    auto r = QueryEngine::parse_isolation_level("   ");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// Empty string must fail with INVALID_ARGUMENT.
TEST(QA_GDB978_Parse, EmptyStringIsInvalidArgument) {
    auto r = QueryEngine::parse_isolation_level("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// Partially matching prefix should not succeed.
TEST(QA_GDB978_Parse, PartialPrefixRejected) {
    auto r = QueryEngine::parse_isolation_level("read");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// "read uncommitted" is not a recognized level for this engine.
TEST(QA_GDB978_Parse, ReadUncommittedRejected) {
    auto r = QueryEngine::parse_isolation_level("read uncommitted");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// QA_GDB978: Session SET default_transaction_isolation validation
// =============================================================================

TEST(QA_GDB978_Session, SetValidIsolationLevelSucceeds) {
    Session sess(42);
    EXPECT_TRUE(sess.set_variable("default_transaction_isolation", "read committed").has_value());
    EXPECT_TRUE(
        sess.set_variable("default_transaction_isolation", "snapshot isolation").has_value());
    EXPECT_TRUE(sess.set_variable("default_transaction_isolation", "repeatable read").has_value());
    EXPECT_TRUE(sess.set_variable("default_transaction_isolation", "serializable").has_value());
}

TEST(QA_GDB978_Session, SetInvalidIsolationLevelFails) {
    Session sess(42);
    auto r = sess.set_variable("default_transaction_isolation", "garbage");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB978_Session, SetEmptyIsolationLevelFails) {
    Session sess(42);
    auto r = sess.set_variable("default_transaction_isolation", "");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// QA_GDB978: set_isolation_change_callback fires with normalized value
// =============================================================================

TEST(QA_GDB978_Callback, CallbackFiredWithNormalizedValue) {
    Session sess(99);
    std::string captured;
    sess.set_isolation_change_callback([&](const std::string& v) { captured = v; });

    auto r = sess.set_variable("default_transaction_isolation", "SNAPSHOT ISOLATION");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    // Callback should receive lowercase normalized value.
    EXPECT_EQ(captured, "snapshot isolation");
}

TEST(QA_GDB978_Callback, CallbackNotFiredForOtherVariables) {
    Session sess(99);
    bool fired = false;
    sess.set_isolation_change_callback([&](const std::string&) { fired = true; });

    auto r = sess.set_variable("work_mem", "8MB");
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(fired) << "Callback should only fire for default_transaction_isolation";
}

TEST(QA_GDB978_Callback, CallbackClearedWithNullptr) {
    Session sess(99);
    bool fired = false;
    sess.set_isolation_change_callback([&](const std::string&) { fired = true; });
    sess.set_isolation_change_callback(nullptr); // clear
    auto r = sess.set_variable("default_transaction_isolation", "serializable");
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(fired) << "Cleared callback must not fire";
}

// =============================================================================
// QA_GDB978: Mutation-detecting differential via get_statement_snapshot()
//
// This is the critical test the existing unit tests omit. It directly verifies
// that get_statement_snapshot() returns a FROZEN snapshot under SI (same xmax
// both calls) and an ADVANCING snapshot under RC (xmax increases after a
// competing autocommit transaction begins).
//
// The test uses transaction_manager() (the public accessor on QueryEngine) to
// drive a second concurrent transaction within the same TransactionManager,
// then compares the snapshot xmax returned for the open explicit transaction
// between the two isolation levels.
// =============================================================================

TEST_F(GDB978Fixture, SnapshotIsolationSnapshotIsFrozenBetweenStatements) {
    // Open a SI explicit transaction; snapshot frozen at BEGIN.
    engine_->set_session_isolation(IsolationLevel::SNAPSHOT_ISOLATION);
    exec_ok("BEGIN");

    txn_id_t si_txn_id = engine_->active_transaction_id();
    ASSERT_NE(si_txn_id, invalid_txn_id);

    // Capture the snapshot xmax immediately after BEGIN.
    auto& tm = engine_->transaction_manager();
    txn_id_t xmax_before = tm.get_statement_snapshot(si_txn_id).xmax;

    // Simulate a competing transaction: begin + commit via TransactionManager
    // directly. This advances next_txn_id_ inside the shared TxnManager so
    // that a fresh RC snapshot taken after this point would have a larger xmax.
    auto t2 = tm.begin(IsolationLevel::READ_COMMITTED);
    ASSERT_TRUE(t2.has_value());
    txn_id_t t2_id = (*t2)->txn_id;
    ASSERT_TRUE(tm.commit(t2_id).has_value());

    // Under SI, get_statement_snapshot must still return the frozen BEGIN
    // snapshot; xmax must NOT have advanced.
    txn_id_t xmax_after = tm.get_statement_snapshot(si_txn_id).xmax;

    EXPECT_EQ(xmax_before, xmax_after)
        << "SI: snapshot xmax must be frozen at BEGIN; it advanced after a competing commit";

    exec_ok("COMMIT");
}

TEST_F(GDB978Fixture, ReadCommittedSnapshotAdvancesBetweenStatements) {
    // Open an RC explicit transaction.
    engine_->set_session_isolation(IsolationLevel::READ_COMMITTED);
    exec_ok("BEGIN");

    txn_id_t rc_txn_id = engine_->active_transaction_id();
    ASSERT_NE(rc_txn_id, invalid_txn_id);

    auto& tm = engine_->transaction_manager();
    txn_id_t xmax_before = tm.get_statement_snapshot(rc_txn_id).xmax;

    // Competing transaction commits; this advances the committed horizon.
    auto t2 = tm.begin(IsolationLevel::READ_COMMITTED);
    ASSERT_TRUE(t2.has_value());
    txn_id_t t2_id = (*t2)->txn_id;
    ASSERT_TRUE(tm.commit(t2_id).has_value());

    // Under RC, get_statement_snapshot refreshes the snapshot; xmax must
    // have advanced to include the newly committed transaction.
    txn_id_t xmax_after = tm.get_statement_snapshot(rc_txn_id).xmax;

    EXPECT_GT(xmax_after, xmax_before)
        << "RC: snapshot xmax must advance after a competing commit (fresh per-statement snapshot)";

    exec_ok("COMMIT");
}

// =============================================================================
// QA_GDB978: Default isolation level is READ COMMITTED
// =============================================================================

TEST_F(GDB978Fixture, DefaultSessionIsolationIsReadCommitted) {
    // A freshly constructed engine must default to RC.
    DiskManager dm2;
    Catalog cat2;
    init_test_catalog(cat2);
    auto tmp = std::filesystem::temp_directory_path() / "sixseven_qa_gdb978_default";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    StorageManager sm2(dm2, tmp);
    QueryEngine eng2(cat2, sm2);

    EXPECT_EQ(eng2.session_isolation(), IsolationLevel::READ_COMMITTED);
    std::filesystem::remove_all(tmp);
}

// =============================================================================
// QA_GDB978: SET inside SI txn does not change the frozen snapshot
// =============================================================================

TEST_F(GDB978Fixture, ChangingIsolationMidTransactionDoesNotAffectFrozenSnapshot) {
    // Open SI transaction.
    engine_->set_session_isolation(IsolationLevel::SNAPSHOT_ISOLATION);
    exec_ok("BEGIN");

    txn_id_t si_txn_id = engine_->active_transaction_id();
    auto& tm = engine_->transaction_manager();
    txn_id_t xmax_before = tm.get_statement_snapshot(si_txn_id).xmax;

    // Changing the session isolation level mid-transaction should NOT affect
    // the already-frozen snapshot for the current txn.
    engine_->set_session_isolation(IsolationLevel::READ_COMMITTED);

    txn_id_t xmax_after = tm.get_statement_snapshot(si_txn_id).xmax;
    EXPECT_EQ(xmax_before, xmax_after)
        << "Changing session_isolation_ mid-txn must not unfreeze an SI snapshot";

    exec_ok("COMMIT");
}
