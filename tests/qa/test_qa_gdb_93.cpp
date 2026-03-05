/// QA adversarial tests for GDB-93: B+ tree insert with page splits.
/// Tests split correctness, cascading splits, sibling pointer integrity,
/// boundary capacities, and various insert orderings.

#include "sixseven/storage/wal.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <numeric>
#include <random>
#include <set>
#include <vector>

#include "test_btree_helpers.h"

using namespace sixseven;
using namespace sixseven::test;

// =============================================================================
// Minimal Capacity Splits (max_keys = 1)
// =============================================================================

TEST(QA_GDB93_MinCapacity, MaxKeysOneLeafSplit) {
    // With max_keys=1, every 2nd insert causes a split
    auto tree = make_test_index(1, 1);

    for (int i = 1; i <= 5; ++i) {
        auto ins = tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at key " << i * 10;
    }
    EXPECT_EQ(tree.size(), 5u);

    // All keys searchable
    for (int i = 1; i <= 5; ++i) {
        auto s = tree.search(make_key(i * 10));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i * 10 << " not found";
        EXPECT_EQ(s->value().page_id, static_cast<uint32_t>(i));
    }
}

TEST(QA_GDB93_MinCapacity, MaxKeysOneRangeScan) {
    auto tree = make_test_index(1, 1);

    for (int i = 1; i <= 8; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 8u);

    for (size_t i = 0; i < entries->size(); ++i) {
        EXPECT_EQ(key_val((*entries)[i].first), static_cast<int64_t>(i + 1));
    }
}

TEST(QA_GDB93_MinCapacity, MaxKeysTwoCascadingSplits) {
    auto tree = make_test_index(2, 2);

    for (int i = 1; i <= 20; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at key " << i;
    }
    EXPECT_EQ(tree.size(), 20u);

    for (int i = 1; i <= 20; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i << " not found";
    }
}

// =============================================================================
// Root Split Verification
// =============================================================================

TEST(QA_GDB93_RootSplit, FirstSplitCreatesNewRoot) {
    auto tree = make_test_index(3, 3);

    // Insert 3 keys -> root leaf is full
    for (int i = 1; i <= 3; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }
    EXPECT_EQ(tree.size(), 3u);

    // 4th insert -> leaf split -> new root created
    auto ins = tree.insert(make_key(25), make_rid(4));
    ASSERT_TRUE(ins.has_value());
    EXPECT_EQ(tree.size(), 4u);

    // Root should now be an internal node
    EXPECT_FALSE(tree.is_leaf_node(tree.root_page_id()));

    // All keys searchable
    for (int k : {10, 20, 25, 30}) {
        auto s = tree.search(make_key(k));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << k << " not found";
    }
}

TEST(QA_GDB93_RootSplit, InternalRootSplit) {
    // With max_keys=3 for internals, after enough leaf splits the internal root
    // itself must split, creating a new level in the tree.
    auto tree = make_test_index(3, 3);

    // Insert enough keys to force multiple levels
    for (int i = 1; i <= 30; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at key " << i;
    }
    EXPECT_EQ(tree.size(), 30u);

    // Verify all keys
    for (int i = 1; i <= 30; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i << " not found";
    }
}

// =============================================================================
// Insert Orderings That Stress Splits
// =============================================================================

TEST(QA_GDB93_InsertOrder, DescendingOrder) {
    auto tree = make_test_index(4, 4);

    // Descending inserts: every insert goes to the leftmost leaf
    for (int i = 30; i >= 1; --i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at key " << i;
    }
    EXPECT_EQ(tree.size(), 30u);

    // Full range scan must be sorted
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 30u);

    for (size_t i = 0; i < entries->size(); ++i) {
        EXPECT_EQ(key_val((*entries)[i].first), static_cast<int64_t>(i + 1));
    }
}

TEST(QA_GDB93_InsertOrder, AlternatingHighLow) {
    auto tree = make_test_index(4, 4);

    // Insert alternating high/low keys: 1, 100, 2, 99, 3, 98, ...
    std::vector<int> keys;
    for (int i = 0; i < 25; ++i) {
        keys.push_back(i + 1);
        keys.push_back(100 - i);
    }

    for (int k : keys) {
        auto ins = tree.insert(make_key(k), make_rid(static_cast<uint32_t>(k)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at key " << k;
    }
    EXPECT_EQ(tree.size(), 50u);

    // All keys searchable
    for (int k : keys) {
        auto s = tree.search(make_key(k));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << k << " not found";
    }
}

TEST(QA_GDB93_InsertOrder, MiddleOutInserts) {
    auto tree = make_test_index(4, 4);

    // Insert from middle outward: 50, 49, 51, 48, 52, ...
    std::vector<int> keys;
    for (int i = 0; i < 25; ++i) {
        keys.push_back(50 - i);
        keys.push_back(51 + i);
    }

    for (int k : keys) {
        auto ins = tree.insert(make_key(k), make_rid(static_cast<uint32_t>(k)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at key " << k;
    }
    EXPECT_EQ(tree.size(), 50u);

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 50u);

    for (size_t i = 1; i < entries->size(); ++i) {
        EXPECT_LT(key_val((*entries)[i - 1].first), key_val((*entries)[i].first))
            << "not sorted at index " << i;
    }
}

// =============================================================================
// Sibling Pointer Integrity After Splits
// =============================================================================

TEST(QA_GDB93_SiblingIntegrity, ForwardScanMatchesKeyCount) {
    auto tree = make_test_index(3, 3);

    for (int i = 1; i <= 25; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    // Full scan should visit all keys in order
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 25u);

    // Verify strictly increasing
    for (size_t i = 1; i < entries->size(); ++i) {
        EXPECT_LT(key_val((*entries)[i - 1].first), key_val((*entries)[i].first));
    }
}

TEST(QA_GDB93_SiblingIntegrity, SplitPreservesAllEntries) {
    // Insert exactly max_keys+1 to trigger one split, verify nothing is lost
    for (uint16_t max_keys = 2; max_keys <= 8; ++max_keys) {
        auto tree = make_test_index(max_keys, max_keys);

        int count = max_keys + 1;
        for (int i = 1; i <= count; ++i) {
            auto ins = tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
            ASSERT_TRUE(ins.has_value()) << "max=" << max_keys << " key=" << i * 10;
        }
        EXPECT_EQ(tree.size(), static_cast<uint64_t>(count));

        for (int i = 1; i <= count; ++i) {
            auto s = tree.search(make_key(i * 10));
            ASSERT_TRUE(s.has_value());
            ASSERT_TRUE(s->has_value()) << "max=" << max_keys << " key=" << i * 10 << " not found";
        }
    }
}

// =============================================================================
// Boundary Key Values Through Splits
// =============================================================================

TEST(QA_GDB93_BoundaryKeys, Int64Extremes) {
    auto tree = make_test_index(3, 3);
    const auto min_val = std::numeric_limits<int64_t>::min();
    const auto max_val = std::numeric_limits<int64_t>::max();

    std::vector<int64_t> keys = {min_val, min_val + 1, -1, 0, 1, max_val - 1, max_val};

    for (size_t i = 0; i < keys.size(); ++i) {
        auto ins = tree.insert(make_key(keys[i]), make_rid(static_cast<uint32_t>(i + 1)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at key " << keys[i];
    }
    EXPECT_EQ(tree.size(), keys.size());

    for (size_t i = 0; i < keys.size(); ++i) {
        auto s = tree.search(make_key(keys[i]));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << keys[i] << " not found";
    }

    // Range scan should produce sorted output
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), keys.size());

    for (size_t i = 1; i < entries->size(); ++i) {
        EXPECT_LT(key_val((*entries)[i - 1].first), key_val((*entries)[i].first));
    }
}

TEST(QA_GDB93_BoundaryKeys, NegativeKeysOnly) {
    auto tree = make_test_index(3, 3);

    for (int i = -30; i <= -1; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(-i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at key " << i;
    }
    EXPECT_EQ(tree.size(), 30u);

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 30u);

    for (size_t i = 1; i < entries->size(); ++i) {
        EXPECT_LT(key_val((*entries)[i - 1].first), key_val((*entries)[i].first));
    }
}

// =============================================================================
// Duplicate Keys with Splits (Non-Unique)
// =============================================================================

TEST(QA_GDB93_Duplicates, ManyDuplicatesCauseSplits) {
    auto tree = make_test_index(4, 4, false);

    // Insert 20 entries all with the same key
    for (int i = 0; i < 20; ++i) {
        auto ins = tree.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at iteration " << i;
    }
    EXPECT_EQ(tree.size(), 20u);

    // Point search should find one
    auto s = tree.search(make_key(42));
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());

    // Full range scan (no begin key) should find all 20
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 20u);
}

TEST(QA_GDB93_Duplicates, RangeScanWithBeginKeyMissesDuplicates) {
    // FIXED (GDB-240): range_scan(begin_key=K) with many duplicate keys
    // spanning multiple leaves now correctly walks backward through
    // prev_leaf_id pointers to find the leftmost leaf.
    auto tree = make_test_index(4, 4, false);

    for (int i = 0; i < 20; ++i) {
        (void)tree.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
    }
    EXPECT_EQ(tree.size(), 20u);

    auto scan = tree.range_scan(make_key(42), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());

    EXPECT_EQ(entries->size(), 20u);
}

TEST(QA_GDB93_Duplicates, DuplicatesInterleavedWithUniques) {
    auto tree = make_test_index(3, 3, false);

    // Insert pattern: 10, 10, 20, 20, 30, 30, ...
    for (int i = 1; i <= 15; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i * 2 - 1)));
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i * 2)));
    }
    EXPECT_EQ(tree.size(), 30u);

    // All keys should be searchable
    for (int i = 1; i <= 15; ++i) {
        auto s = tree.search(make_key(i * 10));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i * 10 << " not found";
    }
}

// =============================================================================
// Unique Constraint with Splits
// =============================================================================

TEST(QA_GDB93_Unique, RejectDuplicateAfterSplit) {
    auto tree = make_test_index(3, 3, true);

    // Insert enough to cause splits
    for (int i = 1; i <= 10; ++i) {
        auto ins = tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at key " << i * 10;
    }

    // Try to insert a duplicate of a key that might be in a different leaf now
    auto dup = tree.insert(make_key(50), make_rid(999));
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::CONSTRAINT_VIOLATION);
    EXPECT_EQ(tree.size(), 10u);
}

TEST(QA_GDB93_Unique, RejectDuplicateAtSplitBoundary) {
    auto tree = make_test_index(4, 4, true);

    // Fill a leaf exactly to capacity
    for (int i = 1; i <= 4; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Now insert a duplicate of the last key (should fail before split happens)
    auto dup = tree.insert(make_key(40), make_rid(999));
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::CONSTRAINT_VIOLATION);
    EXPECT_EQ(tree.size(), 4u);
}

// =============================================================================
// Composite Keys Through Splits
// =============================================================================

TEST(QA_GDB93_Composite, SplitWithCompositeKeys) {
    BTreeConfig config;
    config.key_types = {TypeId::INT64, TypeId::STRING};
    config.leaf_max_keys = 3;
    config.internal_max_keys = 3;
    BTreeIndex tree(std::move(config));

    std::vector<std::pair<int64_t, std::string>> entries = {
        {1, "apple"},
        {1, "banana"},
        {1, "cherry"},
        {2, "date"},
        {2, "elderberry"},
        {3, "fig"},
        {3, "grape"},
        {4, "honeydew"},
    };

    for (size_t i = 0; i < entries.size(); ++i) {
        auto ins = tree.insert(make_composite_key(entries[i].first, entries[i].second),
                               make_rid(static_cast<uint32_t>(i + 1)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at index " << i;
    }
    EXPECT_EQ(tree.size(), entries.size());

    // Verify all searchable
    for (size_t i = 0; i < entries.size(); ++i) {
        auto s = tree.search(make_composite_key(entries[i].first, entries[i].second));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value())
            << "(" << entries[i].first << ", " << entries[i].second << ") not found";
        EXPECT_EQ(s->value().page_id, static_cast<uint32_t>(i + 1));
    }

    // Range scan should be sorted
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto results = collect_scan(*scan);
    ASSERT_TRUE(results.has_value());
    EXPECT_EQ(results->size(), entries.size());

    for (size_t i = 1; i < results->size(); ++i) {
        auto cmp = compare_keys((*results)[i - 1].first, (*results)[i].first);
        ASSERT_TRUE(cmp.has_value());
        EXPECT_EQ(*cmp, std::strong_ordering::less) << "not sorted at index " << i;
    }
}

// =============================================================================
// Insert into Empty Tree
// =============================================================================

TEST(QA_GDB93_EmptyTree, FirstInsertCreatesRootLeaf) {
    auto tree = make_test_index(4, 4);
    EXPECT_TRUE(tree.empty());
    EXPECT_EQ(tree.root_page_id(), invalid_page_id);

    auto ins = tree.insert(make_key(42), make_rid(1));
    ASSERT_TRUE(ins.has_value());
    EXPECT_FALSE(tree.empty());
    EXPECT_NE(tree.root_page_id(), invalid_page_id);
    EXPECT_TRUE(tree.is_leaf_node(tree.root_page_id()));
}

TEST(QA_GDB93_EmptyTree, SearchOnEmptyTree) {
    auto tree = make_test_index();
    auto result = tree.search(make_key(42));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
}

// =============================================================================
// Split Point Correctness
// =============================================================================

TEST(QA_GDB93_SplitPoint, EvenKeyCount) {
    // With max_keys=4, inserting 5th key: total=5, split_point=2
    // Left gets keys[0..1], Right gets keys[2..4]
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 5; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // All 5 keys must be findable
    for (int i = 1; i <= 5; ++i) {
        auto s = tree.search(make_key(i * 10));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i * 10 << " not found";
    }
}

TEST(QA_GDB93_SplitPoint, OddKeyCount) {
    // With max_keys=5, inserting 6th key: total=6, split_point=3
    auto tree = make_test_index(5, 5);

    for (int i = 1; i <= 6; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    for (int i = 1; i <= 6; ++i) {
        auto s = tree.search(make_key(i * 10));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i * 10 << " not found";
    }
}

// =============================================================================
// WAL Logging
// =============================================================================

TEST(QA_GDB93_WAL, InsertWithWalDoesNotCrash) {
    // Create a WAL writer and ensure it doesn't crash during splits
    auto temp_dir = std::filesystem::temp_directory_path() / "qa_gdb93_wal";
    std::filesystem::create_directories(temp_dir);

    sixseven::WalWriter wal(temp_dir / "wal");
    auto open_result = wal.open();
    ASSERT_TRUE(open_result.has_value());

    BTreeConfig config;
    config.key_types = {TypeId::INT64};
    config.leaf_max_keys = 3;
    config.internal_max_keys = 3;
    BTreeIndex tree(std::move(config), &wal);

    // Insert enough to cause multiple splits
    for (int i = 1; i <= 15; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at key " << i;
    }
    EXPECT_EQ(tree.size(), 15u);

    std::filesystem::remove_all(temp_dir);
}

// =============================================================================
// Stress: Large Random Insert/Verify
// =============================================================================

TEST(QA_GDB93_Stress, RandomInsert500SmallCapacity) {
    auto tree = make_test_index(3, 3);

    std::vector<int> keys(500);
    std::iota(keys.begin(), keys.end(), 1);
    std::mt19937 rng(9999);
    std::shuffle(keys.begin(), keys.end(), rng);

    for (int k : keys) {
        auto ins = tree.insert(make_key(k), make_rid(static_cast<uint32_t>(k)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at key " << k;
    }
    EXPECT_EQ(tree.size(), 500u);

    // All keys searchable
    for (int i = 1; i <= 500; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i << " not found";
        EXPECT_EQ(s->value().page_id, static_cast<uint32_t>(i));
    }

    // Full scan sorted
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 500u);

    for (size_t i = 1; i < entries->size(); ++i) {
        EXPECT_LT(key_val((*entries)[i - 1].first), key_val((*entries)[i].first))
            << "not sorted at index " << i;
    }
}

TEST(QA_GDB93_Stress, InsertIncrementally) {
    // Insert keys one at a time, verifying the tree after every insert
    auto tree = make_test_index(3, 3);

    for (int i = 1; i <= 50; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at key " << i;
        EXPECT_EQ(tree.size(), static_cast<uint64_t>(i));

        // Verify all keys so far
        for (int j = 1; j <= i; ++j) {
            auto s = tree.search(make_key(j));
            ASSERT_TRUE(s.has_value());
            ASSERT_TRUE(s->has_value())
                << "key " << j << " not found after inserting " << i << " keys";
        }
    }
}

// =============================================================================
// Insert After Delete (Re-insert)
// =============================================================================

TEST(QA_GDB93_Reinsert, InsertAfterDeleteWithSplits) {
    auto tree = make_test_index(4, 4);

    // Insert 20 keys
    for (int i = 1; i <= 20; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Delete half
    for (int i = 1; i <= 10; ++i) {
        (void)tree.remove(make_key(i * 10));
    }
    EXPECT_EQ(tree.size(), 10u);

    // Re-insert with different RIDs (may cause new splits)
    for (int i = 1; i <= 10; ++i) {
        auto ins = tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i + 100)));
        ASSERT_TRUE(ins.has_value()) << "re-insert failed at key " << i * 10;
    }
    EXPECT_EQ(tree.size(), 20u);

    // Verify all keys present with correct RIDs
    for (int i = 1; i <= 10; ++i) {
        auto s = tree.search(make_key(i * 10));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i * 10 << " not found";
        EXPECT_EQ(s->value().page_id, static_cast<uint32_t>(i + 100));
    }
    for (int i = 11; i <= 20; ++i) {
        auto s = tree.search(make_key(i * 10));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i * 10 << " not found";
        EXPECT_EQ(s->value().page_id, static_cast<uint32_t>(i));
    }
}

// =============================================================================
// Various Leaf/Internal Capacity Combinations
// =============================================================================

TEST(QA_GDB93_AsymmetricCapacity, LargeLeafSmallInternal) {
    // Large leaf capacity, small internal capacity -> deep tree
    auto tree = make_test_index(10, 2);

    for (int i = 1; i <= 50; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at key " << i;
    }
    EXPECT_EQ(tree.size(), 50u);

    for (int i = 1; i <= 50; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i << " not found";
    }
}

TEST(QA_GDB93_AsymmetricCapacity, SmallLeafLargeInternal) {
    // Small leaf capacity, large internal capacity -> wide tree
    auto tree = make_test_index(2, 20);

    for (int i = 1; i <= 50; ++i) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "insert failed at key " << i;
    }
    EXPECT_EQ(tree.size(), 50u);

    for (int i = 1; i <= 50; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i << " not found";
    }
}
