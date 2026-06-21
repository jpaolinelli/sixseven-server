/// QA tests for GDB-875: ConfigureUpdatesThresholds vacuous-test fix
/// and the new thread-safe config() const accessor.
///
/// Focus areas:
///  1. Mutation-grade check: assertions fire when configure() is a no-op
///     (proven by design — we verify the post-configure values differ from initial).
///  2. config() returns a consistent snapshot (no torn read).
///  3. No deadlock: config() and check_health() both take mutex_ but never
///     call each other while holding the lock.
///  4. Repeated configure() calls: last write wins.
///  5. Concurrent configure() + config() races under AddressSanitizer.
///  6. Default-constructed monitor has expected default thresholds.
///  7. No duplicate TEST names in ReplicationHealthMonitor suite.

#include "sixseven/server/replication_health_monitor.h"
#include "sixseven/server/replication_slot.h"
#include "sixseven/server/wal_sender_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

#include "test_wal_helpers.h"

using namespace sixseven;
using namespace sixseven::test;

// ---------------------------------------------------------------------------
// Helper: build a minimal HealthMonitorConfig with distinct values.
// ---------------------------------------------------------------------------
static HealthMonitorConfig make_cfg(int lag_ms, int disconnect_ms) {
    HealthMonitorConfig c;
    c.lag_warning_threshold = std::chrono::milliseconds(lag_ms);
    c.disconnect_warning_threshold = std::chrono::milliseconds(disconnect_ms);
    return c;
}

// ===========================================================================
// Suite: QA_GDB875_ConfigAccessor
// ===========================================================================

/// AC: config() returns default thresholds when monitor is default-constructed.
TEST(QA_GDB875_ConfigAccessor, DefaultConstructedHasExpectedDefaults) {
    ReplicationHealthMonitor monitor;
    auto cfg = monitor.config();
    // Defaults from HealthMonitorConfig struct definition:
    // lag_warning_threshold{10000}, disconnect_warning_threshold{60000}
    EXPECT_EQ(cfg.lag_warning_threshold, std::chrono::milliseconds(10000));
    EXPECT_EQ(cfg.disconnect_warning_threshold, std::chrono::milliseconds(60000));
}

/// AC: config() reflects the constructor-provided config before any configure() call.
TEST(QA_GDB875_ConfigAccessor, ConstructorConfigReflectedByAccessor) {
    auto monitor = ReplicationHealthMonitor(make_cfg(1234, 5678));
    auto got = monitor.config();
    EXPECT_EQ(got.lag_warning_threshold, std::chrono::milliseconds(1234));
    EXPECT_EQ(got.disconnect_warning_threshold, std::chrono::milliseconds(5678));
}

/// MUTATION-GRADE: after configure() the accessor must return the NEW values.
/// If configure() were a no-op, the initial values (5000 / 30000) would be
/// returned and both assertions below would FAIL — proving non-vacuity.
TEST(QA_GDB875_ConfigAccessor, ConfigureUpdatesThresholdsMutationGrade) {
    ReplicationHealthMonitor monitor(make_cfg(5000, 30000));

    monitor.configure(make_cfg(2000, 10000));

    auto got = monitor.config();
    EXPECT_EQ(got.lag_warning_threshold, std::chrono::milliseconds(2000));
    EXPECT_EQ(got.disconnect_warning_threshold, std::chrono::milliseconds(10000));
}

/// Distinguish pre- and post-configure snapshots to ensure the change is real.
TEST(QA_GDB875_ConfigAccessor, PreAndPostConfigureDiffer) {
    ReplicationHealthMonitor monitor(make_cfg(9999, 9999));

    auto before = monitor.config();
    EXPECT_EQ(before.lag_warning_threshold, std::chrono::milliseconds(9999));

    monitor.configure(make_cfg(1, 1));

    auto after = monitor.config();
    EXPECT_EQ(after.lag_warning_threshold, std::chrono::milliseconds(1));
    EXPECT_NE(before.lag_warning_threshold, after.lag_warning_threshold);
}

/// configure() called multiple times: last write wins.
TEST(QA_GDB875_ConfigAccessor, RepeatedConfigureLastWriteWins) {
    ReplicationHealthMonitor monitor(make_cfg(100, 200));

    for (int i = 1; i <= 100; ++i) {
        monitor.configure(make_cfg(i * 10, i * 20));
    }

    auto got = monitor.config();
    EXPECT_EQ(got.lag_warning_threshold, std::chrono::milliseconds(1000));
    EXPECT_EQ(got.disconnect_warning_threshold, std::chrono::milliseconds(2000));
}

/// config() called multiple times returns the same value each time (idempotent).
TEST(QA_GDB875_ConfigAccessor, MultipleConfigCallsIdempotent) {
    ReplicationHealthMonitor monitor(make_cfg(3333, 7777));

    for (int i = 0; i < 50; ++i) {
        auto got = monitor.config();
        EXPECT_EQ(got.lag_warning_threshold, std::chrono::milliseconds(3333));
        EXPECT_EQ(got.disconnect_warning_threshold, std::chrono::milliseconds(7777));
    }
}

// ===========================================================================
// Suite: QA_GDB875_LockSafety
// ===========================================================================

/// config() must not deadlock when called repeatedly without any configure().
TEST(QA_GDB875_LockSafety, ConfigDoesNotDeadlock) {
    ReplicationHealthMonitor monitor(make_cfg(500, 1000));
    // If config() tries to re-acquire mutex_ (e.g., calls configure() internally)
    // this would deadlock on a non-recursive mutex. The call must return quickly.
    for (int i = 0; i < 1000; ++i) {
        auto got = monitor.config();
        (void)got;
    }
    // If we reach here the lock is not recursive-deadlocked.
    SUCCEED();
}

/// check_health() and config() must not deadlock when interleaved on the same
/// monitor. Both take mutex_; neither must call the other while holding it.
TEST(QA_GDB875_LockSafety, CheckHealthAndConfigInterleavedNoDeadlock) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderManager sender_mgr(dir.path(), nullptr, writer, 10);
    ReplicationSlotManager slot_mgr(dir.path());

    ReplicationHealthMonitor monitor(make_cfg(1000, 5000));

    for (int i = 0; i < 20; ++i) {
        monitor.check_health(sender_mgr, &slot_mgr, writer);
        auto got = monitor.config();
        (void)got;
    }

    sender_mgr.stop_all();
    ASSERT_TRUE(writer.close().has_value());
}

// ===========================================================================
// Suite: QA_GDB875_Concurrency
// ===========================================================================

/// Concurrent configure() calls: no data race (ASan TSan bait).
/// Final config() must return one of the two written values — not a torn read.
TEST(QA_GDB875_Concurrency, ConcurrentConfigureNoDataRace) {
    ReplicationHealthMonitor monitor(make_cfg(1000, 2000));

    constexpr int kThreads = 8;
    constexpr int kIter = 200;

    std::vector<std::thread> writers;
    writers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([&monitor, t] {
            for (int i = 0; i < kIter; ++i) {
                monitor.configure(make_cfg((t + 1) * 100, (t + 1) * 200));
            }
        });
    }
    for (auto& th : writers) th.join();

    // config() must return a coherent (non-torn) snapshot — both fields must
    // belong to the same valid configure() call.
    auto got = monitor.config();
    auto lag = got.lag_warning_threshold.count();
    auto disc = got.disconnect_warning_threshold.count();

    // Each thread wrote lag=(t+1)*100, disc=(t+1)*200, so disc == lag*2 always.
    EXPECT_GT(lag, 0);
    EXPECT_GT(disc, 0);
    EXPECT_EQ(disc, lag * 2) << "torn read: lag=" << lag << " disc=" << disc;
}

/// Concurrent configure() writers + config() readers: no crash or data race.
TEST(QA_GDB875_Concurrency, ConcurrentWritersAndReadersNoRace) {
    ReplicationHealthMonitor monitor(make_cfg(500, 1000));

    constexpr int kWriters = 4;
    constexpr int kReaders = 4;
    constexpr int kIter = 300;
    std::atomic<bool> stop{false};

    std::vector<std::thread> threads;
    threads.reserve(kWriters + kReaders);

    for (int t = 0; t < kWriters; ++t) {
        threads.emplace_back([&monitor, &stop, t] {
            int i = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                monitor.configure(make_cfg((t + 1) * 50 + i % 10, (t + 1) * 100 + i % 10));
                ++i;
                if (i >= kIter) break;
            }
        });
    }

    for (int t = 0; t < kReaders; ++t) {
        threads.emplace_back([&monitor, &stop] {
            int i = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                auto got = monitor.config();
                EXPECT_GE(got.lag_warning_threshold.count(), 0);
                EXPECT_GE(got.disconnect_warning_threshold.count(), 0);
                ++i;
                if (i >= kIter) break;
            }
        });
    }

    for (auto& th : threads) th.join();
    stop.store(true);
}

/// Concurrent configure() + check_health() calls: no deadlock or data race.
TEST(QA_GDB875_Concurrency, ConcurrentConfigureAndCheckHealthNoDeadlock) {
    TempWalDir dir;
    WalWriter writer(dir.path(), test_wal_opts());
    ASSERT_TRUE(writer.open().has_value());

    WalSenderManager sender_mgr(dir.path(), nullptr, writer, 10);
    ReplicationSlotManager slot_mgr(dir.path());

    ReplicationHealthMonitor monitor(make_cfg(1000, 5000));

    constexpr int kIter = 50;

    std::thread configurator([&monitor] {
        for (int i = 0; i < kIter; ++i) {
            monitor.configure(make_cfg(100 + i, 200 + i));
        }
    });

    std::thread checker([&monitor, &sender_mgr, &slot_mgr, &writer] {
        for (int i = 0; i < kIter; ++i) {
            monitor.check_health(sender_mgr, &slot_mgr, writer);
        }
    });

    configurator.join();
    checker.join();

    // No deadlock — both threads completed.
    SUCCEED();

    sender_mgr.stop_all();
    ASSERT_TRUE(writer.close().has_value());
}

// ===========================================================================
// Suite: QA_GDB875_BoundaryValues
// ===========================================================================

/// configure() with zero-duration thresholds (boundary: minimum value).
TEST(QA_GDB875_BoundaryValues, ZeroDurationThresholds) {
    ReplicationHealthMonitor monitor;
    monitor.configure(make_cfg(0, 0));

    auto got = monitor.config();
    EXPECT_EQ(got.lag_warning_threshold, std::chrono::milliseconds(0));
    EXPECT_EQ(got.disconnect_warning_threshold, std::chrono::milliseconds(0));
}

/// configure() with very large thresholds (INT32_MAX ms).
TEST(QA_GDB875_BoundaryValues, VeryLargeThresholds) {
    constexpr int kMax = 2147483647;
    ReplicationHealthMonitor monitor;
    monitor.configure(make_cfg(kMax, kMax));

    auto got = monitor.config();
    EXPECT_EQ(got.lag_warning_threshold.count(), kMax);
    EXPECT_EQ(got.disconnect_warning_threshold.count(), kMax);
}

/// configure() with asymmetric thresholds (lag < disconnect, common case).
TEST(QA_GDB875_BoundaryValues, AsymmetricThresholds) {
    ReplicationHealthMonitor monitor;
    monitor.configure(make_cfg(1, 2147483647));

    auto got = monitor.config();
    EXPECT_EQ(got.lag_warning_threshold.count(), 1);
    EXPECT_EQ(got.disconnect_warning_threshold.count(), 2147483647);
}
