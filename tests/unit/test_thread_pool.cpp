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

} // namespace sixseven
