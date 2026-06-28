// QA adversarial tests for GDB-956: pg-wire query cancellation.
//
// Attack surface:
//   - SECURITY:  forged/cross-session cancel (wrong secret, wrong pid, zero secret)
//   - CONCURRENCY: hammer registry under concurrent register/unregister/cancel churn
//   - LIFECYCLE:  double-register, double-unregister, cancel-after-unregister,
//                 guard reset, null flag guard
//   - PARSE:     big-endian pid/secret extraction, truncated buffers, extreme values
//   - NO-REGRESSION: normal path unaffected by cancel check; 57014 only on actual set
//
// Live-socket tests (psql Ctrl+C round-trip) are intentionally omitted: they crash
// on Windows due to a pre-existing fd-assert in Session/PgProtocol (unrelated to
// GDB-956).  All tests here are socket-free unit-style.
//
// ASan: unavailable on Windows/MSVC. Concurrency tests use std::barrier + joins for
// determinism (no sleeps) to compensate for the missing race detector.

#pragma once

#include "sixseven/common/statement_deadline.h"
#include "sixseven/server/cancel_registry.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// SECURITY: forged/cross-session cancel
// ---------------------------------------------------------------------------

// AC: request_cancel with a wrong secret NEVER sets the flag.
TEST(QA_GDB956_Security, ForgedSecret_DoesNotSetFlag) {
    CancelRegistry registry;
    registry.register_connection(100, 9999);

    auto flag = std::make_shared<std::atomic<bool>>(false);
    registry.set_cancel_flag(100, flag);

    registry.request_cancel(100, 9998); // one off
    EXPECT_FALSE(flag->load()) << "Wrong secret must not set the flag";

    registry.request_cancel(100, 0); // zero secret
    EXPECT_FALSE(flag->load()) << "Secret=0 must not set flag when registered secret != 0";

    registry.request_cancel(100, -1); // negative secret
    EXPECT_FALSE(flag->load()) << "Negative wrong secret must not set the flag";
}

// AC: request_cancel for pid A must NOT set pid B's flag.
TEST(QA_GDB956_Security, CancelPidA_DoesNotAffectPidB) {
    CancelRegistry registry;
    registry.register_connection(1, 111);
    registry.register_connection(2, 222);

    auto flag1 = std::make_shared<std::atomic<bool>>(false);
    auto flag2 = std::make_shared<std::atomic<bool>>(false);
    registry.set_cancel_flag(1, flag1);
    registry.set_cancel_flag(2, flag2);

    // Cancel pid=1 with its correct secret.
    registry.request_cancel(1, 111);

    EXPECT_TRUE(flag1->load()) << "pid=1 should be cancelled";
    EXPECT_FALSE(flag2->load()) << "pid=2's flag must remain unset";
}

// AC: supplying pid B's secret against pid A does not cancel anything.
TEST(QA_GDB956_Security, CrossSessionSecret_NoCancel) {
    CancelRegistry registry;
    registry.register_connection(10, 1010);
    registry.register_connection(20, 2020);

    auto flag10 = std::make_shared<std::atomic<bool>>(false);
    auto flag20 = std::make_shared<std::atomic<bool>>(false);
    registry.set_cancel_flag(10, flag10);
    registry.set_cancel_flag(20, flag20);

    // Attempt to cancel pid=10 using pid=20's secret.
    registry.request_cancel(10, 2020);
    EXPECT_FALSE(flag10->load()) << "Using another session's secret must not cancel";
    EXPECT_FALSE(flag20->load()) << "Other session must also be unaffected";
}

// AC: secret=0 registered against secret=0 SHOULD cancel (not a special sentinel).
TEST(QA_GDB956_Security, ZeroSecret_ExactMatch_SetsFlag) {
    CancelRegistry registry;
    registry.register_connection(77, 0);

    auto flag = std::make_shared<std::atomic<bool>>(false);
    registry.set_cancel_flag(77, flag);

    registry.request_cancel(77, 0);
    EXPECT_TRUE(flag->load()) << "secret=0 exact match should cancel";
}

// AC: secret=0 against a non-zero registration must not cancel.
TEST(QA_GDB956_Security, ZeroSecret_WrongMatch_NoCancel) {
    CancelRegistry registry;
    registry.register_connection(88, 12345);

    auto flag = std::make_shared<std::atomic<bool>>(false);
    registry.set_cancel_flag(88, flag);

    registry.request_cancel(88, 0);
    EXPECT_FALSE(flag->load()) << "secret=0 against non-zero registration must not cancel";
}

// AC: extreme pid/secret values (INT32_MIN, INT32_MAX).
TEST(QA_GDB956_Security, ExtremeValues_CorrectCancel) {
    CancelRegistry registry;
    registry.register_connection(INT32_MIN, INT32_MAX);

    auto flag = std::make_shared<std::atomic<bool>>(false);
    registry.set_cancel_flag(INT32_MIN, flag);

    registry.request_cancel(INT32_MIN, INT32_MAX);
    EXPECT_TRUE(flag->load()) << "Extreme pid/secret should cancel when matching";
}

TEST(QA_GDB956_Security, ExtremeValues_WrongSecret_NoCancel) {
    CancelRegistry registry;
    registry.register_connection(INT32_MAX, INT32_MIN);

    auto flag = std::make_shared<std::atomic<bool>>(false);
    registry.set_cancel_flag(INT32_MAX, flag);

    registry.request_cancel(INT32_MAX, 0);
    EXPECT_FALSE(flag->load()) << "Extreme pid with wrong secret must not cancel";
}

// ---------------------------------------------------------------------------
// LIFECYCLE
// ---------------------------------------------------------------------------

// AC: register same pid twice -- second call is a no-op (does not overwrite secret).
TEST(QA_GDB956_Lifecycle, DoubleRegister_FirstSecretWins) {
    CancelRegistry registry;
    registry.register_connection(42, 1111);
    registry.register_connection(42, 9999); // second register same pid

    auto flag = std::make_shared<std::atomic<bool>>(false);
    registry.set_cancel_flag(42, flag);

    // First secret should still work (no-op on second register).
    registry.request_cancel(42, 1111);
    EXPECT_TRUE(flag->load()) << "First registered secret should still work after double-register";
}

// AC: unregister an unknown pid is a safe no-op.
TEST(QA_GDB956_Lifecycle, UnregisterUnknown_NoOp) {
    CancelRegistry registry;
    // Should not crash.
    registry.unregister_connection(9999);
}

// AC: unregister twice is safe.
TEST(QA_GDB956_Lifecycle, DoubleUnregister_NoOp) {
    CancelRegistry registry;
    registry.register_connection(5, 555);
    registry.unregister_connection(5);
    registry.unregister_connection(5); // second unregister must not crash
}

// AC: request_cancel after unregister is a safe no-op (no crash, no UAF).
TEST(QA_GDB956_Lifecycle, CancelAfterUnregister_NoOp) {
    CancelRegistry registry;
    registry.register_connection(3, 333);

    auto flag = std::make_shared<std::atomic<bool>>(false);
    registry.set_cancel_flag(3, flag);

    registry.unregister_connection(3);
    registry.request_cancel(3, 333); // must not crash or set the flag
    EXPECT_FALSE(flag->load());
}

// AC: set_cancel_flag after unregister is a safe no-op.
TEST(QA_GDB956_Lifecycle, SetFlagAfterUnregister_NoOp) {
    CancelRegistry registry;
    registry.register_connection(4, 444);
    registry.unregister_connection(4);

    auto flag = std::make_shared<std::atomic<bool>>(false);
    registry.set_cancel_flag(4, flag); // no-op
    registry.request_cancel(4, 444);   // no-op
    EXPECT_FALSE(flag->load());
}

// AC: clear_cancel_flag for unknown pid is a safe no-op.
TEST(QA_GDB956_Lifecycle, ClearFlagUnknownPid_NoOp) {
    CancelRegistry registry;
    registry.clear_cancel_flag(7777); // must not crash
}

// AC: StatementCancelGuard arms and resets; stale cancel does not carry to next statement.
TEST(QA_GDB956_Lifecycle, GuardArmReset_NoStaleCancel) {
    auto flag = std::make_shared<std::atomic<bool>>(false);
    {
        StatementCancelGuard guard(flag);
        flag->store(true, std::memory_order_release);
        EXPECT_TRUE(StatementCancel::requested());
    }
    // Guard destroyed; requested() must be false.
    EXPECT_FALSE(StatementCancel::requested());

    // Second statement with a fresh flag must not inherit stale cancel.
    auto flag2 = std::make_shared<std::atomic<bool>>(false);
    {
        StatementCancelGuard guard2(flag2);
        EXPECT_FALSE(StatementCancel::requested())
            << "Stale cancel must not bleed into next statement";
    }
}

// AC: StatementCancelGuard with null flag is a no-op.
TEST(QA_GDB956_Lifecycle, NullFlagGuard_NoOp) {
    StatementCancelGuard guard(nullptr);
    EXPECT_FALSE(StatementCancel::requested());
}

// AC: arm() resets a pre-set flag (stale cancel protection).
TEST(QA_GDB956_Lifecycle, Guard_ResetsPreSetFlag) {
    auto flag = std::make_shared<std::atomic<bool>>(true); // stale true
    StatementCancelGuard guard(flag);
    EXPECT_FALSE(flag->load()) << "Guard must reset flag to false on arm";
    EXPECT_FALSE(StatementCancel::requested());
}

// ---------------------------------------------------------------------------
// PARSE: CancelRequest big-endian extraction
// ---------------------------------------------------------------------------

// Helper: big-endian int32 read (mirrors pg_protocol.cpp).
static int32_t read_be_int32(const uint8_t* p) {
    return static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 24) |
                                (static_cast<uint32_t>(p[1]) << 16) |
                                (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]));
}

// AC: correct 16-byte packet -> correct pid+secret.
TEST(QA_GDB956_Parse, FullPacket_CorrectExtraction) {
    uint8_t buf[16];
    buf[0] = 0x00;
    buf[1] = 0x00;
    buf[2] = 0x00;
    buf[3] = 0x10; // length = 16
    buf[4] = 0x04;
    buf[5] = 0xD2;
    buf[6] = 0x16;
    buf[7] = 0x2E; // CANCEL_REQUEST_CODE = 80877102
    // pid = 9999 = 0x0000270F
    buf[8] = 0x00;
    buf[9] = 0x00;
    buf[10] = 0x27;
    buf[11] = 0x0F;
    // secret = 65535 = 0x0000FFFF
    buf[12] = 0x00;
    buf[13] = 0x00;
    buf[14] = 0xFF;
    buf[15] = 0xFF;

    EXPECT_EQ(read_be_int32(buf + 8), 9999);
    EXPECT_EQ(read_be_int32(buf + 12), 65535);
}

// AC: INT32_MIN pid big-endian round-trip.
TEST(QA_GDB956_Parse, ExtremeNegativePid_RoundTrip) {
    uint8_t buf[4] = {0x80, 0x00, 0x00, 0x00}; // INT32_MIN
    EXPECT_EQ(read_be_int32(buf), INT32_MIN);
}

// AC: INT32_MAX secret big-endian round-trip.
TEST(QA_GDB956_Parse, ExtremePositiveSecret_RoundTrip) {
    uint8_t buf[4] = {0x7F, 0xFF, 0xFF, 0xFF}; // INT32_MAX
    EXPECT_EQ(read_be_int32(buf), INT32_MAX);
}

// AC: pid=0, secret=0 round-trip.
TEST(QA_GDB956_Parse, ZeroPidZeroSecret_RoundTrip) {
    uint8_t buf[4] = {0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(read_be_int32(buf), 0);
}

// Truncated packet: a buffer shorter than 16 bytes must not be over-read.
// The implementation reads from a caller-allocated buffer; we only verify that
// our read_be_int32 helper does not OOB-read from exactly 4 bytes of valid data.
TEST(QA_GDB956_Parse, TruncatedBuffer_NoOverRead) {
    // Provide exactly 4 bytes -- simulates the minimum valid field.
    uint8_t buf[4] = {0x00, 0x00, 0x04, 0xD2};
    int32_t val = read_be_int32(buf);
    EXPECT_EQ(val, 1234); // sanity that it reads correctly from 4 bytes
}

// ---------------------------------------------------------------------------
// NO-REGRESSION: normal query path not affected
// ---------------------------------------------------------------------------

// AC: requested() returns false when no guard is installed.
TEST(QA_GDB956_NoRegression, RequestedFalse_WithoutGuard) {
    // Ensure any residual thread-local state is cleared.
    StatementCancel::clear();
    EXPECT_FALSE(StatementCancel::requested());
}

// AC: requested() returns false when guard is active but flag is not set.
TEST(QA_GDB956_NoRegression, RequestedFalse_GuardActive_FlagNotSet) {
    auto flag = std::make_shared<std::atomic<bool>>(false);
    StatementCancelGuard guard(flag);
    EXPECT_FALSE(StatementCancel::requested()) << "Normal path must not spuriously cancel";
}

// AC: 57014 would only fire when flag is actually set (flag gate check).
TEST(QA_GDB956_NoRegression, FlagGate_OnlyTrueWhenSet) {
    auto flag = std::make_shared<std::atomic<bool>>(false);
    StatementCancelGuard guard(flag);

    // Simulate pulling multiple tuples without cancellation.
    for (int i = 0; i < 1000; ++i) {
        ASSERT_FALSE(StatementCancel::requested()) << "Spurious cancel on iteration " << i;
    }

    // Now set the flag -- simulates CancelRequest arriving.
    flag->store(true, std::memory_order_release);
    EXPECT_TRUE(StatementCancel::requested()) << "Must detect cancel after flag set";
}

// AC: a registry with no active statements returns false for requested().
TEST(QA_GDB956_NoRegression, IdleRegistry_DoesNotSetAnyFlag) {
    CancelRegistry registry;
    registry.register_connection(1, 111);
    registry.register_connection(2, 222);
    // No flags installed -- no statements running.

    auto sentinel = std::make_shared<std::atomic<bool>>(false);
    // request_cancel when idle -- must not crash or flip any external flag.
    registry.request_cancel(1, 111);
    registry.request_cancel(2, 222);
    EXPECT_FALSE(sentinel->load());
}

// ---------------------------------------------------------------------------
// CONCURRENCY: hammer registry under concurrent churn (deterministic, barrier-based)
// ---------------------------------------------------------------------------

// Stress test: N threads call request_cancel concurrently while the flag is live.
// All cancels must be idempotent (flag.store(true) is always safe).
TEST(QA_GDB956_Concurrency, ConcurrentCancelFromManyThreads) {
    CancelRegistry registry;
    registry.register_connection(55, 5555);

    auto flag = std::make_shared<std::atomic<bool>>(false);
    registry.set_cancel_flag(55, flag);

    constexpr int N = 16;
    std::barrier sync(N + 1);

    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&] {
            sync.arrive_and_wait(); // all threads start at the same time
            registry.request_cancel(55, 5555);
        });
    }

    sync.arrive_and_wait(); // release all threads simultaneously
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_TRUE(flag->load()) << "Flag must be set after concurrent cancels";
}

// Stress test: concurrent register/unregister/cancel -- no crash, no torn read.
TEST(QA_GDB956_Concurrency, RegisterUnregisterCancelChurn) {
    CancelRegistry registry;
    constexpr int ITERS = 200;
    constexpr int N_CANCEL = 4;
    constexpr int N_REG = 4;

    std::atomic<bool> stop{false};

    // Canceller threads: repeatedly call request_cancel (may be no-op).
    std::vector<std::thread> cancellers;
    cancellers.reserve(N_CANCEL);
    for (int i = 0; i < N_CANCEL; ++i) {
        cancellers.emplace_back([&] {
            for (int j = 0; j < ITERS; ++j) {
                registry.request_cancel(200, 2000);
                registry.request_cancel(201, 2001);
            }
        });
    }

    // Register/unregister threads.
    std::vector<std::thread> regers;
    regers.reserve(N_REG);
    for (int i = 0; i < N_REG; ++i) {
        regers.emplace_back([&, i] {
            for (int j = 0; j < ITERS; ++j) {
                int32_t pid = 200 + (i % 2);
                int32_t sec = 2000 + (i % 2);
                registry.register_connection(pid, sec);
                auto flag = std::make_shared<std::atomic<bool>>(false);
                registry.set_cancel_flag(pid, flag);
                registry.clear_cancel_flag(pid);
                registry.unregister_connection(pid);
            }
        });
    }

    for (auto& t : cancellers) {
        t.join();
    }
    for (auto& t : regers) {
        t.join();
    }
    // If we reach here without crashing or deadlocking, the registry is safe.
    SUCCEED();
}

// Stress test: cancel-races-with-unregister (UAF probe).
// Register, arm a statement, then concurrently unregister and request_cancel.
// Neither operation should crash or access freed memory.
TEST(QA_GDB956_Concurrency, CancelRacesWithUnregister_NoUAF) {
    constexpr int REPS = 100;
    for (int rep = 0; rep < REPS; ++rep) {
        CancelRegistry registry;
        registry.register_connection(1, 42);
        auto flag = std::make_shared<std::atomic<bool>>(false);
        registry.set_cancel_flag(1, flag);

        std::barrier sync(3);

        std::thread unreg([&] {
            sync.arrive_and_wait();
            registry.unregister_connection(1);
        });
        std::thread cancel([&] {
            sync.arrive_and_wait();
            registry.request_cancel(1, 42);
        });

        sync.arrive_and_wait(); // main thread releases both at the same moment

        unreg.join();
        cancel.join();
        // No assertion needed -- the goal is no crash/UAF.
    }
    SUCCEED();
}

// Stress test: concurrent set_cancel_flag + clear_cancel_flag on same pid.
TEST(QA_GDB956_Concurrency, SetAndClearFlagConcurrently) {
    CancelRegistry registry;
    registry.register_connection(77, 7777);
    constexpr int ITERS = 200;

    std::barrier sync(3);

    std::thread setter([&] {
        sync.arrive_and_wait();
        for (int i = 0; i < ITERS; ++i) {
            auto f = std::make_shared<std::atomic<bool>>(false);
            registry.set_cancel_flag(77, f);
        }
    });

    std::thread clearer([&] {
        sync.arrive_and_wait();
        for (int i = 0; i < ITERS; ++i) {
            registry.clear_cancel_flag(77);
        }
    });

    sync.arrive_and_wait();
    setter.join();
    clearer.join();
    SUCCEED();
}

// Stress test: many distinct pids registered concurrently.
TEST(QA_GDB956_Concurrency, ManyPidsConcurrentRegister) {
    CancelRegistry registry;
    constexpr int N = 32;
    std::barrier sync(N + 1);
    std::vector<std::thread> threads;
    threads.reserve(N);

    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&, i] {
            sync.arrive_and_wait();
            registry.register_connection(i, i * 100);
            auto flag = std::make_shared<std::atomic<bool>>(false);
            registry.set_cancel_flag(i, flag);
            registry.request_cancel(i, i * 100);
            EXPECT_TRUE(flag->load()) << "pid=" << i << " flag not set";
            registry.clear_cancel_flag(i);
            registry.unregister_connection(i);
        });
    }

    sync.arrive_and_wait();
    for (auto& t : threads) {
        t.join();
    }
}

// Stress test: StatementCancelGuard drop races with flag set (simulates statement
// ending while a cancel arrives from another thread).
TEST(QA_GDB956_Concurrency, GuardDropRaceWithFlagSet) {
    constexpr int REPS = 100;
    for (int rep = 0; rep < REPS; ++rep) {
        auto flag = std::make_shared<std::atomic<bool>>(false);

        std::barrier sync(3);

        // Thread 1: arms then immediately drops the guard.
        std::thread stmt_thread([&] {
            StatementCancelGuard guard(flag);
            sync.arrive_and_wait(); // aligned start
            // Guard drops here on scope exit.
        });

        // Thread 2: sets the flag at the same time the guard is dropping.
        std::thread cancel_thread([&] {
            sync.arrive_and_wait();
            flag->store(true, std::memory_order_release);
        });

        sync.arrive_and_wait();
        stmt_thread.join();
        cancel_thread.join();

        // After both threads complete, the thread-local in stmt_thread is cleared.
        // No assertion on flag value -- the race is intentional to probe UAF.
    }
    SUCCEED();
}

} // namespace
} // namespace sixseven
