#include "sixseven/server/thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace sixseven {

TEST(ThreadPool, ConstructAndDestruct) {
    ThreadPool pool(2);
    EXPECT_TRUE(pool.is_running());
    EXPECT_EQ(pool.num_workers(), 2u);
}

TEST(ThreadPool, SubmitAndExecuteTask) {
    ThreadPool pool(2);

    std::atomic<int> counter{0};
    pool.submit([&counter] { counter.fetch_add(1); });

    // Wait for the task to complete.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (counter.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(counter.load(), 1);
}

TEST(ThreadPool, MultipleConcurrentTasks) {
    ThreadPool pool(4);

    constexpr int NUM_TASKS = 100;
    std::atomic<int> counter{0};
    for (int i = 0; i < NUM_TASKS; ++i) {
        ASSERT_TRUE(pool.submit([&counter] { counter.fetch_add(1); }));
    }

    pool.shutdown();
    EXPECT_EQ(counter.load(), NUM_TASKS);
}

TEST(ThreadPool, ShutdownDrainsPendingTasks) {
    ThreadPool pool(1);

    std::atomic<int> counter{0};
    // Submit tasks that take some time.
    for (int i = 0; i < 5; ++i) {
        pool.submit([&counter] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            counter.fetch_add(1);
        });
    }

    pool.shutdown();
    // All tasks should have completed.
    EXPECT_EQ(counter.load(), 5);
}

TEST(ThreadPool, SubmitAfterShutdownReturnsFalse) {
    ThreadPool pool(1);
    pool.shutdown();
    EXPECT_FALSE(pool.is_running());
    EXPECT_FALSE(pool.submit([] {}));
}

TEST(ThreadPool, DoubleShutdownIsSafe) {
    ThreadPool pool(2);
    pool.shutdown();
    pool.shutdown(); // Should not hang or crash.
    EXPECT_FALSE(pool.is_running());
}

TEST(ThreadPool, PendingTasksCount) {
    // Use a pool with 0 initial tasks to observe the queue.
    ThreadPool pool(1);

    // Block the single worker.
    std::atomic<bool> gate{false};
    pool.submit([&gate] {
        while (!gate.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // Give the worker time to pick up the blocking task.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Now queue more tasks — they should pile up.
    pool.submit([] {});
    pool.submit([] {});
    EXPECT_GE(pool.pending_tasks(), 1u); // At least one should be queued.

    gate.store(true);
    pool.shutdown();
}

// --- Bounded shutdown(deadline) tests (GDB-1279) -----------------------
//
// Contract under test: finish-then-hard-cap. In-flight/queued work may
// finish, but only up to `deadline`; shutdown(deadline) must return at
// approximately the deadline (not wait for tasks to naturally complete),
// and a smaller deadline must return sooner than a larger one, for tasks
// that intentionally outlive the deadline.

TEST(ThreadPool, BoundedShutdownReturnsAtApproxDeadlineForHungTask) {
    ThreadPool pool(1);

    // Task sleeps far longer than the shutdown deadline below, so
    // shutdown(deadline) must force-abandon it rather than wait it out.
    pool.submit([] { std::this_thread::sleep_for(std::chrono::seconds(30)); });

    // Let the worker pick up the task before we shut down.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto start = std::chrono::steady_clock::now();
    pool.shutdown(std::chrono::seconds(1));
    auto elapsed = std::chrono::steady_clock::now() - start;

    // Generous tolerance for a loaded CI/dev box: must return well before
    // the task's 30s natural completion, and within a few seconds of the
    // 1s deadline.
    EXPECT_LT(elapsed, std::chrono::seconds(10));
    EXPECT_FALSE(pool.is_running());
}

TEST(ThreadPool, BoundedShutdownProportionalToDeadline) {
    // A 1s deadline shutdown must return meaningfully sooner than a 5s
    // deadline shutdown, for a task that outlives both. This checks that
    // the deadline is actually being honored (not e.g. always waiting for
    // some fixed unrelated duration, or always returning instantly).
    auto run_with_deadline = [](std::chrono::seconds deadline) {
        ThreadPool pool(1);
        pool.submit([] { std::this_thread::sleep_for(std::chrono::seconds(30)); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto start = std::chrono::steady_clock::now();
        pool.shutdown(deadline);
        return std::chrono::steady_clock::now() - start;
    };

    auto elapsed_short = run_with_deadline(std::chrono::seconds(1));
    auto elapsed_long = run_with_deadline(std::chrono::seconds(4));

    EXPECT_LT(elapsed_short, elapsed_long);
    // The short-deadline run should complete well under the long deadline.
    EXPECT_LT(elapsed_short, std::chrono::seconds(3));
}

TEST(ThreadPool, BoundedShutdownLetsQuickTaskFinishWithinDeadline) {
    ThreadPool pool(1);

    std::atomic<int> counter{0};
    pool.submit([&counter] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        counter.fetch_add(1);
    });

    // Deadline comfortably longer than the task's natural runtime: the
    // finish-then-hard-cap contract says in-flight work gets to finish.
    pool.shutdown(std::chrono::seconds(5));

    EXPECT_EQ(counter.load(), 1);
    EXPECT_FALSE(pool.is_running());
}

TEST(ThreadPool, BoundedShutdownAbandonsQueuedTasksPastDeadline) {
    ThreadPool pool(1);

    std::atomic<int> counter{0};
    // First task blocks the single worker past the deadline.
    pool.submit([&counter] {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        counter.fetch_add(1);
    });
    // These never get a worker before the deadline elapses, so they must be
    // abandoned rather than executed.
    pool.submit([&counter] { counter.fetch_add(1); });
    pool.submit([&counter] { counter.fetch_add(1); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pool.shutdown(std::chrono::seconds(1));

    // The blocked task's increment never lands (thread was force-abandoned
    // before it could finish); the two queued increments never ran either.
    EXPECT_EQ(counter.load(), 0);
}

TEST(ThreadPool, BoundedShutdownZeroDeadlineIsImmediateHardCap) {
    ThreadPool pool(1);

    std::atomic<int> counter{0};
    pool.submit([&counter] {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        counter.fetch_add(1);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto start = std::chrono::steady_clock::now();
    pool.shutdown(std::chrono::seconds(0));
    auto elapsed = std::chrono::steady_clock::now() - start;

    // 0s must mean "don't wait at all", not "unbounded" -- returns almost
    // immediately, well under the task's 30s runtime.
    EXPECT_LT(elapsed, std::chrono::seconds(5));
    EXPECT_EQ(counter.load(), 0);
}

TEST(ThreadPool, BoundedDoubleShutdownIsSafe) {
    ThreadPool pool(2);
    pool.shutdown(std::chrono::seconds(1));
    pool.shutdown(std::chrono::seconds(1)); // Second call must be a no-op.
    EXPECT_FALSE(pool.is_running());
}

} // namespace sixseven
