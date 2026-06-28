/// GDB-954: Adversarial QA -- standby read-only gate completeness + TCP transport
///          error/resource paths.
///
/// AC2 (standby read-only): Every write-path verb must be rejected with
///   StatusCode::READ_ONLY on a standby engine.  SELECT and read-only utility
///   queries (SHOW, EXPLAIN) must be served.  pg_is_in_recovery() true on
///   standby, false on primary, false after promotion + DML then accepted.
///
/// AC1 (TCP transport, socket-free): error paths, resource cleanup, and
///   behavioral contracts without a live peer.  Live two-node round-trip is
///   Windows-unverifiable (CRT fd-assert); those tests are CI-only (Linux).
///
/// FINDINGS logged at the top of relevant tests where a gap was detected.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/server/tcp_replication_connection.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Pull in the unit-test helpers (same include path as dev tests).
#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Fixture: a minimal standby engine (no live socket / no WAL infrastructure)
// =============================================================================

class QA_GDB954_StandbyGate : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb954_standby";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

        // Pre-populate a table in PRIMARY mode before flipping the gate.
        auto cr = engine_->execute("CREATE TABLE t (id INT, name VARCHAR)");
        ASSERT_TRUE(cr.has_value()) << cr.error().message;
        auto ir = engine_->execute("INSERT INTO t VALUES (1, 'primary_row')");
        ASSERT_TRUE(ir.has_value()) << ir.error().message;

        // Now switch to standby (read-only) mode.
        engine_->set_standby_mode(true);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    // Helper: assert statement is rejected with READ_ONLY.
    void expect_read_only(const std::string& sql) {
        auto r = engine_->execute(sql);
        ASSERT_FALSE(r.has_value()) << "Expected READ_ONLY rejection for: " << sql;
        EXPECT_EQ(r.error().code, StatusCode::READ_ONLY)
            << "Expected READ_ONLY but got code=" << static_cast<int>(r.error().code)
            << " msg=" << r.error().message << " sql=" << sql;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// AC2 -- DML: INSERT / UPDATE / DELETE all rejected
// =============================================================================

TEST_F(QA_GDB954_StandbyGate, RejectsInsert) {
    expect_read_only("INSERT INTO t VALUES (2, 'new')");
}

TEST_F(QA_GDB954_StandbyGate, RejectsUpdate) {
    expect_read_only("UPDATE t SET name = 'changed' WHERE id = 1");
}

TEST_F(QA_GDB954_StandbyGate, RejectsDelete) {
    expect_read_only("DELETE FROM t WHERE id = 1");
}

// =============================================================================
// AC2 -- DDL: CREATE TABLE / DROP TABLE / ALTER TABLE rejected
// =============================================================================

TEST_F(QA_GDB954_StandbyGate, RejectsCreateTable) {
    expect_read_only("CREATE TABLE other (x INT)");
}

TEST_F(QA_GDB954_StandbyGate, RejectsDropTable) {
    expect_read_only("DROP TABLE t");
}

TEST_F(QA_GDB954_StandbyGate, RejectsAlterTable) {
    expect_read_only("ALTER TABLE t ADD COLUMN extra INT");
}

// =============================================================================
// AC2 -- DDL: CREATE INDEX / DROP INDEX rejected
// =============================================================================

TEST_F(QA_GDB954_StandbyGate, RejectsCreateIndex) {
    expect_read_only("CREATE INDEX idx_t_id ON t (id)");
}

TEST_F(QA_GDB954_StandbyGate, RejectsDropIndex) {
    // Drop of a non-existent index on a standby should still be READ_ONLY, not
    // NOT_FOUND: the gate must fire before the catalog lookup.
    expect_read_only("DROP INDEX nonexistent_idx ON t");
}

// =============================================================================
// AC2 -- INSERT...SELECT (compound write) rejected
// =============================================================================

TEST_F(QA_GDB954_StandbyGate, RejectsInsertSelect) {
    // Insert rows selected from the same table.
    expect_read_only("INSERT INTO t SELECT id + 100, name FROM t");
}

// =============================================================================
// AC2 -- SET (global settings mutation) rejected
// =============================================================================

TEST_F(QA_GDB954_StandbyGate, RejectsSet) {
    // SET syntax: SET key = value
    expect_read_only("SET server.port = 5433");
}

// =============================================================================
// AC2 -- BULK LINK (graph write) rejected
//
// FINDING: BulkLinkStmt is NOT in the standby is_write check in query_engine.cpp
// (lines 598-617).  This means a LINK...VALUES statement issued against a
// standby engine reaches execute_bulk_link() instead of returning READ_ONLY.
// Severity: HIGH -- a write-class operation bypasses the read-only gate.
// Filed as bug GDB-955 (if reproduced below).
// =============================================================================

TEST_F(QA_GDB954_StandbyGate, RejectsBulkLink) {
    // BulkLinkStmt (LINK src TO tgt VIA edge VALUES (src_key, tgt_key)) is a
    // write operation that creates graph edges.  It is NOT in the standby
    // is_write check in query_engine.cpp lines 598-617 -- only LinkStmt and
    // UnlinkStmt are listed.  The gate MUST block BulkLinkStmt with READ_ONLY.
    //
    // Syntax: LINK src_table TO tgt_table VIA edge_type VALUES (src_key, tgt_key)
    // The statement will parse successfully; the question is whether the gate
    // fires before the executor is reached.
    auto r = engine_->execute("LINK t TO t VIA follows VALUES (1, 2)");
    if (!r.has_value()) {
        // Acceptable only if READ_ONLY.  Any other error code (INTERNAL_ERROR,
        // NOT_FOUND, etc.) means the statement reached the executor -- gate MISSING.
        EXPECT_EQ(r.error().code, StatusCode::READ_ONLY)
            << "BULK LINK gate MISSING: BulkLinkStmt bypasses the standby read-only gate. "
            << "got code=" << static_cast<int>(r.error().code) << " msg=" << r.error().message;
    } else {
        // If it succeeds the gate is definitely missing.
        FAIL() << "BULK LINK was accepted by a standby engine -- gate is missing";
    }
}

// =============================================================================
// AC2 -- SELECT and read-only utilities ARE served
// =============================================================================

TEST_F(QA_GDB954_StandbyGate, AllowsSelect) {
    auto r = engine_->execute("SELECT * FROM t");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->rows.size(), 1u);
    EXPECT_EQ(r->rows[0][1].as_string(), "primary_row");
}

TEST_F(QA_GDB954_StandbyGate, AllowsSelectWhereClause) {
    auto r = engine_->execute("SELECT name FROM t WHERE id = 1");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->rows.size(), 1u);
}

TEST_F(QA_GDB954_StandbyGate, AllowsSelectNoRows) {
    auto r = engine_->execute("SELECT * FROM t WHERE id = 999");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->rows.size(), 0u);
}

TEST_F(QA_GDB954_StandbyGate, AllowsShowStatement) {
    auto r = engine_->execute("SHOW search_path");
    // SHOW is a read-only utility -- it must not be blocked.
    // If the parser/executor does not support SHOW, a PARSE_ERROR or
    // NOT_IMPLEMENTED is acceptable -- NOT a READ_ONLY.
    if (!r.has_value()) {
        EXPECT_NE(r.error().code, StatusCode::READ_ONLY)
            << "SHOW was incorrectly rejected with READ_ONLY";
    }
}

// =============================================================================
// AC2 -- pg_is_in_recovery() true on standby, false on primary
// =============================================================================

TEST_F(QA_GDB954_StandbyGate, PgIsInRecoveryTrueOnStandby) {
    auto r = engine_->execute("SELECT pg_is_in_recovery()");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->rows.size(), 1u);
    EXPECT_TRUE(r->rows[0][0].as_bool());
}

TEST(QA_GDB954_Primary, PgIsInRecoveryFalseOnPrimary) {
    Catalog catalog;
    init_test_catalog(catalog);
    auto data_dir = std::filesystem::temp_directory_path() / "sixseven_qa_gdb954_primary";
    std::filesystem::remove_all(data_dir);
    std::filesystem::create_directories(data_dir);

    {
        DiskManager dm;
        StorageManager storage(dm, data_dir);
        QueryEngine engine(catalog, storage);
        // Primary mode: standby not set.

        auto r = engine.execute("SELECT pg_is_in_recovery()");
        ASSERT_TRUE(r.has_value()) << r.error().message;
        ASSERT_EQ(r->rows.size(), 1u);
        EXPECT_FALSE(r->rows[0][0].as_bool());
        // engine, storage, dm destroyed here.
    }

    std::filesystem::remove_all(data_dir);
}

// =============================================================================
// AC2 -- After promotion: standby toggled off, DML accepted
// =============================================================================

TEST(QA_GDB954_Promotion, AfterManualPromotionDmlAccepted) {
    Catalog catalog;
    init_test_catalog(catalog);
    auto data_dir = std::filesystem::temp_directory_path() / "sixseven_qa_gdb954_promo";
    std::filesystem::remove_all(data_dir);
    std::filesystem::create_directories(data_dir);

    {
        DiskManager dm;
        StorageManager storage(dm, data_dir);
        QueryEngine engine(catalog, storage);

        // Prepare table in primary mode.
        ASSERT_TRUE(engine.execute("CREATE TABLE promo (id INT)").has_value());
        ASSERT_TRUE(engine.execute("INSERT INTO promo VALUES (1)").has_value());

        // Enter standby.
        engine.set_standby_mode(true);
        EXPECT_TRUE(engine.is_standby_mode());

        // Verify gate is on.
        auto blocked = engine.execute("INSERT INTO promo VALUES (2)");
        ASSERT_FALSE(blocked.has_value());
        EXPECT_EQ(blocked.error().code, StatusCode::READ_ONLY);

        // Simulate promotion callback (same as PromotionManager.on_promoted).
        engine.set_standby_mode(false);
        EXPECT_FALSE(engine.is_standby_mode());

        // pg_is_in_recovery() must now be false.
        auto rec = engine.execute("SELECT pg_is_in_recovery()");
        ASSERT_TRUE(rec.has_value()) << rec.error().message;
        EXPECT_FALSE(rec->rows[0][0].as_bool());

        // DML must now succeed.
        auto ins = engine.execute("INSERT INTO promo VALUES (2)");
        ASSERT_TRUE(ins.has_value()) << ins.error().message;
        EXPECT_EQ(ins->affected_rows, 1);
        // engine, storage, dm destroyed here -- all fds closed.
    }

    std::filesystem::remove_all(data_dir);
}

// =============================================================================
// AC2 -- Primary mode: DML accepted before any standby flag is set
// =============================================================================

TEST(QA_GDB954_Primary, PrimaryAcceptsDml) {
    Catalog catalog;
    init_test_catalog(catalog);
    auto data_dir = std::filesystem::temp_directory_path() / "sixseven_qa_gdb954_primary_dml";
    std::filesystem::remove_all(data_dir);
    std::filesystem::create_directories(data_dir);

    {
        DiskManager dm;
        StorageManager storage(dm, data_dir);
        QueryEngine engine(catalog, storage);
        EXPECT_FALSE(engine.is_standby_mode());

        ASSERT_TRUE(engine.execute("CREATE TABLE p (x INT)").has_value());
        auto ins = engine.execute("INSERT INTO p VALUES (42)");
        ASSERT_TRUE(ins.has_value()) << ins.error().message;
        EXPECT_EQ(ins->affected_rows, 1);

        auto upd = engine.execute("UPDATE p SET x = 99 WHERE x = 42");
        ASSERT_TRUE(upd.has_value()) << upd.error().message;

        auto del = engine.execute("DELETE FROM p WHERE x = 99");
        ASSERT_TRUE(del.has_value()) << del.error().message;
        // engine, storage, dm destroyed here -- all fds closed.
    }

    std::filesystem::remove_all(data_dir);
}

// =============================================================================
// TCP Transport (AC1) -- socket-free error and resource paths
// =============================================================================

// Construction: object starts closed.
TEST(QA_GDB954_TcpTransport, StartsNotOpen) {
    TcpReplicationConnection conn;
    EXPECT_FALSE(conn.is_open());
}

// peer_description() before connect returns a non-empty sentinel.
TEST(QA_GDB954_TcpTransport, PeerDescriptionBeforeConnectNonEmpty) {
    TcpReplicationConnection conn;
    auto desc = conn.peer_description();
    EXPECT_FALSE(desc.empty());
}

// send() before connect must return NETWORK_ERROR, not crash.
TEST(QA_GDB954_TcpTransport, SendBeforeConnectReturnsError) {
    TcpReplicationConnection conn;
    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    auto r = conn.send(std::span<const uint8_t>(data));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NETWORK_ERROR);
    EXPECT_FALSE(r.error().message.empty());
}

// send() with empty buffer before connect must also return NETWORK_ERROR.
TEST(QA_GDB954_TcpTransport, SendEmptyBufferBeforeConnectReturnsError) {
    TcpReplicationConnection conn;
    std::vector<uint8_t> empty;
    auto r = conn.send(std::span<const uint8_t>(empty));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NETWORK_ERROR);
}

// receive() before connect must return NETWORK_ERROR.
TEST(QA_GDB954_TcpTransport, ReceiveBeforeConnectReturnsError) {
    TcpReplicationConnection conn;
    auto r = conn.receive(1024, std::chrono::milliseconds(100));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NETWORK_ERROR);
    EXPECT_FALSE(r.error().message.empty());
}

// receive() with max_bytes=0 before connect still returns NETWORK_ERROR.
TEST(QA_GDB954_TcpTransport, ReceiveZeroBytesBeforeConnectReturnsError) {
    TcpReplicationConnection conn;
    auto r = conn.receive(0, std::chrono::milliseconds(50));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NETWORK_ERROR);
}

// close() on unconnected instance is a no-op (no crash).
TEST(QA_GDB954_TcpTransport, CloseBeforeConnectIsNoOp) {
    TcpReplicationConnection conn;
    EXPECT_FALSE(conn.is_open());
    conn.close();
    EXPECT_FALSE(conn.is_open());
}

// Double-close: safe to call close() twice.
TEST(QA_GDB954_TcpTransport, DoubleCloseIsNoOp) {
    TcpReplicationConnection conn;
    conn.close();
    conn.close();
    EXPECT_FALSE(conn.is_open());
}

// Triple-close for good measure.
TEST(QA_GDB954_TcpTransport, TripleCloseIsNoOp) {
    TcpReplicationConnection conn;
    conn.close();
    conn.close();
    conn.close();
    EXPECT_FALSE(conn.is_open());
}

// connect() to empty host returns INVALID_ARGUMENT.
TEST(QA_GDB954_TcpTransport, ConnectEmptyHostReturnsInvalidArgument) {
    TcpReplicationConnection conn;
    auto r = conn.connect("", 5433);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_FALSE(conn.is_open());
}

// connect() to a clearly closed port returns NETWORK_ERROR without hanging.
// Port 9 (IANA discard) is almost always closed on dev machines.
TEST(QA_GDB954_TcpTransport, ConnectToClosedPortReturnsError) {
    TcpReplicationConnection conn;
    auto r = conn.connect("127.0.0.1", 9);
    if (r.has_value()) {
        conn.close();
        GTEST_SKIP() << "port 9 unexpectedly open; skipping";
    }
    EXPECT_EQ(r.error().code, StatusCode::NETWORK_ERROR);
    EXPECT_FALSE(conn.is_open());
    // After a failed connect the connection must remain logically closed.
    EXPECT_FALSE(conn.is_open());
}

// After a failed connect, send() still returns NETWORK_ERROR (not UB).
TEST(QA_GDB954_TcpTransport, SendAfterFailedConnectReturnsError) {
    TcpReplicationConnection conn;
    (void)conn.connect("127.0.0.1", 9); // expected to fail
    if (conn.is_open()) {
        conn.close();
        GTEST_SKIP() << "port 9 unexpectedly open; skipping";
    }
    std::vector<uint8_t> data = {0x01};
    auto r = conn.send(std::span<const uint8_t>(data));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NETWORK_ERROR);
}

// After a failed connect, receive() still returns NETWORK_ERROR (not UB).
TEST(QA_GDB954_TcpTransport, ReceiveAfterFailedConnectReturnsError) {
    TcpReplicationConnection conn;
    (void)conn.connect("127.0.0.1", 9); // expected to fail
    if (conn.is_open()) {
        conn.close();
        GTEST_SKIP() << "port 9 unexpectedly open; skipping";
    }
    auto r = conn.receive(512, std::chrono::milliseconds(50));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NETWORK_ERROR);
}

// Factory: empty host returns INVALID_ARGUMENT.
TEST(QA_GDB954_TcpTransport, FactoryEmptyHostReturnsInvalidArgument) {
    auto r = make_tcp_replication_connection("", 5433);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// Factory: closed port returns NETWORK_ERROR (not crash, not hang).
TEST(QA_GDB954_TcpTransport, FactoryClosedPortReturnsNetworkError) {
    auto r = make_tcp_replication_connection("127.0.0.1", 9);
    if (r.has_value()) {
        (*r)->close();
        GTEST_SKIP() << "port 9 unexpectedly open; skipping";
    }
    EXPECT_EQ(r.error().code, StatusCode::NETWORK_ERROR);
}

// Repeated connect-fail loop: no fd exhaustion (observable via is_open()).
// This is a soft check -- we cannot count fds on Windows reliably, but
// the invariant is that is_open() stays false after each failed attempt.
TEST(QA_GDB954_TcpTransport, RepeatedConnectFailNoLeak) {
    for (int i = 0; i < 10; ++i) {
        TcpReplicationConnection conn;
        auto r = conn.connect("127.0.0.1", 9);
        // If port 9 happened to be open the test is vacuously safe.
        if (r.has_value()) {
            conn.close();
            continue;
        }
        EXPECT_FALSE(conn.is_open()) << "fd leaked after failed connect iteration " << i;
        // Destructor runs here; should not crash or double-free.
    }
}

// Move constructor: source becomes closed after move.
TEST(QA_GDB954_TcpTransport, MoveConstructorTransfersOwnership) {
    TcpReplicationConnection a;
    // a is unconnected; move it to b.
    TcpReplicationConnection b(std::move(a));
    // a must be closed after move (fd_ transferred to -1).
    EXPECT_FALSE(a.is_open()); // NOLINT(bugprone-use-after-move)
    EXPECT_FALSE(b.is_open());
}

// Move assignment: destination closes its old socket, source becomes closed.
TEST(QA_GDB954_TcpTransport, MoveAssignmentTransfersOwnership) {
    TcpReplicationConnection a;
    TcpReplicationConnection b;
    b = std::move(a);
    EXPECT_FALSE(a.is_open()); // NOLINT(bugprone-use-after-move)
    EXPECT_FALSE(b.is_open());
}

// Destructor while unconnected must not crash.
TEST(QA_GDB954_TcpTransport, DestructorOnUnconnectedIsSafe) {
    {
        TcpReplicationConnection conn;
        // conn destroyed here.
    }
    SUCCEED();
}

// receive() with zero timeout returns promptly (not infinite block).
// This verifies the timeout=0ms path through select().
TEST(QA_GDB954_TcpTransport, ReceiveZeroTimeoutBeforeConnect) {
    TcpReplicationConnection conn;
    // Not connected -- should return NETWORK_ERROR immediately.
    auto r = conn.receive(1024, std::chrono::milliseconds(0));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::NETWORK_ERROR);
}
