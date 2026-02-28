/// QA adversarial tests for GDB-97: B+ tree bulk loading and unique constraint enforcement.
/// Tests edge cases in bulk load construction and unique constraint interactions.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

#include "test_btree_helpers.h"

using namespace giodb;
using namespace giodb::test;

// =============================================================================
// Bulk Load Edge Cases
// =============================================================================

TEST(QA_GDB97_BulkLoad, ExactlyOneLeafCapacity) {
    auto tree = make_test_index(10, 10);

    // Fill exactly one leaf
    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 0; i < 10; ++i) {
        entries.emplace_back(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 10u);

    // All keys searchable
    for (int i = 0; i < 10; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value());
    }
}

TEST(QA_GDB97_BulkLoad, ExactlyOneOverLeafCapacity) {
    auto tree = make_test_index(10, 10);

    // One more than leaf capacity — forces second leaf
    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 0; i < 11; ++i) {
        entries.emplace_back(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 11u);

    for (int i = 0; i < 11; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value()) << "Key " << i << " missing";
    }
}

TEST(QA_GDB97_BulkLoad, MinCapacityTree) {
    auto tree = make_test_index(1, 1);

    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 0; i < 10; ++i) {
        entries.emplace_back(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 10u);

    for (int i = 0; i < 10; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value()) << "Key " << i << " missing";
    }
}

TEST(QA_GDB97_BulkLoad, MinCapacityMax2) {
    auto tree = make_test_index(2, 2);

    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 0; i < 20; ++i) {
        entries.emplace_back(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 20u);

    // Range scan should be complete and sorted
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto all = collect_scan(*scan);
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->size(), 20u);

    for (size_t i = 1; i < all->size(); ++i) {
        EXPECT_LT(key_val((*all)[i - 1].first), key_val((*all)[i].first));
    }
}

TEST(QA_GDB97_BulkLoad, INT64BoundaryValues) {
    auto tree = make_test_index(10, 10);

    std::vector<std::pair<KeyType, RID>> entries;
    int64_t values[] = {
        std::numeric_limits<int64_t>::min(),
        std::numeric_limits<int64_t>::min() + 1,
        -1000,
        -1,
        0,
        1,
        1000,
        std::numeric_limits<int64_t>::max() - 1,
        std::numeric_limits<int64_t>::max(),
    };

    for (int i = 0; i < 9; ++i) {
        entries.emplace_back(make_key(values[i]), make_rid(static_cast<uint32_t>(i)));
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 9u);

    for (int i = 0; i < 9; ++i) {
        auto s = tree.search(make_key(values[i]));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value());
    }
}

TEST(QA_GDB97_BulkLoad, LargeLoad) {
    auto tree = make_test_index(50, 50);

    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 0; i < 10000; ++i) {
        entries.emplace_back(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 10000u);

    // Spot check
    for (int i = 0; i < 10000; i += 100) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value()) << "Key " << i << " missing";
    }

    // Range scan should return sorted
    auto scan = tree.range_scan(make_key(5000), make_key(5100));
    ASSERT_TRUE(scan.has_value());
    auto results = collect_scan(*scan);
    ASSERT_TRUE(results.has_value());
    EXPECT_EQ(results->size(), 100u);
}

TEST(QA_GDB97_BulkLoad, DuplicateKeysNonUnique) {
    auto tree = make_test_index(6, 6);

    // Sorted with duplicates (allowed for non-unique)
    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 0; i < 30; ++i) {
        entries.emplace_back(make_key(i / 3), make_rid(static_cast<uint32_t>(i)));
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 30u);
}

TEST(QA_GDB97_BulkLoad, AllSameKeyNonUnique) {
    auto tree = make_test_index(6, 6);

    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 0; i < 20; ++i) {
        entries.emplace_back(make_key(42), make_rid(static_cast<uint32_t>(i)));
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 20u);
}

// =============================================================================
// Bulk Load Validation Failures
// =============================================================================

TEST(QA_GDB97_BulkLoadFail, UnsortedAtEnd) {
    auto tree = make_test_index(10, 10);

    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 0; i < 10; ++i) {
        entries.emplace_back(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }
    // Make last entry out of order
    entries.emplace_back(make_key(5), make_rid(10));

    auto result = tree.bulk_load(entries);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_TRUE(tree.empty()); // Tree should remain empty on failure
}

TEST(QA_GDB97_BulkLoadFail, UnsortedAtStart) {
    auto tree = make_test_index(10, 10);

    std::vector<std::pair<KeyType, RID>> entries;
    entries.emplace_back(make_key(10), make_rid(0));
    entries.emplace_back(make_key(5), make_rid(1));
    entries.emplace_back(make_key(20), make_rid(2));

    auto result = tree.bulk_load(entries);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB97_BulkLoadFail, NonEmptyTree) {
    auto tree = make_test_index(10, 10);
    (void)tree.insert(make_key(1), make_rid(1));

    std::vector<std::pair<KeyType, RID>> entries;
    entries.emplace_back(make_key(2), make_rid(2));

    auto result = tree.bulk_load(entries);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// Unique Constraint on Bulk Load
// =============================================================================

TEST(QA_GDB97_Unique, BulkLoadRejectsDuplicates) {
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT64};
    cfg.leaf_max_keys = 10;
    cfg.internal_max_keys = 10;
    cfg.is_unique = true;
    BTreeIndex tree(cfg);

    std::vector<std::pair<KeyType, RID>> entries;
    entries.emplace_back(make_key(1), make_rid(1));
    entries.emplace_back(make_key(2), make_rid(2));
    entries.emplace_back(make_key(2), make_rid(3)); // duplicate
    entries.emplace_back(make_key(3), make_rid(4));

    auto result = tree.bulk_load(entries);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
    EXPECT_TRUE(tree.empty());
}

TEST(QA_GDB97_Unique, BulkLoadAllDuplicatesUnique) {
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT64};
    cfg.leaf_max_keys = 6;
    cfg.internal_max_keys = 6;
    cfg.is_unique = true;
    BTreeIndex tree(cfg);

    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 0; i < 10; ++i) {
        entries.emplace_back(make_key(42), make_rid(static_cast<uint32_t>(i)));
    }

    auto result = tree.bulk_load(entries);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST(QA_GDB97_Unique, BulkLoadNoDuplicatesSucceeds) {
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT64};
    cfg.leaf_max_keys = 10;
    cfg.internal_max_keys = 10;
    cfg.is_unique = true;
    BTreeIndex tree(cfg);

    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 0; i < 50; ++i) {
        entries.emplace_back(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 50u);
}

// =============================================================================
// Unique Constraint on Insert
// =============================================================================

TEST(QA_GDB97_Unique, InsertDuplicateRejected) {
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT64};
    cfg.leaf_max_keys = 6;
    cfg.internal_max_keys = 6;
    cfg.is_unique = true;
    BTreeIndex tree(cfg);

    auto ins1 = tree.insert(make_key(42), make_rid(1));
    ASSERT_TRUE(ins1.has_value());

    auto ins2 = tree.insert(make_key(42), make_rid(2));
    ASSERT_FALSE(ins2.has_value());
    EXPECT_EQ(ins2.error().code, StatusCode::CONSTRAINT_VIOLATION);
    EXPECT_EQ(tree.size(), 1u);
}

TEST(QA_GDB97_Unique, InsertDuplicateAfterSplit) {
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT64};
    cfg.leaf_max_keys = 4;
    cfg.internal_max_keys = 4;
    cfg.is_unique = true;
    BTreeIndex tree(cfg);

    // Fill enough to cause splits
    for (int i = 0; i < 20; ++i) {
        auto ins = tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value());
    }

    // Try inserting a duplicate of a key that's in a non-root leaf
    auto dup = tree.insert(make_key(100), make_rid(999));
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::CONSTRAINT_VIOLATION);
    EXPECT_EQ(tree.size(), 20u);
}

TEST(QA_GDB97_Unique, InsertDeleteInsertSameKey) {
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT64};
    cfg.leaf_max_keys = 6;
    cfg.internal_max_keys = 6;
    cfg.is_unique = true;
    BTreeIndex tree(cfg);

    // Insert
    auto ins1 = tree.insert(make_key(42), make_rid(1));
    ASSERT_TRUE(ins1.has_value());

    // Delete
    auto del = tree.remove(make_key(42));
    ASSERT_TRUE(del.has_value());
    EXPECT_TRUE(*del);

    // Re-insert should succeed (key is gone)
    auto ins2 = tree.insert(make_key(42), make_rid(2));
    ASSERT_TRUE(ins2.has_value());
    EXPECT_EQ(tree.size(), 1u);

    auto s = tree.search(make_key(42));
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
    EXPECT_EQ(s->value().page_id, 2u);
}

// =============================================================================
// Bulk Load then Operations
// =============================================================================

TEST(QA_GDB97_BulkLoadOps, InsertAfterBulkLoad) {
    auto tree = make_test_index(6, 6);

    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 0; i < 30; i += 2) {
        entries.emplace_back(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());

    // Insert interleaved keys
    for (int i = 1; i < 30; i += 2) {
        auto ins = tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Failed to insert " << i;
    }

    EXPECT_EQ(tree.size(), 30u);

    // Full scan should be sorted
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto all = collect_scan(*scan);
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->size(), 30u);

    for (size_t i = 1; i < all->size(); ++i) {
        EXPECT_LT(key_val((*all)[i - 1].first), key_val((*all)[i].first));
    }
}

TEST(QA_GDB97_BulkLoadOps, DeleteAfterBulkLoad) {
    auto tree = make_test_index(6, 6);

    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 0; i < 50; ++i) {
        entries.emplace_back(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());

    // Delete all even keys
    for (int i = 0; i < 50; i += 2) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value());
        EXPECT_TRUE(*del) << "Key " << i << " not found for delete";
    }

    EXPECT_EQ(tree.size(), 25u);

    // Odd keys remain
    for (int i = 1; i < 50; i += 2) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value()) << "Key " << i << " missing";
    }
}

TEST(QA_GDB97_BulkLoadOps, DeleteAllAfterBulkLoad) {
    auto tree = make_test_index(4, 4);

    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 0; i < 30; ++i) {
        entries.emplace_back(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    (void)tree.bulk_load(entries);

    // Delete all
    for (int i = 0; i < 30; ++i) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value());
        EXPECT_TRUE(*del);
    }

    EXPECT_TRUE(tree.empty());

    // Re-insert should work
    auto ins = tree.insert(make_key(100), make_rid(100));
    ASSERT_TRUE(ins.has_value());
    EXPECT_EQ(tree.size(), 1u);
}

// =============================================================================
// Composite Key Bulk Load
// =============================================================================

TEST(QA_GDB97_BulkLoad, CompositeKeys) {
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT64, TypeId::STRING};
    cfg.leaf_max_keys = 6;
    cfg.internal_max_keys = 6;
    cfg.is_unique = true;
    BTreeIndex tree(cfg);

    std::vector<std::pair<KeyType, RID>> entries;
    // Sort: by INT64 first, then STRING
    entries.emplace_back(make_composite_key(1, "alpha"), make_rid(0));
    entries.emplace_back(make_composite_key(1, "beta"), make_rid(1));
    entries.emplace_back(make_composite_key(2, "alpha"), make_rid(2));
    entries.emplace_back(make_composite_key(2, "gamma"), make_rid(3));
    entries.emplace_back(make_composite_key(3, "delta"), make_rid(4));

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 5u);

    // Search for each
    auto s = tree.search(make_composite_key(2, "alpha"));
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
    EXPECT_EQ(s->value().page_id, 2u);
}

// =============================================================================
// Stress: Bulk Load + Mixed Operations
// =============================================================================

TEST(QA_GDB97_Stress, BulkLoadThenStress) {
    auto tree = make_test_index(8, 8);

    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 0; i < 500; ++i) {
        entries.emplace_back(make_key(i * 2), make_rid(static_cast<uint32_t>(i)));
    }

    (void)tree.bulk_load(entries);

    // Insert odd keys
    for (int i = 0; i < 500; ++i) {
        auto ins = tree.insert(make_key(i * 2 + 1), make_rid(static_cast<uint32_t>(i + 500)));
        ASSERT_TRUE(ins.has_value());
    }

    EXPECT_EQ(tree.size(), 1000u);

    // Delete first 200 keys
    for (int i = 0; i < 200; ++i) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value());
    }

    EXPECT_EQ(tree.size(), 800u);

    // Verify remaining via scan
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto all = collect_scan(*scan);
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->size(), 800u);

    // Should be sorted
    for (size_t i = 1; i < all->size(); ++i) {
        EXPECT_LT(key_val((*all)[i - 1].first), key_val((*all)[i].first));
    }
}

TEST(QA_GDB97_Stress, AsymmetricCapacityBulkLoad) {
    // Different leaf and internal capacities
    auto tree = make_test_index(3, 8);

    std::vector<std::pair<KeyType, RID>> entries;
    for (int i = 0; i < 100; ++i) {
        entries.emplace_back(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    auto result = tree.bulk_load(entries);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(tree.size(), 100u);

    // All keys accessible
    for (int i = 0; i < 100; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value()) << "Key " << i << " missing";
    }
}
