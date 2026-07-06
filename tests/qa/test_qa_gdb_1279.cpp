// QA adversarial tests for GDB-1279: bounded ThreadPool::shutdown(deadline)
// and Server::do_shutdown() enforcement of shutdown_timeout_s.
//
// Contract under test (finish-then-hard-cap):
//   - In-flight/queued work is allowed to finish up to the deadline.
//   - At the deadline, remaining queued tasks are dropped and still-running
//     workers are force-abandoned (detached) rather than joined.
//   - shutdown(deadline) returns ~promptly at the deadline, never hangs.
//   - A prior UAF (detached worker touching freed ThreadPool state) was
//     fixed via a CAS handshake (ThreadPoolWorkerControl) between the
//     worker and the shutdown() caller.
//
// NOTE: ThreadPool::shutdown(std::chrono::seconds) only accepts whole-second
// deadlines (no implicit conversion from milliseconds), so all deadlines and
// task durations below are scaled to whole seconds even though that makes
// individual tests slower.
//
// These tests specifically hammer the CAS race and lifetime story with many
// workers, many iterations, and boundary-timed tasks, since ASan/TSAN are
// unavailable on this Windows box.

#include "sixseven/server/thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Many workers, all hung past the deadline simultaneously: stress the CAS
// handshake so that (ideally) every worker either wins (kSafeToContinue,
// joined) or loses (kAbandoned, detached) -- never both, never neither.
// ---------------------------------------------------------------------------
TEST(QA_GDB1279_ThreadPool, ManyWorkersAllHungAtDeadlineNoCrashNoHang) {
    constexpr size_t kWorkers = 16;
    constexpr int kIterations = 8;

    for (int iter = 0; iter < kIterations; ++iter) {
        auto pool = std::make_unique<ThreadPool>(kWorkers);
        std::atomic<int> started{0};

        for (size_t i = 0; i < kWorkers; ++i) {
            ASSERT_TRUE(pool->submit([&started] {
                ++started;
                // Long enough to guarantee still running at the 0s deadline.
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }));
        }

        // Wait for all workers to have picked up their task before timing
        // the bounded shutdown, so the deadline race actually engages all
        // of them concurrently rather than racing the queue drain.
        auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (started.load() < static_cast<int>(kWorkers) &&
               std::chrono::steady_clock::now() < wait_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(started.load(), static_cast<int>(kWorkers));

        auto t0 = std::chrono::steady_clock::now();
        pool->shutdown(std::chrono::seconds(0)); // immediate hard cap -- all hung workers abandoned
        auto elapsed = std::chrono::steady_clock::now() - t0;

        // Must return promptly -- not wait out the 300ms sleeps.
        EXPECT_LT(elapsed, std::chrono::milliseconds(250))
            << "iteration " << iter << ": bounded shutdown did not return promptly";

        // Destroying the pool here must not crash or hang even though
        // detached workers may still be sleeping in the background.
        pool.reset();
    }

    // Let any still-sleeping detached workers finish in the background
    // before the test binary exits, so late completions don't race process
    // teardown.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
}

// ---------------------------------------------------------------------------
// Vary task durations so some finish exactly at/near the 1s deadline
// boundary, racing the CAS. The claim to verify: no crash, no torn state,
// and every task either fully completes or is cleanly abandoned -- there is
// no half-executed observable side effect from the CAS handshake itself.
// ---------------------------------------------------------------------------
TEST(QA_GDB1279_ThreadPool, BoundaryTimedTasksRaceTheDeadlineSafely) {
    constexpr size_t kWorkers = 8;
    constexpr int kIterations = 6;

    for (int iter = 0; iter < kIterations; ++iter) {
        auto pool = std::make_unique<ThreadPool>(kWorkers);
        std::atomic<int> completed{0};

        // Deadline is 1s. Stagger task durations around it: some clearly
        // finish before, some clearly after, some right at the boundary.
        const int durations_ms[kWorkers] = {100, 500, 900, 990, 1000, 1010, 1500, 3000};
        for (size_t i = 0; i < kWorkers; ++i) {
            int d = durations_ms[i];
            ASSERT_TRUE(pool->submit([&completed, d] {
                std::this_thread::sleep_for(std::chrono::milliseconds(d));
                ++completed; // Only incremented on genuine completion.
            }));
        }

        auto t0 = std::chrono::steady_clock::now();
        pool->shutdown(std::chrono::seconds(1));
        auto elapsed = std::chrono::steady_clock::now() - t0;

        // Generous tolerance band: shutdown must not hang out to the longest
        // task duration (3s) -- it should return close to the deadline,
        // plus join-fallback slack for workers that won the CAS race.
        EXPECT_LT(elapsed, std::chrono::milliseconds(2000))
            << "iteration " << iter << ": shutdown blocked far past deadline";

        pool.reset();
        // completed is inherently racy (background detached workers may
        // still be running), but reading it must not crash/UB. At least the
        // shortest task (100ms) should have had time to complete.
        EXPECT_GE(completed.load(), 1);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

// ---------------------------------------------------------------------------
// A task that finishes JUST before the deadline must be allowed to complete
// (the "finish-then" half of finish-then-hard-cap) -- not abandoned purely
// because it was close.
// ---------------------------------------------------------------------------
TEST(QA_GDB1279_ThreadPool, TaskFinishingJustBeforeDeadlineCompletes) {
    ThreadPool pool(1);
    std::atomic<bool> ran_to_completion{false};

    ASSERT_TRUE(pool.submit([&ran_to_completion] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ran_to_completion.store(true, std::memory_order_release);
    }));

    // Deadline comfortably after the task's 200ms duration.
    pool.shutdown(std::chrono::seconds(2));

    EXPECT_TRUE(ran_to_completion.load(std::memory_order_acquire));
}

// ---------------------------------------------------------------------------
// Deadline accuracy: smaller timeout returns proportionally sooner, larger
// tolerance band than the dev test to absorb Windows scheduler jitter, but
// still catching a completely-broken (e.g. always-immediate or
// always-unbounded) implementation.
// ---------------------------------------------------------------------------
TEST(QA_GDB1279_ThreadPool, SmallerDeadlineReturnsSoonerThanLargerDeadline) {
    auto run_with_deadline = [](std::chrono::seconds deadline) {
        ThreadPool pool(2);
        for (int i = 0; i < 2; ++i) {
            EXPECT_TRUE(pool.submit([] { std::this_thread::sleep_for(std::chrono::seconds(30)); }));
        }
        auto t0 = std::chrono::steady_clock::now();
        pool.shutdown(deadline);
        auto elapsed = std::chrono::steady_clock::now() - t0;
        return elapsed;
    };

    auto short_elapsed = run_with_deadline(std::chrono::seconds(1));
    auto long_elapsed = run_with_deadline(std::chrono::seconds(3));

    EXPECT_LT(short_elapsed, long_elapsed);
    EXPECT_LT(short_elapsed, std::chrono::milliseconds(2000));
    EXPECT_LT(long_elapsed, std::chrono::milliseconds(4500));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

// ---------------------------------------------------------------------------
// Zero deadline: immediate hard cap, never hangs, under repeated stress.
// This is also the value Server::do_shutdown clamps a negative
// shutdown_timeout_s down to (see test_server_lifecycle.cpp for the
// Server-level clamp coverage); this test guards the ThreadPool side of the
// zero contract under stress.
// ---------------------------------------------------------------------------
TEST(QA_GDB1279_ThreadPool, ZeroDeadlineNeverHangsUnderRepeatedStress) {
    for (int iter = 0; iter < 15; ++iter) {
        ThreadPool pool(4);
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(pool.submit([] { std::this_thread::sleep_for(std::chrono::seconds(10)); }));
        }
        auto t0 = std::chrono::steady_clock::now();
        pool.shutdown(std::chrono::seconds(0));
        auto elapsed = std::chrono::steady_clock::now() - t0;
        EXPECT_LT(elapsed, std::chrono::milliseconds(300))
            << "iteration " << iter << ": zero-deadline shutdown did not return immediately";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

// ---------------------------------------------------------------------------
// Repeated rapid construct/submit-long-work/bounded-shutdown/destroy cycles:
// looks for detached-thread/control-block growth issues or crashes from
// accumulating abandoned workers across many pool generations.
// ---------------------------------------------------------------------------
TEST(QA_GDB1279_ThreadPool, RepeatedConstructShutdownDestroyCyclesDoNotCrash) {
    constexpr int kCycles = 25;
    for (int i = 0; i < kCycles; ++i) {
        ThreadPool pool(4);
        for (int j = 0; j < 4; ++j) {
            ASSERT_TRUE(pool.submit([] { std::this_thread::sleep_for(std::chrono::milliseconds(500)); }));
        }
        pool.shutdown(std::chrono::seconds(0));
        // Pool destructor runs here (unbounded shutdown() again) while some
        // workers from THIS generation may already be detached-and-running.
    }
    // Give detached stragglers from all generations a chance to finish
    // before the test binary exits.
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Double-shutdown after a bounded shutdown already force-abandoned workers:
// a second call to either shutdown() or shutdown(deadline) must be a no-op,
// not attempt a second join/detach against threads already handled.
// ---------------------------------------------------------------------------
TEST(QA_GDB1279_ThreadPool, DoubleShutdownAfterBoundedAbandonmentIsSafe) {
    ThreadPool pool(4);
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(pool.submit([] { std::this_thread::sleep_for(std::chrono::seconds(2)); }));
    }
    pool.shutdown(std::chrono::seconds(0)); // force-abandons all 4
    // Second bounded call.
    EXPECT_NO_FATAL_FAILURE(pool.shutdown(std::chrono::seconds(1)));
    // Third call: unbounded shutdown() (used by destructor) must also be
    // a safe no-op post-shutdown.
    EXPECT_NO_FATAL_FAILURE(pool.shutdown());

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

// ---------------------------------------------------------------------------
// Interaction between the destructor's unbounded shutdown() and having
// already run a bounded shutdown(deadline) manually beforehand -- the
// destructor must not deadlock or double-join.
// ---------------------------------------------------------------------------
TEST(QA_GDB1279_ThreadPool, DestructorAfterManualBoundedShutdownDoesNotDeadlock) {
    auto pool = std::make_unique<ThreadPool>(4);
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(pool->submit([] { std::this_thread::sleep_for(std::chrono::seconds(2)); }));
    }
    pool->shutdown(std::chrono::seconds(0));

    auto t0 = std::chrono::steady_clock::now();
    pool.reset(); // Runs ~ThreadPool -> shutdown() (unbounded).
    auto elapsed = std::chrono::steady_clock::now() - t0;

    // Should be essentially instant: running_ is already false and all
    // workers are already joined or detached.
    EXPECT_LT(elapsed, std::chrono::milliseconds(200));

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
}

// ---------------------------------------------------------------------------
// Large worker count with a short deadline: stresses the sequential
// per-worker join-with-poll loop in shutdown(deadline) -- confirm total
// wall time stays bounded near the deadline rather than accumulating
// per-worker (e.g. N * deadline instead of ~deadline).
// ---------------------------------------------------------------------------
TEST(QA_GDB1279_ThreadPool, TotalShutdownTimeDoesNotScaleWithWorkerCount) {
    constexpr size_t kWorkers = 32;
    ThreadPool pool(kWorkers);
    for (size_t i = 0; i < kWorkers; ++i) {
        ASSERT_TRUE(pool.submit([] { std::this_thread::sleep_for(std::chrono::seconds(10)); }));
    }

    auto t0 = std::chrono::steady_clock::now();
    pool.shutdown(std::chrono::seconds(1));
    auto elapsed = std::chrono::steady_clock::now() - t0;

    // If shutdown accidentally re-applied the deadline per worker instead of
    // treating it as a single absolute deadline_point, 32 workers * 1s
    // would be 32s. Assert it stays close to a single deadline instead.
    EXPECT_LT(elapsed, std::chrono::milliseconds(3000))
        << "shutdown time appears to scale with worker count, not a single deadline";

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
}

} // namespace
} // namespace sixseven
