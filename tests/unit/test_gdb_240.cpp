/// Tests for GDB-240: B+ tree range_scan with begin_key misses duplicate
/// key entries in earlier leaves.
///
/// Verifies that range_scan correctly walks backward through prev_leaf_id
/// pointers to find the leftmost leaf when duplicate keys span multiple leaves.

#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <vector>

#include "test_btree_helpers.h"

using namespace giodb;
using namespace giodb::test;

// =============================================================================
// Core Bug Fix: range_scan with begin_key and duplicate keys
// =============================================================================

TEST(GDB240, RangeScanBeginKeyFindsAllDuplicates) {
    // 20 entries with key=42, leaf_max=4 -> spans 5 leaves.
    // range_scan(begin_key=42) must return all 20.
    auto tree = make_test_index(4, 4, false);

    for (int i = 0; i < 20; ++i) {
        auto ins = tree.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at i=" << i;
    }
    EXPECT_EQ(tree.size(), 20u);

    auto scan = tree.range_scan(make_key(42), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 20u);
}

TEST(GDB240, RangeScanBeginKeyMatchesOpenBeginScan) {
    // The result of range_scan(begin_key=K) on an all-duplicates tree
    // must match range_scan(nullopt, nullopt).
    auto tree = make_test_index(4, 4, false);

    for (int i = 0; i < 20; ++i) {
        (void)tree.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan_with_key = tree.range_scan(make_key(42), std::nullopt);
    ASSERT_TRUE(scan_with_key.has_value());
    auto keyed = collect_scan(*scan_with_key);
    ASSERT_TRUE(keyed.has_value());

    auto scan_open = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan_open.has_value());
    auto open = collect_scan(*scan_open);
    ASSERT_TRUE(open.has_value());

    EXPECT_EQ(keyed->size(), open->size());
}

// =============================================================================
// Duplicates spanning exactly two leaves
// =============================================================================

TEST(GDB240, DuplicatesSpanTwoLeaves) {
    // leaf_max=4, insert 5 duplicate keys -> 2 leaves.
    auto tree = make_test_index(4, 4, false);

    for (int i = 0; i < 5; ++i) {
        (void)tree.insert(make_key(10), make_rid(static_cast<uint32_t>(i)));
    }
    EXPECT_EQ(tree.size(), 5u);

    auto scan = tree.range_scan(make_key(10), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 5u);
}

// =============================================================================
// Duplicates mixed with other keys
// =============================================================================

TEST(GDB240, DuplicatesMixedWithUniqueKeys) {
    // Insert unique keys + many duplicates of a middle key.
    // range_scan(begin_key=middle) must return all duplicates + higher keys.
    auto tree = make_test_index(3, 3, false);

    // Insert keys: 10, 20, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 40, 50
    (void)tree.insert(make_key(10), make_rid(1));
    (void)tree.insert(make_key(20), make_rid(2));
    for (int i = 0; i < 10; ++i) {
        (void)tree.insert(make_key(30), make_rid(static_cast<uint32_t>(100 + i)));
    }
    (void)tree.insert(make_key(40), make_rid(3));
    (void)tree.insert(make_key(50), make_rid(4));

    EXPECT_EQ(tree.size(), 14u);

    // range_scan(30, nullopt) -> all 10 copies of 30 + key 40 + key 50 = 12
    auto scan = tree.range_scan(make_key(30), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 12u);

    // Verify first 10 entries are key=30
    for (size_t i = 0; i < 10; ++i) {
        EXPECT_EQ(key_val((*entries)[i].first), 30) << "entry " << i;
    }
    EXPECT_EQ(key_val((*entries)[10].first), 40);
    EXPECT_EQ(key_val((*entries)[11].first), 50);
}

// =============================================================================
// Bounded range scan with duplicates
// =============================================================================

TEST(GDB240, BoundedRangeScanWithDuplicates) {
    // range_scan(begin_key=42, end_key=43) must return all duplicates of 42.
    auto tree = make_test_index(4, 4, false);

    for (int i = 0; i < 15; ++i) {
        (void)tree.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
    }
    EXPECT_EQ(tree.size(), 15u);

    auto scan = tree.range_scan(make_key(42), make_key(43));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 15u);
}

// =============================================================================
// begin_key that doesn't exist (no backward walk needed)
// =============================================================================

TEST(GDB240, BeginKeyNotPresentNoBackwardWalk) {
    // Ensure the fix doesn't regress the normal case where begin_key is unique.
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 20; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // range_scan(begin_key=55) -> keys 60, 70, ..., 200 = 15 entries
    auto scan = tree.range_scan(make_key(55), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 15u);
    EXPECT_EQ(key_val((*entries)[0].first), 60);
}

// =============================================================================
// Minimal capacity with duplicates
// =============================================================================

TEST(GDB240, MinCapacityDuplicates) {
    // leaf_max=1 -> every insert creates a new leaf. 10 duplicates = 10 leaves.
    auto tree = make_test_index(1, 1, false);

    for (int i = 0; i < 10; ++i) {
        auto ins = tree.insert(make_key(5), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at i=" << i;
    }
    EXPECT_EQ(tree.size(), 10u);

    auto scan = tree.range_scan(make_key(5), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 10u);
}

// =============================================================================
// RID correctness: all RIDs are present after the fix
// =============================================================================

TEST(GDB240, AllRIDsPresent) {
    auto tree = make_test_index(4, 4, false);

    std::vector<uint32_t> expected_rids;
    for (int i = 0; i < 20; ++i) {
        expected_rids.push_back(static_cast<uint32_t>(i));
        (void)tree.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan = tree.range_scan(make_key(42), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 20u);

    // Collect all returned RID page_ids and verify the full set.
    std::vector<uint32_t> actual_rids;
    for (const auto& [key, rid] : *entries) {
        actual_rids.push_back(rid.page_id);
    }
    std::sort(actual_rids.begin(), actual_rids.end());
    std::sort(expected_rids.begin(), expected_rids.end());
    EXPECT_EQ(actual_rids, expected_rids);
}
