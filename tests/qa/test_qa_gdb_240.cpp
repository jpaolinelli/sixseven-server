/// QA adversarial tests for GDB-240: range_scan backward walk fix for
/// duplicate keys spanning multiple leaves.
///
/// Focuses on the backward walk through prev_leaf_id pointers that was added
/// to find the leftmost leaf when begin_key matches duplicate keys.

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
// Backward Walk Edge Cases
// =============================================================================

TEST(QA_GDB240, BackwardWalkSingleLeafNoDuplicates) {
    // No split, no backward walk needed.  Single leaf with unique keys.
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 4; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan = tree.range_scan(make_key(20), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    // Keys 20, 30, 40.
    EXPECT_EQ(entries->size(), 3u);
    EXPECT_EQ(key_val((*entries)[0].first), 20);
}

TEST(QA_GDB240, BackwardWalkSingleLeafAllDuplicates) {
    // All duplicates fit in one leaf — no backward walk triggered.
    auto tree = make_test_index(10, 4, false);

    for (int i = 0; i < 10; ++i) {
        (void)tree.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
    }
    EXPECT_EQ(tree.size(), 10u);

    auto scan = tree.range_scan(make_key(42), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 10u);
}

TEST(QA_GDB240, BackwardWalkReachesFirstLeaf) {
    // All keys identical, many leaves.  Walk must go all the way to leaf 0.
    auto tree = make_test_index(2, 2, false);

    for (int i = 0; i < 20; ++i) {
        auto ins = tree.insert(make_key(99), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at i=" << i;
    }
    EXPECT_EQ(tree.size(), 20u);

    auto scan = tree.range_scan(make_key(99), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 20u);
}

TEST(QA_GDB240, BackwardWalkStopsAtCorrectLeaf) {
    // Keys: [1, 2, 3, ..., 5, 10, 10, 10, ..., 10 (x15)].
    // Backward walk for begin_key=10 should stop when prev leaf's
    // last key < 10, not walk into the leaves containing keys < 10.
    auto tree = make_test_index(3, 3, false);

    for (int i = 1; i <= 5; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }
    for (int i = 0; i < 15; ++i) {
        (void)tree.insert(make_key(10), make_rid(static_cast<uint32_t>(100 + i)));
    }
    EXPECT_EQ(tree.size(), 20u);

    auto scan = tree.range_scan(make_key(10), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    // Only the 15 entries with key=10.
    EXPECT_EQ(entries->size(), 15u);
    for (const auto& [key, rid] : *entries) {
        EXPECT_EQ(key_val(key), 10);
    }
}

TEST(QA_GDB240, BackwardWalkWithMixedLeaf) {
    // Previous leaf has mixed keys ending with begin_key.
    // E.g., prev leaf: [5, 10], current leaf: [10, 10].
    // Walk should include the prev leaf because last key (10) >= begin_key.
    auto tree = make_test_index(2, 2, false);

    (void)tree.insert(make_key(5), make_rid(1));
    for (int i = 0; i < 7; ++i) {
        (void)tree.insert(make_key(10), make_rid(static_cast<uint32_t>(10 + i)));
    }
    EXPECT_EQ(tree.size(), 8u);

    auto scan = tree.range_scan(make_key(10), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 7u);
    for (const auto& [key, rid] : *entries) {
        EXPECT_EQ(key_val(key), 10);
    }
}

// =============================================================================
// Exclusive End Key with Duplicates
// =============================================================================

TEST(QA_GDB240, EndKeyEqualToBeginKeyReturnsEmpty) {
    // end_key is exclusive, so range_scan(42, 42) should return nothing.
    auto tree = make_test_index(4, 4, false);

    for (int i = 0; i < 15; ++i) {
        (void)tree.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan = tree.range_scan(make_key(42), make_key(42));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 0u);
}

TEST(QA_GDB240, EndKeyOnePastDuplicatesReturnsAll) {
    // range_scan(42, 43) should return all 42s (end is exclusive).
    auto tree = make_test_index(3, 3, false);

    for (int i = 0; i < 12; ++i) {
        (void)tree.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan = tree.range_scan(make_key(42), make_key(43));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 12u);
}

// =============================================================================
// Multiple Groups of Duplicates
// =============================================================================

TEST(QA_GDB240, MultipleGroupsDuplicates) {
    // Three groups: key=10 (x8), key=20 (x8), key=30 (x8).
    // Each group individually exercises the backward walk.
    auto tree = make_test_index(3, 3, false);

    for (int k : {10, 20, 30}) {
        for (int i = 0; i < 8; ++i) {
            (void)tree.insert(make_key(k),
                              make_rid(static_cast<uint32_t>(k * 100 + i)));
        }
    }
    EXPECT_EQ(tree.size(), 24u);

    // Scan for each group with begin_key.
    for (int k : {10, 20, 30}) {
        auto scan = tree.range_scan(make_key(k), make_key(k + 1));
        ASSERT_TRUE(scan.has_value()) << "scan failed for key=" << k;
        auto entries = collect_scan(*scan);
        ASSERT_TRUE(entries.has_value()) << "collect failed for key=" << k;
        EXPECT_EQ(entries->size(), 8u) << "wrong count for key=" << k;
        for (const auto& [key, rid] : *entries) {
            EXPECT_EQ(key_val(key), k);
        }
    }
}

TEST(QA_GDB240, ScanMiddleGroupOnly) {
    // Insert keys [1..5], [50 x10], [100..105].
    // range_scan(50, 51) must return exactly 10 entries.
    auto tree = make_test_index(3, 3, false);

    for (int i = 1; i <= 5; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }
    for (int i = 0; i < 10; ++i) {
        (void)tree.insert(make_key(50), make_rid(static_cast<uint32_t>(500 + i)));
    }
    for (int i = 100; i <= 105; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }
    EXPECT_EQ(tree.size(), 21u);

    auto scan = tree.range_scan(make_key(50), make_key(51));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 10u);
}

// =============================================================================
// begin_key Not Present in Tree
// =============================================================================

TEST(QA_GDB240, BeginKeyBetweenDuplicateGroups) {
    // Keys: [10 x5, 30 x5].  begin_key=20 should return all 30s.
    auto tree = make_test_index(3, 3, false);

    for (int i = 0; i < 5; ++i) {
        (void)tree.insert(make_key(10), make_rid(static_cast<uint32_t>(10 + i)));
    }
    for (int i = 0; i < 5; ++i) {
        (void)tree.insert(make_key(30), make_rid(static_cast<uint32_t>(30 + i)));
    }

    auto scan = tree.range_scan(make_key(20), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 5u);
    for (const auto& [key, rid] : *entries) {
        EXPECT_EQ(key_val(key), 30);
    }
}

TEST(QA_GDB240, BeginKeyGreaterThanAllKeys) {
    // All keys < begin_key -> empty result.
    auto tree = make_test_index(3, 3, false);

    for (int i = 0; i < 10; ++i) {
        (void)tree.insert(make_key(5), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan = tree.range_scan(make_key(100), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 0u);
}

TEST(QA_GDB240, BeginKeyLessThanAllKeys) {
    // All keys > begin_key -> return everything.
    auto tree = make_test_index(3, 3, false);

    for (int i = 0; i < 10; ++i) {
        (void)tree.insert(make_key(50), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan = tree.range_scan(make_key(1), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 10u);
}

// =============================================================================
// Boundary Key Values
// =============================================================================

TEST(QA_GDB240, DuplicateInt64MaxKeys) {
    // Duplicate keys at INT64_MAX boundary.
    auto tree = make_test_index(3, 3, false);
    const auto max_val = std::numeric_limits<int64_t>::max();

    for (int i = 0; i < 10; ++i) {
        auto ins = tree.insert(make_key(max_val), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at i=" << i;
    }

    auto scan = tree.range_scan(make_key(max_val), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 10u);
}

TEST(QA_GDB240, DuplicateInt64MinKeys) {
    // Duplicate keys at INT64_MIN boundary.
    auto tree = make_test_index(3, 3, false);
    const auto min_val = std::numeric_limits<int64_t>::min();

    for (int i = 0; i < 10; ++i) {
        auto ins = tree.insert(make_key(min_val), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at i=" << i;
    }

    auto scan = tree.range_scan(make_key(min_val), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 10u);
}

TEST(QA_GDB240, DuplicateZeroKeys) {
    // Duplicate keys at zero.
    auto tree = make_test_index(3, 3, false);

    for (int i = 0; i < 10; ++i) {
        (void)tree.insert(make_key(0), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan = tree.range_scan(make_key(0), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 10u);
}

TEST(QA_GDB240, DuplicateNegativeKeys) {
    // Duplicate keys with negative values.
    auto tree = make_test_index(3, 3, false);

    for (int i = 0; i < 10; ++i) {
        (void)tree.insert(make_key(-42), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan = tree.range_scan(make_key(-42), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 10u);
}

// =============================================================================
// RID Completeness After Backward Walk
// =============================================================================

TEST(QA_GDB240, AllRIDsPresentAfterBackwardWalk) {
    // Insert 30 entries with same key across many leaves.
    // Verify every single RID is returned.
    auto tree = make_test_index(3, 3, false);

    std::vector<uint32_t> expected_rids;
    for (int i = 0; i < 30; ++i) {
        expected_rids.push_back(static_cast<uint32_t>(i));
        (void)tree.insert(make_key(77), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan = tree.range_scan(make_key(77), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 30u);

    std::vector<uint32_t> actual_rids;
    for (const auto& [key, rid] : *entries) {
        actual_rids.push_back(rid.page_id);
    }
    std::sort(actual_rids.begin(), actual_rids.end());
    std::sort(expected_rids.begin(), expected_rids.end());
    EXPECT_EQ(actual_rids, expected_rids);
}

// =============================================================================
// Duplicates After Deletions
// =============================================================================

TEST(QA_GDB240, RangeScanAfterPartialDeletion) {
    // Insert 20 duplicates of key=42, delete some (remove deletes one at a
    // time), then range_scan(42) should return the remainder.
    auto tree = make_test_index(4, 4, false);

    for (int i = 0; i < 20; ++i) {
        (void)tree.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
    }
    EXPECT_EQ(tree.size(), 20u);

    // Remove 5 entries (remove() removes one entry per call for non-unique).
    for (int i = 0; i < 5; ++i) {
        auto r = tree.remove(make_key(42));
        ASSERT_TRUE(r.has_value());
        ASSERT_TRUE(*r);
    }
    EXPECT_EQ(tree.size(), 15u);

    auto scan = tree.range_scan(make_key(42), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 15u);
}

// =============================================================================
// Open-Begin vs Begin-Key Equivalence
// =============================================================================

TEST(QA_GDB240, OpenBeginEquivalenceWithDuplicatesSmallCapacity) {
    // With leaf_max=2, duplicates create many leaves.
    // range_scan(K, nullopt) must equal range_scan(nullopt, nullopt) when
    // all keys in the tree equal K.
    auto tree = make_test_index(2, 2, false);

    for (int i = 0; i < 15; ++i) {
        (void)tree.insert(make_key(33), make_rid(static_cast<uint32_t>(i)));
    }

    auto open_scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(open_scan.has_value());
    auto open_entries = collect_scan(*open_scan);
    ASSERT_TRUE(open_entries.has_value());

    auto keyed_scan = tree.range_scan(make_key(33), std::nullopt);
    ASSERT_TRUE(keyed_scan.has_value());
    auto keyed_entries = collect_scan(*keyed_scan);
    ASSERT_TRUE(keyed_entries.has_value());

    EXPECT_EQ(keyed_entries->size(), open_entries->size());
    EXPECT_EQ(keyed_entries->size(), 15u);
}

// =============================================================================
// Bulk Load Then Range Scan with Duplicates
// =============================================================================

TEST(QA_GDB240, BulkLoadDuplicatesThenRangeScan) {
    auto tree = make_test_index(4, 4, false);

    // Sorted input with duplicates.
    std::vector<std::pair<KeyType, RID>> sorted;
    for (int i = 0; i < 5; ++i) {
        sorted.push_back({make_key(10), make_rid(static_cast<uint32_t>(10 + i))});
    }
    for (int i = 0; i < 10; ++i) {
        sorted.push_back({make_key(20), make_rid(static_cast<uint32_t>(20 + i))});
    }
    for (int i = 0; i < 5; ++i) {
        sorted.push_back({make_key(30), make_rid(static_cast<uint32_t>(30 + i))});
    }

    auto load = tree.bulk_load(sorted);
    ASSERT_TRUE(load.has_value());
    EXPECT_EQ(tree.size(), 20u);

    // range_scan(20, 21) should return all 10 entries with key=20.
    auto scan = tree.range_scan(make_key(20), make_key(21));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 10u);
    for (const auto& [key, rid] : *entries) {
        EXPECT_EQ(key_val(key), 20);
    }
}

// =============================================================================
// Composite Key Duplicates
// =============================================================================

TEST(QA_GDB240, CompositeKeyPartialDuplicates) {
    // Composite keys where first column is the same.
    // (10, "a"), (10, "b"), (10, "c"), (10, "d"), (10, "e"), (20, "a").
    // range_scan((10, "a")) should return all entries >= (10, "a").
    BTreeConfig config;
    config.key_types = {TypeId::INT64, TypeId::STRING};
    config.leaf_max_keys = 2;
    config.internal_max_keys = 2;
    config.is_unique = false;
    BTreeIndex tree(std::move(config));

    std::vector<std::string> suffixes = {"a", "b", "c", "d", "e"};
    for (size_t i = 0; i < suffixes.size(); ++i) {
        auto ins = tree.insert(make_composite_key(10, suffixes[i]),
                               make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at suffix=" << suffixes[i];
    }
    (void)tree.insert(make_composite_key(20, "a"), make_rid(99));
    EXPECT_EQ(tree.size(), 6u);

    // Scan from (10, "a") -> should return all 6 entries.
    auto scan = tree.range_scan(make_composite_key(10, "a"), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 6u);

    // Scan from (10, "c") -> should return (10,"c"), (10,"d"), (10,"e"), (20,"a") = 4.
    auto scan2 = tree.range_scan(make_composite_key(10, "c"), std::nullopt);
    ASSERT_TRUE(scan2.has_value());
    auto entries2 = collect_scan(*scan2);
    ASSERT_TRUE(entries2.has_value());
    EXPECT_EQ(entries2->size(), 4u);
}

// =============================================================================
// Stress: Large Number of Duplicates
// =============================================================================

TEST(QA_GDB240, Stress500DuplicatesSmallCapacity) {
    auto tree = make_test_index(3, 3, false);

    for (int i = 0; i < 500; ++i) {
        auto ins = tree.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at i=" << i;
    }
    EXPECT_EQ(tree.size(), 500u);

    auto scan = tree.range_scan(make_key(42), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 500u);

    // Verify every RID present.
    std::vector<uint32_t> rids;
    for (const auto& [key, rid] : *entries) {
        rids.push_back(rid.page_id);
    }
    std::sort(rids.begin(), rids.end());
    for (int i = 0; i < 500; ++i) {
        EXPECT_EQ(rids[static_cast<size_t>(i)], static_cast<uint32_t>(i)) << "missing RID " << i;
    }
}

TEST(QA_GDB240, StressMixedDuplicatesAndUniques) {
    // 200 unique keys + 200 duplicates of key=500.
    auto tree = make_test_index(4, 4, false);

    for (int i = 1; i <= 200; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }
    for (int i = 0; i < 200; ++i) {
        (void)tree.insert(make_key(500), make_rid(static_cast<uint32_t>(1000 + i)));
    }
    EXPECT_EQ(tree.size(), 400u);

    // range_scan(500) should return exactly 200 entries.
    auto scan = tree.range_scan(make_key(500), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 200u);
    for (const auto& [key, rid] : *entries) {
        EXPECT_EQ(key_val(key), 500);
    }
}

// =============================================================================
// Regression: Unique Index Should Not Be Affected
// =============================================================================

TEST(QA_GDB240, UniqueIndexRangeScanUnchanged) {
    // The backward walk should be harmless for unique indices (no duplicates).
    auto tree = make_test_index(4, 4, true);

    for (int i = 1; i <= 30; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // range_scan(150) -> keys 150, 160, ..., 300 = 16 entries.
    auto scan = tree.range_scan(make_key(150), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 16u);
    EXPECT_EQ(key_val((*entries)[0].first), 150);
    EXPECT_EQ(key_val((*entries)[15].first), 300);
}

// =============================================================================
// Empty Tree Edge Case
// =============================================================================

TEST(QA_GDB240, RangeScanOnEmptyTree) {
    auto tree = make_test_index(4, 4);

    auto scan = tree.range_scan(make_key(42), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 0u);
}

// =============================================================================
// Single Entry Tree
// =============================================================================

TEST(QA_GDB240, SingleEntryRangeScanWithBeginKey) {
    auto tree = make_test_index(4, 4, false);

    (void)tree.insert(make_key(42), make_rid(1));

    auto scan = tree.range_scan(make_key(42), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 1u);
}

// =============================================================================
// Two Entries Same Key (Exactly One Leaf)
// =============================================================================

TEST(QA_GDB240, TwoEntriesSameKeyNoSplit) {
    auto tree = make_test_index(4, 4, false);

    (void)tree.insert(make_key(42), make_rid(1));
    (void)tree.insert(make_key(42), make_rid(2));

    auto scan = tree.range_scan(make_key(42), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 2u);
}

// =============================================================================
// Exactly max_keys + 1 Duplicates (One Split)
// =============================================================================

TEST(QA_GDB240, ExactlyOneSplitDuplicates) {
    // leaf_max=4, insert 5 duplicates -> exactly one split.
    // This is the minimal case where the backward walk is needed.
    auto tree = make_test_index(4, 4, false);

    for (int i = 0; i < 5; ++i) {
        (void)tree.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan = tree.range_scan(make_key(42), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 5u);
}

// =============================================================================
// Insert Order Variations
// =============================================================================

TEST(QA_GDB240, DuplicatesInsertedAfterOtherKeys) {
    // Insert unique keys first, then a batch of duplicates after.
    auto tree = make_test_index(3, 3, false);

    for (int i = 1; i <= 10; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }
    // Now insert 10 duplicates of key=5 (middle of range).
    for (int i = 0; i < 10; ++i) {
        (void)tree.insert(make_key(5), make_rid(static_cast<uint32_t>(100 + i)));
    }
    EXPECT_EQ(tree.size(), 20u);

    // range_scan(5, 6) should return 11 entries: 1 original + 10 duplicates.
    auto scan = tree.range_scan(make_key(5), make_key(6));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 11u);
    for (const auto& [key, rid] : *entries) {
        EXPECT_EQ(key_val(key), 5);
    }
}

// =============================================================================
// Concurrent-Safe: Multiple Range Scans on Same Tree
// =============================================================================

TEST(QA_GDB240, TwoSequentialScansReturnSameResults) {
    auto tree = make_test_index(3, 3, false);

    for (int i = 0; i < 20; ++i) {
        (void)tree.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
    }

    // First scan.
    auto scan1 = tree.range_scan(make_key(42), std::nullopt);
    ASSERT_TRUE(scan1.has_value());
    auto entries1 = collect_scan(*scan1);
    ASSERT_TRUE(entries1.has_value());

    // Second scan on same tree (should produce identical results).
    auto scan2 = tree.range_scan(make_key(42), std::nullopt);
    ASSERT_TRUE(scan2.has_value());
    auto entries2 = collect_scan(*scan2);
    ASSERT_TRUE(entries2.has_value());

    EXPECT_EQ(entries1->size(), entries2->size());
    EXPECT_EQ(entries1->size(), 20u);
}

// =============================================================================
// Various Leaf Capacities
// =============================================================================

TEST(QA_GDB240, DuplicatesAcrossCapacities) {
    // Test the fix works for leaf_max = 1, 2, 3, 4, 5, 8.
    for (uint16_t cap : {1, 2, 3, 4, 5, 8}) {
        auto tree = make_test_index(cap, cap, false);

        int count = cap * 5; // Enough to span multiple leaves.
        for (int i = 0; i < count; ++i) {
            auto ins = tree.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
            ASSERT_TRUE(ins.has_value()) << "cap=" << cap << " i=" << i;
        }

        auto scan = tree.range_scan(make_key(42), std::nullopt);
        ASSERT_TRUE(scan.has_value()) << "cap=" << cap;
        auto entries = collect_scan(*scan);
        ASSERT_TRUE(entries.has_value()) << "cap=" << cap;
        EXPECT_EQ(entries->size(), static_cast<size_t>(count))
            << "cap=" << cap << " expected=" << count;
    }
}
