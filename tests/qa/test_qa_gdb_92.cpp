/// QA adversarial tests for GDB-92: B+ tree internal and leaf node page layouts.
/// Tests edge cases, boundary values, stress scenarios, and error paths.

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "test_btree_helpers.h"

using namespace giodb;
using namespace giodb::test;

// =============================================================================
// Leaf Node: Empty Node Operations
// =============================================================================

TEST(QA_GDB92_LeafEmpty, SearchOnEmptyReturnsNullopt) {
    BTreeLeafNode leaf(1, 10);
    auto result = leaf.search(make_key(42));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
}

TEST(QA_GDB92_LeafEmpty, DeleteOnEmptyReturnsFalse) {
    BTreeLeafNode leaf(1, 10);
    auto result = leaf.remove(make_key(42));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(*result);
}

TEST(QA_GDB92_LeafEmpty, LowerBoundOnEmptyReturnsZero) {
    BTreeLeafNode leaf(1, 10);
    auto result = leaf.lower_bound(make_key(42));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0u);
}

TEST(QA_GDB92_LeafEmpty, KeyCountIsZero) {
    BTreeLeafNode leaf(1, 10);
    EXPECT_EQ(leaf.key_count(), 0u);
    EXPECT_FALSE(leaf.is_full());
}

// =============================================================================
// Leaf Node: Boundary Key Values
// =============================================================================

TEST(QA_GDB92_LeafBoundary, Int64MinMax) {
    BTreeLeafNode leaf(1, 10);
    const auto min_val = std::numeric_limits<int64_t>::min();
    const auto max_val = std::numeric_limits<int64_t>::max();

    auto ins1 = leaf.insert(make_key(min_val), make_rid(1));
    ASSERT_TRUE(ins1.has_value());
    auto ins2 = leaf.insert(make_key(max_val), make_rid(2));
    ASSERT_TRUE(ins2.has_value());
    auto ins3 = leaf.insert(make_key(0), make_rid(3));
    ASSERT_TRUE(ins3.has_value());

    EXPECT_EQ(leaf.key_count(), 3u);

    // Verify sorted order: min < 0 < max
    EXPECT_EQ(key_val(leaf.key_at(0)), min_val);
    EXPECT_EQ(key_val(leaf.key_at(1)), 0);
    EXPECT_EQ(key_val(leaf.key_at(2)), max_val);

    // Search for each
    auto s1 = leaf.search(make_key(min_val));
    ASSERT_TRUE(s1.has_value());
    ASSERT_TRUE(s1->has_value());
    EXPECT_EQ(s1->value().page_id, 1u);

    auto s2 = leaf.search(make_key(max_val));
    ASSERT_TRUE(s2.has_value());
    ASSERT_TRUE(s2->has_value());
    EXPECT_EQ(s2->value().page_id, 2u);
}

TEST(QA_GDB92_LeafBoundary, NegativeKeys) {
    BTreeLeafNode leaf(1, 10);

    (void)leaf.insert(make_key(-100), make_rid(1));
    (void)leaf.insert(make_key(-1), make_rid(2));
    (void)leaf.insert(make_key(-50), make_rid(3));

    // Verify sorted order
    EXPECT_EQ(key_val(leaf.key_at(0)), -100);
    EXPECT_EQ(key_val(leaf.key_at(1)), -50);
    EXPECT_EQ(key_val(leaf.key_at(2)), -1);
}

TEST(QA_GDB92_LeafBoundary, ZeroKey) {
    BTreeLeafNode leaf(1, 10);
    auto ins = leaf.insert(make_key(0), make_rid(1));
    ASSERT_TRUE(ins.has_value());

    auto s = leaf.search(make_key(0));
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
    EXPECT_EQ(s->value().page_id, 1u);
}

TEST(QA_GDB92_LeafBoundary, AdjacentInt64Values) {
    BTreeLeafNode leaf(1, 10);
    // Insert INT64_MAX and INT64_MAX-1 to test adjacent values
    const auto max_val = std::numeric_limits<int64_t>::max();
    (void)leaf.insert(make_key(max_val), make_rid(1));
    (void)leaf.insert(make_key(max_val - 1), make_rid(2));

    auto s1 = leaf.search(make_key(max_val));
    ASSERT_TRUE(s1.has_value());
    ASSERT_TRUE(s1->has_value());
    EXPECT_EQ(s1->value().page_id, 1u);

    auto s2 = leaf.search(make_key(max_val - 1));
    ASSERT_TRUE(s2.has_value());
    ASSERT_TRUE(s2->has_value());
    EXPECT_EQ(s2->value().page_id, 2u);
}

// =============================================================================
// Leaf Node: Capacity and Overflow
// =============================================================================

TEST(QA_GDB92_LeafCapacity, ExactCapacity) {
    const uint16_t max_keys = 5;
    BTreeLeafNode leaf(1, max_keys);

    for (int i = 0; i < max_keys; ++i) {
        auto ins = leaf.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value());
    }

    EXPECT_TRUE(leaf.is_full());
    EXPECT_EQ(leaf.key_count(), max_keys);

    // All keys should be searchable
    for (int i = 0; i < max_keys; ++i) {
        auto s = leaf.search(make_key(i * 10));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value());
    }
}

TEST(QA_GDB92_LeafCapacity, InsertBeyondCapacityByDesign) {
    // Leaf insert does NOT enforce capacity (by design for insert-then-split).
    const uint16_t max_keys = 3;
    BTreeLeafNode leaf(1, max_keys);

    for (int i = 0; i < max_keys + 2; ++i) {
        auto ins = leaf.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value());
    }

    EXPECT_TRUE(leaf.is_full());
    EXPECT_EQ(leaf.key_count(), max_keys + 2);
}

TEST(QA_GDB92_LeafCapacity, MaxKeysOne) {
    // Edge case: max_keys = 1
    BTreeLeafNode leaf(1, 1);

    auto ins = leaf.insert(make_key(42), make_rid(1));
    ASSERT_TRUE(ins.has_value());
    EXPECT_TRUE(leaf.is_full());

    // Can still insert (by design)
    auto ins2 = leaf.insert(make_key(10), make_rid(2));
    ASSERT_TRUE(ins2.has_value());
    EXPECT_EQ(leaf.key_count(), 2u);
}

// =============================================================================
// Leaf Node: Duplicate Key Handling
// =============================================================================

TEST(QA_GDB92_LeafDuplicates, ManyDuplicatesNonUnique) {
    BTreeLeafNode leaf(1, 100);

    // Insert 10 entries with the same key
    for (int i = 0; i < 10; ++i) {
        auto ins = leaf.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value());
    }

    EXPECT_EQ(leaf.key_count(), 10u);

    // Search should find one of them
    auto s = leaf.search(make_key(42));
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
}

TEST(QA_GDB92_LeafDuplicates, RemoveOneOfManyDuplicates) {
    BTreeLeafNode leaf(1, 100);

    for (int i = 0; i < 5; ++i) {
        (void)leaf.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
    }

    auto del = leaf.remove(make_key(42));
    ASSERT_TRUE(del.has_value());
    EXPECT_TRUE(*del);
    EXPECT_EQ(leaf.key_count(), 4u);

    // Key should still be searchable (remaining duplicates)
    auto s = leaf.search(make_key(42));
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
}

TEST(QA_GDB92_LeafDuplicates, RemoveAllDuplicatesOneByOne) {
    BTreeLeafNode leaf(1, 100);

    for (int i = 0; i < 5; ++i) {
        (void)leaf.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
    }

    for (int i = 0; i < 5; ++i) {
        auto del = leaf.remove(make_key(42));
        ASSERT_TRUE(del.has_value());
        EXPECT_TRUE(*del);
    }

    EXPECT_EQ(leaf.key_count(), 0u);

    // Should return not-found now
    auto del = leaf.remove(make_key(42));
    ASSERT_TRUE(del.has_value());
    EXPECT_FALSE(*del);
}

TEST(QA_GDB92_LeafDuplicates, UniqueRejectsDuplicate) {
    BTreeLeafNode leaf(1, 10);

    auto ins1 = leaf.insert(make_key(42), make_rid(1), /*is_unique=*/true);
    ASSERT_TRUE(ins1.has_value());

    auto ins2 = leaf.insert(make_key(42), make_rid(2), /*is_unique=*/true);
    ASSERT_FALSE(ins2.has_value());
    EXPECT_EQ(ins2.error().code, StatusCode::CONSTRAINT_VIOLATION);
    EXPECT_EQ(leaf.key_count(), 1u);
}

// =============================================================================
// Leaf Node: Insert/Delete Interleaving
// =============================================================================

TEST(QA_GDB92_LeafInterleave, InsertDeleteInsertSameKey) {
    BTreeLeafNode leaf(1, 10);

    (void)leaf.insert(make_key(42), make_rid(1));
    (void)leaf.remove(make_key(42));
    (void)leaf.insert(make_key(42), make_rid(2));

    auto s = leaf.search(make_key(42));
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
    EXPECT_EQ(s->value().page_id, 2u);
}

TEST(QA_GDB92_LeafInterleave, InsertAllDeleteAllInsertAll) {
    BTreeLeafNode leaf(1, 20);

    // Insert 10 keys
    for (int i = 0; i < 10; ++i) {
        (void)leaf.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }
    EXPECT_EQ(leaf.key_count(), 10u);

    // Delete all
    for (int i = 0; i < 10; ++i) {
        auto del = leaf.remove(make_key(i));
        ASSERT_TRUE(del.has_value());
        EXPECT_TRUE(*del);
    }
    EXPECT_EQ(leaf.key_count(), 0u);

    // Insert again with different RIDs
    for (int i = 0; i < 10; ++i) {
        (void)leaf.insert(make_key(i), make_rid(static_cast<uint32_t>(i + 100)));
    }
    EXPECT_EQ(leaf.key_count(), 10u);

    // Verify new RIDs
    for (int i = 0; i < 10; ++i) {
        auto s = leaf.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value());
        EXPECT_EQ(s->value().page_id, static_cast<uint32_t>(i + 100));
    }
}

TEST(QA_GDB92_LeafInterleave, DeleteFromMiddle) {
    BTreeLeafNode leaf(1, 10);

    for (int i = 0; i < 5; ++i) {
        (void)leaf.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }
    // Keys: [0, 10, 20, 30, 40]

    // Delete middle key
    (void)leaf.remove(make_key(20));
    // Keys: [0, 10, 30, 40]

    EXPECT_EQ(leaf.key_count(), 4u);

    // Verify remaining are sorted
    EXPECT_EQ(key_val(leaf.key_at(0)), 0);
    EXPECT_EQ(key_val(leaf.key_at(1)), 10);
    EXPECT_EQ(key_val(leaf.key_at(2)), 30);
    EXPECT_EQ(key_val(leaf.key_at(3)), 40);
}

TEST(QA_GDB92_LeafInterleave, DeleteFirstAndLast) {
    BTreeLeafNode leaf(1, 10);

    (void)leaf.insert(make_key(10), make_rid(1));
    (void)leaf.insert(make_key(20), make_rid(2));
    (void)leaf.insert(make_key(30), make_rid(3));

    // Delete first
    (void)leaf.remove(make_key(10));
    EXPECT_EQ(leaf.key_count(), 2u);
    EXPECT_EQ(key_val(leaf.key_at(0)), 20);

    // Delete last
    (void)leaf.remove(make_key(30));
    EXPECT_EQ(leaf.key_count(), 1u);
    EXPECT_EQ(key_val(leaf.key_at(0)), 20);
}

// =============================================================================
// Leaf Node: Null Key Handling
// =============================================================================

TEST(QA_GDB92_LeafNull, InsertSearchDeleteNullKey) {
    BTreeLeafNode leaf(1, 10);

    KeyType null_key = {Value::make_null()};
    auto ins = leaf.insert(null_key, make_rid(1));
    ASSERT_TRUE(ins.has_value());

    auto s = leaf.search(null_key);
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
    EXPECT_EQ(s->value().page_id, 1u);

    auto del = leaf.remove(null_key);
    ASSERT_TRUE(del.has_value());
    EXPECT_TRUE(*del);
    EXPECT_EQ(leaf.key_count(), 0u);
}

TEST(QA_GDB92_LeafNull, NullSortsBeforeAll) {
    BTreeLeafNode leaf(1, 10);

    (void)leaf.insert(make_key(100), make_rid(1));
    (void)leaf.insert({Value::make_null()}, make_rid(2));
    (void)leaf.insert(make_key(-100), make_rid(3));

    EXPECT_EQ(leaf.key_count(), 3u);
    // Null should be first
    EXPECT_TRUE(std::holds_alternative<std::monostate>(leaf.key_at(0)[0].data()));
}

// =============================================================================
// Leaf Node: Composite Keys
// =============================================================================

TEST(QA_GDB92_LeafComposite, InsertAndSearchCompositeKeys) {
    BTreeLeafNode leaf(1, 10);

    (void)leaf.insert(make_composite_key(10, "apple"), make_rid(1));
    (void)leaf.insert(make_composite_key(10, "banana"), make_rid(2));
    (void)leaf.insert(make_composite_key(20, "apple"), make_rid(3));

    EXPECT_EQ(leaf.key_count(), 3u);

    auto s = leaf.search(make_composite_key(10, "banana"));
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
    EXPECT_EQ(s->value().page_id, 2u);
}

TEST(QA_GDB92_LeafComposite, CompositeKeySortedCorrectly) {
    BTreeLeafNode leaf(1, 10);

    (void)leaf.insert(make_composite_key(20, "apple"), make_rid(1));
    (void)leaf.insert(make_composite_key(10, "banana"), make_rid(2));
    (void)leaf.insert(make_composite_key(10, "apple"), make_rid(3));

    // Should be sorted: (10,"apple"), (10,"banana"), (20,"apple")
    EXPECT_EQ(std::get<int64_t>(leaf.key_at(0)[0].data()), 10);
    EXPECT_EQ(std::get<std::string>(leaf.key_at(0)[1].data()), "apple");
    EXPECT_EQ(std::get<int64_t>(leaf.key_at(1)[0].data()), 10);
    EXPECT_EQ(std::get<std::string>(leaf.key_at(1)[1].data()), "banana");
    EXPECT_EQ(std::get<int64_t>(leaf.key_at(2)[0].data()), 20);
}

// =============================================================================
// Leaf Node: String Keys
// =============================================================================

TEST(QA_GDB92_LeafString, EmptyStringKey) {
    BTreeLeafNode leaf(1, 10);
    KeyType empty_str_key = {Value(std::string(""))};

    auto ins = leaf.insert(empty_str_key, make_rid(1));
    ASSERT_TRUE(ins.has_value());

    auto s = leaf.search(empty_str_key);
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
}

TEST(QA_GDB92_LeafString, LongStringKey) {
    BTreeLeafNode leaf(1, 10);
    std::string long_str(10000, 'x');
    KeyType long_key = {Value(long_str)};

    auto ins = leaf.insert(long_key, make_rid(1));
    ASSERT_TRUE(ins.has_value());

    auto s = leaf.search(long_key);
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
}

// =============================================================================
// Leaf Node: is_underfull Thresholds
// =============================================================================

TEST(QA_GDB92_LeafUnderfull, VariousMaxKeys) {
    // max_keys=10: min = (10+1)/2 = 5
    {
        BTreeLeafNode leaf(1, 10);
        for (int i = 0; i < 4; ++i) {
            (void)leaf.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
        }
        EXPECT_TRUE(leaf.is_underfull(false));   // 4 < 5
        (void)leaf.insert(make_key(100), make_rid(100));
        EXPECT_FALSE(leaf.is_underfull(false));  // 5 >= 5
    }

    // max_keys=3: min = (3+1)/2 = 2
    {
        BTreeLeafNode leaf(1, 3);
        (void)leaf.insert(make_key(1), make_rid(1));
        EXPECT_TRUE(leaf.is_underfull(false));   // 1 < 2
        (void)leaf.insert(make_key(2), make_rid(2));
        EXPECT_FALSE(leaf.is_underfull(false));  // 2 >= 2
    }

    // max_keys=1: min = (1+1)/2 = 1
    {
        BTreeLeafNode leaf(1, 1);
        EXPECT_TRUE(leaf.is_underfull(false));   // 0 < 1
        (void)leaf.insert(make_key(1), make_rid(1));
        EXPECT_FALSE(leaf.is_underfull(false));  // 1 >= 1
    }

    // max_keys=2: min = (2+1)/2 = 1
    {
        BTreeLeafNode leaf(1, 2);
        EXPECT_TRUE(leaf.is_underfull(false));   // 0 < 1
        (void)leaf.insert(make_key(1), make_rid(1));
        EXPECT_FALSE(leaf.is_underfull(false));  // 1 >= 1
    }
}

TEST(QA_GDB92_LeafUnderfull, RootNeverUnderfull) {
    BTreeLeafNode leaf(1, 10);
    EXPECT_FALSE(leaf.is_underfull(true));  // Root with 0 keys is NOT underfull
}

// =============================================================================
// Leaf Node: Sibling Pointers
// =============================================================================

TEST(QA_GDB92_LeafSiblings, DefaultInvalid) {
    BTreeLeafNode leaf(1, 10);
    EXPECT_EQ(leaf.next_leaf_id(), invalid_page_id);
    EXPECT_EQ(leaf.prev_leaf_id(), invalid_page_id);
}

TEST(QA_GDB92_LeafSiblings, SetAndClear) {
    BTreeLeafNode leaf(1, 10);

    leaf.set_next_leaf_id(42);
    leaf.set_prev_leaf_id(7);
    EXPECT_EQ(leaf.next_leaf_id(), 42u);
    EXPECT_EQ(leaf.prev_leaf_id(), 7u);

    // Reset to invalid
    leaf.set_next_leaf_id(invalid_page_id);
    leaf.set_prev_leaf_id(invalid_page_id);
    EXPECT_EQ(leaf.next_leaf_id(), invalid_page_id);
    EXPECT_EQ(leaf.prev_leaf_id(), invalid_page_id);
}

// =============================================================================
// Leaf Node: lower_bound Edge Cases
// =============================================================================

TEST(QA_GDB92_LeafLowerBound, AllSameKeys) {
    BTreeLeafNode leaf(1, 10);

    for (int i = 0; i < 5; ++i) {
        (void)leaf.insert(make_key(42), make_rid(static_cast<uint32_t>(i)));
    }

    auto result = leaf.lower_bound(make_key(42));
    ASSERT_TRUE(result.has_value());
    // Should return position of first occurrence (0)
    EXPECT_EQ(*result, 0u);
}

TEST(QA_GDB92_LeafLowerBound, BetweenKeys) {
    BTreeLeafNode leaf(1, 10);

    (void)leaf.insert(make_key(10), make_rid(1));
    (void)leaf.insert(make_key(30), make_rid(2));

    // Key 20 is between 10 and 30
    auto result = leaf.lower_bound(make_key(20));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1u); // Position of 30
}

// =============================================================================
// Internal Node: Empty Node
// =============================================================================

TEST(QA_GDB92_InternalEmpty, SearchOnEmptyFails) {
    BTreeInternalNode node(1, 10);
    auto result = node.search(make_key(42));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INTERNAL_ERROR);
}

TEST(QA_GDB92_InternalEmpty, KeyCountIsZero) {
    BTreeInternalNode node(1, 10);
    EXPECT_EQ(node.key_count(), 0u);
    EXPECT_FALSE(node.is_full());
}

// =============================================================================
// Internal Node: Single Key Routing
// =============================================================================

TEST(QA_GDB92_InternalRouting, SingleKey) {
    BTreeInternalNode node(1, 10);
    node.children().push_back(100);
    (void)node.insert_at(0, make_key(50), 200);

    // key < 50 -> child 100
    auto r1 = node.search(make_key(25));
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(*r1, 100u);

    // key == 50 -> child 200
    auto r2 = node.search(make_key(50));
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, 200u);

    // key > 50 -> child 200
    auto r3 = node.search(make_key(75));
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(*r3, 200u);
}

TEST(QA_GDB92_InternalRouting, SingleChildOnly) {
    // Node with just one child and no keys (like a new root after being created)
    BTreeInternalNode node(1, 10);
    node.children().push_back(100);

    auto result = node.search(make_key(42));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 100u);
}

// =============================================================================
// Internal Node: Many Keys Routing
// =============================================================================

TEST(QA_GDB92_InternalRouting, ManyKeys) {
    BTreeInternalNode node(1, 20);

    node.children().push_back(100); // child for keys < 10
    for (int i = 0; i < 10; ++i) {
        (void)node.insert_at(static_cast<uint16_t>(i),
                             make_key((i + 1) * 10),
                             static_cast<PageId>(200 + i));
    }
    // Keys: [10, 20, 30, 40, 50, 60, 70, 80, 90, 100]
    // Children: [100, 200, 201, ..., 209]

    // Verify routing for each boundary
    for (int i = 0; i < 10; ++i) {
        int key = (i + 1) * 10;
        // Key exactly at separator -> goes right
        auto r = node.search(make_key(key));
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(*r, static_cast<PageId>(200 + i));
    }

    // Key before all separators
    auto r0 = node.search(make_key(5));
    ASSERT_TRUE(r0.has_value());
    EXPECT_EQ(*r0, 100u);

    // Key after all separators
    auto rn = node.search(make_key(150));
    ASSERT_TRUE(rn.has_value());
    EXPECT_EQ(*rn, 209u);
}

// =============================================================================
// Internal Node: Capacity
// =============================================================================

TEST(QA_GDB92_InternalCapacity, FillToMax) {
    const uint16_t max_keys = 5;
    BTreeInternalNode node(1, max_keys);

    node.children().push_back(100);
    for (int i = 0; i < max_keys; ++i) {
        auto ins = node.insert_at(
            static_cast<uint16_t>(i), make_key(i * 10),
            static_cast<PageId>(200 + i));
        ASSERT_TRUE(ins.has_value());
    }

    EXPECT_TRUE(node.is_full());
    EXPECT_EQ(node.key_count(), max_keys);
    EXPECT_EQ(node.children().size(), static_cast<size_t>(max_keys + 1));
}

TEST(QA_GDB92_InternalCapacity, InsertWhenFullFails) {
    const uint16_t max_keys = 3;
    BTreeInternalNode node(1, max_keys);

    node.children().push_back(100);
    for (int i = 0; i < max_keys; ++i) {
        (void)node.insert_at(static_cast<uint16_t>(i), make_key(i * 10),
                             static_cast<PageId>(200 + i));
    }

    EXPECT_TRUE(node.is_full());

    auto ins = node.insert_at(static_cast<uint16_t>(max_keys),
                              make_key(max_keys * 10), 999);
    ASSERT_FALSE(ins.has_value());
    EXPECT_EQ(ins.error().code, StatusCode::INTERNAL_ERROR);
}

TEST(QA_GDB92_InternalCapacity, MaxKeysOne) {
    BTreeInternalNode node(1, 1);

    node.children().push_back(100);
    auto ins = node.insert_at(0, make_key(50), 200);
    ASSERT_TRUE(ins.has_value());
    EXPECT_TRUE(node.is_full());

    auto ins2 = node.insert_at(1, make_key(100), 300);
    ASSERT_FALSE(ins2.has_value());
}

// =============================================================================
// Internal Node: Remove Operations
// =============================================================================

TEST(QA_GDB92_InternalRemove, RemoveFirst) {
    BTreeInternalNode node(1, 10);
    node.children().push_back(100);
    (void)node.insert_at(0, make_key(20), 200);
    (void)node.insert_at(1, make_key(40), 300);
    (void)node.insert_at(2, make_key(60), 400);

    node.remove_at(0); // Remove key 20, child 200

    EXPECT_EQ(node.key_count(), 2u);
    EXPECT_EQ(node.child_at(0), 100u);
    EXPECT_EQ(key_val(node.key_at(0)), 40);
    EXPECT_EQ(node.child_at(1), 300u);
    EXPECT_EQ(key_val(node.key_at(1)), 60);
    EXPECT_EQ(node.child_at(2), 400u);
}

TEST(QA_GDB92_InternalRemove, RemoveLast) {
    BTreeInternalNode node(1, 10);
    node.children().push_back(100);
    (void)node.insert_at(0, make_key(20), 200);
    (void)node.insert_at(1, make_key(40), 300);

    node.remove_at(1); // Remove key 40, child 300

    EXPECT_EQ(node.key_count(), 1u);
    EXPECT_EQ(node.children().size(), 2u);
    EXPECT_EQ(node.child_at(0), 100u);
    EXPECT_EQ(node.child_at(1), 200u);
}

TEST(QA_GDB92_InternalRemove, RemoveOnlyKey) {
    BTreeInternalNode node(1, 10);
    node.children().push_back(100);
    (void)node.insert_at(0, make_key(20), 200);

    node.remove_at(0); // Remove key 20, child 200

    EXPECT_EQ(node.key_count(), 0u);
    EXPECT_EQ(node.children().size(), 1u);
    EXPECT_EQ(node.child_at(0), 100u);
}

// =============================================================================
// Internal Node: is_underfull Thresholds
// =============================================================================

TEST(QA_GDB92_InternalUnderfull, VariousMaxKeys) {
    // max_keys=10: min = (10+1)/2 = 5
    {
        BTreeInternalNode node(1, 10);
        node.children().push_back(100);
        for (int i = 0; i < 4; ++i) {
            (void)node.insert_at(static_cast<uint16_t>(i), make_key(i * 10),
                                 static_cast<PageId>(200 + i));
        }
        EXPECT_TRUE(node.is_underfull(false));   // 4 < 5
        (void)node.insert_at(4, make_key(40), 300);
        EXPECT_FALSE(node.is_underfull(false));  // 5 >= 5
    }

    // max_keys=1: min = (1+1)/2 = 1
    {
        BTreeInternalNode node(1, 1);
        node.children().push_back(100);
        EXPECT_TRUE(node.is_underfull(false));   // 0 < 1
    }
}

TEST(QA_GDB92_InternalUnderfull, RootNeverUnderfull) {
    BTreeInternalNode node(1, 10);
    node.children().push_back(100);
    EXPECT_FALSE(node.is_underfull(true));  // Root with 0 keys is NOT underfull
}

// =============================================================================
// Internal Node: Parent Page ID
// =============================================================================

TEST(QA_GDB92_InternalParent, DefaultIsInvalid) {
    BTreeInternalNode node(1, 10);
    EXPECT_EQ(node.parent_page_id(), invalid_page_id);
}

TEST(QA_GDB92_InternalParent, SetAndGet) {
    BTreeInternalNode node(1, 10);
    node.set_parent_page_id(42);
    EXPECT_EQ(node.parent_page_id(), 42u);
}

// =============================================================================
// Key Comparison: Edge Cases
// =============================================================================

TEST(QA_GDB92_KeyCompare, EmptyKeysCompareEqual) {
    KeyType empty1 = {};
    KeyType empty2 = {};
    auto result = compare_keys(empty1, empty2);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::equal);
}

TEST(QA_GDB92_KeyCompare, ColumnCountMismatch) {
    auto result = compare_keys(make_key(1), make_composite_key(1, "x"));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB92_KeyCompare, BlobKeysError) {
    KeyType blob1 = {Value(Blob{0x01, 0x02})};
    KeyType blob2 = {Value(Blob{0x03, 0x04})};
    auto result = compare_keys(blob1, blob2);
    ASSERT_FALSE(result.has_value());
}

TEST(QA_GDB92_KeyCompare, NullVsNull) {
    KeyType null1 = {Value::make_null()};
    KeyType null2 = {Value::make_null()};
    auto result = compare_keys(null1, null2);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::equal);
}

// =============================================================================
// Stress Test
// =============================================================================

TEST(QA_GDB92_Stress, LeafInsertSearchDeleteMany) {
    const int count = 1000;
    BTreeLeafNode leaf(1, static_cast<uint16_t>(count + 10));

    // Insert 1000 keys in random order
    std::vector<int64_t> keys(count);
    std::iota(keys.begin(), keys.end(), 0);
    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);

    for (auto k : keys) {
        auto ins = leaf.insert(make_key(k), make_rid(static_cast<uint32_t>(k)));
        ASSERT_TRUE(ins.has_value());
    }

    EXPECT_EQ(leaf.key_count(), static_cast<uint16_t>(count));

    // Verify all searchable
    for (int64_t i = 0; i < count; ++i) {
        auto s = leaf.search(make_key(i));
        ASSERT_TRUE(s.has_value()) << "search failed for key " << i;
        ASSERT_TRUE(s->has_value()) << "key " << i << " not found";
        EXPECT_EQ(s->value().page_id, static_cast<uint32_t>(i));
    }

    // Verify sorted order
    for (uint16_t i = 1; i < leaf.key_count(); ++i) {
        auto cmp = compare_keys(leaf.key_at(i - 1), leaf.key_at(i));
        ASSERT_TRUE(cmp.has_value());
        EXPECT_EQ(*cmp, std::strong_ordering::less)
            << "keys not sorted at index " << i;
    }

    // Delete all in random order
    std::shuffle(keys.begin(), keys.end(), rng);
    for (auto k : keys) {
        auto del = leaf.remove(make_key(k));
        ASSERT_TRUE(del.has_value()) << "delete failed for key " << k;
        EXPECT_TRUE(*del) << "key " << k << " not found for deletion";
    }

    EXPECT_EQ(leaf.key_count(), 0u);
}

TEST(QA_GDB92_Stress, InternalNodeSearchConsistency) {
    const int count = 100;
    BTreeInternalNode node(1, static_cast<uint16_t>(count + 10));

    node.children().push_back(0); // leftmost child
    for (int i = 0; i < count; ++i) {
        (void)node.insert_at(static_cast<uint16_t>(i),
                             make_key((i + 1) * 10),
                             static_cast<PageId>(i + 1));
    }

    // Search for each key and verify routing consistency
    for (int i = 0; i < count; ++i) {
        int key = (i + 1) * 10;
        auto result = node.search(make_key(key));
        ASSERT_TRUE(result.has_value()) << "search failed for key " << key;
        // Key equal to separator goes right
        EXPECT_EQ(*result, static_cast<PageId>(i + 1));
    }

    // Search between all separators
    for (int i = 0; i < count - 1; ++i) {
        int key = (i + 1) * 10 + 5; // between separator i and i+1
        auto result = node.search(make_key(key));
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, static_cast<PageId>(i + 1));
    }
}

// =============================================================================
// RID Edge Cases
// =============================================================================

TEST(QA_GDB92_RID, MaxValues) {
    RID rid{std::numeric_limits<PageId>::max(),
            std::numeric_limits<SlotId>::max()};

    BTreeLeafNode leaf(1, 10);
    auto ins = leaf.insert(make_key(42), rid);
    ASSERT_TRUE(ins.has_value());

    auto s = leaf.search(make_key(42));
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
    EXPECT_EQ(s->value().page_id, std::numeric_limits<PageId>::max());
    EXPECT_EQ(s->value().slot_id, std::numeric_limits<SlotId>::max());
}

TEST(QA_GDB92_RID, OrderingComparison) {
    RID a{1, 0};
    RID b{1, 1};
    RID c{2, 0};

    EXPECT_LT(a, b);
    EXPECT_LT(b, c);
    EXPECT_LT(a, c);
}
