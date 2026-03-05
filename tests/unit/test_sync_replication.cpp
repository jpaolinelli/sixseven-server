#include "sixseven/server/sync_replication.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace sixseven;

// -- Parsing tests ------------------------------------------------------------

TEST(SyncReplication, ParseSyncLevelOff) {
    auto result = parse_sync_level("off");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, SyncLevel::OFF);
}

TEST(SyncReplication, ParseSyncLevelRemoteWrite) {
    auto result = parse_sync_level("remote_write");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, SyncLevel::REMOTE_WRITE);
}

TEST(SyncReplication, ParseSyncLevelRemoteFlush) {
    auto result = parse_sync_level("remote_flush");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, SyncLevel::REMOTE_FLUSH);
}

TEST(SyncReplication, ParseSyncLevelRemoteApply) {
    auto result = parse_sync_level("remote_apply");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, SyncLevel::REMOTE_APPLY);
}

TEST(SyncReplication, ParseSyncLevelInvalid) {
    auto result = parse_sync_level("bogus");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(SyncReplication, ParseSyncFallbackError) {
    auto result = parse_sync_fallback("error");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, SyncFallback::ERROR);
}

TEST(SyncReplication, ParseSyncFallbackWarn) {
    auto result = parse_sync_fallback("warn");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, SyncFallback::WARN);
}

TEST(SyncReplication, ParseSyncFallbackBlock) {
    auto result = parse_sync_fallback("block");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, SyncFallback::BLOCK);
}

TEST(SyncReplication, ParseSyncFallbackInvalid) {
    auto result = parse_sync_fallback("unknown");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(SyncReplication, SyncLevelName) {
    EXPECT_STREQ(sync_level_name(SyncLevel::OFF), "off");
    EXPECT_STREQ(sync_level_name(SyncLevel::REMOTE_WRITE), "remote_write");
    EXPECT_STREQ(sync_level_name(SyncLevel::REMOTE_FLUSH), "remote_flush");
    EXPECT_STREQ(sync_level_name(SyncLevel::REMOTE_APPLY), "remote_apply");
}

TEST(SyncReplication, SyncFallbackName) {
    EXPECT_STREQ(sync_fallback_name(SyncFallback::ERROR), "error");
    EXPECT_STREQ(sync_fallback_name(SyncFallback::WARN), "warn");
    EXPECT_STREQ(sync_fallback_name(SyncFallback::BLOCK), "block");
}

// -- Default construction / OFF mode ------------------------------------------

TEST(SyncReplication, DefaultIsOff) {
    SyncReplicationManager mgr;
    EXPECT_FALSE(mgr.is_sync_enabled());
    EXPECT_EQ(mgr.sync_level(), SyncLevel::OFF);
}

TEST(SyncReplication, WaitForSyncReturnsNotEnabledWhenOff) {
    SyncReplicationManager mgr;
    EXPECT_EQ(mgr.wait_for_sync(100), SyncWaitResult::NOT_ENABLED);
}

// -- Basic sync flow: pre-acknowledged LSN ------------------------------------

TEST(SyncReplication, SyncedImmediatelyWhenReplicaAlreadyAhead) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 1;
    cfg.standby_names = {"replica_1"};
    cfg.timeout = std::chrono::milliseconds(100);
    cfg.fallback = SyncFallback::ERROR;

    SyncReplicationManager mgr(cfg);
    EXPECT_TRUE(mgr.is_sync_enabled());

    // Report that replica has flushed up to LSN 200.
    mgr.report_replica_progress("replica_1", 200, 200, 150);

    // Commit at LSN 100 should succeed immediately.
    EXPECT_EQ(mgr.wait_for_sync(100), SyncWaitResult::SYNCED);
}

TEST(SyncReplication, CommitCountZeroAlwaysSynced) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 0;
    cfg.timeout = std::chrono::milliseconds(100);

    SyncReplicationManager mgr(cfg);
    EXPECT_EQ(mgr.wait_for_sync(100), SyncWaitResult::SYNCED);
}

// -- Different sync levels ----------------------------------------------------

TEST(SyncReplication, RemoteWriteUsesReceivedLsn) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_WRITE;
    cfg.commit_count = 1;
    cfg.standby_names = {"r1"};
    cfg.timeout = std::chrono::milliseconds(100);
    cfg.fallback = SyncFallback::ERROR;

    SyncReplicationManager mgr(cfg);

    // received=100, flushed=50, applied=30 — remote_write only cares about received.
    mgr.report_replica_progress("r1", 100, 50, 30);

    EXPECT_EQ(mgr.wait_for_sync(100), SyncWaitResult::SYNCED);
    EXPECT_EQ(mgr.wait_for_sync(50), SyncWaitResult::SYNCED);
}

TEST(SyncReplication, RemoteFlushUsesFlushedLsn) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 1;
    cfg.standby_names = {"r1"};
    cfg.timeout = std::chrono::milliseconds(100);
    cfg.fallback = SyncFallback::ERROR;

    SyncReplicationManager mgr(cfg);

    // received=100, flushed=50, applied=30 — remote_flush cares about flushed.
    mgr.report_replica_progress("r1", 100, 50, 30);

    EXPECT_EQ(mgr.wait_for_sync(50), SyncWaitResult::SYNCED);
    // LSN 51 is NOT flushed yet — should timeout.
    EXPECT_EQ(mgr.wait_for_sync(51), SyncWaitResult::TIMED_OUT_ERROR);
}

TEST(SyncReplication, RemoteApplyUsesAppliedLsn) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_APPLY;
    cfg.commit_count = 1;
    cfg.standby_names = {"r1"};
    cfg.timeout = std::chrono::milliseconds(100);
    cfg.fallback = SyncFallback::ERROR;

    SyncReplicationManager mgr(cfg);

    // received=100, flushed=80, applied=30 — remote_apply cares about applied.
    mgr.report_replica_progress("r1", 100, 80, 30);

    EXPECT_EQ(mgr.wait_for_sync(30), SyncWaitResult::SYNCED);
    // LSN 31 is NOT applied yet — should timeout.
    EXPECT_EQ(mgr.wait_for_sync(31), SyncWaitResult::TIMED_OUT_ERROR);
}

// -- Timeout and fallback behavior --------------------------------------------

TEST(SyncReplication, TimeoutWithErrorFallback) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 1;
    cfg.standby_names = {"r1"};
    cfg.timeout = std::chrono::milliseconds(50);
    cfg.fallback = SyncFallback::ERROR;

    SyncReplicationManager mgr(cfg);
    // No replica progress reported — should timeout.
    auto start = std::chrono::steady_clock::now();
    auto result = mgr.wait_for_sync(100);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(result, SyncWaitResult::TIMED_OUT_ERROR);
    EXPECT_GE(elapsed, std::chrono::milliseconds(40));
}

TEST(SyncReplication, TimeoutWithWarnFallback) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 1;
    cfg.standby_names = {"r1"};
    cfg.timeout = std::chrono::milliseconds(50);
    cfg.fallback = SyncFallback::WARN;

    SyncReplicationManager mgr(cfg);
    auto result = mgr.wait_for_sync(100);
    EXPECT_EQ(result, SyncWaitResult::TIMED_OUT_WARN);
}

TEST(SyncReplication, BlockFallbackWaitsIndefinitely) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 1;
    cfg.standby_names = {"r1"};
    cfg.timeout = std::chrono::milliseconds(50); // Ignored for BLOCK.
    cfg.fallback = SyncFallback::BLOCK;

    SyncReplicationManager mgr(cfg);

    // Start a blocking wait in a background thread.
    auto future = std::async(std::launch::async, [&] { return mgr.wait_for_sync(100); });

    // Wait a bit — it should NOT have completed.
    auto status = future.wait_for(std::chrono::milliseconds(80));
    EXPECT_EQ(status, std::future_status::timeout);

    // Now report progress — the wait should unblock.
    mgr.report_replica_progress("r1", 100, 100, 50);

    auto result = future.get();
    EXPECT_EQ(result, SyncWaitResult::SYNCED);
}

// -- Async wake-up: replica reports progress after wait starts -----------------

TEST(SyncReplication, WaiterWokenByReplicaProgress) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 1;
    cfg.standby_names = {"r1"};
    cfg.timeout = std::chrono::milliseconds(5000);
    cfg.fallback = SyncFallback::ERROR;

    SyncReplicationManager mgr(cfg);

    // Start the wait in a background thread.
    auto future = std::async(std::launch::async, [&] { return mgr.wait_for_sync(100); });

    // Small delay, then report progress.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    mgr.report_replica_progress("r1", 100, 100, 50);

    auto result = future.get();
    EXPECT_EQ(result, SyncWaitResult::SYNCED);
}

// -- Multi-replica scenarios --------------------------------------------------

TEST(SyncReplication, MultiReplicaCommitCountOne) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 1;
    cfg.standby_names = {"r1", "r2", "r3"};
    cfg.timeout = std::chrono::milliseconds(100);
    cfg.fallback = SyncFallback::ERROR;

    SyncReplicationManager mgr(cfg);

    // Only r2 has caught up.
    mgr.report_replica_progress("r1", 50, 50, 50);
    mgr.report_replica_progress("r2", 200, 200, 200);
    mgr.report_replica_progress("r3", 60, 60, 60);

    EXPECT_EQ(mgr.wait_for_sync(100), SyncWaitResult::SYNCED);
    EXPECT_EQ(mgr.count_synced_replicas(100), 1u);
}

TEST(SyncReplication, MultiReplicaCommitCountTwo) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 2;
    cfg.standby_names = {"r1", "r2", "r3"};
    cfg.timeout = std::chrono::milliseconds(100);
    cfg.fallback = SyncFallback::ERROR;

    SyncReplicationManager mgr(cfg);

    // Only one replica has caught up — should timeout.
    mgr.report_replica_progress("r1", 50, 50, 50);
    mgr.report_replica_progress("r2", 200, 200, 200);
    mgr.report_replica_progress("r3", 60, 60, 60);

    EXPECT_EQ(mgr.wait_for_sync(100), SyncWaitResult::TIMED_OUT_ERROR);
    EXPECT_EQ(mgr.count_synced_replicas(100), 1u);

    // Second replica catches up.
    mgr.report_replica_progress("r1", 100, 100, 100);
    EXPECT_EQ(mgr.wait_for_sync(100), SyncWaitResult::SYNCED);
    EXPECT_EQ(mgr.count_synced_replicas(100), 2u);
}

TEST(SyncReplication, MultiReplicaAsyncWakeup) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 2;
    cfg.standby_names = {"r1", "r2"};
    cfg.timeout = std::chrono::milliseconds(5000);
    cfg.fallback = SyncFallback::ERROR;

    SyncReplicationManager mgr(cfg);

    // One replica already caught up.
    mgr.report_replica_progress("r1", 200, 200, 200);

    auto future = std::async(std::launch::async, [&] { return mgr.wait_for_sync(100); });

    // Wait a bit, then second replica catches up.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    mgr.report_replica_progress("r2", 100, 100, 50);

    EXPECT_EQ(future.get(), SyncWaitResult::SYNCED);
}

// -- Standby name filtering ---------------------------------------------------

TEST(SyncReplication, NonSyncStandbyIgnored) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 1;
    cfg.standby_names = {"r1"};
    cfg.timeout = std::chrono::milliseconds(50);
    cfg.fallback = SyncFallback::ERROR;

    SyncReplicationManager mgr(cfg);

    // Report from a replica NOT in the standby names list.
    mgr.report_replica_progress("r_not_sync", 200, 200, 200);

    // Should still timeout because r_not_sync is not in the set.
    EXPECT_EQ(mgr.wait_for_sync(100), SyncWaitResult::TIMED_OUT_ERROR);
    EXPECT_EQ(mgr.count_synced_replicas(100), 0u);
}

TEST(SyncReplication, EmptyStandbyNamesTracksAllReplicas) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 1;
    cfg.standby_names = {}; // Wildcard: all replicas participate.
    cfg.timeout = std::chrono::milliseconds(100);
    cfg.fallback = SyncFallback::ERROR;

    SyncReplicationManager mgr(cfg);

    mgr.report_replica_progress("any_replica", 200, 200, 200);
    EXPECT_EQ(mgr.wait_for_sync(100), SyncWaitResult::SYNCED);
}

// -- Remove replica -----------------------------------------------------------

TEST(SyncReplication, RemoveReplicaStopsTracking) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 1;
    cfg.standby_names = {"r1"};
    cfg.timeout = std::chrono::milliseconds(50);
    cfg.fallback = SyncFallback::ERROR;

    SyncReplicationManager mgr(cfg);

    mgr.report_replica_progress("r1", 200, 200, 200);
    EXPECT_EQ(mgr.wait_for_sync(100), SyncWaitResult::SYNCED);

    mgr.remove_replica("r1");
    // After removal, the replica is no longer tracked.
    EXPECT_EQ(mgr.wait_for_sync(100), SyncWaitResult::TIMED_OUT_ERROR);
}

// -- Runtime reconfiguration --------------------------------------------------

TEST(SyncReplication, ReconfigureToOff) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 1;
    cfg.standby_names = {"r1"};
    cfg.timeout = std::chrono::milliseconds(5000);
    cfg.fallback = SyncFallback::ERROR;

    SyncReplicationManager mgr(cfg);

    // Start waiting in background.
    auto future = std::async(std::launch::async, [&] { return mgr.wait_for_sync(100); });

    // Reconfigure to OFF — should wake the waiter.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    SyncReplicationConfig off_cfg;
    off_cfg.level = SyncLevel::OFF;
    mgr.configure(off_cfg);

    EXPECT_EQ(future.get(), SyncWaitResult::NOT_ENABLED);
    EXPECT_FALSE(mgr.is_sync_enabled());
}

TEST(SyncReplication, ReconfigureLevelChange) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_APPLY;
    cfg.commit_count = 1;
    cfg.standby_names = {"r1"};
    cfg.timeout = std::chrono::milliseconds(100);
    cfg.fallback = SyncFallback::ERROR;

    SyncReplicationManager mgr(cfg);

    // Replica has flushed=100 but applied=50.
    mgr.report_replica_progress("r1", 100, 100, 50);

    // remote_apply requires applied >= 100 — should timeout.
    EXPECT_EQ(mgr.wait_for_sync(100), SyncWaitResult::TIMED_OUT_ERROR);

    // Reconfigure to remote_flush — now flushed=100 is sufficient.
    cfg.level = SyncLevel::REMOTE_FLUSH;
    mgr.configure(cfg);

    EXPECT_EQ(mgr.wait_for_sync(100), SyncWaitResult::SYNCED);
}

// -- Blocking fallback with config change unblock -----------------------------

TEST(SyncReplication, BlockFallbackUnblockedByConfigChangeToOff) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 1;
    cfg.standby_names = {"r1"};
    cfg.timeout = std::chrono::milliseconds(100);
    cfg.fallback = SyncFallback::BLOCK;

    SyncReplicationManager mgr(cfg);

    auto future = std::async(std::launch::async, [&] { return mgr.wait_for_sync(100); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Switch to OFF — should unblock the waiter.
    SyncReplicationConfig off_cfg;
    off_cfg.level = SyncLevel::OFF;
    mgr.configure(off_cfg);

    EXPECT_EQ(future.get(), SyncWaitResult::NOT_ENABLED);
}

// -- Monitoring ---------------------------------------------------------------

TEST(SyncReplication, ReplicaProgressSnapshot) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 1;

    SyncReplicationManager mgr(cfg);

    mgr.report_replica_progress("r1", 100, 80, 50);
    mgr.report_replica_progress("r2", 200, 150, 100);

    auto progress = mgr.replica_progress();
    ASSERT_EQ(progress.size(), 2u);
    EXPECT_EQ(progress["r1"].received_lsn, 100u);
    EXPECT_EQ(progress["r1"].flushed_lsn, 80u);
    EXPECT_EQ(progress["r1"].applied_lsn, 50u);
    EXPECT_EQ(progress["r2"].received_lsn, 200u);
    EXPECT_EQ(progress["r2"].flushed_lsn, 150u);
    EXPECT_EQ(progress["r2"].applied_lsn, 100u);
}

TEST(SyncReplication, CurrentConfigSnapshot) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_APPLY;
    cfg.commit_count = 3;
    cfg.standby_names = {"a", "b", "c"};
    cfg.timeout = std::chrono::milliseconds(5000);
    cfg.fallback = SyncFallback::WARN;

    SyncReplicationManager mgr(cfg);

    auto snap = mgr.current_config();
    EXPECT_EQ(snap.level, SyncLevel::REMOTE_APPLY);
    EXPECT_EQ(snap.commit_count, 3u);
    EXPECT_EQ(snap.standby_names.size(), 3u);
    EXPECT_EQ(snap.timeout, std::chrono::milliseconds(5000));
    EXPECT_EQ(snap.fallback, SyncFallback::WARN);
}

// -- Concurrent waiters -------------------------------------------------------

TEST(SyncReplication, MultipleConcurrentWaiters) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 1;
    cfg.standby_names = {"r1"};
    cfg.timeout = std::chrono::milliseconds(5000);
    cfg.fallback = SyncFallback::ERROR;

    SyncReplicationManager mgr(cfg);

    // Start multiple waiters for different LSNs.
    auto f1 = std::async(std::launch::async, [&] { return mgr.wait_for_sync(50); });
    auto f2 = std::async(std::launch::async, [&] { return mgr.wait_for_sync(100); });
    auto f3 = std::async(std::launch::async, [&] { return mgr.wait_for_sync(150); });

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Report LSN 100 — should satisfy f1 and f2 but not f3.
    mgr.report_replica_progress("r1", 100, 100, 50);

    EXPECT_EQ(f1.get(), SyncWaitResult::SYNCED);
    EXPECT_EQ(f2.get(), SyncWaitResult::SYNCED);

    // f3 still waiting — report more progress.
    mgr.report_replica_progress("r1", 200, 200, 200);
    EXPECT_EQ(f3.get(), SyncWaitResult::SYNCED);
}

// -- Progress update (LSN advances) -------------------------------------------

TEST(SyncReplication, ProgressUpdatesAdvanceLsn) {
    SyncReplicationConfig cfg;
    cfg.level = SyncLevel::REMOTE_FLUSH;
    cfg.commit_count = 1;
    cfg.standby_names = {"r1"};
    cfg.timeout = std::chrono::milliseconds(100);
    cfg.fallback = SyncFallback::ERROR;

    SyncReplicationManager mgr(cfg);

    mgr.report_replica_progress("r1", 50, 50, 50);
    EXPECT_EQ(mgr.wait_for_sync(50), SyncWaitResult::SYNCED);
    EXPECT_EQ(mgr.wait_for_sync(51), SyncWaitResult::TIMED_OUT_ERROR);

    // Progress advances.
    mgr.report_replica_progress("r1", 100, 100, 100);
    EXPECT_EQ(mgr.wait_for_sync(100), SyncWaitResult::SYNCED);
}
