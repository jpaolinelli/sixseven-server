#include "giodb/txn/mvcc.h"
#include "giodb/txn/mvcc_tuple.h"
#include "giodb/txn/transaction.h"
#include "giodb/txn/txn_manager.h"

#include <gtest/gtest.h>

using namespace giodb;

// =============================================================================
// MvccTupleHeader serialization tests
// =============================================================================

TEST(MvccTuple, HeaderSerializeRoundTrip) {
    MvccTupleHeader header;
    header.xmin = 42;
    header.xmax = 99;
    header.t_ctid = {10, 5};

    uint8_t buf[mvcc_header_size];
    serialize_mvcc_header(header, buf);

    auto decoded = deserialize_mvcc_header(buf);
    EXPECT_EQ(decoded.xmin, 42u);
    EXPECT_EQ(decoded.xmax, 99u);
    EXPECT_EQ(decoded.t_ctid.page_id, 10u);
    EXPECT_EQ(decoded.t_ctid.slot_id, 5u);
}

TEST(MvccTuple, HeaderDefaultValues) {
    MvccTupleHeader header;
    EXPECT_EQ(header.xmin, invalid_txn_id);
    EXPECT_EQ(header.xmax, invalid_txn_id);
    EXPECT_EQ(header.t_ctid, RID::invalid());
    EXPECT_FALSE(header.has_next_version());
}

TEST(MvccTuple, HasNextVersion) {
    MvccTupleHeader header;
    header.t_ctid = {3, 7};
    EXPECT_TRUE(header.has_next_version());
}

TEST(MvccTuple, PrependAndStripHeader) {
    MvccTupleHeader header;
    header.xmin = 10;
    header.xmax = 0;

    std::vector<uint8_t> user_data = {0xAA, 0xBB, 0xCC, 0xDD};
    auto combined = prepend_mvcc_header(header, user_data);

    EXPECT_EQ(combined.size(), mvcc_header_size + user_data.size());

    // Read back header.
    auto read_header = read_mvcc_header(combined);
    EXPECT_EQ(read_header.xmin, 10u);
    EXPECT_EQ(read_header.xmax, 0u);

    // Strip header and verify user data.
    auto stripped = strip_mvcc_header(combined);
    ASSERT_EQ(stripped.size(), user_data.size());
    EXPECT_EQ(stripped[0], 0xAA);
    EXPECT_EQ(stripped[1], 0xBB);
    EXPECT_EQ(stripped[2], 0xCC);
    EXPECT_EQ(stripped[3], 0xDD);
}

TEST(MvccTuple, StripHeaderEmptyData) {
    // Buffer too small for a header returns empty span.
    std::vector<uint8_t> small = {1, 2, 3};
    auto stripped = strip_mvcc_header(small);
    EXPECT_TRUE(stripped.empty());
}

TEST(MvccTuple, WriteXmaxInPlace) {
    MvccTupleHeader header;
    header.xmin = 5;
    header.xmax = 0;

    std::vector<uint8_t> user_data = {0xFF};
    auto combined = prepend_mvcc_header(header, user_data);

    // Write xmax in-place.
    write_mvcc_xmax(combined.data(), 42);

    auto read_header = read_mvcc_header(combined);
    EXPECT_EQ(read_header.xmin, 5u);
    EXPECT_EQ(read_header.xmax, 42u);

    // User data unchanged.
    auto stripped = strip_mvcc_header(combined);
    EXPECT_EQ(stripped[0], 0xFF);
}

TEST(MvccTuple, WriteCtidInPlace) {
    MvccTupleHeader header;
    header.xmin = 1;

    std::vector<uint8_t> user_data = {0xEE};
    auto combined = prepend_mvcc_header(header, user_data);

    RID new_ctid = {100, 8};
    write_mvcc_ctid(combined.data(), new_ctid);

    auto read_header = read_mvcc_header(combined);
    EXPECT_EQ(read_header.t_ctid.page_id, 100u);
    EXPECT_EQ(read_header.t_ctid.slot_id, 8u);
}

// =============================================================================
// TransactionManager tests
// =============================================================================

TEST(TransactionManager, BeginAllocatesSequentialIds) {
    TransactionManager mgr;

    auto* t1 = mgr.begin().value();
    auto* t2 = mgr.begin().value();
    auto* t3 = mgr.begin().value();

    EXPECT_EQ(t1->txn_id, 1u);
    EXPECT_EQ(t2->txn_id, 2u);
    EXPECT_EQ(t3->txn_id, 3u);
    EXPECT_EQ(mgr.next_txn_id(), 4u);
}

TEST(TransactionManager, BeginSetsIsolationLevel) {
    TransactionManager mgr;

    auto* t1 = mgr.begin(IsolationLevel::READ_COMMITTED).value();
    auto* t2 = mgr.begin(IsolationLevel::SNAPSHOT_ISOLATION).value();
    auto* t3 = mgr.begin(IsolationLevel::SERIALIZABLE).value();

    EXPECT_EQ(t1->isolation_level, IsolationLevel::READ_COMMITTED);
    EXPECT_EQ(t2->isolation_level, IsolationLevel::SNAPSHOT_ISOLATION);
    EXPECT_EQ(t3->isolation_level, IsolationLevel::SERIALIZABLE);
}

TEST(TransactionManager, CommitAndAbort) {
    TransactionManager mgr;

    auto* t1 = mgr.begin().value();
    auto* t2 = mgr.begin().value();

    EXPECT_EQ(mgr.get_status(t1->txn_id), TransactionStatus::ACTIVE);
    EXPECT_EQ(mgr.get_status(t2->txn_id), TransactionStatus::ACTIVE);

    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());
    ASSERT_TRUE(mgr.abort(t2->txn_id).has_value());

    EXPECT_EQ(mgr.get_status(t1->txn_id), TransactionStatus::COMMITTED);
    EXPECT_EQ(mgr.get_status(t2->txn_id), TransactionStatus::ABORTED);
}

TEST(TransactionManager, CommitNonActive) {
    TransactionManager mgr;
    auto* t1 = mgr.begin().value();
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    // Double commit fails.
    auto result = mgr.commit(t1->txn_id);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TXN_ABORTED);
}

TEST(TransactionManager, AbortNonActive) {
    TransactionManager mgr;
    auto* t1 = mgr.begin().value();
    ASSERT_TRUE(mgr.abort(t1->txn_id).has_value());

    // Double abort fails.
    auto result = mgr.abort(t1->txn_id);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(TransactionManager, CommitNotFound) {
    TransactionManager mgr;
    auto result = mgr.commit(999);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(TransactionManager, UnknownStatusTreatedAsAborted) {
    TransactionManager mgr;
    EXPECT_EQ(mgr.get_status(999), TransactionStatus::ABORTED);
}

TEST(TransactionManager, GetTransaction) {
    TransactionManager mgr;
    auto* t1 = mgr.begin().value();

    auto* found = mgr.get_transaction(t1->txn_id);
    EXPECT_NE(found, nullptr);
    EXPECT_EQ(found->txn_id, t1->txn_id);

    EXPECT_EQ(mgr.get_transaction(999), nullptr);
}

// =============================================================================
// Snapshot tests
// =============================================================================

TEST(Snapshot, CapturesActiveTransactions) {
    TransactionManager mgr;

    auto* t1 = mgr.begin().value();
    auto* t2 = mgr.begin().value();
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());
    // t2 still active.

    auto* t3 = mgr.begin().value();
    // At this point t1=committed, t2=active, t3=active.
    auto snap = mgr.take_snapshot();

    EXPECT_EQ(snap.xmin, t2->txn_id);        // Smallest active.
    EXPECT_EQ(snap.xmax, mgr.next_txn_id()); // Next to be assigned.

    // t2 and t3 should be in the active list.
    EXPECT_TRUE(snap.is_active(t2->txn_id));
    EXPECT_TRUE(snap.is_active(t3->txn_id));
    // t1 committed, not in active list.
    EXPECT_FALSE(snap.is_active(t1->txn_id));
}

TEST(Snapshot, EmptyActiveList) {
    TransactionManager mgr;

    auto* t1 = mgr.begin().value();
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    auto snap = mgr.take_snapshot();
    EXPECT_TRUE(snap.active_txn_ids.empty());
    EXPECT_EQ(snap.xmin, mgr.next_txn_id());
}

// =============================================================================
// Visibility tests
// =============================================================================

class VisibilityTest : public ::testing::Test {
protected:
    TransactionManager mgr;
};

TEST_F(VisibilityTest, CommittedTupleVisibleToNewSnapshot) {
    // T1 inserts and commits.
    auto* t1 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t1->txn_id;
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    // T2 takes a snapshot — should see t1's tuple.
    auto* t2 = mgr.begin().value();
    EXPECT_TRUE(is_visible(header, t2->snapshot, mgr, t2->txn_id));
}

TEST_F(VisibilityTest, AbortedTupleInvisible) {
    // T1 inserts and aborts.
    auto* t1 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t1->txn_id;
    ASSERT_TRUE(mgr.abort(t1->txn_id).has_value());

    // T2 should NOT see the aborted tuple.
    auto* t2 = mgr.begin().value();
    EXPECT_FALSE(is_visible(header, t2->snapshot, mgr, t2->txn_id));
}

TEST_F(VisibilityTest, InProgressTupleInvisibleToOthers) {
    // T1 starts and inserts (still active).
    auto* t1 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t1->txn_id;

    // T2 takes a snapshot — should NOT see t1's uncommitted tuple.
    auto* t2 = mgr.begin().value();
    EXPECT_FALSE(is_visible(header, t2->snapshot, mgr, t2->txn_id));
}

TEST_F(VisibilityTest, OwnUncommittedTupleVisible) {
    // T1 inserts (still active).
    auto* t1 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t1->txn_id;

    // T1 should see its own uncommitted tuple.
    EXPECT_TRUE(is_visible(header, t1->snapshot, mgr, t1->txn_id));
}

TEST_F(VisibilityTest, OwnDeletedTupleInvisible) {
    // T1 inserts and then deletes (still active).
    auto* t1 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t1->txn_id;
    header.xmax = t1->txn_id; // Self-delete.

    // T1 should NOT see its own deleted tuple.
    EXPECT_FALSE(is_visible(header, t1->snapshot, mgr, t1->txn_id));
}

TEST_F(VisibilityTest, DeletedByCommittedTxnInvisible) {
    // T1 inserts and commits.
    auto* t1 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t1->txn_id;
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    // T2 deletes and commits.
    auto* t2 = mgr.begin().value();
    header.xmax = t2->txn_id;
    ASSERT_TRUE(mgr.commit(t2->txn_id).has_value());

    // T3 should NOT see the deleted tuple.
    auto* t3 = mgr.begin().value();
    EXPECT_FALSE(is_visible(header, t3->snapshot, mgr, t3->txn_id));
}

TEST_F(VisibilityTest, DeletedByAbortedTxnStillVisible) {
    // T1 inserts and commits.
    auto* t1 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t1->txn_id;
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    // T2 tries to delete but aborts.
    auto* t2 = mgr.begin().value();
    header.xmax = t2->txn_id;
    ASSERT_TRUE(mgr.abort(t2->txn_id).has_value());

    // T3 should still see the tuple (deletion was rolled back).
    auto* t3 = mgr.begin().value();
    EXPECT_TRUE(is_visible(header, t3->snapshot, mgr, t3->txn_id));
}

TEST_F(VisibilityTest, DeletedByInProgressTxnStillVisible) {
    // T1 inserts and commits.
    auto* t1 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t1->txn_id;
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    // T2 starts deleting (still active).
    auto* t2 = mgr.begin().value();
    header.xmax = t2->txn_id;

    // T3 should still see the tuple (deletion not committed).
    auto* t3 = mgr.begin().value();
    EXPECT_TRUE(is_visible(header, t3->snapshot, mgr, t3->txn_id));
}

TEST_F(VisibilityTest, TupleCreatedAfterSnapshotInvisible) {
    // T1 takes a snapshot first.
    auto* t1 = mgr.begin().value();

    // T2 inserts and commits after T1's snapshot.
    auto* t2 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t2->txn_id;
    ASSERT_TRUE(mgr.commit(t2->txn_id).has_value());

    // T1 should NOT see t2's tuple (created after snapshot).
    EXPECT_FALSE(is_visible(header, t1->snapshot, mgr, t1->txn_id));
}

TEST_F(VisibilityTest, TupleDeletedAfterSnapshotStillVisible) {
    // T1 inserts and commits.
    auto* t1 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t1->txn_id;
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    // T2 takes a snapshot.
    auto* t2 = mgr.begin().value();

    // T3 deletes and commits after T2's snapshot.
    auto* t3 = mgr.begin().value();
    header.xmax = t3->txn_id;
    ASSERT_TRUE(mgr.commit(t3->txn_id).has_value());

    // T2 should still see the tuple (deleted after its snapshot).
    EXPECT_TRUE(is_visible(header, t2->snapshot, mgr, t2->txn_id));
}

TEST_F(VisibilityTest, ConcurrentInsertInvisible) {
    // T1 and T2 start concurrently.
    auto* t1 = mgr.begin().value();
    auto* t2 = mgr.begin().value();

    // T2 inserts a tuple.
    MvccTupleHeader header;
    header.xmin = t2->txn_id;

    // T1 cannot see T2's tuple (T2 in T1's active list, even if committed later).
    // Note: T2 is in T1's snapshot.active_txn_ids.
    EXPECT_FALSE(is_visible(header, t1->snapshot, mgr, t1->txn_id));

    // Even after T2 commits.
    ASSERT_TRUE(mgr.commit(t2->txn_id).has_value());
    // T1 still uses its original snapshot, so T2's insert is still invisible.
    EXPECT_FALSE(is_visible(header, t1->snapshot, mgr, t1->txn_id));
}

// =============================================================================
// Version chain (UPDATE) tests
// =============================================================================

TEST_F(VisibilityTest, UpdateCreatesVersionChain) {
    // Simulate UPDATE: delete old version, insert new version with t_ctid link.
    auto* t1 = mgr.begin().value();

    // Old version at RID{1, 0}.
    MvccTupleHeader old_header;
    old_header.xmin = t1->txn_id;
    old_header.xmax = t1->txn_id; // Deleted by the update.
    old_header.t_ctid = {1, 1};   // Points to new version.

    // New version at RID{1, 1}.
    MvccTupleHeader new_header;
    new_header.xmin = t1->txn_id;

    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    auto* t2 = mgr.begin().value();

    // Old version should be invisible (xmin == xmax => self-deleted).
    // But for a non-self viewer, xmin committed and xmax committed and both visible.
    EXPECT_FALSE(is_visible(old_header, t2->snapshot, mgr, t2->txn_id));

    // New version should be visible.
    EXPECT_TRUE(is_visible(new_header, t2->snapshot, mgr, t2->txn_id));

    // Version chain pointer is set correctly.
    EXPECT_TRUE(old_header.has_next_version());
    EXPECT_EQ(old_header.t_ctid.page_id, 1u);
    EXPECT_EQ(old_header.t_ctid.slot_id, 1u);
}

// =============================================================================
// Write-write conflict detection tests
// =============================================================================

TEST(WriteConflict, DetectedUnderSnapshotIsolation) {
    TransactionManager mgr;

    // T1 and T2 both start under SI.
    auto* t1 = mgr.begin(IsolationLevel::SNAPSHOT_ISOLATION).value();
    auto* t2 = mgr.begin(IsolationLevel::SNAPSHOT_ISOLATION).value();

    RID shared_rid = {1, 0};
    mgr.record_write(t1->txn_id, shared_rid);
    mgr.record_write(t2->txn_id, shared_rid);

    // T1 commits first — should succeed.
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    // T2 tries to commit — should fail with TXN_CONFLICT.
    auto result = mgr.commit(t2->txn_id);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TXN_CONFLICT);

    // T2 should be aborted.
    EXPECT_EQ(mgr.get_status(t2->txn_id), TransactionStatus::ABORTED);
}

TEST(WriteConflict, NoConflictOnDifferentRIDs) {
    TransactionManager mgr;

    auto* t1 = mgr.begin(IsolationLevel::SNAPSHOT_ISOLATION).value();
    auto* t2 = mgr.begin(IsolationLevel::SNAPSHOT_ISOLATION).value();

    mgr.record_write(t1->txn_id, {1, 0});
    mgr.record_write(t2->txn_id, {1, 1}); // Different slot.

    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());
    ASSERT_TRUE(mgr.commit(t2->txn_id).has_value());
}

TEST(WriteConflict, NoConflictUnderReadCommitted) {
    TransactionManager mgr;

    auto* t1 = mgr.begin(IsolationLevel::READ_COMMITTED).value();
    auto* t2 = mgr.begin(IsolationLevel::READ_COMMITTED).value();

    RID shared_rid = {1, 0};
    mgr.record_write(t1->txn_id, shared_rid);
    mgr.record_write(t2->txn_id, shared_rid);

    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());
    // Under Read Committed, no write-write conflict checking.
    ASSERT_TRUE(mgr.commit(t2->txn_id).has_value());
}

// =============================================================================
// xmin_horizon tests
// =============================================================================

TEST(XminHorizon, NoActiveTransactions) {
    TransactionManager mgr;
    auto* t1 = mgr.begin().value();
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    // No active transactions — horizon equals next_txn_id.
    EXPECT_EQ(mgr.xmin_horizon(), mgr.next_txn_id());
}

TEST(XminHorizon, ReflectsOldestActive) {
    TransactionManager mgr;

    auto* t1 = mgr.begin().value();
    auto* t2 = mgr.begin().value();
    auto* t3 = mgr.begin().value();

    ASSERT_TRUE(mgr.commit(t2->txn_id).has_value());
    // t1 and t3 active; horizon should be t1's ID (smallest active).
    EXPECT_EQ(mgr.xmin_horizon(), t1->txn_id);

    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());
    // t3 is the only active txn, but its snapshot was taken when t1 was active
    // (snapshot.xmin=1), so the horizon remains at 1 — t3 still needs data from t1.
    EXPECT_EQ(mgr.xmin_horizon(), t1->txn_id);

    // Once t3 completes, the horizon advances to next_txn_id.
    ASSERT_TRUE(mgr.commit(t3->txn_id).has_value());
    EXPECT_EQ(mgr.xmin_horizon(), mgr.next_txn_id());
}

TEST(XminHorizon, AdvancesWhenOldTransactionsComplete) {
    TransactionManager mgr;

    // t1 starts alone — its snapshot has no active transactions.
    auto* t1 = mgr.begin().value();
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    // t2 starts after t1 committed — clean snapshot.
    auto* t2 = mgr.begin().value();
    // Horizon should be t2's txn_id (no older active transactions).
    EXPECT_EQ(mgr.xmin_horizon(), t2->txn_id);
}

// =============================================================================
// is_dead tests
// =============================================================================

TEST(IsDead, AbortedXminIsDead) {
    TransactionManager mgr;
    auto* t1 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t1->txn_id;
    ASSERT_TRUE(mgr.abort(t1->txn_id).has_value());

    EXPECT_TRUE(is_dead(header, mgr.xmin_horizon(), mgr));
}

TEST(IsDead, LiveTupleNotDead) {
    TransactionManager mgr;
    auto* t1 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t1->txn_id;
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    // xmax not set — tuple is live.
    EXPECT_FALSE(is_dead(header, mgr.xmin_horizon(), mgr));
}

TEST(IsDead, DeletedAndBelowHorizon) {
    TransactionManager mgr;

    auto* t1 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t1->txn_id;
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    auto* t2 = mgr.begin().value();
    header.xmax = t2->txn_id;
    ASSERT_TRUE(mgr.commit(t2->txn_id).has_value());

    // No active transactions — horizon is past both t1 and t2.
    EXPECT_TRUE(is_dead(header, mgr.xmin_horizon(), mgr));
}

TEST(IsDead, DeletedButAboveHorizonNotDead) {
    TransactionManager mgr;

    auto* t1 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t1->txn_id;
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    // T2 is active (keeps horizon low).
    auto* t2 = mgr.begin().value();

    auto* t3 = mgr.begin().value();
    header.xmax = t3->txn_id;
    ASSERT_TRUE(mgr.commit(t3->txn_id).has_value());

    // Horizon is at t2's txn_id. xmax == t3 > t2 = horizon, so NOT dead.
    EXPECT_FALSE(is_dead(header, mgr.xmin_horizon(), mgr));

    // Now commit t2, horizon advances past t3.
    ASSERT_TRUE(mgr.commit(t2->txn_id).has_value());
    EXPECT_TRUE(is_dead(header, mgr.xmin_horizon(), mgr));
}

TEST(IsDead, DeletedByAbortedTxnNotDead) {
    TransactionManager mgr;

    auto* t1 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t1->txn_id;
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    auto* t2 = mgr.begin().value();
    header.xmax = t2->txn_id;
    ASSERT_TRUE(mgr.abort(t2->txn_id).has_value());

    // Deletion was aborted — tuple is still live, not dead.
    EXPECT_FALSE(is_dead(header, mgr.xmin_horizon(), mgr));
}

TEST(IsDead, ActiveXminNotDead) {
    TransactionManager mgr;
    auto* t1 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t1->txn_id;

    // xmin still active — not dead (creator might commit).
    EXPECT_FALSE(is_dead(header, mgr.xmin_horizon(), mgr));
}

// =============================================================================
// Record write/read set tests
// =============================================================================

TEST(TransactionManager, RecordWriteAndRead) {
    TransactionManager mgr;
    auto* t1 = mgr.begin().value();

    RID rid1 = {1, 0};
    RID rid2 = {2, 3};

    mgr.record_write(t1->txn_id, rid1);
    mgr.record_write(t1->txn_id, rid2);
    mgr.record_read(t1->txn_id, rid1);

    EXPECT_EQ(t1->write_set.size(), 2u);
    EXPECT_EQ(t1->read_set.size(), 1u);
    EXPECT_TRUE(t1->write_set.count(rid1) > 0);
    EXPECT_TRUE(t1->write_set.count(rid2) > 0);
    EXPECT_TRUE(t1->read_set.count(rid1) > 0);
}

// =============================================================================
// Isolation level and status name helpers
// =============================================================================

// =============================================================================
// Read Committed: per-statement snapshot tests
// =============================================================================

TEST(ReadCommitted, StatementSnapshotRefreshesEachCall) {
    TransactionManager mgr;

    // T1 inserts and commits.
    auto* t1 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t1->txn_id;
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    // T2 starts under Read Committed.
    auto* t2 = mgr.begin(IsolationLevel::READ_COMMITTED).value();

    // First statement: t1 already committed, so t1's tuple is visible.
    auto snap1 = mgr.get_statement_snapshot(t2->txn_id);
    EXPECT_TRUE(is_visible(header, snap1, mgr, t2->txn_id));

    // T3 inserts and commits between T2's statements.
    auto* t3 = mgr.begin().value();
    MvccTupleHeader header2;
    header2.xmin = t3->txn_id;
    ASSERT_TRUE(mgr.commit(t3->txn_id).has_value());

    // Second statement: refreshed snapshot should now see t3's tuple.
    auto snap2 = mgr.get_statement_snapshot(t2->txn_id);
    EXPECT_TRUE(is_visible(header2, snap2, mgr, t2->txn_id));

    // Snapshots are different — snap2 has a higher xmax.
    EXPECT_GT(snap2.xmax, snap1.xmax);
}

// =============================================================================
// Snapshot Isolation: snapshot stability tests
// =============================================================================

TEST(SnapshotIsolation, SnapshotDoesNotRefresh) {
    TransactionManager mgr;

    // T1 inserts and commits.
    auto* t1 = mgr.begin().value();
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    // T2 starts under Snapshot Isolation.
    auto* t2 = mgr.begin(IsolationLevel::SNAPSHOT_ISOLATION).value();
    auto snap1 = mgr.get_statement_snapshot(t2->txn_id);

    // T3 inserts and commits between T2's statements.
    auto* t3 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t3->txn_id;
    ASSERT_TRUE(mgr.commit(t3->txn_id).has_value());

    // Second statement: snapshot should NOT refresh — t3 invisible.
    auto snap2 = mgr.get_statement_snapshot(t2->txn_id);
    EXPECT_FALSE(is_visible(header, snap2, mgr, t2->txn_id));

    // Snapshots are identical.
    EXPECT_EQ(snap1.xmax, snap2.xmax);
    EXPECT_EQ(snap1.xmin, snap2.xmin);
}

// =============================================================================
// Serializable (SSI): rw-dependency cycle detection
// =============================================================================

TEST(SSI, WriteSkewDetected) {
    // Classic write skew scenario:
    // T1 reads X, writes Y.
    // T2 reads Y, writes X.
    // Both commit — serialization anomaly (cycle).
    TransactionManager mgr;

    auto* t1 = mgr.begin(IsolationLevel::SERIALIZABLE).value();
    auto* t2 = mgr.begin(IsolationLevel::SERIALIZABLE).value();

    RID rid_x = {1, 0};
    RID rid_y = {1, 1};

    // T1 reads X, writes Y.
    mgr.record_read(t1->txn_id, rid_x);
    mgr.record_write(t1->txn_id, rid_y);

    // T2 reads Y, writes X.
    mgr.record_read(t2->txn_id, rid_y);
    mgr.record_write(t2->txn_id, rid_x);

    // T1 commits first — succeeds (no concurrent committed yet).
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    // T2 tries to commit — should fail due to rw-dependency cycle.
    // T1 wrote rid_y which T2 read (rw-in: T2 -> T1).
    // T2 wrote rid_x which T1 read (rw-out: T2 -> T1).
    // Actually: T1 read X that T2 wrote (rw: T2->T1), T2 read Y that T1 wrote (rw: T1->T2).
    // For T2 committing: rw-out = T2 reads rid_y which T1 wrote, rw-in = T2 writes rid_x which T1
    // read.
    auto result = mgr.commit(t2->txn_id);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TXN_CONFLICT);
}

TEST(SSI, NoConflictWhenNoRwCycle) {
    // T1 reads X, writes X.
    // T2 reads Y, writes Y.
    // No rw-dependency cycle.
    TransactionManager mgr;

    auto* t1 = mgr.begin(IsolationLevel::SERIALIZABLE).value();
    auto* t2 = mgr.begin(IsolationLevel::SERIALIZABLE).value();

    RID rid_x = {1, 0};
    RID rid_y = {1, 1};

    mgr.record_read(t1->txn_id, rid_x);
    mgr.record_write(t1->txn_id, rid_x);

    mgr.record_read(t2->txn_id, rid_y);
    mgr.record_write(t2->txn_id, rid_y);

    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());
    // No rw-cycle — T2 should commit successfully.
    ASSERT_TRUE(mgr.commit(t2->txn_id).has_value());
}

TEST(SSI, ReadOnlyTransactionsNoConflict) {
    // T1 reads X. T2 writes X. No cycle because T1 has no writes.
    TransactionManager mgr;

    auto* t1 = mgr.begin(IsolationLevel::SERIALIZABLE).value();
    auto* t2 = mgr.begin(IsolationLevel::SERIALIZABLE).value();

    RID rid_x = {1, 0};

    mgr.record_read(t1->txn_id, rid_x);
    mgr.record_write(t2->txn_id, rid_x);

    ASSERT_TRUE(mgr.commit(t2->txn_id).has_value());
    // T1 is read-only — no rw-in possible, so no cycle.
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());
}

// =============================================================================
// Default isolation level tests
// =============================================================================

TEST(DefaultIsolationLevel, DefaultIsReadCommitted) {
    TransactionManager mgr;
    EXPECT_EQ(mgr.default_isolation_level(), IsolationLevel::READ_COMMITTED);
}

TEST(DefaultIsolationLevel, CanBeChanged) {
    TransactionManager mgr;

    mgr.set_default_isolation_level(IsolationLevel::SNAPSHOT_ISOLATION);
    EXPECT_EQ(mgr.default_isolation_level(), IsolationLevel::SNAPSHOT_ISOLATION);

    mgr.set_default_isolation_level(IsolationLevel::SERIALIZABLE);
    EXPECT_EQ(mgr.default_isolation_level(), IsolationLevel::SERIALIZABLE);
}

// =============================================================================
// Isolation level and status name helpers
// =============================================================================

TEST(Helpers, IsolationLevelNames) {
    EXPECT_STREQ(isolation_level_name(IsolationLevel::READ_COMMITTED), "READ COMMITTED");
    EXPECT_STREQ(isolation_level_name(IsolationLevel::SNAPSHOT_ISOLATION), "REPEATABLE READ");
    EXPECT_STREQ(isolation_level_name(IsolationLevel::SERIALIZABLE), "SERIALIZABLE");
}

TEST(Helpers, TransactionStatusNames) {
    EXPECT_STREQ(transaction_status_name(TransactionStatus::ACTIVE), "ACTIVE");
    EXPECT_STREQ(transaction_status_name(TransactionStatus::COMMITTED), "COMMITTED");
    EXPECT_STREQ(transaction_status_name(TransactionStatus::ABORTED), "ABORTED");
}

// =============================================================================
// SAVEPOINT / ROLLBACK TO / RELEASE SAVEPOINT tests
// =============================================================================

TEST(Savepoint, CreateAndRollbackRestoresWriteSet) {
    TransactionManager mgr;
    auto* t1 = mgr.begin().value();

    RID rid_a = {1, 0};
    RID rid_b = {1, 1};
    RID rid_c = {2, 0};

    // Write rid_a, then create a savepoint.
    mgr.record_write(t1->txn_id, rid_a);
    ASSERT_TRUE(mgr.savepoint(t1->txn_id, "sp1").has_value());

    // Write more after the savepoint.
    mgr.record_write(t1->txn_id, rid_b);
    mgr.record_write(t1->txn_id, rid_c);
    EXPECT_EQ(t1->write_set.size(), 3u);

    // Rollback to sp1 — should restore write set to {rid_a}.
    ASSERT_TRUE(mgr.rollback_to_savepoint(t1->txn_id, "sp1").has_value());
    EXPECT_EQ(t1->write_set.size(), 1u);
    EXPECT_TRUE(t1->write_set.count(rid_a) > 0);
    EXPECT_FALSE(t1->write_set.count(rid_b) > 0);
    EXPECT_FALSE(t1->write_set.count(rid_c) > 0);
}

TEST(Savepoint, RollbackAlsoRestoresReadSet) {
    TransactionManager mgr;
    auto* t1 = mgr.begin().value();

    RID rid_x = {1, 0};
    RID rid_y = {2, 0};

    mgr.record_read(t1->txn_id, rid_x);
    ASSERT_TRUE(mgr.savepoint(t1->txn_id, "sp1").has_value());

    mgr.record_read(t1->txn_id, rid_y);
    EXPECT_EQ(t1->read_set.size(), 2u);

    ASSERT_TRUE(mgr.rollback_to_savepoint(t1->txn_id, "sp1").has_value());
    EXPECT_EQ(t1->read_set.size(), 1u);
    EXPECT_TRUE(t1->read_set.count(rid_x) > 0);
    EXPECT_FALSE(t1->read_set.count(rid_y) > 0);
}

TEST(Savepoint, NestedSavepoints) {
    TransactionManager mgr;
    auto* t1 = mgr.begin().value();

    mgr.record_write(t1->txn_id, {1, 0});
    ASSERT_TRUE(mgr.savepoint(t1->txn_id, "sp1").has_value());

    mgr.record_write(t1->txn_id, {2, 0});
    ASSERT_TRUE(mgr.savepoint(t1->txn_id, "sp2").has_value());

    mgr.record_write(t1->txn_id, {3, 0});
    EXPECT_EQ(t1->write_set.size(), 3u);

    // Rollback to sp2 — only undoes writes after sp2.
    ASSERT_TRUE(mgr.rollback_to_savepoint(t1->txn_id, "sp2").has_value());
    EXPECT_EQ(t1->write_set.size(), 2u);
    EXPECT_TRUE(t1->write_set.count(RID{1, 0}) > 0);
    EXPECT_TRUE(t1->write_set.count(RID{2, 0}) > 0);

    // Rollback to sp1 — further undoes.
    ASSERT_TRUE(mgr.rollback_to_savepoint(t1->txn_id, "sp1").has_value());
    EXPECT_EQ(t1->write_set.size(), 1u);
    EXPECT_TRUE(t1->write_set.count(RID{1, 0}) > 0);
}

TEST(Savepoint, RollbackToMiddleDestroysLaterSavepoints) {
    TransactionManager mgr;
    auto* t1 = mgr.begin().value();

    ASSERT_TRUE(mgr.savepoint(t1->txn_id, "sp1").has_value());
    ASSERT_TRUE(mgr.savepoint(t1->txn_id, "sp2").has_value());
    ASSERT_TRUE(mgr.savepoint(t1->txn_id, "sp3").has_value());
    EXPECT_EQ(t1->savepoints.size(), 3u);

    // Rollback to sp1 — destroys sp2 and sp3.
    ASSERT_TRUE(mgr.rollback_to_savepoint(t1->txn_id, "sp1").has_value());
    EXPECT_EQ(t1->savepoints.size(), 1u);
    EXPECT_EQ(t1->savepoints[0].name, "sp1");

    // sp2 and sp3 no longer exist.
    auto result = mgr.rollback_to_savepoint(t1->txn_id, "sp2");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(Savepoint, SavepointRetainedAfterRollback) {
    // Per SQL standard: ROLLBACK TO keeps the target savepoint, allowing reuse.
    TransactionManager mgr;
    auto* t1 = mgr.begin().value();

    ASSERT_TRUE(mgr.savepoint(t1->txn_id, "sp1").has_value());
    mgr.record_write(t1->txn_id, {1, 0});
    ASSERT_TRUE(mgr.rollback_to_savepoint(t1->txn_id, "sp1").has_value());
    EXPECT_EQ(t1->write_set.size(), 0u);

    // Write again and rollback a second time — should still work.
    mgr.record_write(t1->txn_id, {2, 0});
    ASSERT_TRUE(mgr.rollback_to_savepoint(t1->txn_id, "sp1").has_value());
    EXPECT_EQ(t1->write_set.size(), 0u);
}

TEST(Savepoint, ReleaseSavepoint) {
    TransactionManager mgr;
    auto* t1 = mgr.begin().value();

    ASSERT_TRUE(mgr.savepoint(t1->txn_id, "sp1").has_value());
    ASSERT_TRUE(mgr.savepoint(t1->txn_id, "sp2").has_value());
    EXPECT_EQ(t1->savepoints.size(), 2u);

    // Release sp1 — removes sp1 and sp2 (established after sp1).
    ASSERT_TRUE(mgr.release_savepoint(t1->txn_id, "sp1").has_value());
    EXPECT_TRUE(t1->savepoints.empty());

    // Cannot rollback to released savepoint.
    auto result = mgr.rollback_to_savepoint(t1->txn_id, "sp1");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(Savepoint, NonExistentSavepointFails) {
    TransactionManager mgr;
    auto* t1 = mgr.begin().value();

    auto result = mgr.rollback_to_savepoint(t1->txn_id, "nosuchsp");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);

    auto rel = mgr.release_savepoint(t1->txn_id, "nosuchsp");
    EXPECT_FALSE(rel.has_value());
    EXPECT_EQ(rel.error().code, StatusCode::NOT_FOUND);
}

TEST(Savepoint, SavepointInNonActiveTransaction) {
    TransactionManager mgr;
    auto* t1 = mgr.begin().value();
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    auto result = mgr.savepoint(t1->txn_id, "sp1");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(Savepoint, DuplicateNameUsesLatest) {
    TransactionManager mgr;
    auto* t1 = mgr.begin().value();

    // Create two savepoints with the same name at different points.
    mgr.record_write(t1->txn_id, {1, 0});
    ASSERT_TRUE(mgr.savepoint(t1->txn_id, "dup").has_value());

    mgr.record_write(t1->txn_id, {2, 0});
    ASSERT_TRUE(mgr.savepoint(t1->txn_id, "dup").has_value());

    mgr.record_write(t1->txn_id, {3, 0});
    EXPECT_EQ(t1->write_set.size(), 3u);

    // Rollback to "dup" — uses the LATEST one (which saved {1,0} and {2,0}).
    ASSERT_TRUE(mgr.rollback_to_savepoint(t1->txn_id, "dup").has_value());
    EXPECT_EQ(t1->write_set.size(), 2u);
    EXPECT_TRUE(t1->write_set.count(RID{1, 0}) > 0);
    EXPECT_TRUE(t1->write_set.count(RID{2, 0}) > 0);
}

TEST(Savepoint, CommitAfterSavepointSucceeds) {
    TransactionManager mgr;
    auto* t1 = mgr.begin(IsolationLevel::SNAPSHOT_ISOLATION).value();

    mgr.record_write(t1->txn_id, {1, 0});
    ASSERT_TRUE(mgr.savepoint(t1->txn_id, "sp1").has_value());
    mgr.record_write(t1->txn_id, {2, 0});

    // Commit should succeed — savepoints don't interfere with commit.
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());
    EXPECT_EQ(mgr.get_status(t1->txn_id), TransactionStatus::COMMITTED);
}

// =============================================================================
// Transaction GC (garbage collection) tests
// =============================================================================

TEST(TransactionGC, CleansUpCompletedTransactions) {
    TransactionManager mgr;

    // Create and commit several transactions.
    auto* t1 = mgr.begin().value();
    auto* t2 = mgr.begin().value();
    auto* t3 = mgr.begin().value();
    auto id1 = t1->txn_id;
    auto id2 = t2->txn_id;
    auto id3 = t3->txn_id;
    ASSERT_TRUE(mgr.commit(id1).has_value());
    ASSERT_TRUE(mgr.commit(id2).has_value());
    ASSERT_TRUE(mgr.commit(id3).has_value());

    // All completed — gc should remove them.
    mgr.gc_completed_transactions();

    // Transactions should no longer be in the map (pointers are now dangling).
    EXPECT_EQ(mgr.get_transaction(id1), nullptr);
    EXPECT_EQ(mgr.get_transaction(id2), nullptr);
    EXPECT_EQ(mgr.get_transaction(id3), nullptr);

    // But status should still be correct (from pruned_committed_).
    EXPECT_EQ(mgr.get_status(id1), TransactionStatus::COMMITTED);
    EXPECT_EQ(mgr.get_status(id2), TransactionStatus::COMMITTED);
    EXPECT_EQ(mgr.get_status(id3), TransactionStatus::COMMITTED);
}

TEST(TransactionGC, PreservesActiveTransactions) {
    TransactionManager mgr;

    auto* t1 = mgr.begin().value();
    auto* t2 = mgr.begin().value();
    auto id1 = t1->txn_id;
    auto id2 = t2->txn_id;
    ASSERT_TRUE(mgr.commit(id1).has_value());
    // t2 still active.

    mgr.gc_completed_transactions();

    // With t2 active, horizon = min(snapshot.xmin=1, txn_id=2) = 1.
    // t1 txn_id=1, 1 < 1 is false — not GC'd.
    // t2 is active — not GC'd.
    EXPECT_NE(mgr.get_transaction(id1), nullptr);
    EXPECT_NE(mgr.get_transaction(id2), nullptr);

    // After t2 commits, all can be GC'd.
    ASSERT_TRUE(mgr.commit(id2).has_value());
    mgr.gc_completed_transactions();
    EXPECT_EQ(mgr.get_transaction(id1), nullptr);
    EXPECT_EQ(mgr.get_transaction(id2), nullptr);
    EXPECT_EQ(mgr.get_status(id1), TransactionStatus::COMMITTED);
    EXPECT_EQ(mgr.get_status(id2), TransactionStatus::COMMITTED);
}

TEST(TransactionGC, AbortedTransactionsGCdCorrectly) {
    TransactionManager mgr;

    auto* t1 = mgr.begin().value();
    auto t1_id = t1->txn_id;
    ASSERT_TRUE(mgr.abort(t1_id).has_value());

    mgr.gc_completed_transactions();

    // Aborted transaction should be removed.
    EXPECT_EQ(mgr.get_transaction(t1_id), nullptr);
    // Status defaults to ABORTED for unknown txns — correct behavior.
    EXPECT_EQ(mgr.get_status(t1_id), TransactionStatus::ABORTED);
}

TEST(TransactionGC, VisibilityCorrectAfterGC) {
    TransactionManager mgr;

    // T1 inserts and commits.
    auto* t1 = mgr.begin().value();
    MvccTupleHeader header;
    header.xmin = t1->txn_id;
    ASSERT_TRUE(mgr.commit(t1->txn_id).has_value());

    // GC t1.
    mgr.gc_completed_transactions();

    // T2 starts — should still see t1's committed tuple.
    auto* t2 = mgr.begin().value();
    EXPECT_TRUE(is_visible(header, t2->snapshot, mgr, t2->txn_id));
}
