/// @file test_qa_gdb_17.cpp
/// QA adversarial tests for GDB-17: B+ Tree Index.
///
/// Designed to break the implementation with edge cases, boundary values,
/// error paths, and stress tests across all subtasks (GDB-92..GDB-97).

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <numeric>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "test_btree_helpers.h"

using namespace giodb;
using namespace giodb::test;

// =============================================================================
// Helper: verify tree integrity by searching all expected keys and doing a
// full range scan to check sorted order and count.
// =============================================================================

namespace {

void verify_tree_integrity(BTreeIndex& tree, const std::set<int64_t>& expected_keys) {
    // All expected keys must be found.
    for (int64_t k : expected_keys) {
        auto result = tree.search(make_key(k));
        ASSERT_TRUE(result.has_value()) << "search() returned error for key " << k;
        EXPECT_TRUE(result->has_value()) << "Key " << k << " not found";
    }

    // Size must match.
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(expected_keys.size()));

    // Full range scan must be sorted and have correct count.
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), expected_keys.size());

    for (size_t i = 1; i < entries->size(); ++i) {
        EXPECT_LT(key_val((*entries)[i - 1].first), key_val((*entries)[i].first))
            << "Not sorted at scan index " << i;
    }
}

} // namespace

// =============================================================================
// Edge Case: max_keys = 1 (minimum valid configuration)
// =============================================================================

TEST(BTreeQA_EdgeCase, MaxKeysOne_InsertThree) {
    auto tree = make_test_index(1, 1);

    for (int i = 1; i <= 3; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Failed to insert key " << i;
    }

    std::set<int64_t> expected = {1, 2, 3};
    verify_tree_integrity(tree, expected);
}

TEST(BTreeQA_EdgeCase, MaxKeysOne_InsertTen) {
    auto tree = make_test_index(1, 1);

    std::set<int64_t> expected;
    for (int i = 1; i <= 10; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Failed to insert key " << i;
        expected.insert(i);
    }

    verify_tree_integrity(tree, expected);
}

TEST(BTreeQA_EdgeCase, MaxKeysOne_DeleteAll) {
    auto tree = make_test_index(1, 1);

    for (int i = 1; i <= 5; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value());
    }

    for (int i = 1; i <= 5; ++i) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value()) << "remove() returned error for key " << i;
        EXPECT_TRUE(*del) << "Key " << i << " not found for deletion";
    }

    EXPECT_TRUE(tree.empty());
    EXPECT_EQ(tree.size(), 0u);
}

TEST(BTreeQA_EdgeCase, MaxKeysOne_InsertDeleteInterleaved) {
    auto tree = make_test_index(1, 1);

    // Insert 1, 2, 3 then delete 2, insert 4, delete 1.
    ASSERT_TRUE(tree.insert(make_key(1), make_rid(1)).has_value());
    ASSERT_TRUE(tree.insert(make_key(2), make_rid(2)).has_value());
    ASSERT_TRUE(tree.insert(make_key(3), make_rid(3)).has_value());

    auto del = tree.remove(make_key(2));
    ASSERT_TRUE(del.has_value());
    EXPECT_TRUE(*del);

    ASSERT_TRUE(tree.insert(make_key(4), make_rid(4)).has_value());

    del = tree.remove(make_key(1));
    ASSERT_TRUE(del.has_value());
    EXPECT_TRUE(*del);

    std::set<int64_t> expected = {3, 4};
    verify_tree_integrity(tree, expected);
}

// =============================================================================
// Edge Case: max_keys = 2
// =============================================================================

TEST(BTreeQA_EdgeCase, MaxKeysTwo_InsertAndDeleteAll) {
    auto tree = make_test_index(2, 2);

    std::set<int64_t> expected;
    for (int i = 1; i <= 10; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Failed to insert key " << i;
        expected.insert(i);
    }

    verify_tree_integrity(tree, expected);

    // Delete all keys one by one.
    for (int i = 1; i <= 10; ++i) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value()) << "remove() error for key " << i;
        EXPECT_TRUE(*del) << "Key " << i << " not found for deletion";
        expected.erase(i);

        // After each delete, verify remaining keys are intact.
        verify_tree_integrity(tree, expected);
    }

    EXPECT_TRUE(tree.empty());
}

TEST(BTreeQA_EdgeCase, MaxKeysTwo_ReverseInsert) {
    auto tree = make_test_index(2, 2);

    std::set<int64_t> expected;
    for (int i = 10; i >= 1; --i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Failed to insert key " << i;
        expected.insert(i);
    }

    verify_tree_integrity(tree, expected);
}

// =============================================================================
// Delete: Specific Merge/Redistribute Paths
// =============================================================================

TEST(BTreeQA_Delete, TriggerLeftRedistribution) {
    // Build a tree where deleting from a non-leftmost leaf forces
    // redistribution from the left sibling (left has > min_keys).
    auto tree = make_test_index(4, 4);
    // min_keys = (4+1)/2 = 2

    // Insert 8 keys to get 2+ leaves.
    for (int i = 1; i <= 8; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    // Delete from the right side to trigger left redistribution.
    // After deleting enough from the right leaf, it becomes underfull
    // and steals from the left.
    std::set<int64_t> expected;
    for (int i = 1; i <= 8; ++i) {
        expected.insert(i * 10);
    }

    // Delete keys from the upper range one at a time.
    for (int k : {70, 80}) {
        auto del = tree.remove(make_key(k));
        ASSERT_TRUE(del.has_value()) << "remove() error for key " << k;
        expected.erase(k);
    }

    verify_tree_integrity(tree, expected);
}

TEST(BTreeQA_Delete, TriggerRightRedistribution) {
    // Trigger redistribution from the right sibling.
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 8; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    std::set<int64_t> expected;
    for (int i = 1; i <= 8; ++i) {
        expected.insert(i * 10);
    }

    // Delete keys from the lower range.
    for (int k : {10, 20}) {
        auto del = tree.remove(make_key(k));
        ASSERT_TRUE(del.has_value()) << "remove() error for key " << k;
        expected.erase(k);
    }

    verify_tree_integrity(tree, expected);
}

TEST(BTreeQA_Delete, TriggerLeftMerge) {
    // Force a left merge by depleting both the leaf and its left sibling
    // below redistribution threshold.
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 12; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    std::set<int64_t> expected;
    for (int i = 1; i <= 12; ++i) {
        expected.insert(i * 10);
    }

    // Delete enough keys to force merges.
    for (int k : {50, 60, 70, 80, 90, 100}) {
        auto del = tree.remove(make_key(k));
        ASSERT_TRUE(del.has_value());
        expected.erase(k);
    }

    verify_tree_integrity(tree, expected);
}

TEST(BTreeQA_Delete, TriggerRightMerge) {
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 12; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    std::set<int64_t> expected;
    for (int i = 1; i <= 12; ++i) {
        expected.insert(i * 10);
    }

    // Delete from left side to force right merges.
    for (int k : {10, 20, 30, 40, 50, 60}) {
        auto del = tree.remove(make_key(k));
        ASSERT_TRUE(del.has_value());
        expected.erase(k);
    }

    verify_tree_integrity(tree, expected);
}

TEST(BTreeQA_Delete, CascadingInternalMerges) {
    // Use max_keys = 3 to create a deep tree, then delete aggressively
    // to trigger cascading internal merges.
    auto tree = make_test_index(3, 3);

    std::set<int64_t> expected;
    for (int i = 1; i <= 30; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
        expected.insert(i);
    }

    // Delete in a pattern that triggers cascading merges.
    for (int i = 1; i <= 20; ++i) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value()) << "remove() error for key " << i;
        EXPECT_TRUE(*del) << "Key " << i << " not deleted";
        expected.erase(i);
    }

    verify_tree_integrity(tree, expected);
}

TEST(BTreeQA_Delete, MultipleRootShrinks) {
    // Force multiple root shrinks by creating a tall tree and deleting most keys.
    auto tree = make_test_index(3, 3);

    std::set<int64_t> expected;
    for (int i = 1; i <= 50; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
        expected.insert(i);
    }

    // Delete all but 2 keys, forcing the tree to shrink from multiple levels
    // down to a single root leaf.
    for (int i = 1; i <= 48; ++i) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value()) << "remove() error at key " << i;
        EXPECT_TRUE(*del) << "Key " << i << " not found";
        expected.erase(i);
    }

    EXPECT_EQ(tree.size(), 2u);
    verify_tree_integrity(tree, expected);
}

TEST(BTreeQA_Delete, DeleteInReverseOrder) {
    auto tree = make_test_index(4, 4);

    std::set<int64_t> expected;
    for (int i = 1; i <= 20; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i))).has_value());
        expected.insert(i * 10);
    }

    // Delete in reverse insertion order.
    for (int i = 20; i >= 1; --i) {
        auto del = tree.remove(make_key(i * 10));
        ASSERT_TRUE(del.has_value());
        EXPECT_TRUE(*del);
        expected.erase(i * 10);
        verify_tree_integrity(tree, expected);
    }

    EXPECT_TRUE(tree.empty());
}

TEST(BTreeQA_Delete, DeleteFromMiddleRepeatedly) {
    // Delete keys from the middle of the key range to stress redistribute/merge
    // from both sides.
    auto tree = make_test_index(4, 4);

    std::set<int64_t> expected;
    for (int i = 1; i <= 20; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
        expected.insert(i);
    }

    // Delete middle keys: 10, 11, 9, 12, 8, 13, ...
    std::vector<int> delete_order;
    int lo = 10, hi = 11;
    for (int round = 0; round < 10; ++round) {
        if (round % 2 == 0) {
            delete_order.push_back(lo--);
        } else {
            delete_order.push_back(hi++);
        }
    }

    for (int k : delete_order) {
        auto del = tree.remove(make_key(k));
        ASSERT_TRUE(del.has_value());
        EXPECT_TRUE(*del) << "Key " << k << " not found";
        expected.erase(k);
    }

    verify_tree_integrity(tree, expected);
}

// =============================================================================
// Internal Node Merge Overflow (odd max_keys)
// =============================================================================

TEST(BTreeQA_InternalMerge, OddMaxKeysThree_HeavyDelete) {
    // With max_keys = 3, min_keys = 2. Internal merges add a parent separator,
    // potentially creating a node with > max_keys entries.
    // Verify the tree remains functional after heavy deletions.
    auto tree = make_test_index(3, 3);

    std::set<int64_t> expected;
    for (int i = 1; i <= 40; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
        expected.insert(i);
    }

    // Delete keys to trigger internal merges.
    // Delete from both ends to maximize merge pressure on middle internal nodes.
    for (int i = 1; i <= 15; ++i) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value());
        expected.erase(i);
    }
    for (int i = 40; i >= 30; --i) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value());
        expected.erase(i);
    }

    verify_tree_integrity(tree, expected);

    // Continue deleting until only 2 keys remain.
    for (int i = 16; i <= 27; ++i) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value());
        expected.erase(i);
    }

    verify_tree_integrity(tree, expected);
}

TEST(BTreeQA_InternalMerge, OddMaxKeysFive_HeavyDelete) {
    // Same test with max_keys = 5 (also odd).
    auto tree = make_test_index(5, 5);

    std::set<int64_t> expected;
    for (int i = 1; i <= 80; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
        expected.insert(i);
    }

    // Delete 60 keys to force many internal merges.
    for (int i = 1; i <= 60; ++i) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value());
        expected.erase(i);
    }

    verify_tree_integrity(tree, expected);
}

TEST(BTreeQA_InternalMerge, OddMaxKeys_InsertAfterMerge) {
    // After heavy deletions with odd max_keys, verify inserts still work.
    auto tree = make_test_index(3, 3);

    std::set<int64_t> expected;
    for (int i = 1; i <= 30; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
        expected.insert(i);
    }

    // Delete most keys.
    for (int i = 1; i <= 25; ++i) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value());
        expected.erase(i);
    }

    // Now re-insert into the (potentially overfull) tree.
    for (int i = 100; i <= 120; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Failed to insert key " << i << " after merges";
        expected.insert(i);
    }

    verify_tree_integrity(tree, expected);
}

// =============================================================================
// Range Scan Edge Cases
// =============================================================================

TEST(BTreeQA_RangeScan, BeginAfterAllKeys) {
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 10; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    // begin = 200, well past all keys.
    auto scan = tree.range_scan(make_key(200), std::nullopt);
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 0u);
}

TEST(BTreeQA_RangeScan, EndBeforeAllKeys) {
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 10; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    // end = 5, before all keys.
    auto scan = tree.range_scan(std::nullopt, make_key(5));
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 0u);
}

TEST(BTreeQA_RangeScan, SingleElementResult) {
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 10; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    // Range [50, 60) should return only key 50.
    auto scan = tree.range_scan(make_key(50), make_key(60));
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 1u);
    EXPECT_EQ(key_val((*entries)[0].first), 50);
}

TEST(BTreeQA_RangeScan, BeginEqualsEndOnExistingKey) {
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 10; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    // Range [50, 50) — exclusive end means 50 is NOT included.
    auto scan = tree.range_scan(make_key(50), make_key(50));
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 0u);
}

TEST(BTreeQA_RangeScan, BeginGreaterThanEnd) {
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 10; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    // Range [80, 20) — begin > end, should return empty.
    auto scan = tree.range_scan(make_key(80), make_key(20));
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 0u);
}

TEST(BTreeQA_RangeScan, NegativeKeys) {
    auto tree = make_test_index(4, 4);

    // Insert negative keys.
    for (int i = -10; i <= 10; ++i) {
        ASSERT_TRUE(
            tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i + 100))).has_value());
    }

    // Range [-5, 5).
    auto scan = tree.range_scan(make_key(-5), make_key(5));
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    // Keys: -5, -4, -3, -2, -1, 0, 1, 2, 3, 4 → 10 entries.
    EXPECT_EQ(entries->size(), 10u);
    EXPECT_EQ(key_val((*entries)[0].first), -5);
    EXPECT_EQ(key_val(entries->back().first), 4);
}

TEST(BTreeQA_RangeScan, RangeScanAcrossManySplits) {
    auto tree = make_test_index(3, 3);

    for (int i = 1; i <= 100; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    // Full range scan.
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 100u);

    for (size_t i = 0; i < entries->size(); ++i) {
        EXPECT_EQ(key_val((*entries)[i].first), static_cast<int64_t>(i + 1));
    }
}

// =============================================================================
// Unique Constraint Edge Cases
// =============================================================================

TEST(BTreeQA_Unique, DuplicateAfterSplit) {
    // Insert enough keys to cause a split, then try to insert a duplicate
    // that would be in the split portion.
    auto tree = make_test_index(4, 4, true);

    for (int i = 1; i <= 5; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    // Key 30 should exist in the tree (possibly in either leaf after split).
    auto dup = tree.insert(make_key(30), make_rid(99));
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::CONSTRAINT_VIOLATION);
    EXPECT_EQ(tree.size(), 5u);
}

TEST(BTreeQA_Unique, DuplicateAtLeafBoundary) {
    // The separator key after a split is a copy of a leaf key.
    // Try to insert a duplicate of the separator.
    auto tree = make_test_index(4, 4, true);

    // Insert 1, 2, 3, 4, 5. After leaf split, the separator might be 3.
    for (int i = 1; i <= 5; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    // Try duplicating each key.
    for (int i = 1; i <= 5; ++i) {
        auto dup = tree.insert(make_key(i), make_rid(99));
        ASSERT_FALSE(dup.has_value()) << "Duplicate key " << i << " was accepted";
        EXPECT_EQ(dup.error().code, StatusCode::CONSTRAINT_VIOLATION);
    }
    EXPECT_EQ(tree.size(), 5u);
}

TEST(BTreeQA_Unique, UniqueCompositeKey) {
    BTreeConfig config;
    config.key_types = {TypeId::INT64, TypeId::STRING};
    config.leaf_max_keys = 4;
    config.internal_max_keys = 4;
    config.is_unique = true;
    BTreeIndex tree(std::move(config));

    ASSERT_TRUE(tree.insert(make_composite_key(1, "a"), make_rid(1)).has_value());
    ASSERT_TRUE(tree.insert(make_composite_key(1, "b"), make_rid(2)).has_value());
    ASSERT_TRUE(tree.insert(make_composite_key(2, "a"), make_rid(3)).has_value());

    // Exact duplicate should fail.
    auto dup = tree.insert(make_composite_key(1, "a"), make_rid(4));
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::CONSTRAINT_VIOLATION);
    EXPECT_EQ(tree.size(), 3u);
}

TEST(BTreeQA_Unique, InsertMaxThenDuplicate) {
    // Fill a leaf to max_keys with unique keys, then try to insert a duplicate.
    auto tree = make_test_index(4, 4, true);

    for (int i = 1; i <= 4; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    // Leaf is full. Inserting a duplicate should fail BEFORE splitting.
    auto dup = tree.insert(make_key(20), make_rid(99));
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::CONSTRAINT_VIOLATION);
    // Size should not change.
    EXPECT_EQ(tree.size(), 4u);
}

TEST(BTreeQA_Unique, UniqueAfterMultipleSplits) {
    auto tree = make_test_index(3, 3, true);

    for (int i = 1; i <= 20; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    // Every key should reject duplicates.
    for (int i = 1; i <= 20; ++i) {
        auto dup = tree.insert(make_key(i), make_rid(99));
        ASSERT_FALSE(dup.has_value()) << "Duplicate key " << i << " was accepted";
    }
    EXPECT_EQ(tree.size(), 20u);
}

// =============================================================================
// Bulk Load Edge Cases
// =============================================================================

TEST(BTreeQA_BulkLoad, ExactlyLeafMax) {
    auto tree = make_test_index(10, 10);

    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 1; i <= 10; ++i) {
        entries.push_back({make_key(i), make_rid(static_cast<uint32_t>(i))});
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 10u);

    std::set<int64_t> expected;
    for (int i = 1; i <= 10; ++i) {
        expected.insert(i);
    }
    verify_tree_integrity(tree, expected);
}

TEST(BTreeQA_BulkLoad, LeafMaxPlusOne) {
    auto tree = make_test_index(10, 10);

    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 1; i <= 11; ++i) {
        entries.push_back({make_key(i), make_rid(static_cast<uint32_t>(i))});
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 11u);

    // All keys searchable.
    for (int i = 1; i <= 11; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value()) << "Key " << i << " not found";
    }
}

TEST(BTreeQA_BulkLoad, TenThousandEntries) {
    // Per acceptance criteria: "bulk load 10K entries, verify tree structure"
    auto tree = make_test_index(50, 50);

    std::vector<std::pair<KeyType, RID>> entries;
    entries.reserve(10000);
    for (int i = 0; i < 10000; ++i) {
        entries.push_back({make_key(i), make_rid(static_cast<uint32_t>(i))});
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 10000u);

    // Spot-check specific keys.
    for (int k : {0, 1, 4999, 5000, 9998, 9999}) {
        auto s = tree.search(make_key(k));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value()) << "Key " << k << " not found";
        EXPECT_EQ(s->value().page_id, static_cast<uint32_t>(k));
    }

    // Full range scan must return all 10K in order.
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());

    int count = 0;
    int64_t prev = -1;
    while (!scan->is_end()) {
        auto entry = scan->next();
        ASSERT_TRUE(entry.has_value());
        if (!entry->has_value()) {
            break;
        }
        int64_t val = key_val(entry->value().first);
        EXPECT_GT(val, prev) << "Not sorted at count " << count;
        prev = val;
        ++count;
    }
    EXPECT_EQ(count, 10000);
}

TEST(BTreeQA_BulkLoad, BulkLoadThenDeleteAll) {
    auto tree = make_test_index(5, 5);

    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 1; i <= 50; ++i) {
        entries.push_back({make_key(i), make_rid(static_cast<uint32_t>(i))});
    }

    ASSERT_TRUE(tree.bulk_load(entries).has_value());
    EXPECT_EQ(tree.size(), 50u);

    // Delete all.
    for (int i = 1; i <= 50; ++i) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value()) << "remove() error at key " << i;
        EXPECT_TRUE(*del) << "Key " << i << " not found";
    }

    EXPECT_TRUE(tree.empty());
}

TEST(BTreeQA_BulkLoad, BulkLoadDuplicatesAllowed) {
    auto tree = make_test_index(4, 4, false);

    std::vector<std::pair<KeyType, RID>> entries = {
        {make_key(10), make_rid(1)},
        {make_key(10), make_rid(2)},
        {make_key(20), make_rid(3)},
        {make_key(20), make_rid(4)},
        {make_key(30), make_rid(5)},
    };

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 5u);
}

// =============================================================================
// Composite Key Tests
// =============================================================================

TEST(BTreeQA_Composite, RangeScanComposite) {
    BTreeConfig config;
    config.key_types = {TypeId::INT64, TypeId::STRING};
    config.leaf_max_keys = 4;
    config.internal_max_keys = 4;
    BTreeIndex tree(std::move(config));

    // Insert entries with composite keys (id, name).
    ASSERT_TRUE(tree.insert(make_composite_key(1, "alpha"), make_rid(1)).has_value());
    ASSERT_TRUE(tree.insert(make_composite_key(1, "beta"), make_rid(2)).has_value());
    ASSERT_TRUE(tree.insert(make_composite_key(2, "alpha"), make_rid(3)).has_value());
    ASSERT_TRUE(tree.insert(make_composite_key(2, "gamma"), make_rid(4)).has_value());
    ASSERT_TRUE(tree.insert(make_composite_key(3, "alpha"), make_rid(5)).has_value());

    // Range scan for all entries with first column >= 1 and < 3.
    auto scan =
        tree.range_scan(make_composite_key(1, ""), make_composite_key(3, ""));
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    // Should include: (1,"alpha"), (1,"beta"), (2,"alpha"), (2,"gamma") → 4 entries.
    EXPECT_EQ(entries->size(), 4u);
}

TEST(BTreeQA_Composite, SplitsWithCompositeKeys) {
    BTreeConfig config;
    config.key_types = {TypeId::INT64, TypeId::STRING};
    config.leaf_max_keys = 3;
    config.internal_max_keys = 3;
    BTreeIndex tree(std::move(config));

    // Insert enough to force splits.
    for (int i = 1; i <= 10; ++i) {
        std::string name = "key_" + std::to_string(i);
        ASSERT_TRUE(
            tree.insert(make_composite_key(i, name), make_rid(static_cast<uint32_t>(i)))
                .has_value());
    }
    EXPECT_EQ(tree.size(), 10u);

    // Verify all keys searchable.
    for (int i = 1; i <= 10; ++i) {
        std::string name = "key_" + std::to_string(i);
        auto s = tree.search(make_composite_key(i, name));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value()) << "Composite key (" << i << ", " << name << ") not found";
    }
}

// =============================================================================
// Stress Tests
// =============================================================================

TEST(BTreeQA_Stress, InsertDeleteSameKeyRepeatedly) {
    auto tree = make_test_index(4, 4, true);

    for (int round = 0; round < 100; ++round) {
        auto ins = tree.insert(make_key(42), make_rid(static_cast<uint32_t>(round)));
        ASSERT_TRUE(ins.has_value()) << "Insert failed at round " << round;
        EXPECT_EQ(tree.size(), 1u);

        auto s = tree.search(make_key(42));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value()) << "Key not found at round " << round;

        auto del = tree.remove(make_key(42));
        ASSERT_TRUE(del.has_value());
        EXPECT_TRUE(*del);
        EXPECT_EQ(tree.size(), 0u);
    }
}

TEST(BTreeQA_Stress, RandomMixedOperations) {
    // Use a unique index so each key can exist at most once.
    auto tree = make_test_index(5, 5, true);

    std::mt19937 rng(12345);
    std::set<int64_t> present;
    constexpr int ops = 5000;

    for (int op = 0; op < ops; ++op) {
        int action = rng() % 3; // 0 = insert, 1 = delete, 2 = search

        if (action == 0 || present.empty()) {
            // Insert a key. Use unique index, so duplicates are rejected.
            int64_t key = static_cast<int64_t>(rng() % 1000);
            auto ins = tree.insert(make_key(key), make_rid(static_cast<uint32_t>(key)));
            if (ins.has_value()) {
                present.insert(key);
            } else {
                // Must be constraint violation (duplicate in unique index).
                EXPECT_EQ(ins.error().code, StatusCode::CONSTRAINT_VIOLATION);
                EXPECT_TRUE(present.count(key) > 0)
                    << "Constraint violation for key " << key << " but not in present set";
            }
        } else if (action == 1 && !present.empty()) {
            // Delete a random existing key.
            auto it = present.begin();
            std::advance(it, static_cast<long>(rng() % present.size()));
            int64_t key = *it;
            auto del = tree.remove(make_key(key));
            ASSERT_TRUE(del.has_value()) << "remove() error for key " << key;
            EXPECT_TRUE(*del) << "Key " << key << " not found for deletion";
            present.erase(it);
        } else {
            // Search.
            int64_t key = static_cast<int64_t>(rng() % 1000);
            auto s = tree.search(make_key(key));
            ASSERT_TRUE(s.has_value());
            if (present.count(key) > 0) {
                EXPECT_TRUE(s->has_value()) << "Key " << key << " should exist but not found";
            } else {
                EXPECT_FALSE(s->has_value()) << "Key " << key << " should NOT exist but found";
            }
        }
    }

    // Final integrity check.
    verify_tree_integrity(tree, present);
}

TEST(BTreeQA_Stress, MonotonicInsertReverseDelete) {
    auto tree = make_test_index(4, 4);

    constexpr int n = 500;

    // Insert 1..n.
    for (int i = 1; i <= n; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Insert failed at " << i;
    }
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(n));

    // Delete n..1 (reverse order).
    for (int i = n; i >= 1; --i) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value()) << "remove() error at key " << i;
        EXPECT_TRUE(*del) << "Key " << i << " not found";
    }

    EXPECT_TRUE(tree.empty());
}

TEST(BTreeQA_Stress, AlternatingGrowShrink) {
    // Grow the tree, shrink it, grow it again, shrink it again.
    auto tree = make_test_index(4, 4);
    std::set<int64_t> expected;

    // Phase 1: insert 50 keys.
    for (int i = 1; i <= 50; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
        expected.insert(i);
    }

    // Phase 2: delete 40 keys.
    for (int i = 1; i <= 40; ++i) {
        ASSERT_TRUE(tree.remove(make_key(i)).has_value());
        expected.erase(i);
    }
    verify_tree_integrity(tree, expected);

    // Phase 3: insert 100 more keys.
    for (int i = 100; i <= 199; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
        expected.insert(i);
    }
    verify_tree_integrity(tree, expected);

    // Phase 4: delete 80 keys.
    for (int i = 100; i <= 179; ++i) {
        ASSERT_TRUE(tree.remove(make_key(i)).has_value());
        expected.erase(i);
    }
    verify_tree_integrity(tree, expected);
}

// =============================================================================
// Iterator Tests
// =============================================================================

TEST(BTreeQA_Iterator, DefaultConstructedIsEnd) {
    BTreeIterator it;
    EXPECT_TRUE(it.is_end());

    auto entry = it.next();
    ASSERT_TRUE(entry.has_value());
    EXPECT_FALSE(entry->has_value());
}

TEST(BTreeQA_Iterator, NextOnExhaustedReturnsNullopt) {
    auto tree = make_test_index(4, 4);
    ASSERT_TRUE(tree.insert(make_key(10), make_rid(1)).has_value());

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());

    // First next() returns the entry.
    auto e1 = scan->next();
    ASSERT_TRUE(e1.has_value());
    EXPECT_TRUE(e1->has_value());

    // Second next() should return nullopt (exhausted).
    auto e2 = scan->next();
    ASSERT_TRUE(e2.has_value());
    EXPECT_FALSE(e2->has_value());

    // Third next() should still return nullopt.
    auto e3 = scan->next();
    ASSERT_TRUE(e3.has_value());
    EXPECT_FALSE(e3->has_value());
}

// =============================================================================
// Null/Special Value Keys
// =============================================================================

TEST(BTreeQA_NullKey, InsertAndSearchNull) {
    auto tree = make_test_index(4, 4);

    KeyType null_key = {Value::make_null()};
    auto ins = tree.insert(null_key, make_rid(1));
    ASSERT_TRUE(ins.has_value());

    auto s = tree.search(null_key);
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(s->has_value());
}

TEST(BTreeQA_NullKey, NullSortsBeforeIntegers) {
    auto tree = make_test_index(4, 4);

    ASSERT_TRUE(tree.insert({Value::make_null()}, make_rid(1)).has_value());
    ASSERT_TRUE(tree.insert(make_key(0), make_rid(2)).has_value());
    ASSERT_TRUE(tree.insert(make_key(-100), make_rid(3)).has_value());
    ASSERT_TRUE(tree.insert(make_key(100), make_rid(4)).has_value());

    // Full scan should have null first.
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 4u);

    // First entry should be the null key.
    EXPECT_TRUE(std::holds_alternative<std::monostate>((*entries)[0].first[0].data()));
}

// =============================================================================
// Concurrency Adversarial
// =============================================================================

TEST(BTreeQA_Concurrency, ConcurrentInsertOverlappingKeys) {
    // Multiple threads inserting overlapping key ranges (non-unique index).
    auto tree = make_test_index(8, 8, false);

    constexpr int keys_per_thread = 100;
    constexpr int num_threads = 4;

    std::vector<std::thread> threads;
    std::atomic<int> failures{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&tree, &failures, t]() {
            for (int i = 0; i < keys_per_thread; ++i) {
                auto ins =
                    tree.insert(make_key(i), make_rid(static_cast<uint32_t>(t * 1000 + i)));
                if (!ins.has_value()) {
                    failures.fetch_add(1);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(failures.load(), 0);
    // Each thread inserts 100 keys, all overlapping.
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(num_threads * keys_per_thread));
}

TEST(BTreeQA_Concurrency, ConcurrentRangeScanDuringInsert) {
    auto tree = make_test_index(10, 10);
    constexpr int initial = 100;

    // Pre-populate.
    for (int i = 0; i < initial; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    std::atomic<int> scan_failures{0};
    std::atomic<int> insert_failures{0};
    std::atomic<bool> done{false};

    // Writer thread: insert more keys.
    std::thread writer([&]() {
        for (int i = initial; i < initial + 200; ++i) {
            auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
            if (!ins.has_value()) {
                insert_failures.fetch_add(1);
            }
        }
        done.store(true);
    });

    // Reader thread: do range scans on the pre-existing range.
    std::thread reader([&]() {
        while (!done.load()) {
            auto scan = tree.range_scan(make_key(0), make_key(initial));
            if (!scan.has_value()) {
                scan_failures.fetch_add(1);
                continue;
            }

            int count = 0;
            int64_t prev = -1;
            while (!scan->is_end()) {
                auto entry = scan->next();
                if (!entry.has_value()) {
                    scan_failures.fetch_add(1);
                    break;
                }
                if (!entry->has_value()) {
                    break;
                }
                int64_t val = key_val(entry->value().first);
                if (val <= prev) {
                    scan_failures.fetch_add(1);
                }
                prev = val;
                ++count;
            }

            // Should always see exactly `initial` keys in [0, initial).
            if (count != initial) {
                scan_failures.fetch_add(1);
            }
        }
    });

    writer.join();
    reader.join();

    EXPECT_EQ(insert_failures.load(), 0);
    EXPECT_EQ(scan_failures.load(), 0);
}

// =============================================================================
// WAL Integration
// =============================================================================

TEST(BTreeQA_WAL, InsertWithWalWriter) {
    // Verify that split operations log WAL records when a WAL writer is provided.
    // We use a minimal WalWriter to count split records.
    // Since WalWriter requires a file, we just verify the tree works with nullptr
    // WAL (no logging) - which is the default path.
    auto tree = make_test_index(3, 3);

    for (int i = 1; i <= 20; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    // Verify the tree is correct.
    EXPECT_EQ(tree.size(), 20u);
    for (int i = 1; i <= 20; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value());
    }
}

// =============================================================================
// Boundary: Very Large Keys
// =============================================================================

TEST(BTreeQA_Boundary, LargeInt64Values) {
    auto tree = make_test_index(4, 4);

    int64_t max_val = std::numeric_limits<int64_t>::max();
    int64_t min_val = std::numeric_limits<int64_t>::min();

    ASSERT_TRUE(tree.insert(make_key(0), make_rid(1)).has_value());
    ASSERT_TRUE(tree.insert(make_key(max_val), make_rid(2)).has_value());
    ASSERT_TRUE(tree.insert(make_key(min_val), make_rid(3)).has_value());
    ASSERT_TRUE(tree.insert(make_key(max_val - 1), make_rid(4)).has_value());
    ASSERT_TRUE(tree.insert(make_key(min_val + 1), make_rid(5)).has_value());

    EXPECT_EQ(tree.size(), 5u);

    // Verify sorted order.
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 5u);

    // min, min+1, 0, max-1, max.
    EXPECT_EQ(key_val((*entries)[0].first), min_val);
    EXPECT_EQ(key_val((*entries)[1].first), min_val + 1);
    EXPECT_EQ(key_val((*entries)[2].first), 0);
    EXPECT_EQ(key_val((*entries)[3].first), max_val - 1);
    EXPECT_EQ(key_val((*entries)[4].first), max_val);
}

// =============================================================================
// Delete then Range Scan Consistency
// =============================================================================

TEST(BTreeQA_Consistency, DeleteThenRangeScanConsistent) {
    // Insert many keys, delete a specific pattern, verify range scan
    // returns exactly the expected remaining keys.
    auto tree = make_test_index(4, 4);

    std::set<int64_t> expected;
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
        expected.insert(i);
    }

    // Delete all multiples of 7.
    for (int i = 0; i < 100; i += 7) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value());
        EXPECT_TRUE(*del);
        expected.erase(i);
    }

    verify_tree_integrity(tree, expected);

    // Bounded range scan [20, 50).
    auto scan = tree.range_scan(make_key(20), make_key(50));
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());

    // Count expected keys in [20, 50) that are not multiples of 7.
    size_t expected_count = 0;
    for (int i = 20; i < 50; ++i) {
        if (expected.count(i) > 0) {
            ++expected_count;
        }
    }
    EXPECT_EQ(entries->size(), expected_count);
}
