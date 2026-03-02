/// QA adversarial tests for GDB-94: B+ tree point lookup and range scan.
/// Tests iterator behavior, boundary conditions, edge cases, and error paths.

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

#include "test_btree_helpers.h"

using namespace giodb;
using namespace giodb::test;

// =============================================================================
// Point Lookup Edge Cases
// =============================================================================

TEST(QA_GDB94_Search, SearchInEmptyTree) {
    auto tree = make_test_index();
    auto s = tree.search(make_key(0));
    ASSERT_TRUE(s.has_value());
    EXPECT_FALSE(s->has_value());
}

TEST(QA_GDB94_Search, SearchForInt64MinMax) {
    auto tree = make_test_index(4, 4);
    const auto min_val = std::numeric_limits<int64_t>::min();
    const auto max_val = std::numeric_limits<int64_t>::max();

    (void)tree.insert(make_key(min_val), make_rid(1));
    (void)tree.insert(make_key(max_val), make_rid(2));
    (void)tree.insert(make_key(0), make_rid(3));

    auto s1 = tree.search(make_key(min_val));
    ASSERT_TRUE(s1.has_value());
    ASSERT_TRUE(s1->has_value());
    EXPECT_EQ(s1->value().page_id, 1u);

    auto s2 = tree.search(make_key(max_val));
    ASSERT_TRUE(s2.has_value());
    ASSERT_TRUE(s2->has_value());
    EXPECT_EQ(s2->value().page_id, 2u);

    auto s3 = tree.search(make_key(0));
    ASSERT_TRUE(s3.has_value());
    ASSERT_TRUE(s3->has_value());
    EXPECT_EQ(s3->value().page_id, 3u);
}

TEST(QA_GDB94_Search, SearchMissingKeyBetweenExisting) {
    auto tree = make_test_index(4, 4);

    for (int i = 0; i < 20; i += 2) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    // Search for odd keys (not present)
    for (int i = 1; i < 20; i += 2) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        EXPECT_FALSE(s->has_value()) << "key " << i << " should not be found";
    }
}

TEST(QA_GDB94_Search, SearchAfterManySplits) {
    auto tree = make_test_index(3, 3);

    std::vector<int> keys(100);
    std::iota(keys.begin(), keys.end(), 0);
    std::mt19937 rng(77);
    std::shuffle(keys.begin(), keys.end(), rng);

    for (int k : keys) {
        (void)tree.insert(make_key(k), make_rid(static_cast<uint32_t>(k)));
    }

    // Every key should be found
    for (int i = 0; i < 100; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i << " not found";
        EXPECT_EQ(s->value().page_id, static_cast<uint32_t>(i));
    }

    // Keys outside range should not be found
    auto s_neg = tree.search(make_key(-1));
    ASSERT_TRUE(s_neg.has_value());
    EXPECT_FALSE(s_neg->has_value());

    auto s_high = tree.search(make_key(100));
    ASSERT_TRUE(s_high.has_value());
    EXPECT_FALSE(s_high->has_value());
}

TEST(QA_GDB94_Search, SearchWithNullKey) {
    auto tree = make_test_index(4, 4);

    KeyType null_key = {Value::make_null()};
    (void)tree.insert(null_key, make_rid(1));
    (void)tree.insert(make_key(10), make_rid(2));

    auto s = tree.search(null_key);
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
    EXPECT_EQ(s->value().page_id, 1u);
}

// =============================================================================
// Range Scan: Full Scan
// =============================================================================

TEST(QA_GDB94_RangeScan, FullScanSingleEntry) {
    auto tree = make_test_index(4, 4);
    (void)tree.insert(make_key(42), make_rid(1));

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 1u);
    EXPECT_EQ(key_val((*entries)[0].first), 42);
}

TEST(QA_GDB94_RangeScan, FullScanMultipleLeaves) {
    auto tree = make_test_index(3, 3);

    for (int i = 1; i <= 30; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 30u);

    // Verify strictly sorted
    for (size_t i = 1; i < entries->size(); ++i) {
        EXPECT_LT(key_val((*entries)[i - 1].first), key_val((*entries)[i].first));
    }
}

// =============================================================================
// Range Scan: Bounded Ranges
// =============================================================================

TEST(QA_GDB94_RangeScan, BeginExactMatch) {
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 10; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // begin_key = 50 (exists)
    auto scan = tree.range_scan(make_key(50), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 6u); // 50,60,70,80,90,100
    EXPECT_EQ(key_val((*entries)[0].first), 50);
}

TEST(QA_GDB94_RangeScan, BeginBetweenKeys) {
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 10; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // begin_key = 55 (between 50 and 60)
    auto scan = tree.range_scan(make_key(55), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 5u); // 60,70,80,90,100
    EXPECT_EQ(key_val((*entries)[0].first), 60);
}

TEST(QA_GDB94_RangeScan, EndExactMatch) {
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 10; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // end_key = 50 (exclusive: should NOT include 50)
    auto scan = tree.range_scan(std::nullopt, make_key(50));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 4u); // 10,20,30,40
    EXPECT_EQ(key_val(entries->back().first), 40);
}

TEST(QA_GDB94_RangeScan, EndBetweenKeys) {
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 10; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // end_key = 55 (between 50 and 60)
    auto scan = tree.range_scan(std::nullopt, make_key(55));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 5u); // 10,20,30,40,50
    EXPECT_EQ(key_val(entries->back().first), 50);
}

TEST(QA_GDB94_RangeScan, BeginAfterAllKeys) {
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 5; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan = tree.range_scan(make_key(100), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 0u);
}

TEST(QA_GDB94_RangeScan, EndBeforeAllKeys) {
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 5; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan = tree.range_scan(std::nullopt, make_key(5));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 0u);
}

TEST(QA_GDB94_RangeScan, BeginGreaterThanEnd) {
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 10; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // begin > end should return empty
    auto scan = tree.range_scan(make_key(80), make_key(20));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 0u);
}

TEST(QA_GDB94_RangeScan, SingleKeyRange) {
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 10; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Range [50, 60) should return only key 50
    auto scan = tree.range_scan(make_key(50), make_key(60));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 1u);
    EXPECT_EQ(key_val((*entries)[0].first), 50);
}

// =============================================================================
// Range Scan: Cross-Leaf Boundary
// =============================================================================

TEST(QA_GDB94_CrossLeaf, RangeStartsInMiddleOfLeaf) {
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 20; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    // Start from key 7, which will be in the middle of some leaf
    auto scan = tree.range_scan(make_key(7), make_key(14));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 7u); // 7,8,9,10,11,12,13

    for (size_t i = 0; i < entries->size(); ++i) {
        EXPECT_EQ(key_val((*entries)[i].first), static_cast<int64_t>(7 + i));
    }
}

TEST(QA_GDB94_CrossLeaf, ScanAcrossManyLeaves) {
    auto tree = make_test_index(2, 2);

    for (int i = 1; i <= 50; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan = tree.range_scan(make_key(10), make_key(41));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 31u); // 10..40

    for (size_t i = 0; i < entries->size(); ++i) {
        EXPECT_EQ(key_val((*entries)[i].first), static_cast<int64_t>(10 + i));
    }
}

// =============================================================================
// Iterator Behavior
// =============================================================================

TEST(QA_GDB94_Iterator, EmptyIterator) {
    BTreeIterator it;
    EXPECT_TRUE(it.is_end());

    auto next = it.next();
    ASSERT_TRUE(next.has_value());
    EXPECT_FALSE(next->has_value());
}

TEST(QA_GDB94_Iterator, NextAfterExhaustion) {
    auto tree = make_test_index(4, 4);
    (void)tree.insert(make_key(42), make_rid(1));

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());

    // First next: returns entry
    auto n1 = scan->next();
    ASSERT_TRUE(n1.has_value());
    ASSERT_TRUE(n1->has_value());

    // Second next: exhausted
    auto n2 = scan->next();
    ASSERT_TRUE(n2.has_value());
    EXPECT_FALSE(n2->has_value());
    EXPECT_TRUE(scan->is_end());

    // Third next: still exhausted
    auto n3 = scan->next();
    ASSERT_TRUE(n3.has_value());
    EXPECT_FALSE(n3->has_value());
}

TEST(QA_GDB94_Iterator, ManualIteration) {
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 5; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());

    std::vector<int64_t> collected;
    while (!scan->is_end()) {
        auto entry = scan->next();
        ASSERT_TRUE(entry.has_value());
        if (!entry->has_value()) break;
        collected.push_back(key_val(entry->value().first));
    }

    EXPECT_EQ(collected.size(), 5u);
    for (size_t i = 0; i < collected.size(); ++i) {
        EXPECT_EQ(collected[i], static_cast<int64_t>((i + 1) * 10));
    }
}

// =============================================================================
// Composite Key Search and Scan
// =============================================================================

TEST(QA_GDB94_Composite, PointLookupComposite) {
    BTreeConfig config;
    config.key_types = {TypeId::INT64, TypeId::STRING};
    config.leaf_max_keys = 4;
    config.internal_max_keys = 4;
    BTreeIndex tree(std::move(config));

    (void)tree.insert(make_composite_key(1, "alpha"), make_rid(1));
    (void)tree.insert(make_composite_key(1, "beta"), make_rid(2));
    (void)tree.insert(make_composite_key(2, "gamma"), make_rid(3));

    // Exact match
    auto s1 = tree.search(make_composite_key(1, "beta"));
    ASSERT_TRUE(s1.has_value());
    ASSERT_TRUE(s1->has_value());
    EXPECT_EQ(s1->value().page_id, 2u);

    // Miss: different string
    auto s2 = tree.search(make_composite_key(1, "delta"));
    ASSERT_TRUE(s2.has_value());
    EXPECT_FALSE(s2->has_value());

    // Miss: different int
    auto s3 = tree.search(make_composite_key(3, "alpha"));
    ASSERT_TRUE(s3.has_value());
    EXPECT_FALSE(s3->has_value());
}

TEST(QA_GDB94_Composite, RangeScanComposite) {
    BTreeConfig config;
    config.key_types = {TypeId::INT64, TypeId::STRING};
    config.leaf_max_keys = 3;
    config.internal_max_keys = 3;
    BTreeIndex tree(std::move(config));

    (void)tree.insert(make_composite_key(1, "apple"), make_rid(1));
    (void)tree.insert(make_composite_key(1, "banana"), make_rid(2));
    (void)tree.insert(make_composite_key(2, "cherry"), make_rid(3));
    (void)tree.insert(make_composite_key(2, "date"), make_rid(4));
    (void)tree.insert(make_composite_key(3, "elderberry"), make_rid(5));

    // Range scan [1,"banana") to (2,"date")
    auto scan = tree.range_scan(
        make_composite_key(1, "banana"), make_composite_key(2, "date"));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    // Should get: (1,"banana"), (2,"cherry")
    EXPECT_EQ(entries->size(), 2u);
}

// =============================================================================
// Negative Key Range Scans
// =============================================================================

TEST(QA_GDB94_NegativeKeys, RangeScanAcrossZero) {
    auto tree = make_test_index(4, 4);
    for (int i = -10; i <= 10; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i + 100)));
    }

    // Range [-5, 5)
    auto scan = tree.range_scan(make_key(-5), make_key(5));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 10u); // -5,-4,-3,-2,-1,0,1,2,3,4

    EXPECT_EQ(key_val((*entries)[0].first), -5);
    EXPECT_EQ(key_val(entries->back().first), 4);
}

// =============================================================================
// Stress Tests
// =============================================================================

TEST(QA_GDB94_Stress, PointLookupAfterRandomInserts) {
    auto tree = make_test_index(5, 5);

    std::vector<int> keys(300);
    std::iota(keys.begin(), keys.end(), -150);
    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);

    for (int k : keys) {
        (void)tree.insert(make_key(k), make_rid(static_cast<uint32_t>(k + 500)));
    }

    // Verify all present
    for (int i = -150; i < 150; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i << " not found";
        EXPECT_EQ(s->value().page_id, static_cast<uint32_t>(i + 500));
    }

    // Verify boundaries not present
    auto s1 = tree.search(make_key(-151));
    ASSERT_TRUE(s1.has_value());
    EXPECT_FALSE(s1->has_value());

    auto s2 = tree.search(make_key(150));
    ASSERT_TRUE(s2.has_value());
    EXPECT_FALSE(s2->has_value());
}

TEST(QA_GDB94_Stress, RangeScanConsistencyWithPointLookup) {
    auto tree = make_test_index(4, 4);

    for (int i = 0; i < 100; ++i) {
        (void)tree.insert(make_key(i * 3), make_rid(static_cast<uint32_t>(i)));
    }

    // Full scan should match size
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 100u);

    // Every entry from scan should be findable via point lookup
    for (const auto& [key, rid] : *entries) {
        auto s = tree.search(key);
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value());
    }
}

TEST(QA_GDB94_Stress, ManySmallRangeScans) {
    auto tree = make_test_index(4, 4);

    for (int i = 0; i < 100; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    // Many small range scans
    for (int start = 0; start < 90; start += 10) {
        auto scan = tree.range_scan(make_key(start), make_key(start + 10));
        ASSERT_TRUE(scan.has_value());
        auto entries = collect_scan(*scan);
        ASSERT_TRUE(entries.has_value());
        EXPECT_EQ(entries->size(), 10u)
            << "range [" << start << ", " << start + 10 << ") has wrong count";

        for (size_t i = 0; i < entries->size(); ++i) {
            EXPECT_EQ(key_val((*entries)[i].first), start + static_cast<int64_t>(i));
        }
    }
}
