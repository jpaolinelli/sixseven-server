/// @file test_qa_gdb_244_245_246.cpp
/// @brief QA adversarial tests for bug fixes GDB-244, GDB-245, GDB-246.
///
/// GDB-244: HashIndex pathological hash collision causing unbounded directory growth.
/// GDB-245: ThreadPool unhandled task exception calls std::terminate.
/// GDB-246: OpenAIProvider uncaught nlohmann::json exception on non-numeric embedding values.

#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/index/btree_key.h"
#include "giodb/index/hash_index.h"
#include "giodb/index/rid.h"
#include "giodb/server/thread_pool.h"
#include "giodb/vector/http_client.h"
#include "giodb/vector/openai_provider.h"

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

using namespace giodb;

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

class MockHttpClient : public HttpClient {
public:
    void set_post_response(int status, const std::string& body) {
        post_status_ = status;
        post_body_ = body;
    }

    Result<HttpResponse>
    post(const std::string& /*url*/,
         const std::string& /*body*/,
         const std::vector<std::pair<std::string, std::string>>& /*headers*/) override {
        HttpResponse r;
        r.status_code = post_status_;
        r.body = post_body_;
        r.content_type = "application/json";
        return ok(std::move(r));
    }

    Result<HttpResponse>
    get(const std::string& /*url*/,
        const std::vector<std::pair<std::string, std::string>>& /*headers*/) override {
        HttpResponse r;
        r.status_code = 200;
        r.body = "{}";
        r.content_type = "application/json";
        return ok(std::move(r));
    }

private:
    int post_status_ = 200;
    std::string post_body_;
};

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

// Array value in embedding array.
TEST(QA_GDB_246, NestedArrayValue) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200,
                            R"({"data": [{"embedding": [0.1, [0.2, 0.3], 0.4], "index": 0}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    EXPECT_NE(result.error().message.find("non-numeric"), std::string::npos);
}

// Non-numeric at first position.
TEST(QA_GDB_246, NonNumericAtFirstPosition) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": [{"embedding": ["bad", 0.2, 0.3], "index": 0}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    EXPECT_NE(result.error().message.find("index 0"), std::string::npos);
}

// Non-numeric at last position.
TEST(QA_GDB_246, NonNumericAtLastPosition) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": [{"embedding": [0.1, 0.2, null], "index": 0}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    EXPECT_NE(result.error().message.find("index 2"), std::string::npos);
}

// All non-numeric values.
TEST(QA_GDB_246, AllNonNumericValues) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": [{"embedding": ["a", "b", "c"], "index": 0}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
    EXPECT_NE(result.error().message.find("index 0"), std::string::npos);
}

// Valid integer embedding values accepted.
TEST(QA_GDB_246, ValidIntegerValuesAccepted) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": [{"embedding": [1, 2, 3], "index": 0}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 3u);
    EXPECT_FLOAT_EQ((*result)[0], 1.0F);
    EXPECT_FLOAT_EQ((*result)[1], 2.0F);
    EXPECT_FLOAT_EQ((*result)[2], 3.0F);
}

// Valid negative float values accepted.
TEST(QA_GDB_246, ValidNegativeFloatsAccepted) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": [{"embedding": [-0.5, -1.0, -0.001], "index": 0}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 3u);
    EXPECT_FLOAT_EQ((*result)[0], -0.5F);
}

// embed_batch with non-numeric in one embedding.
TEST(QA_GDB_246, BatchEmbedWithNonNumeric) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(
        200,
        R"({"data": [{"embedding": [0.1, 0.2, 0.3], "index": 0}, {"embedding": [0.4, true, 0.6], "index": 1}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed_batch({"hello", "world"});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// Boolean false value (would be silently coerced to 0.0 without the fix).
TEST(QA_GDB_246, BooleanFalseValue) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": [{"embedding": [0.1, false, 0.3], "index": 0}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// Empty string value.
TEST(QA_GDB_246, EmptyStringValue) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": [{"embedding": [0.1, "", 0.3], "index": 0}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.embed("hello");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// health_check path also goes through request_embeddings validation.
TEST(QA_GDB_246, HealthCheckWithNonNumericEmbedding) {
    auto mock = std::make_unique<MockHttpClient>();
    mock->set_post_response(200, R"({"data": [{"embedding": [0.1, "bad", 0.3], "index": 0}]})");

    OpenAIProvider provider("sk-test", "test", 3, std::move(mock));
    auto result = provider.health_check();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}
