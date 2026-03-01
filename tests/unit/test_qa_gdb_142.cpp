/// @file test_qa_gdb_142.cpp
/// @brief QA adversarial tests for GDB-142: Extensible hash index.
///
/// Targets edge cases: bucket capacity boundaries, pathological hash collisions,
/// directory growth, delete-after-split, non-unique duplicates across splits,
/// zero-hash types, concurrent insert+delete, extreme key values, mixed types.

#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/index/btree_key.h"
#include "giodb/index/hash_index.h"
#include "giodb/index/rid.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace giodb;

// =============================================================================
// Test helpers
// =============================================================================

namespace {

HashIndex make_index(uint32_t capacity = 4, bool unique = false,
                     std::vector<TypeId> types = {TypeId::INT64}) {
    HashIndexConfig cfg;
    cfg.key_types = std::move(types);
    cfg.bucket_capacity = capacity;
    cfg.is_unique = unique;
    return HashIndex(std::move(cfg));
}

KeyType key(int64_t v) { return {Value(v)}; }
KeyType skey(const std::string& s) { return {Value(s)}; }
RID rid(uint32_t p, uint16_t s = 0) { return {p, s}; }

} // namespace

// =============================================================================
// Bucket capacity = 1 (every insert forces a split)
// =============================================================================

TEST(QA_GDB_142, BucketCapacity1ForcesFrequentSplits) {
    auto idx = make_index(1);

    for (int i = 0; i < 20; ++i) {
        auto r = idx.insert(key(i * 7), rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value()) << "insert i=" << i << ": " << r.error().message;
    }

    EXPECT_EQ(idx.size(), 20u);

    // Every entry still findable.
    for (int i = 0; i < 20; ++i) {
        auto r = idx.search(key(i * 7));
        ASSERT_TRUE(r.has_value());
        ASSERT_TRUE(r->has_value()) << "missing key " << (i * 7);
        EXPECT_EQ(r->value().page_id, static_cast<uint32_t>(i));
    }
}

// =============================================================================
// Exact capacity boundary: insert exactly bucket_capacity entries (no split)
// =============================================================================

TEST(QA_GDB_142, ExactlyBucketCapacityNoSplit) {
    auto idx = make_index(8);

    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(idx.insert(key(i), rid(static_cast<uint32_t>(i))).has_value());
    }

    EXPECT_EQ(idx.size(), 8u);
    // With only 8 entries in a capacity-8 bucket, no split should have occurred
    // (split triggers when entries.size() > capacity, i.e. > 8 means 9+).
    EXPECT_EQ(idx.global_depth(), 0u);
    EXPECT_EQ(idx.bucket_count(), 1u);
}

TEST(QA_GDB_142, BucketCapacityPlusOneTriggersSplit) {
    auto idx = make_index(4);

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(idx.insert(key(i * 1000), rid(static_cast<uint32_t>(i))).has_value());
    }

    EXPECT_EQ(idx.size(), 5u);
    EXPECT_GT(idx.global_depth(), 0u);
}

// =============================================================================
// Pathological: all keys hash to same value (bucket overflow guard)
// =============================================================================

// GDB-244 fix: insert() now skips split_bucket() when all entries share the
// same hash value, preventing unbounded directory growth.
TEST(QA_GDB_142, AllSameKeysDifferentRIDs) {
    auto idx = make_index(4);

    for (int i = 0; i < 100; ++i) {
        auto r = idx.insert(key(42), rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value()) << "insert i=" << i << ": " << r.error().message;
    }

    EXPECT_EQ(idx.size(), 100u);

    auto all = idx.search_all(key(42));
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->size(), 100u);
}

// Lighter variant: use a large bucket capacity so no split is triggered,
// confirming basic non-unique duplicate storage works independently of splits.
TEST(QA_GDB_142, SameKeysDifferentRIDsLargeBucket) {
    auto idx = make_index(200);

    for (int i = 0; i < 100; ++i) {
        auto r = idx.insert(key(42), rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value()) << "insert i=" << i << ": " << r.error().message;
    }

    EXPECT_EQ(idx.size(), 100u);

    auto all = idx.search_all(key(42));
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->size(), 100u);
}

// =============================================================================
// INT64 extreme values
// =============================================================================

TEST(QA_GDB_142, ExtremeInt64Values) {
    auto idx = make_index(8);

    constexpr auto i64_min = std::numeric_limits<int64_t>::min();
    constexpr auto i64_max = std::numeric_limits<int64_t>::max();

    ASSERT_TRUE(idx.insert(key(i64_min), rid(1)).has_value());
    ASSERT_TRUE(idx.insert(key(i64_max), rid(2)).has_value());
    ASSERT_TRUE(idx.insert(key(0), rid(3)).has_value());
    ASSERT_TRUE(idx.insert(key(-1), rid(4)).has_value());
    ASSERT_TRUE(idx.insert(key(1), rid(5)).has_value());

    auto r1 = idx.search(key(i64_min));
    ASSERT_TRUE(r1.has_value() && r1->has_value());
    EXPECT_EQ(r1->value(), rid(1));

    auto r2 = idx.search(key(i64_max));
    ASSERT_TRUE(r2.has_value() && r2->has_value());
    EXPECT_EQ(r2->value(), rid(2));
}

// =============================================================================
// Delete entries that crossed a split boundary
// =============================================================================

TEST(QA_GDB_142, DeleteAfterSplitPreservesOthers) {
    auto idx = make_index(4);

    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(idx.insert(key(i), rid(static_cast<uint32_t>(i))).has_value());
    }
    EXPECT_GT(idx.global_depth(), 0u);

    // Delete every other entry.
    for (int i = 0; i < 20; i += 2) {
        auto r = idx.remove(key(i));
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(*r) << "failed to remove key=" << i;
    }
    EXPECT_EQ(idx.size(), 10u);

    // Remaining entries still findable.
    for (int i = 1; i < 20; i += 2) {
        auto r = idx.search(key(i));
        ASSERT_TRUE(r.has_value());
        ASSERT_TRUE(r->has_value()) << "key not found after split+delete: " << i;
    }

    // Deleted entries gone.
    for (int i = 0; i < 20; i += 2) {
        auto r = idx.search(key(i));
        ASSERT_TRUE(r.has_value());
        EXPECT_FALSE(r->has_value()) << "deleted key still found: " << i;
    }
}

// =============================================================================
// Non-unique: remove(key) only removes first match
// =============================================================================

TEST(QA_GDB_142, RemoveKeyRemovesOnlyFirstDuplicate) {
    auto idx = make_index(64);

    ASSERT_TRUE(idx.insert(key(99), rid(1)).has_value());
    ASSERT_TRUE(idx.insert(key(99), rid(2)).has_value());
    ASSERT_TRUE(idx.insert(key(99), rid(3)).has_value());

    auto r = idx.remove(key(99));
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(*r);
    EXPECT_EQ(idx.size(), 2u);

    auto remaining = idx.search_all(key(99));
    ASSERT_TRUE(remaining.has_value());
    EXPECT_EQ(remaining->size(), 2u);
}

// =============================================================================
// Non-unique duplicates surviving a split
// =============================================================================

TEST(QA_GDB_142, DuplicateKeySurvivesSplit) {
    auto idx = make_index(4);

    // Insert 3 copies of key=10, then fill bucket with other keys to force split.
    ASSERT_TRUE(idx.insert(key(10), rid(100)).has_value());
    ASSERT_TRUE(idx.insert(key(10), rid(101)).has_value());
    ASSERT_TRUE(idx.insert(key(10), rid(102)).has_value());

    for (int i = 20; i < 30; ++i) {
        ASSERT_TRUE(idx.insert(key(i), rid(static_cast<uint32_t>(i))).has_value());
    }

    auto all = idx.search_all(key(10));
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->size(), 3u);

    std::set<uint32_t> pids;
    for (const auto& r : *all) {
        pids.insert(r.page_id);
    }
    EXPECT_TRUE(pids.count(100));
    EXPECT_TRUE(pids.count(101));
    EXPECT_TRUE(pids.count(102));
}

// =============================================================================
// Remove by (key, rid) when multiple entries share the same key
// =============================================================================

TEST(QA_GDB_142, RemoveByKeyAndRIDTargetsExactEntry) {
    auto idx = make_index(64);

    ASSERT_TRUE(idx.insert(key(7), rid(10, 1)).has_value());
    ASSERT_TRUE(idx.insert(key(7), rid(10, 2)).has_value());
    ASSERT_TRUE(idx.insert(key(7), rid(20, 0)).has_value());

    // Remove only (key=7, rid={10,2}).
    auto r = idx.remove(key(7), rid(10, 2));
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(*r);
    EXPECT_EQ(idx.size(), 2u);

    // Remaining entries.
    auto remaining = idx.search_all(key(7));
    ASSERT_TRUE(remaining.has_value());
    ASSERT_EQ(remaining->size(), 2u);

    std::set<uint16_t> slots;
    for (const auto& ri : *remaining) {
        slots.insert(ri.slot_id);
    }
    EXPECT_TRUE(slots.count(1));
    EXPECT_TRUE(slots.count(0));
    EXPECT_FALSE(slots.count(2));
}

// =============================================================================
// search_all on empty index
// =============================================================================

TEST(QA_GDB_142, SearchAllEmptyIndex) {
    auto idx = make_index(4);
    auto r = idx.search_all(key(1));
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->empty());
}

// =============================================================================
// Unique constraint still enforced after split
// =============================================================================

TEST(QA_GDB_142, UniqueConstraintAfterSplit) {
    auto idx = make_index(4, true);

    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(idx.insert(key(i), rid(static_cast<uint32_t>(i))).has_value());
    }
    EXPECT_GT(idx.global_depth(), 0u);

    // Try inserting a duplicate after splits have occurred.
    auto r = idx.insert(key(10), rid(999));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::CONSTRAINT_VIOLATION);
    EXPECT_EQ(idx.size(), 20u);
}

// =============================================================================
// Multiple NULL keys in non-unique index
// =============================================================================

TEST(QA_GDB_142, MultipleNullKeys) {
    auto idx = make_index(64);

    KeyType nk = {Value::make_null()};
    ASSERT_TRUE(idx.insert(nk, rid(1)).has_value());
    ASSERT_TRUE(idx.insert(nk, rid(2)).has_value());
    ASSERT_TRUE(idx.insert(nk, rid(3)).has_value());

    auto all = idx.search_all(nk);
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->size(), 3u);
}

// =============================================================================
// NULL key uniqueness: unique index allows only one NULL
// =============================================================================

TEST(QA_GDB_142, UniqueNullKeyRejectsDuplicate) {
    auto idx = make_index(64, true);

    KeyType nk = {Value::make_null()};
    ASSERT_TRUE(idx.insert(nk, rid(1)).has_value());

    auto r = idx.insert(nk, rid(2));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

// =============================================================================
// String key edge cases
// =============================================================================

TEST(QA_GDB_142, EmptyStringKey) {
    auto idx = make_index(8, false, {TypeId::STRING});

    ASSERT_TRUE(idx.insert(skey(""), rid(1)).has_value());
    ASSERT_TRUE(idx.insert(skey("a"), rid(2)).has_value());

    auto r = idx.search(skey(""));
    ASSERT_TRUE(r.has_value() && r->has_value());
    EXPECT_EQ(r->value(), rid(1));
}

TEST(QA_GDB_142, VeryLongStringKey) {
    auto idx = make_index(8, false, {TypeId::STRING});

    std::string long_key(10000, 'x');
    ASSERT_TRUE(idx.insert(skey(long_key), rid(1)).has_value());

    auto r = idx.search(skey(long_key));
    ASSERT_TRUE(r.has_value() && r->has_value());
    EXPECT_EQ(r->value(), rid(1));

    // Slightly different key should not match.
    std::string long_key2(10000, 'x');
    long_key2[9999] = 'y';
    auto r2 = idx.search(skey(long_key2));
    ASSERT_TRUE(r2.has_value());
    EXPECT_FALSE(r2->has_value());
}

// =============================================================================
// Composite key with 3 columns
// =============================================================================

TEST(QA_GDB_142, ThreeColumnCompositeKey) {
    auto idx = make_index(4, true, {TypeId::INT64, TypeId::STRING, TypeId::INT64});

    KeyType k1 = {Value(int64_t{1}), Value(std::string("a")), Value(int64_t{100})};
    KeyType k2 = {Value(int64_t{1}), Value(std::string("a")), Value(int64_t{200})};
    KeyType k3 = {Value(int64_t{1}), Value(std::string("b")), Value(int64_t{100})};

    ASSERT_TRUE(idx.insert(k1, rid(1)).has_value());
    ASSERT_TRUE(idx.insert(k2, rid(2)).has_value());
    ASSERT_TRUE(idx.insert(k3, rid(3)).has_value());

    // Exact matches.
    auto r1 = idx.search(k1);
    ASSERT_TRUE(r1.has_value() && r1->has_value());
    EXPECT_EQ(r1->value(), rid(1));

    auto r2 = idx.search(k2);
    ASSERT_TRUE(r2.has_value() && r2->has_value());
    EXPECT_EQ(r2->value(), rid(2));

    // Unique violation.
    auto dup = idx.insert(k1, rid(99));
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

// =============================================================================
// Delete all entries then reinsert
// =============================================================================

TEST(QA_GDB_142, DeleteAllAndReinsert) {
    auto idx = make_index(4);

    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(idx.insert(key(i), rid(static_cast<uint32_t>(i))).has_value());
    }

    for (int i = 0; i < 20; ++i) {
        auto r = idx.remove(key(i));
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(*r);
    }
    EXPECT_EQ(idx.size(), 0u);
    EXPECT_TRUE(idx.empty());

    // Reinsert after complete drain.
    for (int i = 100; i < 120; ++i) {
        ASSERT_TRUE(idx.insert(key(i), rid(static_cast<uint32_t>(i))).has_value());
    }
    EXPECT_EQ(idx.size(), 20u);

    for (int i = 100; i < 120; ++i) {
        auto r = idx.search(key(i));
        ASSERT_TRUE(r.has_value() && r->has_value());
        EXPECT_EQ(r->value().page_id, static_cast<uint32_t>(i));
    }
}

// =============================================================================
// bucket_count() correctness
// =============================================================================

TEST(QA_GDB_142, BucketCountAccurate) {
    auto idx = make_index(4);

    // Initially 1 bucket.
    EXPECT_EQ(idx.bucket_count(), 1u);

    // Insert enough to force at least one split.
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(idx.insert(key(i * 1000), rid(static_cast<uint32_t>(i))).has_value());
    }

    // After splits, bucket_count should be > 1 and <= directory size.
    auto bc = idx.bucket_count();
    EXPECT_GT(bc, 1u);
    EXPECT_LE(bc, 1u << idx.global_depth());
}

// =============================================================================
// Stress: 5000 entries with small buckets
// =============================================================================

TEST(QA_GDB_142, StressManyInserts) {
    auto idx = make_index(8);
    constexpr int N = 5000;

    for (int i = 0; i < N; ++i) {
        auto r = idx.insert(key(static_cast<int64_t>(i)), rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value()) << "insert failed at i=" << i << ": " << r.error().message;
    }

    EXPECT_EQ(idx.size(), static_cast<uint64_t>(N));

    // Spot-check: first, middle, last.
    for (int i : {0, N / 2, N - 1}) {
        auto r = idx.search(key(static_cast<int64_t>(i)));
        ASSERT_TRUE(r.has_value());
        ASSERT_TRUE(r->has_value()) << "missing key=" << i;
        EXPECT_EQ(r->value().page_id, static_cast<uint32_t>(i));
    }
}

// =============================================================================
// Concurrent insert + delete
// =============================================================================

TEST(QA_GDB_142, ConcurrentInsertAndDelete) {
    auto idx = make_index(16);

    // Pre-populate.
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(idx.insert(key(i), rid(static_cast<uint32_t>(i))).has_value());
    }

    std::atomic<int> insert_errors{0};
    std::atomic<int> delete_errors{0};

    // One thread inserts 100..199, another deletes 0..99.
    std::thread inserter([&]() {
        for (int i = 100; i < 200; ++i) {
            auto r = idx.insert(key(i), rid(static_cast<uint32_t>(i)));
            if (!r.has_value()) {
                ++insert_errors;
            }
        }
    });

    std::thread deleter([&]() {
        for (int i = 0; i < 100; ++i) {
            auto r = idx.remove(key(i));
            if (!r.has_value()) {
                ++delete_errors;
            }
        }
    });

    inserter.join();
    deleter.join();

    EXPECT_EQ(insert_errors.load(), 0);
    EXPECT_EQ(delete_errors.load(), 0);
    EXPECT_EQ(idx.size(), 100u);
}

// =============================================================================
// Bool key type
// =============================================================================

TEST(QA_GDB_142, BoolKeyType) {
    auto idx = make_index(8, true, {TypeId::BOOL});

    KeyType kt = {Value(true)};
    KeyType kf = {Value(false)};

    ASSERT_TRUE(idx.insert(kt, rid(1)).has_value());
    ASSERT_TRUE(idx.insert(kf, rid(2)).has_value());

    auto r1 = idx.search(kt);
    ASSERT_TRUE(r1.has_value() && r1->has_value());
    EXPECT_EQ(r1->value(), rid(1));

    auto r2 = idx.search(kf);
    ASSERT_TRUE(r2.has_value() && r2->has_value());
    EXPECT_EQ(r2->value(), rid(2));
}

// =============================================================================
// Double key type
// =============================================================================

TEST(QA_GDB_142, DoubleKeyType) {
    auto idx = make_index(8, false, {TypeId::FLOAT64});

    KeyType k1 = {Value(3.14)};
    KeyType k2 = {Value(2.718)};
    KeyType k3 = {Value(0.0)};

    ASSERT_TRUE(idx.insert(k1, rid(1)).has_value());
    ASSERT_TRUE(idx.insert(k2, rid(2)).has_value());
    ASSERT_TRUE(idx.insert(k3, rid(3)).has_value());

    auto r1 = idx.search(k1);
    ASSERT_TRUE(r1.has_value() && r1->has_value());
    EXPECT_EQ(r1->value(), rid(1));

    auto r3 = idx.search(k3);
    ASSERT_TRUE(r3.has_value() && r3->has_value());
    EXPECT_EQ(r3->value(), rid(3));
}

// =============================================================================
// Interleaved insert and search
// =============================================================================

TEST(QA_GDB_142, InterleavedInsertSearch) {
    auto idx = make_index(4);

    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(idx.insert(key(i), rid(static_cast<uint32_t>(i))).has_value());

        // Search for all previously inserted keys.
        for (int j = 0; j <= i; ++j) {
            auto r = idx.search(key(j));
            ASSERT_TRUE(r.has_value()) << "at i=" << i << " j=" << j;
            ASSERT_TRUE(r->has_value()) << "missing key=" << j << " after insert of " << i;
        }
    }
}

// =============================================================================
// Negative key values
// =============================================================================

TEST(QA_GDB_142, NegativeKeys) {
    auto idx = make_index(4);

    for (int i = -50; i < 0; ++i) {
        ASSERT_TRUE(
            idx.insert(key(i), rid(static_cast<uint32_t>(i + 100))).has_value());
    }

    EXPECT_EQ(idx.size(), 50u);

    for (int i = -50; i < 0; ++i) {
        auto r = idx.search(key(i));
        ASSERT_TRUE(r.has_value() && r->has_value()) << "missing key=" << i;
        EXPECT_EQ(r->value().page_id, static_cast<uint32_t>(i + 100));
    }
}

// =============================================================================
// Global depth grows proportionally with distinct-hash keys
// =============================================================================

TEST(QA_GDB_142, GlobalDepthGrowsLogDirectorySize) {
    auto idx = make_index(2); // Very small buckets.

    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(idx.insert(key(i * 37), rid(static_cast<uint32_t>(i))).has_value());
    }

    // Directory size = 2^global_depth.
    uint32_t gd = idx.global_depth();
    EXPECT_GT(gd, 0u);
    // Bucket count should be <= 2^gd.
    EXPECT_LE(idx.bucket_count(), 1u << gd);
}

// =============================================================================
// search returns first match for non-unique (deterministic)
// =============================================================================

TEST(QA_GDB_142, SearchReturnsFirstInsertedForDuplicates) {
    auto idx = make_index(64);

    ASSERT_TRUE(idx.insert(key(5), rid(10)).has_value());
    ASSERT_TRUE(idx.insert(key(5), rid(20)).has_value());
    ASSERT_TRUE(idx.insert(key(5), rid(30)).has_value());

    // search() returns the first matching entry.
    auto r = idx.search(key(5));
    ASSERT_TRUE(r.has_value() && r->has_value());
    EXPECT_EQ(r->value(), rid(10));
}

// =============================================================================
// Insert same key+RID pair twice (non-unique allows it)
// =============================================================================

TEST(QA_GDB_142, InsertSameKeyAndRIDTwice) {
    auto idx = make_index(64);

    ASSERT_TRUE(idx.insert(key(42), rid(1)).has_value());
    ASSERT_TRUE(idx.insert(key(42), rid(1)).has_value());

    EXPECT_EQ(idx.size(), 2u);

    auto all = idx.search_all(key(42));
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->size(), 2u);
}

// =============================================================================
// Remove by (key, rid) when RID doesn't match
// =============================================================================

TEST(QA_GDB_142, RemoveByKeyRIDNoMatchingRID) {
    auto idx = make_index(64);

    ASSERT_TRUE(idx.insert(key(42), rid(1)).has_value());

    auto r = idx.remove(key(42), rid(999));
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(*r); // No match.
    EXPECT_EQ(idx.size(), 1u);
}

// =============================================================================
// Large bucket capacity (no splits ever)
// =============================================================================

TEST(QA_GDB_142, LargeBucketCapacityNeverSplits) {
    auto idx = make_index(100000);

    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(idx.insert(key(i), rid(static_cast<uint32_t>(i))).has_value());
    }

    EXPECT_EQ(idx.global_depth(), 0u);
    EXPECT_EQ(idx.bucket_count(), 1u);
    EXPECT_EQ(idx.size(), 1000u);
}
