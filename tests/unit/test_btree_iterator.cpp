#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "test_btree_helpers.h"

using namespace sixseven;
using namespace sixseven::test;

// =============================================================================
// GDB-863: BTreeIterator unit tests
// =============================================================================

// ---------------------------------------------------------------------------
// Empty tree
// ---------------------------------------------------------------------------

TEST(BTreeIteratorTest, EmptyTreeIsEnd) {
    auto tree = make_test_index();
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    // An empty tree yields an exhausted iterator immediately.
    EXPECT_TRUE(scan->is_end());
}

TEST(BTreeIteratorTest, EmptyTreeNextReturnsNullopt) {
    auto tree = make_test_index();
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entry = scan->next();
    ASSERT_TRUE(entry.has_value());
    EXPECT_FALSE(entry->has_value());
}

// ---------------------------------------------------------------------------
// Default-constructed (exhausted) iterator
// ---------------------------------------------------------------------------

TEST(BTreeIteratorTest, DefaultConstructedIsEnd) {
    BTreeIterator it;
    EXPECT_TRUE(it.is_end());
}

TEST(BTreeIteratorTest, DefaultConstructedNextReturnsNullopt) {
    BTreeIterator it;
    auto entry = it.next();
    ASSERT_TRUE(entry.has_value());
    EXPECT_FALSE(entry->has_value());
}

// ---------------------------------------------------------------------------
// Single-entry tree
// ---------------------------------------------------------------------------

TEST(BTreeIteratorTest, SingleEntryForwardIteration) {
    auto tree = make_test_index();
    ASSERT_TRUE(tree.insert(make_key(100), make_rid(1, 0)).has_value());

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    EXPECT_FALSE(scan->is_end());

    // Use collect_scan to avoid clang-tidy unchecked-optional-access on direct next() calls.
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 1U);
    EXPECT_EQ(key_val((*entries)[0].first), 100);
    EXPECT_EQ((*entries)[0].second.page_id, 1U);

    // After collect_scan the iterator must be exhausted.
    EXPECT_TRUE(scan->is_end());
}

// ---------------------------------------------------------------------------
// Forward iteration -- sorted order
// ---------------------------------------------------------------------------

TEST(BTreeIteratorTest, ForwardIterationSortedOrder) {
    auto tree = make_test_index();
    // Insert in arbitrary order; iterator must return in ascending order.
    for (int v : {50, 10, 40, 30, 20}) {
        ASSERT_TRUE(tree.insert(make_key(v), make_rid(static_cast<uint32_t>(v))).has_value());
    }

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 5U);

    // Hardcoded expected sorted order.
    const std::vector<int64_t> expected = {10, 20, 30, 40, 50};
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(key_val((*entries)[i].first), expected[i]) << "Mismatch at index " << i;
    }
}

// ---------------------------------------------------------------------------
// Multi-page traversal (crosses leaf boundaries)
// ---------------------------------------------------------------------------

TEST(BTreeIteratorTest, MultiPageForwardIteration) {
    // leaf_max=4 forces splits; 20 entries creates multiple leaf pages.
    auto tree = make_test_index(4, 4);
    constexpr int entry_count = 20;
    for (int i = 1; i <= entry_count; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), static_cast<size_t>(entry_count));

    // Every key must appear exactly once in ascending order.
    for (int i = 0; i < entry_count; ++i) {
        EXPECT_EQ(key_val((*entries)[static_cast<size_t>(i)].first), static_cast<int64_t>(i + 1))
            << "Wrong key at position " << i;
    }
}

// ---------------------------------------------------------------------------
// End-key exclusivity (exclusive upper bound)
// ---------------------------------------------------------------------------

TEST(BTreeIteratorTest, EndKeyIsExclusive) {
    auto tree = make_test_index();
    for (int i = 10; i <= 50; i += 10) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    // [10, 40) should yield 10, 20, 30 -- NOT 40.
    auto scan = tree.range_scan(make_key(10), make_key(40));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 3U);
    EXPECT_EQ(key_val((*entries)[0].first), 10);
    EXPECT_EQ(key_val((*entries)[1].first), 20);
    EXPECT_EQ(key_val((*entries)[2].first), 30);
}

TEST(BTreeIteratorTest, EndKeyEqualBeginKeyIsEmpty) {
    // [30, 30) is an empty range.
    auto tree = make_test_index();
    for (int i = 10; i <= 50; i += 10) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    auto scan = tree.range_scan(make_key(30), make_key(30));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 0U);
}

// ---------------------------------------------------------------------------
// Open-ended bounds
// ---------------------------------------------------------------------------

TEST(BTreeIteratorTest, OpenEndBoundReturnsAll) {
    auto tree = make_test_index();
    for (int i = 1; i <= 5; ++i) {
        ASSERT_TRUE(
            tree.insert(make_key(int64_t{i} * 10), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    // No begin key, no end key -> all 5 entries.
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 5U);
}

TEST(BTreeIteratorTest, OpenEndBoundFromMiddle) {
    auto tree = make_test_index();
    for (int i = 10; i <= 50; i += 10) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    // [30, inf) -> 30, 40, 50.
    auto scan = tree.range_scan(make_key(30), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 3U);
    EXPECT_EQ(key_val((*entries)[0].first), 30);
    EXPECT_EQ(key_val((*entries)[2].first), 50);
}

// ---------------------------------------------------------------------------
// Seek to non-existing start key
// ---------------------------------------------------------------------------

TEST(BTreeIteratorTest, BeginKeyNotInTreeStartsAtNextLarger) {
    auto tree = make_test_index();
    for (int i = 10; i <= 50; i += 10) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    // Begin at 25 (not present); should start at 30.
    auto scan = tree.range_scan(make_key(25), make_key(45));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 2U);
    EXPECT_EQ(key_val((*entries)[0].first), 30);
    EXPECT_EQ(key_val((*entries)[1].first), 40);
}

// ---------------------------------------------------------------------------
// Exhaustion after end
// ---------------------------------------------------------------------------

TEST(BTreeIteratorTest, IteratorExhaustedAfterFullScan) {
    auto tree = make_test_index();
    ASSERT_TRUE(tree.insert(make_key(1), make_rid(1)).has_value());
    ASSERT_TRUE(tree.insert(make_key(2), make_rid(2)).has_value());

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());

    // Drain all entries.
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 2U);

    // Iterator must report end.
    EXPECT_TRUE(scan->is_end());

    // Subsequent next() calls on exhausted iterator must return nullopt, not error.
    auto extra = scan->next();
    ASSERT_TRUE(extra.has_value());
    EXPECT_FALSE(extra->has_value());
}

// ---------------------------------------------------------------------------
// End-key boundary at leaf boundary (crosses page)
// ---------------------------------------------------------------------------

TEST(BTreeIteratorTest, EndKeyOnLeafBoundaryExclusion) {
    // With leaf_max=4, keys {1,2,3,4,5,...} span multiple leaves.
    // The split boundary key must be treated as exclusive when it is end_key.
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 10; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    // After a split with leaf_max=4 and 10 keys the split boundary will be
    // around key 5. Use [1, 5) and verify 5 is excluded.
    auto scan = tree.range_scan(make_key(1), make_key(5));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    // Should yield 1, 2, 3, 4 -- exactly 4 entries.
    ASSERT_EQ(entries->size(), 4U);
    EXPECT_EQ(key_val((*entries)[0].first), 1);
    EXPECT_EQ(key_val((*entries)[3].first), 4);
}
