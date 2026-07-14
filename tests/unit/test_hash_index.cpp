#include "sixseven/index/btree_key.h"
#include "sixseven/index/hash_index.h"
#include "sixseven/index/rid.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace sixseven::test {

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

/// Create a HashIndex with small bucket capacity for testing (forces splits).
inline HashIndex make_test_hash_index(uint32_t bucket_capacity = 4, bool is_unique = false) {
    HashIndexConfig config;
    config.key_types = {TypeId::INT64};
    config.bucket_capacity = bucket_capacity;
    config.is_unique = is_unique;
    return HashIndex(std::move(config));
}

inline KeyType make_key(int64_t v) {
    return {Value(v)};
}

inline KeyType make_string_key(const std::string& s) {
    return {Value(s)};
}

inline KeyType make_composite_key(int64_t v, const std::string& s) {
    return {Value(v), Value(s)};
}

inline RID make_rid(uint32_t page_id, uint16_t slot_id = 0) {
    return {page_id, slot_id};
}

// ---------------------------------------------------------------------------
// Basic insert and lookup tests
// ---------------------------------------------------------------------------

TEST(HashIndexInsert, SingleEntry) {
    auto idx = make_test_hash_index();
    auto result = idx.insert(make_key(42), make_rid(1));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(idx.size(), 1u);
}

TEST(HashIndexInsert, MultipleEntries) {
    auto idx = make_test_hash_index();
    for (int i = 1; i <= 10; ++i) {
        auto result = idx.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(result.has_value()) << "insert failed at i=" << i;
    }
    EXPECT_EQ(idx.size(), 10u);
}

TEST(HashIndexSearch, FindExistingKey) {
    auto idx = make_test_hash_index();
    ASSERT_TRUE(idx.insert(make_key(42), make_rid(1)).has_value());

    auto result = idx.search(make_key(42));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value(), make_rid(1));
}

TEST(HashIndexSearch, KeyNotFound) {
    auto idx = make_test_hash_index();
    ASSERT_TRUE(idx.insert(make_key(42), make_rid(1)).has_value());

    auto result = idx.search(make_key(99));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
}

TEST(HashIndexSearch, EmptyIndex) {
    auto idx = make_test_hash_index();
    auto result = idx.search(make_key(1));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
}

TEST(HashIndexSearch, CorrectRIDReturned) {
    auto idx = make_test_hash_index(64); // Large bucket to avoid splits.
    for (int i = 1; i <= 100; ++i) {
        auto ins =
            idx.insert(make_key(i), make_rid(static_cast<uint32_t>(i), static_cast<uint16_t>(i)));
        ASSERT_TRUE(ins.has_value());
    }

    for (int i = 1; i <= 100; ++i) {
        auto result = idx.search(make_key(i));
        ASSERT_TRUE(result.has_value()) << "search failed for key=" << i;
        ASSERT_TRUE(result->has_value()) << "key not found: " << i;
        EXPECT_EQ(result->value().page_id, static_cast<uint32_t>(i));
        EXPECT_EQ(result->value().slot_id, static_cast<uint16_t>(i));
    }
}

// ---------------------------------------------------------------------------
// SearchAll (non-unique index)
// ---------------------------------------------------------------------------

TEST(HashIndexSearchAll, DuplicateKeys) {
    auto idx = make_test_hash_index(64);
    // Insert the same key with different RIDs.
    ASSERT_TRUE(idx.insert(make_key(42), make_rid(1)).has_value());
    ASSERT_TRUE(idx.insert(make_key(42), make_rid(2)).has_value());
    ASSERT_TRUE(idx.insert(make_key(42), make_rid(3)).has_value());

    auto result = idx.search_all(make_key(42));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 3u);

    std::set<uint32_t> page_ids;
    for (const auto& rid : *result) {
        page_ids.insert(rid.page_id);
    }
    EXPECT_TRUE(page_ids.count(1));
    EXPECT_TRUE(page_ids.count(2));
    EXPECT_TRUE(page_ids.count(3));
}

TEST(HashIndexSearchAll, NoMatches) {
    auto idx = make_test_hash_index();
    ASSERT_TRUE(idx.insert(make_key(42), make_rid(1)).has_value());

    auto result = idx.search_all(make_key(99));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

// ---------------------------------------------------------------------------
// Uniqueness constraint
// ---------------------------------------------------------------------------

TEST(HashIndexUnique, AllowsDistinctKeys) {
    auto idx = make_test_hash_index(64, true);
    ASSERT_TRUE(idx.insert(make_key(1), make_rid(1)).has_value());
    ASSERT_TRUE(idx.insert(make_key(2), make_rid(2)).has_value());
    EXPECT_EQ(idx.size(), 2u);
}

TEST(HashIndexUnique, RejectsDuplicateKey) {
    auto idx = make_test_hash_index(64, true);
    ASSERT_TRUE(idx.insert(make_key(42), make_rid(1)).has_value());

    auto result = idx.insert(make_key(42), make_rid(2));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
    EXPECT_EQ(idx.size(), 1u);
}

// ---------------------------------------------------------------------------
// Delete tests
// ---------------------------------------------------------------------------

TEST(HashIndexDelete, RemoveExistingKey) {
    auto idx = make_test_hash_index();
    ASSERT_TRUE(idx.insert(make_key(42), make_rid(1)).has_value());

    auto result = idx.remove(make_key(42));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(*result);
    EXPECT_EQ(idx.size(), 0u);

    // Verify it's actually gone.
    auto search = idx.search(make_key(42));
    ASSERT_TRUE(search.has_value());
    EXPECT_FALSE(search->has_value());
}

TEST(HashIndexDelete, RemoveNonExistentKey) {
    auto idx = make_test_hash_index();
    ASSERT_TRUE(idx.insert(make_key(42), make_rid(1)).has_value());

    auto result = idx.remove(make_key(99));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(*result);
    EXPECT_EQ(idx.size(), 1u);
}

TEST(HashIndexDelete, RemoveByKeyAndRID) {
    auto idx = make_test_hash_index(64);
    ASSERT_TRUE(idx.insert(make_key(42), make_rid(1)).has_value());
    ASSERT_TRUE(idx.insert(make_key(42), make_rid(2)).has_value());
    ASSERT_TRUE(idx.insert(make_key(42), make_rid(3)).has_value());

    // Remove only the entry with RID (2).
    auto result = idx.remove(make_key(42), make_rid(2));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(*result);
    EXPECT_EQ(idx.size(), 2u);

    // Verify the other two are still there.
    auto remaining = idx.search_all(make_key(42));
    ASSERT_TRUE(remaining.has_value());
    EXPECT_EQ(remaining->size(), 2u);
}

TEST(HashIndexDelete, RemoveByKeyAndRIDNotFound) {
    auto idx = make_test_hash_index(64);
    ASSERT_TRUE(idx.insert(make_key(42), make_rid(1)).has_value());

    auto result = idx.remove(make_key(42), make_rid(99));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(*result);
    EXPECT_EQ(idx.size(), 1u);
}

TEST(HashIndexDelete, RemoveFromEmpty) {
    auto idx = make_test_hash_index();
    auto result = idx.remove(make_key(1));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(*result);
}

// ---------------------------------------------------------------------------
// Bucket split tests
// ---------------------------------------------------------------------------

TEST(HashIndexSplit, BasicSplitOnOverflow) {
    // Bucket capacity 4: inserting 5+ entries should trigger a split.
    auto idx = make_test_hash_index(4);

    for (int i = 1; i <= 10; ++i) {
        auto result = idx.insert(make_key(i * 100), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(result.has_value()) << "insert failed at i=" << i;
    }

    // All entries should still be findable after splits.
    for (int i = 1; i <= 10; ++i) {
        auto result = idx.search(make_key(i * 100));
        ASSERT_TRUE(result.has_value()) << "search failed for key=" << (i * 100);
        ASSERT_TRUE(result->has_value()) << "key not found: " << (i * 100);
        EXPECT_EQ(result->value(), make_rid(static_cast<uint32_t>(i)));
    }

    // Global depth should have increased from 0.
    EXPECT_GT(idx.global_depth(), 0u);
}

TEST(HashIndexSplit, DirectoryDoubling) {
    auto idx = make_test_hash_index(2); // Very small buckets.

    // Insert enough entries to force multiple directory doublings.
    for (int i = 1; i <= 50; ++i) {
        auto result = idx.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(result.has_value()) << "insert failed at i=" << i;
    }

    // Verify all entries survive all the splits.
    for (int i = 1; i <= 50; ++i) {
        auto result = idx.search(make_key(i));
        ASSERT_TRUE(result.has_value()) << "search failed for key=" << i;
        ASSERT_TRUE(result->has_value()) << "key not found: " << i;
        EXPECT_EQ(result->value().page_id, static_cast<uint32_t>(i));
    }

    EXPECT_EQ(idx.size(), 50u);
    EXPECT_GT(idx.bucket_count(), 1u);
}

TEST(HashIndexSplit, ManyEntriesCorrectness) {
    auto idx = make_test_hash_index(8);

    // Insert 1000 entries and verify all can be found.
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i) {
        auto result =
            idx.insert(make_key(static_cast<int64_t>(i)), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(result.has_value()) << "insert failed at i=" << i;
    }

    EXPECT_EQ(idx.size(), static_cast<uint64_t>(N));

    for (int i = 0; i < N; ++i) {
        auto result = idx.search(make_key(static_cast<int64_t>(i)));
        ASSERT_TRUE(result.has_value()) << "search failed for key=" << i;
        ASSERT_TRUE(result->has_value()) << "key not found: " << i;
        EXPECT_EQ(result->value().page_id, static_cast<uint32_t>(i));
    }
}

// GDB-1305: Regression for a hash-collision bug where every small-integer
// key hashed to an identical low-bit pattern (ValueHash for numeric types
// delegates to std::hash<double>, which on libc++ is a raw bit-cast with no
// mixing; small integer-valued doubles all have zero low mantissa bits, so
// hash_key()'s single-step boost::hash_combine from a zero seed produced the
// SAME low 32 bits for every such key). directory_index() reads the low bits
// of the hash to route entries to a bucket, so a bucket could never actually
// be separated by splitting: every split moved the whole bucket verbatim to
// one side, local/global depth grew by one on every subsequent insert, and
// the directory doubled without bound -- exhausting memory or crashing.
// Before the fix this test would hang/OOM; after the fix it must complete
// quickly with a bounded global depth and every key still findable.
TEST(HashIndexSplit, SmallIntegerKeysSplitWithBoundedDepth) {
    auto idx = make_test_hash_index(4); // Small bucket capacity forces many splits.

    constexpr int N = 300;
    for (int i = 1; i <= N; ++i) {
        auto result = idx.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(result.has_value()) << "insert failed at i=" << i;
    }

    EXPECT_EQ(idx.size(), static_cast<uint64_t>(N));

    // The pathological bug caused global_depth_ (and the 2^depth directory)
    // to grow by one on every single insert past the first overflow, i.e.
    // it would reach into the hundreds here. A healthy extendible hash over
    // 300 well-distributed keys with bucket_capacity=4 should need only a
    // handful of bits (roughly log2(300/4) =~ 7) to separate them.
    EXPECT_LT(idx.global_depth(), 20u)
        << "global depth grew unboundedly -- bucket splits are not separating entries "
           "(low-order hash bits are colliding across distinct keys)";
    EXPECT_GT(idx.bucket_count(), 1u);

    for (int i = 1; i <= N; ++i) {
        auto result = idx.search(make_key(i));
        ASSERT_TRUE(result.has_value()) << "search failed for key=" << i;
        ASSERT_TRUE(result->has_value()) << "key not found: " << i;
        EXPECT_EQ(result->value().page_id, static_cast<uint32_t>(i));
    }
}

// GDB-1305: Same collision hazard, but for a single-column composite key
// (KeyType with exactly one Value) -- the exact shape exercised by
// HashIndexSearch.CorrectRIDReturned, which crashed deterministically before
// the fix once the single bucket overflowed its (generously large) capacity.
TEST(HashIndexSplit, LargeCapacityStillSplitsOnOverflow) {
    auto idx = make_test_hash_index(64);

    constexpr int N = 200; // Comfortably exceeds capacity to force a split.
    for (int i = 1; i <= N; ++i) {
        auto result =
            idx.insert(make_key(i), make_rid(static_cast<uint32_t>(i), static_cast<uint16_t>(i)));
        ASSERT_TRUE(result.has_value()) << "insert failed at i=" << i;
    }

    EXPECT_GT(idx.global_depth(), 0u);
    EXPECT_LT(idx.global_depth(), 20u);

    for (int i = 1; i <= N; ++i) {
        auto result = idx.search(make_key(i));
        ASSERT_TRUE(result.has_value()) << "search failed for key=" << i;
        ASSERT_TRUE(result->has_value()) << "key not found: " << i;
        EXPECT_EQ(result->value().page_id, static_cast<uint32_t>(i));
        EXPECT_EQ(result->value().slot_id, static_cast<uint16_t>(i));
    }
}

// ---------------------------------------------------------------------------
// Composite key tests
// ---------------------------------------------------------------------------

TEST(HashIndexComposite, InsertAndSearch) {
    HashIndexConfig config;
    config.key_types = {TypeId::INT64, TypeId::STRING};
    config.bucket_capacity = 8;
    auto idx = HashIndex(std::move(config));

    ASSERT_TRUE(idx.insert(make_composite_key(1, "alice"), make_rid(1)).has_value());
    ASSERT_TRUE(idx.insert(make_composite_key(2, "bob"), make_rid(2)).has_value());
    ASSERT_TRUE(idx.insert(make_composite_key(1, "charlie"), make_rid(3)).has_value());

    auto r1 = idx.search(make_composite_key(1, "alice"));
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r1->has_value());
    EXPECT_EQ(r1->value(), make_rid(1));

    auto r2 = idx.search(make_composite_key(2, "bob"));
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r2->has_value());
    EXPECT_EQ(r2->value(), make_rid(2));

    // Partial key should not match.
    auto r3 = idx.search(make_composite_key(1, "bob"));
    ASSERT_TRUE(r3.has_value());
    EXPECT_FALSE(r3->has_value());
}

// ---------------------------------------------------------------------------
// String key tests
// ---------------------------------------------------------------------------

TEST(HashIndexString, InsertAndSearch) {
    HashIndexConfig config;
    config.key_types = {TypeId::STRING};
    config.bucket_capacity = 8;
    auto idx = HashIndex(std::move(config));

    ASSERT_TRUE(idx.insert(make_string_key("hello"), make_rid(1)).has_value());
    ASSERT_TRUE(idx.insert(make_string_key("world"), make_rid(2)).has_value());
    ASSERT_TRUE(idx.insert(make_string_key(""), make_rid(3)).has_value());

    auto r1 = idx.search(make_string_key("hello"));
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r1->has_value());
    EXPECT_EQ(r1->value(), make_rid(1));

    auto r_empty = idx.search(make_string_key(""));
    ASSERT_TRUE(r_empty.has_value());
    ASSERT_TRUE(r_empty->has_value());
    EXPECT_EQ(r_empty->value(), make_rid(3));

    auto r_miss = idx.search(make_string_key("missing"));
    ASSERT_TRUE(r_miss.has_value());
    EXPECT_FALSE(r_miss->has_value());
}

// ---------------------------------------------------------------------------
// NULL value tests
// ---------------------------------------------------------------------------

TEST(HashIndexNull, NullKeyInsertAndSearch) {
    auto idx = make_test_hash_index(64);

    // Insert a NULL key.
    KeyType null_key = {Value::make_null()};
    auto ins = idx.insert(null_key, make_rid(1));
    ASSERT_TRUE(ins.has_value());

    // Search for it.
    auto result = idx.search(null_key);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value(), make_rid(1));
}

// ---------------------------------------------------------------------------
// Empty/boundary tests
// ---------------------------------------------------------------------------

TEST(HashIndexBoundary, EmptyIndex) {
    auto idx = make_test_hash_index();
    EXPECT_TRUE(idx.empty());
    EXPECT_EQ(idx.size(), 0u);
    EXPECT_EQ(idx.global_depth(), 0u);
    EXPECT_EQ(idx.bucket_count(), 1u);
}

TEST(HashIndexBoundary, SingleInsertAndDelete) {
    auto idx = make_test_hash_index();
    ASSERT_TRUE(idx.insert(make_key(1), make_rid(1)).has_value());
    EXPECT_FALSE(idx.empty());

    ASSERT_TRUE(idx.remove(make_key(1)).has_value());
    EXPECT_TRUE(idx.empty());
}

TEST(HashIndexBoundary, InsertDeleteReinsert) {
    auto idx = make_test_hash_index(64);

    for (int i = 0; i < 10; ++i) {
        auto ins =
            idx.insert(make_key(static_cast<int64_t>(i)), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value());
    }
    EXPECT_EQ(idx.size(), 10u);

    // Delete all.
    for (int i = 0; i < 10; ++i) {
        auto del = idx.remove(make_key(static_cast<int64_t>(i)));
        ASSERT_TRUE(del.has_value());
        EXPECT_TRUE(*del);
    }
    EXPECT_EQ(idx.size(), 0u);

    // Reinsert.
    for (int i = 0; i < 10; ++i) {
        auto ins =
            idx.insert(make_key(static_cast<int64_t>(i)), make_rid(static_cast<uint32_t>(i + 100)));
        ASSERT_TRUE(ins.has_value());
    }
    EXPECT_EQ(idx.size(), 10u);

    // Verify new RIDs.
    for (int i = 0; i < 10; ++i) {
        auto result = idx.search(make_key(static_cast<int64_t>(i)));
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->has_value());
        EXPECT_EQ(result->value().page_id, static_cast<uint32_t>(i + 100));
    }
}

// ---------------------------------------------------------------------------
// Concurrency tests
// ---------------------------------------------------------------------------

TEST(HashIndexConcurrency, ConcurrentReads) {
    auto idx = make_test_hash_index(64);
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(
            idx.insert(make_key(static_cast<int64_t>(i)), make_rid(static_cast<uint32_t>(i)))
                .has_value());
    }

    constexpr int NUM_THREADS = 4;
    std::vector<std::thread> threads;
    std::atomic<int> errors{0};

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&idx, &errors]() {
            for (int i = 0; i < 100; ++i) {
                auto result = idx.search(make_key(static_cast<int64_t>(i)));
                if (!result.has_value() || !result->has_value()) {
                    ++errors;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }
    EXPECT_EQ(errors.load(), 0);
}

TEST(HashIndexConcurrency, ConcurrentInserts) {
    auto idx = make_test_hash_index(16);

    constexpr int NUM_THREADS = 4;
    constexpr int PER_THREAD = 100;
    std::vector<std::thread> threads;
    std::atomic<int> errors{0};

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&idx, &errors, t]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                auto key_val = static_cast<int64_t>(t * PER_THREAD + i);
                auto result =
                    idx.insert(make_key(key_val), make_rid(static_cast<uint32_t>(key_val)));
                if (!result.has_value()) {
                    ++errors;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }
    EXPECT_EQ(errors.load(), 0);
    EXPECT_EQ(idx.size(), static_cast<uint64_t>(NUM_THREADS * PER_THREAD));

    // Verify all entries are findable.
    for (int i = 0; i < NUM_THREADS * PER_THREAD; ++i) {
        auto result = idx.search(make_key(static_cast<int64_t>(i)));
        ASSERT_TRUE(result.has_value()) << "search failed for i=" << i;
        ASSERT_TRUE(result->has_value()) << "key not found: " << i;
    }
}

// ---------------------------------------------------------------------------
// Config accessor test
// ---------------------------------------------------------------------------

TEST(HashIndexConfig, ReturnsCorrectConfig) {
    HashIndexConfig config;
    config.key_types = {TypeId::INT64, TypeId::STRING};
    config.bucket_capacity = 32;
    config.is_unique = true;
    auto idx = HashIndex(config);

    EXPECT_EQ(idx.config().key_types.size(), 2u);
    EXPECT_EQ(idx.config().bucket_capacity, 32u);
    EXPECT_TRUE(idx.config().is_unique);
}

} // namespace sixseven::test
