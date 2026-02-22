#include "test_btree_helpers.h"

#include <gtest/gtest.h>

#include <vector>

using namespace giodb;
using namespace giodb::test;

// =============================================================================
// KeyType Comparison Tests
// =============================================================================

TEST(BTreeKey, CompareEqual) {
    auto result = compare_keys(make_key(42), make_key(42));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::equal);
}

TEST(BTreeKey, CompareLess) {
    auto result = compare_keys(make_key(10), make_key(20));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::less);
}

TEST(BTreeKey, CompareGreater) {
    auto result = compare_keys(make_key(30), make_key(20));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::greater);
}

TEST(BTreeKey, CompositeKeyComparison) {
    // Same first column, different second column.
    auto a = make_composite_key(10, "apple");
    auto b = make_composite_key(10, "banana");
    auto result = compare_keys(a, b);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::less); // "apple" < "banana"
}

TEST(BTreeKey, CompositeKeyFirstColumnDiffers) {
    auto a = make_composite_key(5, "zebra");
    auto b = make_composite_key(10, "apple");
    auto result = compare_keys(a, b);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::less); // 5 < 10
}

TEST(BTreeKey, NullSortsBeforeNonNull) {
    KeyType null_key = {Value::make_null()};
    auto result = compare_keys(null_key, make_key(42));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::less);
}

TEST(BTreeKey, ColumnCountMismatchFails) {
    auto result = compare_keys(make_key(1), make_composite_key(1, "x"));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(BTreeKey, NonComparableTypeFails) {
    // BLOB is not comparable.
    KeyType blob_key = {Value(Blob{0x01, 0x02})};
    auto result = compare_keys(blob_key, blob_key);
    ASSERT_FALSE(result.has_value());
}

// =============================================================================
// BTreeLeafNode Tests
// =============================================================================

TEST(BTreeLeafNode, InsertAndSearch) {
    BTreeLeafNode leaf(1, 10);

    auto ins = leaf.insert(make_key(42), make_rid(100, 5));
    ASSERT_TRUE(ins.has_value());

    auto result = leaf.search(make_key(42));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().page_id, 100u);
    EXPECT_EQ(result->value().slot_id, 5u);
}

TEST(BTreeLeafNode, SearchNotFound) {
    BTreeLeafNode leaf(1, 10);
    (void)leaf.insert(make_key(42), make_rid(100));

    auto result = leaf.search(make_key(99));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
}

TEST(BTreeLeafNode, InsertMaintainsSortedOrder) {
    BTreeLeafNode leaf(1, 10);

    // Insert out of order.
    (void)leaf.insert(make_key(30), make_rid(3));
    (void)leaf.insert(make_key(10), make_rid(1));
    (void)leaf.insert(make_key(20), make_rid(2));
    (void)leaf.insert(make_key(50), make_rid(5));
    (void)leaf.insert(make_key(40), make_rid(4));

    EXPECT_EQ(leaf.key_count(), 5u);

    // Verify sorted order.
    for (uint16_t i = 1; i < leaf.key_count(); ++i) {
        auto cmp = compare_keys(leaf.key_at(i - 1), leaf.key_at(i));
        ASSERT_TRUE(cmp.has_value());
        EXPECT_EQ(*cmp, std::strong_ordering::less);
    }
}

TEST(BTreeLeafNode, DeleteKey) {
    BTreeLeafNode leaf(1, 10);
    (void)leaf.insert(make_key(10), make_rid(1));
    (void)leaf.insert(make_key(20), make_rid(2));
    (void)leaf.insert(make_key(30), make_rid(3));

    auto del = leaf.remove(make_key(20));
    ASSERT_TRUE(del.has_value());
    EXPECT_TRUE(*del);
    EXPECT_EQ(leaf.key_count(), 2u);

    // Verify deleted key is gone.
    auto search = leaf.search(make_key(20));
    ASSERT_TRUE(search.has_value());
    EXPECT_FALSE(search->has_value());

    // Others still present.
    auto s1 = leaf.search(make_key(10));
    ASSERT_TRUE(s1.has_value());
    EXPECT_TRUE(s1->has_value());

    auto s3 = leaf.search(make_key(30));
    ASSERT_TRUE(s3.has_value());
    EXPECT_TRUE(s3->has_value());
}

TEST(BTreeLeafNode, DeleteNotFound) {
    BTreeLeafNode leaf(1, 10);
    (void)leaf.insert(make_key(10), make_rid(1));

    auto del = leaf.remove(make_key(99));
    ASSERT_TRUE(del.has_value());
    EXPECT_FALSE(*del);
    EXPECT_EQ(leaf.key_count(), 1u);
}

TEST(BTreeLeafNode, DuplicateInsertNonUnique) {
    BTreeLeafNode leaf(1, 10);
    (void)leaf.insert(make_key(42), make_rid(1));
    auto ins = leaf.insert(make_key(42), make_rid(2));
    ASSERT_TRUE(ins.has_value()); // Should succeed in non-unique mode.
    EXPECT_EQ(leaf.key_count(), 2u);
}

TEST(BTreeLeafNode, DuplicateInsertUnique) {
    BTreeLeafNode leaf(1, 10);
    (void)leaf.insert(make_key(42), make_rid(1), true);
    auto ins = leaf.insert(make_key(42), make_rid(2), true);
    ASSERT_FALSE(ins.has_value());
    EXPECT_EQ(ins.error().code, StatusCode::CONSTRAINT_VIOLATION);
    EXPECT_EQ(leaf.key_count(), 1u);
}

TEST(BTreeLeafNode, LowerBound) {
    BTreeLeafNode leaf(1, 10);
    (void)leaf.insert(make_key(10), make_rid(1));
    (void)leaf.insert(make_key(20), make_rid(2));
    (void)leaf.insert(make_key(30), make_rid(3));
    (void)leaf.insert(make_key(40), make_rid(4));

    // Exact match.
    auto r1 = leaf.lower_bound(make_key(20));
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(*r1, 1u);

    // Between keys.
    auto r2 = leaf.lower_bound(make_key(25));
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, 2u); // Position of 30.

    // Before all keys.
    auto r3 = leaf.lower_bound(make_key(5));
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(*r3, 0u);

    // After all keys.
    auto r4 = leaf.lower_bound(make_key(50));
    ASSERT_TRUE(r4.has_value());
    EXPECT_EQ(*r4, 4u); // Past end.
}

TEST(BTreeLeafNode, SiblingPointers) {
    BTreeLeafNode leaf(1, 10);
    EXPECT_EQ(leaf.next_leaf_id(), invalid_page_id);
    EXPECT_EQ(leaf.prev_leaf_id(), invalid_page_id);

    leaf.set_next_leaf_id(2);
    leaf.set_prev_leaf_id(0);
    EXPECT_EQ(leaf.next_leaf_id(), 2u);
    EXPECT_EQ(leaf.prev_leaf_id(), 0u);
}

TEST(BTreeLeafNode, FillToCapacity) {
    BTreeLeafNode leaf(1, 5);

    for (int i = 0; i < 5; ++i) {
        auto ins = leaf.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value());
    }

    EXPECT_TRUE(leaf.is_full());
    EXPECT_EQ(leaf.key_count(), 5u);

    // Verify all keys searchable.
    for (int i = 0; i < 5; ++i) {
        auto s = leaf.search(make_key(i * 10));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value());
    }
}

TEST(BTreeLeafNode, IsUnderfull) {
    BTreeLeafNode leaf(1, 10);
    // min_keys = ceil(10/2) = 5
    (void)leaf.insert(make_key(10), make_rid(1));
    (void)leaf.insert(make_key(20), make_rid(2));

    EXPECT_TRUE(leaf.is_underfull(false));  // 2 < 5
    EXPECT_FALSE(leaf.is_underfull(true));  // Root is never underfull.
}

// =============================================================================
// BTreeInternalNode Tests
// =============================================================================

TEST(BTreeInternalNode, SearchRouting) {
    BTreeInternalNode node(1, 10);

    // Setup: keys = [20, 40], children = [child1, child2, child3]
    node.children().push_back(100); // Left of 20.
    node.keys().push_back(make_key(20));
    node.children().push_back(200); // Between 20 and 40.
    node.keys().push_back(make_key(40));
    node.children().push_back(300); // Right of 40.

    // Search for key < 20 -> child 100.
    auto r1 = node.search(make_key(10));
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(*r1, 100u);

    // Search for key == 20 -> child 200 (keys_[0] <= key, so go right).
    auto r2 = node.search(make_key(20));
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, 200u);

    // Search for 20 < key < 40 -> child 200.
    auto r3 = node.search(make_key(30));
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(*r3, 200u);

    // Search for key == 40 -> child 300.
    auto r4 = node.search(make_key(40));
    ASSERT_TRUE(r4.has_value());
    EXPECT_EQ(*r4, 300u);

    // Search for key > 40 -> child 300.
    auto r5 = node.search(make_key(50));
    ASSERT_TRUE(r5.has_value());
    EXPECT_EQ(*r5, 300u);
}

TEST(BTreeInternalNode, InsertSeparator) {
    BTreeInternalNode node(1, 10);

    node.children().push_back(100); // Initial single child.

    // Insert separator keys and children.
    auto ins1 = node.insert_at(0, make_key(20), 200);
    ASSERT_TRUE(ins1.has_value());

    auto ins2 = node.insert_at(1, make_key(40), 300);
    ASSERT_TRUE(ins2.has_value());

    EXPECT_EQ(node.key_count(), 2u);
    EXPECT_EQ(node.children().size(), 3u);

    // Verify key ordering.
    auto cmp = compare_keys(node.key_at(0), node.key_at(1));
    ASSERT_TRUE(cmp.has_value());
    EXPECT_EQ(*cmp, std::strong_ordering::less);
}

TEST(BTreeInternalNode, RemoveAt) {
    BTreeInternalNode node(1, 10);

    node.children().push_back(100);
    (void)node.insert_at(0, make_key(20), 200);
    (void)node.insert_at(1, make_key(40), 300);

    // Remove key at index 0 (key 20, removes child 200).
    node.remove_at(0);

    EXPECT_EQ(node.key_count(), 1u);
    EXPECT_EQ(node.children().size(), 2u);
    EXPECT_EQ(node.child_at(0), 100u);
    EXPECT_EQ(node.child_at(1), 300u);
}

TEST(BTreeInternalNode, IsFull) {
    BTreeInternalNode node(1, 3);

    node.children().push_back(100);
    (void)node.insert_at(0, make_key(10), 200);
    (void)node.insert_at(1, make_key(20), 300);
    (void)node.insert_at(2, make_key(30), 400);

    EXPECT_TRUE(node.is_full());
}

TEST(BTreeInternalNode, IsUnderfull) {
    BTreeInternalNode node(1, 10);
    // min_keys = ceil(10/2) = 5

    node.children().push_back(100);
    (void)node.insert_at(0, make_key(10), 200);

    EXPECT_TRUE(node.is_underfull(false));  // 1 < 5
    EXPECT_FALSE(node.is_underfull(true));  // Root is never underfull.
}

TEST(BTreeInternalNode, SearchEmptyFails) {
    BTreeInternalNode node(1, 10);

    auto result = node.search(make_key(10));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INTERNAL_ERROR);
}

TEST(BTreeInternalNode, InsertFullFails) {
    BTreeInternalNode node(1, 2);

    node.children().push_back(100);
    (void)node.insert_at(0, make_key(10), 200);
    (void)node.insert_at(1, make_key(20), 300);

    EXPECT_TRUE(node.is_full());

    auto ins = node.insert_at(2, make_key(30), 400);
    ASSERT_FALSE(ins.has_value());
    EXPECT_EQ(ins.error().code, StatusCode::INTERNAL_ERROR);
}

// =============================================================================
// RID Tests
// =============================================================================

TEST(RID, DefaultIsZero) {
    RID rid;
    EXPECT_EQ(rid.page_id, 0u);
    EXPECT_EQ(rid.slot_id, 0u);
}

TEST(RID, Equality) {
    RID a{10, 5};
    RID b{10, 5};
    RID c{10, 6};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(RID, InvalidSentinel) {
    auto invalid = RID::invalid();
    EXPECT_EQ(invalid.page_id, 0u);
    EXPECT_EQ(invalid.slot_id, 0u);
}
