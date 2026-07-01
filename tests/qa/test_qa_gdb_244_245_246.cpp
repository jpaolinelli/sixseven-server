/// @file test_qa_gdb_244_245_246.cpp
/// @brief QA adversarial tests for bug fixes GDB-244, GDB-245, GDB-246.
///
/// GDB-244: HashIndex pathological hash collision causing unbounded directory growth.
/// GDB-245: ThreadPool unhandled task exception calls std::terminate.
/// GDB-246: OpenAIProvider uncaught nlohmann::json exception on non-numeric embedding values.

#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/index/btree_key.h"
#include "sixseven/index/hash_index.h"
#include "sixseven/index/rid.h"
#include "sixseven/server/thread_pool.h"
#include "sixseven/vector/openai_provider.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "openai_mock_http_client.h"

using namespace sixseven;

// =============================================================================
// Helpers
// =============================================================================

namespace {

HashIndex make_index(uint32_t capacity = 4,
                     bool unique = false,
                     std::vector<TypeId> types = {TypeId::INT64}) {
    HashIndexConfig cfg;
    cfg.key_types = std::move(types);
    cfg.bucket_capacity = capacity;
    cfg.is_unique = unique;
    return HashIndex(std::move(cfg));
}

KeyType key(int64_t v) {
    return {Value(v)};
}
RID rid(uint32_t p, uint16_t s = 0) {
    return {p, s};
}

// MockHttpClient is provided by openai_mock_http_client.h (GDB-1154).
using sixseven::MockHttpClient;

} // namespace

// =============================================================================
// GDB-244: HashIndex pathological hash collision
// =============================================================================

// Verify global_depth stays bounded when inserting many identical keys.
TEST(QA_GDB_244, GlobalDepthBoundedWithIdenticalKeys) {
    auto idx = make_index(4);

    for (int i = 0; i < 200; ++i) {
        auto r = idx.insert(key(42), rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value()) << "insert i=" << i << ": " << r.error().message;
    }

    EXPECT_EQ(idx.size(), 200u);
    // Global depth should not have grown unboundedly. With a single hash value,
    // at most one split should occur (from depth 0 to depth 1) before the
    // all_same_hash guard kicks in.
    EXPECT_LE(idx.global_depth(), 1u);
}

// Bucket capacity of 1 with all same keys: extreme stress on the guard.
TEST(QA_GDB_244, BucketCapacity1AllSameKeys) {
    auto idx = make_index(1);

    for (int i = 0; i < 50; ++i) {
        auto r = idx.insert(key(99), rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value()) << "insert i=" << i << ": " << r.error().message;
    }

    EXPECT_EQ(idx.size(), 50u);
    EXPECT_LE(idx.global_depth(), 1u);

    auto all = idx.search_all(key(99));
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->size(), 50u);
}

// Mix of colliding and non-colliding keys. Non-colliding keys should
// still split normally while the colliding group stays in one bucket.
TEST(QA_GDB_244, MixedCollidingAndNonCollidingKeys) {
    auto idx = make_index(4);

    // Insert 20 copies of key=42 (all collide).
    for (int i = 0; i < 20; ++i) {
        auto r = idx.insert(key(42), rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value()) << "colliding insert i=" << i << ": " << r.error().message;
    }

    // Insert 20 distinct keys (should trigger normal splits).
    for (int i = 1000; i < 1020; ++i) {
        auto r = idx.insert(key(i), rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value()) << "distinct insert i=" << i << ": " << r.error().message;
    }

    EXPECT_EQ(idx.size(), 40u);

    // All colliding keys still searchable.
    auto colliding = idx.search_all(key(42));
    ASSERT_TRUE(colliding.has_value());
    EXPECT_EQ(colliding->size(), 20u);

    // All distinct keys still searchable.
    for (int i = 1000; i < 1020; ++i) {
        auto r = idx.search(key(i));
        ASSERT_TRUE(r.has_value());
        ASSERT_TRUE(r->has_value()) << "missing key " << i;
    }
}

// Insert colliding keys, remove some, verify size and searchability.
TEST(QA_GDB_244, RemoveFromOverCapacityBucket) {
    auto idx = make_index(4);

    for (int i = 0; i < 30; ++i) {
        ASSERT_TRUE(idx.insert(key(7), rid(static_cast<uint32_t>(i))).has_value());
    }
    EXPECT_EQ(idx.size(), 30u);

    // Remove 10 entries by (key, rid).
    for (int i = 0; i < 10; ++i) {
        auto r = idx.remove(key(7), rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(*r) << "failed to remove rid=" << i;
    }
    EXPECT_EQ(idx.size(), 20u);

    auto remaining = idx.search_all(key(7));
    ASSERT_TRUE(remaining.has_value());
    EXPECT_EQ(remaining->size(), 20u);
}

// Two different groups of colliding keys.
TEST(QA_GDB_244, TwoDifferentCollidingKeyGroups) {
    auto idx = make_index(4);

    // 30 copies of key=1.
    for (int i = 0; i < 30; ++i) {
        ASSERT_TRUE(idx.insert(key(1), rid(static_cast<uint32_t>(i))).has_value());
    }

    // 30 copies of key=2.
    for (int i = 0; i < 30; ++i) {
        ASSERT_TRUE(idx.insert(key(2), rid(static_cast<uint32_t>(100 + i))).has_value());
    }

    EXPECT_EQ(idx.size(), 60u);

    auto group1 = idx.search_all(key(1));
    ASSERT_TRUE(group1.has_value());
    EXPECT_EQ(group1->size(), 30u);

    auto group2 = idx.search_all(key(2));
    ASSERT_TRUE(group2.has_value());
    EXPECT_EQ(group2->size(), 30u);
}

// Concurrent inserts of the same key from multiple threads.
TEST(QA_GDB_244, ConcurrentSameKeyInserts) {
    auto idx = make_index(4);
    constexpr int per_thread = 50;
    constexpr int num_threads = 4;

    std::vector<std::thread> threads;
    std::atomic<int> errors{0};
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&idx, &errors, t]() {
            for (int i = 0; i < per_thread; ++i) {
                auto r = idx.insert(key(42), rid(static_cast<uint32_t>(t * per_thread + i)));
                if (!r.has_value()) {
                    errors.fetch_add(1);
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(errors.load(), 0);
    EXPECT_EQ(idx.size(), static_cast<uint64_t>(per_thread * num_threads));

    auto all = idx.search_all(key(42));
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->size(), static_cast<size_t>(per_thread * num_threads));
}

// Insert colliding keys, then insert non-colliding keys that trigger splits,
// then remove all colliding keys. Directory should still work.
TEST(QA_GDB_244, RemoveAllCollidingKeysAfterSplits) {
    auto idx = make_index(4);

    // Insert 20 colliding keys.
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(idx.insert(key(42), rid(static_cast<uint32_t>(i))).has_value());
    }

    // Insert 20 distinct keys.
    for (int i = 1000; i < 1020; ++i) {
        ASSERT_TRUE(idx.insert(key(i), rid(static_cast<uint32_t>(i))).has_value());
    }

    // Remove all colliding keys.
    for (int i = 0; i < 20; ++i) {
        auto r = idx.remove(key(42), rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(*r);
    }

    EXPECT_EQ(idx.size(), 20u);

    // No colliding keys remain.
    auto gone = idx.search_all(key(42));
    ASSERT_TRUE(gone.has_value());
    EXPECT_EQ(gone->size(), 0u);

    // Distinct keys still present.
    for (int i = 1000; i < 1020; ++i) {
        auto r = idx.search(key(i));
        ASSERT_TRUE(r.has_value());
        ASSERT_TRUE(r->has_value()) << "missing key " << i;
    }
}

// =============================================================================
// GDB-245: ThreadPool unhandled task exception
// =============================================================================

// Multiple throwing tasks in sequence -- pool stays alive.
TEST(QA_GDB_245, MultipleThrowingTasksPoolSurvives) {
    ThreadPool pool(2);
    std::atomic<int> counter{0};

    // Submit 10 throwing tasks.
    for (int i = 0; i < 10; ++i) {
        pool.submit([] { throw std::runtime_error("bang"); });
    }

    // Wait for throwing tasks to be consumed.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Submit 10 normal tasks after.
    for (int i = 0; i < 10; ++i) {
        pool.submit([&counter] { counter.fetch_add(1); });
    }

    pool.shutdown();
    EXPECT_EQ(counter.load(), 10);
}

// Interleaved throwing and normal tasks.
TEST(QA_GDB_245, InterleavedThrowingAndNormalTasks) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};

    for (int i = 0; i < 20; ++i) {
        if (i % 3 == 0) {
            pool.submit([] { throw std::logic_error("logic error"); });
        } else {
            pool.submit([&counter] { counter.fetch_add(1); });
        }
    }

    pool.shutdown();
    // 7 tasks throw (i=0,3,6,9,12,15,18), 13 tasks increment.
    EXPECT_EQ(counter.load(), 13);
}

// Throw non-std::exception type (int) -- caught by catch(...).
TEST(QA_GDB_245, ThrowNonStdExceptionType) {
    ThreadPool pool(2);
    std::atomic<int> counter{0};

    pool.submit([] { throw 42; });         // Not a std::exception.
    pool.submit([] { throw "c-string"; }); // Also not std::exception.

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    pool.submit([&counter] { counter.fetch_add(1); });
    pool.shutdown();
    EXPECT_EQ(counter.load(), 1);
}

// All workers hit exceptions simultaneously.
TEST(QA_GDB_245, AllWorkersHitExceptions) {
    constexpr int num_workers = 4;
    ThreadPool pool(num_workers);
    std::atomic<int> counter{0};

    // Submit exactly num_workers blocking-then-throwing tasks.
    std::atomic<int> gate{0};
    for (int i = 0; i < num_workers; ++i) {
        pool.submit([&gate] {
            gate.fetch_add(1);
            while (gate.load() < num_workers) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            throw std::runtime_error("all-at-once");
        });
    }

    // Wait for all to throw.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Workers should still be alive.
    for (int i = 0; i < 20; ++i) {
        pool.submit([&counter] { counter.fetch_add(1); });
    }

    pool.shutdown();
    EXPECT_EQ(counter.load(), 20);
}

// Submit after shutdown returns false.
TEST(QA_GDB_245, SubmitAfterShutdownReturnsFalse) {
    ThreadPool pool(2);
    pool.shutdown();

    bool accepted = pool.submit([] {});
    EXPECT_FALSE(accepted);
}

// Heavy load after exception.
TEST(QA_GDB_245, HeavyLoadAfterException) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};

    pool.submit([] { throw std::runtime_error("early failure"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    constexpr int N = 500;
    for (int i = 0; i < N; ++i) {
        pool.submit([&counter] { counter.fetch_add(1); });
    }

    pool.shutdown();
    EXPECT_EQ(counter.load(), N);
}

// Double shutdown is safe.
TEST(QA_GDB_245, DoubleShutdownIsSafe) {
    ThreadPool pool(2);
    std::atomic<int> counter{0};

    pool.submit([&counter] { counter.fetch_add(1); });
    pool.shutdown();
    pool.shutdown(); // Second shutdown should be a no-op.

    EXPECT_EQ(counter.load(), 1);
}

// =============================================================================
// GDB-246: OpenAIProvider non-numeric embedding values
// =============================================================================
// Canonical coverage lives in QA_GDB_242 (test_qa_gdb_242.cpp). Unique
// scenarios (EmptyStringValue, HealthCheckWithNonNumericEmbedding) were moved
// to QA_GDB_242 during GDB-1154 de-duplication. One representative regression
// test is retained here to tie ticket GDB-246 to the behavior.

// GDB-246 regression marker: non-numeric string value returns PARSE_ERROR.
// (Equivalent scenario covered by QA_GDB_242.EmbeddingWithStringValue.)
TEST(QA_GDB_246, NonNumericStringValueRegressionGDB246) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": [{"embedding": ["bad", 0.2, 0.3], "index": 0}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}
