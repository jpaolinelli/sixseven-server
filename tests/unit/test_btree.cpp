#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

#include "test_btree_helpers.h"

using namespace sixseven;
using namespace sixseven::test;

// =============================================================================
// GDB-93: Insert with Page Splits
// =============================================================================

TEST(BTreeInsert, SingleKey) {
    auto tree = make_test_index();
    auto ins = tree.insert(make_key(42), make_rid(1, 0));
    ASSERT_TRUE(ins.has_value());
    EXPECT_EQ(tree.size(), 1u);
    EXPECT_FALSE(tree.empty());
}

TEST(BTreeInsert, MultipleKeys) {
    auto tree = make_test_index();
    for (int i = 1; i <= 3; ++i) {
        auto ins = tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Failed to insert key " << i * 10;
    }
    EXPECT_EQ(tree.size(), 3u);
}

TEST(BTreeInsert, LeafSplit) {
    auto tree = make_test_index(4, 4); // max 4 keys per leaf.

    // Insert 5 keys -> forces a leaf split.
    for (int i = 1; i <= 5; ++i) {
        auto ins = tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Failed to insert key " << i * 10;
    }
    EXPECT_EQ(tree.size(), 5u);

    // All keys should be searchable after the split.
    for (int i = 1; i <= 5; ++i) {
        auto result = tree.search(make_key(i * 10));
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->has_value()) << "Key " << i * 10 << " not found after split";
        EXPECT_EQ(result->value().page_id, static_cast<uint32_t>(i));
    }
}

TEST(BTreeInsert, MultipleSplits) {
    auto tree = make_test_index(4, 4);

    // Insert 20 keys -> multiple leaf splits and possibly internal splits.
    for (int i = 1; i <= 20; ++i) {
        auto ins = tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Failed to insert key " << i * 10;
    }
    EXPECT_EQ(tree.size(), 20u);

    // Verify all keys.
    for (int i = 1; i <= 20; ++i) {
        auto result = tree.search(make_key(i * 10));
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->has_value()) << "Key " << i * 10 << " not found";
    }
}

TEST(BTreeInsert, CascadingSplits) {
    auto tree = make_test_index(3, 3); // Very small capacity -> cascading splits.

    for (int i = 1; i <= 50; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Failed to insert key " << i;
    }
    EXPECT_EQ(tree.size(), 50u);

    for (int i = 1; i <= 50; ++i) {
        auto result = tree.search(make_key(i));
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->has_value()) << "Key " << i << " not found";
    }
}

TEST(BTreeInsert, ReverseOrder) {
    auto tree = make_test_index(4, 4);

    for (int i = 20; i >= 1; --i) {
        auto ins = tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Failed to insert key " << i * 10;
    }
    EXPECT_EQ(tree.size(), 20u);

    for (int i = 1; i <= 20; ++i) {
        auto result = tree.search(make_key(i * 10));
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->has_value()) << "Key " << i * 10 << " not found";
    }
}

TEST(BTreeInsert, RandomOrder) {
    auto tree = make_test_index(4, 4);

    std::vector<int> keys(30);
    std::iota(keys.begin(), keys.end(), 1);
    std::mt19937 rng(42); // Deterministic seed.
    std::shuffle(keys.begin(), keys.end(), rng);

    for (int k : keys) {
        auto ins = tree.insert(make_key(k), make_rid(static_cast<uint32_t>(k)));
        ASSERT_TRUE(ins.has_value()) << "Failed to insert key " << k;
    }
    EXPECT_EQ(tree.size(), 30u);

    for (int k : keys) {
        auto result = tree.search(make_key(k));
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->has_value()) << "Key " << k << " not found";
    }
}

TEST(BTreeInsert, DuplicateKeysNonUnique) {
    auto tree = make_test_index(4, 4, false);

    auto ins1 = tree.insert(make_key(42), make_rid(1));
    ASSERT_TRUE(ins1.has_value());
    auto ins2 = tree.insert(make_key(42), make_rid(2));
    ASSERT_TRUE(ins2.has_value());
    EXPECT_EQ(tree.size(), 2u);
}

TEST(BTreeInsert, SiblingPointerIntegrity) {
    auto tree = make_test_index(4, 4);

    // Insert enough to cause splits.
    for (int i = 1; i <= 12; ++i) {
        auto ins = tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value());
    }

    // Verify via full range scan that sibling pointers are intact.
    auto scan_result = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan_result.has_value());

    auto entries = collect_scan(*scan_result);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 12u);

    // Verify sorted order.
    for (size_t i = 1; i < entries->size(); ++i) {
        EXPECT_LT(key_val((*entries)[i - 1].first), key_val((*entries)[i].first))
            << "Entries not sorted at index " << i;
    }
}

// =============================================================================
// GDB-94: Point Lookup and Range Scan
// =============================================================================

TEST(BTreeSearch, EmptyTree) {
    auto tree = make_test_index();
    auto result = tree.search(make_key(42));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
}

TEST(BTreeSearch, SingleKey) {
    auto tree = make_test_index();
    (void)tree.insert(make_key(42), make_rid(100, 5));

    auto result = tree.search(make_key(42));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().page_id, 100u);
    EXPECT_EQ(result->value().slot_id, 5u);
}

TEST(BTreeSearch, MissingKey) {
    auto tree = make_test_index();
    (void)tree.insert(make_key(42), make_rid(1));

    auto result = tree.search(make_key(99));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
}

TEST(BTreeSearch, AfterSplits) {
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 20; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Search for each key.
    for (int i = 1; i <= 20; ++i) {
        auto result = tree.search(make_key(i * 10));
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->has_value()) << "Key " << i * 10 << " not found";
        EXPECT_EQ(result->value().page_id, static_cast<uint32_t>(i));
    }

    // Missing keys.
    auto r1 = tree.search(make_key(5));
    ASSERT_TRUE(r1.has_value());
    EXPECT_FALSE(r1->has_value());

    auto r2 = tree.search(make_key(205));
    ASSERT_TRUE(r2.has_value());
    EXPECT_FALSE(r2->has_value());
}

TEST(BTreeRangeScan, EmptyTree) {
    auto tree = make_test_index();
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    EXPECT_TRUE(scan->is_end());
}

TEST(BTreeRangeScan, FullScan) {
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 10; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 10u);

    // Verify sorted order and values.
    for (size_t i = 0; i < entries->size(); ++i) {
        EXPECT_EQ(key_val((*entries)[i].first), static_cast<int64_t>((i + 1) * 10));
    }
}

TEST(BTreeRangeScan, BoundedRange) {
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 10; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Range [30, 70) — should return 30, 40, 50, 60.
    auto scan = tree.range_scan(make_key(30), make_key(70));
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 4u);
    EXPECT_EQ(key_val((*entries)[0].first), 30);
    EXPECT_EQ(key_val((*entries)[3].first), 60);
}

TEST(BTreeRangeScan, OpenBegin) {
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 10; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Range [_, 40) — should return 10, 20, 30.
    auto scan = tree.range_scan(std::nullopt, make_key(40));
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 3u);
    EXPECT_EQ(key_val((*entries)[0].first), 10);
    EXPECT_EQ(key_val((*entries)[2].first), 30);
}

TEST(BTreeRangeScan, OpenEnd) {
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 10; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Range [80, _) — should return 80, 90, 100.
    auto scan = tree.range_scan(make_key(80), std::nullopt);
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 3u);
    EXPECT_EQ(key_val((*entries)[0].first), 80);
    EXPECT_EQ(key_val((*entries)[2].first), 100);
}

TEST(BTreeRangeScan, EmptyRange) {
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 10; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Range [15, 15) — empty (no key equals 15).
    auto scan = tree.range_scan(make_key(15), make_key(15));
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 0u);
}

TEST(BTreeRangeScan, CrossLeafBoundary) {
    auto tree = make_test_index(3, 3);

    for (int i = 1; i <= 15; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    // Full scan should traverse multiple leaves.
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 15u);

    // Verify sorted order.
    for (size_t i = 0; i < entries->size(); ++i) {
        EXPECT_EQ(key_val((*entries)[i].first), static_cast<int64_t>(i + 1));
    }
}

TEST(BTreeRangeScan, RangeAcrossLeaves) {
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 20; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Range [50, 160) should cross leaf boundaries.
    auto scan = tree.range_scan(make_key(50), make_key(160));
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());

    // Keys: 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150.
    EXPECT_EQ(entries->size(), 11u);
    EXPECT_EQ(key_val((*entries)[0].first), 50);
    EXPECT_EQ(key_val(entries->back().first), 150);
}

// =============================================================================
// GDB-95: Delete with Merge/Redistribute
// =============================================================================

TEST(BTreeDelete, SingleKey) {
    auto tree = make_test_index();
    (void)tree.insert(make_key(42), make_rid(1));

    auto del = tree.remove(make_key(42));
    ASSERT_TRUE(del.has_value());
    EXPECT_TRUE(*del);
    EXPECT_EQ(tree.size(), 0u);
    EXPECT_TRUE(tree.empty());

    // Verify key is gone.
    auto result = tree.search(make_key(42));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
}

TEST(BTreeDelete, KeyNotFound) {
    auto tree = make_test_index();
    (void)tree.insert(make_key(42), make_rid(1));

    auto del = tree.remove(make_key(99));
    ASSERT_TRUE(del.has_value());
    EXPECT_FALSE(*del);
    EXPECT_EQ(tree.size(), 1u);
}

TEST(BTreeDelete, EmptyTree) {
    auto tree = make_test_index();
    auto del = tree.remove(make_key(42));
    ASSERT_TRUE(del.has_value());
    EXPECT_FALSE(*del);
}

TEST(BTreeDelete, NoUnderflow) {
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 4; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Delete one key — the root leaf should still be fine.
    auto del = tree.remove(make_key(20));
    ASSERT_TRUE(del.has_value());
    EXPECT_TRUE(*del);
    EXPECT_EQ(tree.size(), 3u);

    // Remaining keys still present.
    for (int k : {10, 30, 40}) {
        auto result = tree.search(make_key(k));
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->has_value()) << "Key " << k << " missing";
    }
}

TEST(BTreeDelete, DeleteFromMultipleLeaves) {
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 12; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }
    EXPECT_EQ(tree.size(), 12u);

    // Delete several keys.
    for (int k : {30, 70, 110}) {
        auto del = tree.remove(make_key(k));
        ASSERT_TRUE(del.has_value());
        EXPECT_TRUE(*del) << "Key " << k << " not deleted";
    }
    EXPECT_EQ(tree.size(), 9u);

    // Remaining keys still present.
    for (int i = 1; i <= 12; ++i) {
        auto result = tree.search(make_key(i * 10));
        ASSERT_TRUE(result.has_value());
        if (i * 10 == 30 || i * 10 == 70 || i * 10 == 110) {
            EXPECT_FALSE(result->has_value()) << "Deleted key " << i * 10 << " still found";
        } else {
            EXPECT_TRUE(result->has_value()) << "Key " << i * 10 << " missing";
        }
    }
}

TEST(BTreeDelete, DeleteAllKeys) {
    auto tree = make_test_index(4, 4);

    std::vector<int> keys;
    for (int i = 1; i <= 15; ++i) {
        keys.push_back(i * 10);
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Delete all keys.
    for (int k : keys) {
        auto del = tree.remove(make_key(k));
        ASSERT_TRUE(del.has_value()) << "Failed to delete key " << k;
        EXPECT_TRUE(*del) << "Key " << k << " not found";
    }
    EXPECT_EQ(tree.size(), 0u);
    EXPECT_TRUE(tree.empty());
}

TEST(BTreeDelete, InterleavedInsertDelete) {
    auto tree = make_test_index(4, 4);

    // Insert 10 keys.
    for (int i = 1; i <= 10; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Delete odd-indexed keys.
    for (int i = 1; i <= 10; i += 2) {
        auto del = tree.remove(make_key(i * 10));
        ASSERT_TRUE(del.has_value());
    }
    EXPECT_EQ(tree.size(), 5u);

    // Insert new keys.
    for (int i = 11; i <= 15; ++i) {
        auto ins = tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value());
    }
    EXPECT_EQ(tree.size(), 10u);

    // Verify remaining keys.
    for (int i = 2; i <= 10; i += 2) {
        auto result = tree.search(make_key(i * 10));
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->has_value()) << "Key " << i * 10 << " missing";
    }
    for (int i = 11; i <= 15; ++i) {
        auto result = tree.search(make_key(i * 10));
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->has_value()) << "Key " << i * 10 << " missing";
    }
}

TEST(BTreeDelete, RangeScanAfterDeletes) {
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 10; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Delete keys 30, 60, 90.
    (void)tree.remove(make_key(30));
    (void)tree.remove(make_key(60));
    (void)tree.remove(make_key(90));

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 7u);

    // Verify sorted.
    for (size_t i = 1; i < entries->size(); ++i) {
        EXPECT_LT(key_val((*entries)[i - 1].first), key_val((*entries)[i].first));
    }
}

TEST(BTreeDelete, RootShrink) {
    auto tree = make_test_index(4, 4);

    // Insert enough to create multi-level tree.
    for (int i = 1; i <= 8; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Delete enough to potentially cause root shrink.
    for (int i = 1; i <= 6; ++i) {
        auto del = tree.remove(make_key(i * 10));
        ASSERT_TRUE(del.has_value());
    }
    EXPECT_EQ(tree.size(), 2u);

    // Remaining keys still work.
    auto r1 = tree.search(make_key(70));
    ASSERT_TRUE(r1.has_value());
    EXPECT_TRUE(r1->has_value());

    auto r2 = tree.search(make_key(80));
    ASSERT_TRUE(r2.has_value());
    EXPECT_TRUE(r2->has_value());
}

// =============================================================================
// GDB-97: Bulk Loading
// =============================================================================

TEST(BTreeBulkLoad, EmptyInput) {
    auto tree = make_test_index();
    std::vector<std::pair<KeyType, RID>> entries;

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(tree.empty());
}

TEST(BTreeBulkLoad, SingleEntry) {
    auto tree = make_test_index();
    std::vector<std::pair<KeyType, RID>> entries = {
        {make_key(42), make_rid(1, 0)},
    };

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 1u);

    auto search = tree.search(make_key(42));
    ASSERT_TRUE(search.has_value());
    EXPECT_TRUE(search->has_value());
}

TEST(BTreeBulkLoad, FitsInOneLeaf) {
    auto tree = make_test_index(10, 10);
    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 1; i <= 5; ++i) {
        entries.push_back({make_key(i * 10), make_rid(static_cast<uint32_t>(i))});
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 5u);

    for (int i = 1; i <= 5; ++i) {
        auto s = tree.search(make_key(i * 10));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value()) << "Key " << i * 10 << " not found";
    }
}

TEST(BTreeBulkLoad, MultipleLeaves) {
    auto tree = make_test_index(4, 4);
    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 1; i <= 20; ++i) {
        entries.push_back({make_key(i * 10), make_rid(static_cast<uint32_t>(i))});
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 20u);

    // All keys searchable.
    for (int i = 1; i <= 20; ++i) {
        auto s = tree.search(make_key(i * 10));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value()) << "Key " << i * 10 << " not found";
    }
}

TEST(BTreeBulkLoad, ThreeLevels) {
    auto tree = make_test_index(3, 3);
    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 1; i <= 50; ++i) {
        entries.push_back({make_key(i), make_rid(static_cast<uint32_t>(i))});
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 50u);

    for (int i = 1; i <= 50; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value()) << "Key " << i << " not found";
    }
}

TEST(BTreeBulkLoad, RangeScanAfterBulkLoad) {
    auto tree = make_test_index(4, 4);
    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 1; i <= 15; ++i) {
        entries.push_back({make_key(i * 10), make_rid(static_cast<uint32_t>(i))});
    }

    (void)tree.bulk_load(entries);

    auto scan = tree.range_scan(make_key(50), make_key(110));
    ASSERT_TRUE(scan.has_value());

    auto results = collect_scan(*scan);
    ASSERT_TRUE(results.has_value());

    // Keys: 50, 60, 70, 80, 90, 100.
    EXPECT_EQ(results->size(), 6u);
    EXPECT_EQ(key_val((*results)[0].first), 50);
    EXPECT_EQ(key_val(results->back().first), 100);
}

TEST(BTreeBulkLoad, InsertAfterBulkLoad) {
    auto tree = make_test_index(4, 4);
    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 1; i <= 10; ++i) {
        entries.push_back({make_key(i * 10), make_rid(static_cast<uint32_t>(i))});
    }

    (void)tree.bulk_load(entries);

    // Insert new keys after bulk load.
    for (int i = 11; i <= 15; ++i) {
        auto ins = tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value());
    }
    EXPECT_EQ(tree.size(), 15u);

    // All keys should be findable.
    for (int i = 1; i <= 15; ++i) {
        auto s = tree.search(make_key(i * 10));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value()) << "Key " << i * 10 << " not found";
    }
}

TEST(BTreeBulkLoad, NonEmptyTreeFails) {
    auto tree = make_test_index();
    (void)tree.insert(make_key(1), make_rid(1));

    std::vector<std::pair<KeyType, RID>> entries = {{make_key(2), make_rid(2)}};
    auto result = tree.bulk_load(entries);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(BTreeBulkLoad, UnsortedInputFails) {
    auto tree = make_test_index(4, 4);
    std::vector<std::pair<KeyType, RID>> entries = {
        {make_key(10), make_rid(1)},
        {make_key(30), make_rid(3)},
        {make_key(20), make_rid(2)}, // Not sorted!
    };

    auto result = tree.bulk_load(entries);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// GDB-97: Unique Constraint
// =============================================================================

TEST(BTreeUnique, RejectDuplicateOnInsert) {
    auto tree = make_test_index(4, 4, true);

    auto ins1 = tree.insert(make_key(42), make_rid(1));
    ASSERT_TRUE(ins1.has_value());

    auto ins2 = tree.insert(make_key(42), make_rid(2));
    ASSERT_FALSE(ins2.has_value());
    EXPECT_EQ(ins2.error().code, StatusCode::CONSTRAINT_VIOLATION);
    EXPECT_EQ(tree.size(), 1u);
}

TEST(BTreeUnique, RejectDuplicateOnBulkLoad) {
    auto tree = make_test_index(4, 4, true);

    std::vector<std::pair<KeyType, RID>> entries = {
        {make_key(10), make_rid(1)},
        {make_key(20), make_rid(2)},
        {make_key(20), make_rid(3)}, // Duplicate!
        {make_key(30), make_rid(4)},
    };

    auto result = tree.bulk_load(entries);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST(BTreeUnique, DeleteThenReinsert) {
    auto tree = make_test_index(4, 4, true);

    (void)tree.insert(make_key(42), make_rid(1));
    (void)tree.remove(make_key(42));

    // Should succeed — key was deleted.
    auto ins = tree.insert(make_key(42), make_rid(2));
    ASSERT_TRUE(ins.has_value());
    EXPECT_EQ(tree.size(), 1u);

    auto result = tree.search(make_key(42));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().page_id, 2u);
}

TEST(BTreeUnique, AllowDifferentKeys) {
    auto tree = make_test_index(4, 4, true);

    for (int i = 1; i <= 10; ++i) {
        auto ins = tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Failed to insert key " << i * 10;
    }
    EXPECT_EQ(tree.size(), 10u);
}

TEST(BTreeUnique, UniqueWithSplits) {
    auto tree = make_test_index(4, 4, true);

    // Insert enough to force splits — all unique.
    for (int i = 1; i <= 20; ++i) {
        auto ins = tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Failed to insert key " << i * 10;
    }
    EXPECT_EQ(tree.size(), 20u);

    // Verify all searchable.
    for (int i = 1; i <= 20; ++i) {
        auto result = tree.search(make_key(i * 10));
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->has_value()) << "Key " << i * 10 << " not found";
    }
}

// =============================================================================
// Composite Key Tests
// =============================================================================

TEST(BTreeComposite, InsertAndSearch) {
    BTreeConfig config;
    config.key_types = {TypeId::INT64, TypeId::STRING};
    config.leaf_max_keys = 4;
    config.internal_max_keys = 4;
    BTreeIndex tree(std::move(config));

    (void)tree.insert(make_composite_key(1, "apple"), make_rid(1));
    (void)tree.insert(make_composite_key(1, "banana"), make_rid(2));
    (void)tree.insert(make_composite_key(2, "cherry"), make_rid(3));

    auto r1 = tree.search(make_composite_key(1, "apple"));
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r1->has_value());
    EXPECT_EQ(r1->value().page_id, 1u);

    auto r2 = tree.search(make_composite_key(1, "banana"));
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r2->has_value());
    EXPECT_EQ(r2->value().page_id, 2u);

    auto r3 = tree.search(make_composite_key(1, "cherry"));
    ASSERT_TRUE(r3.has_value());
    EXPECT_FALSE(r3->has_value()); // Not present.
}

// =============================================================================
// Stress Tests
// =============================================================================

TEST(BTreeStress, LargeInsertAndSearch) {
    auto tree = make_test_index(10, 10);

    const int n = 1000;
    for (int i = 0; i < n; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Failed at key " << i;
    }
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(n));

    for (int i = 0; i < n; ++i) {
        auto result = tree.search(make_key(i));
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->has_value()) << "Key " << i << " not found";
        EXPECT_EQ(result->value().page_id, static_cast<uint32_t>(i));
    }
}

TEST(BTreeStress, LargeRandomInsertDeleteSearch) {
    auto tree = make_test_index(8, 8);

    std::vector<int> keys(500);
    std::iota(keys.begin(), keys.end(), 0);
    std::mt19937 rng(123);
    std::shuffle(keys.begin(), keys.end(), rng);

    // Insert all.
    for (int k : keys) {
        auto ins = tree.insert(make_key(k), make_rid(static_cast<uint32_t>(k)));
        ASSERT_TRUE(ins.has_value());
    }

    // Delete first half.
    for (size_t i = 0; i < 250; ++i) {
        auto del = tree.remove(make_key(keys[i]));
        ASSERT_TRUE(del.has_value());
        EXPECT_TRUE(*del);
    }
    EXPECT_EQ(tree.size(), 250u);

    // Verify second half still present.
    for (size_t i = 250; i < 500; ++i) {
        auto result = tree.search(make_key(keys[i]));
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->has_value()) << "Key " << keys[i] << " missing";
    }

    // Verify first half is gone.
    for (size_t i = 0; i < 250; ++i) {
        auto result = tree.search(make_key(keys[i]));
        ASSERT_TRUE(result.has_value());
        EXPECT_FALSE(result->has_value()) << "Deleted key " << keys[i] << " still found";
    }
}

TEST(BTreeStress, FullScanSortedAfterMixedOps) {
    auto tree = make_test_index(5, 5);

    // Insert 100 keys.
    for (int i = 0; i < 100; ++i) {
        (void)tree.insert(make_key(i * 3), make_rid(static_cast<uint32_t>(i)));
    }

    // Delete every third key.
    for (int i = 0; i < 100; i += 3) {
        (void)tree.remove(make_key(i * 3));
    }

    // Full scan should be sorted.
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());

    // 100 inserted (i*3 for i in 0..99) minus 34 deleted (i*3 for i in 0,3,6,...,99,
    // i.e. multiples of 9) = 66 survivors (multiples of 3 that are not multiples of 9).
    ASSERT_EQ(entries->size(), 66u) << "Expected 66 survivors after deletions";
    EXPECT_EQ(tree.size(), 66u);

    // Build the exact expected survivor set: i*3 for i in 0..99 where i % 3 != 0.
    std::vector<int64_t> expected;
    expected.reserve(66);
    for (int i = 0; i < 100; ++i) {
        if (i % 3 != 0) {
            expected.push_back(static_cast<int64_t>(i * 3));
        }
    }
    // expected is already sorted (i ascending => i*3 ascending).
    ASSERT_EQ(expected.size(), 66u);

    for (size_t i = 0; i < entries->size(); ++i) {
        EXPECT_EQ(key_val((*entries)[i].first), expected[i]) << "Mismatch at scan index " << i;
    }

    for (size_t i = 1; i < entries->size(); ++i) {
        EXPECT_LT(key_val((*entries)[i - 1].first), key_val((*entries)[i].first))
            << "Not sorted at index " << i;
    }
}
