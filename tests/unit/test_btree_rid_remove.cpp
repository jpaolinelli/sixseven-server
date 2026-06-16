/// GDB-833: BTreeIndex RID-qualified remove for non-unique trees.
///
/// Regression suite proving that remove(key, rid) removes EXACTLY the entry
/// whose RID matches and leaves all other entries for the same key intact.
/// All expected RID values are hardcoded — a regression that deletes the wrong
/// duplicate entry MUST fail at least one assertion below.

#include "sixseven/index/btree_index.h"
#include "sixseven/index/btree_key.h"
#include "sixseven/index/btree_node.h"
#include "sixseven/index/rid.h"

#include <gtest/gtest.h>

#include <vector>

#include "test_btree_helpers.h"

using namespace sixseven;
using namespace sixseven::test;

// =============================================================================
// GDB-833: remove(key, rid) — core correctness
// =============================================================================

/// Insert two entries with the SAME key but DIFFERENT RIDs.
/// Remove one via the RID-qualified overload and assert the other is still
/// findable and the removed one is gone.
TEST(BTreeRidRemove, RemoveOneOfTwoDuplicates) {
    auto tree = make_test_index(/*leaf_max=*/8, /*internal_max=*/8, /*is_unique=*/false);

    KeyType key = make_key(42);
    RID rid_a = make_rid(10, 0); // page=10, slot=0
    RID rid_b = make_rid(20, 0); // page=20, slot=0

    ASSERT_TRUE(tree.insert(key, rid_a).has_value());
    ASSERT_TRUE(tree.insert(key, rid_b).has_value());
    ASSERT_EQ(tree.size(), 2u);

    // Remove only the entry for rid_a.
    auto r = tree.remove(key, rid_a);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(*r); // Must report found+deleted.
    EXPECT_EQ(tree.size(), 1u);

    // rid_b must still be findable (search returns the surviving entry).
    auto found = tree.search(key);
    ASSERT_TRUE(found.has_value()) << found.error().message;
    ASSERT_TRUE(found->has_value()) << "rid_b should still be indexed";
    // The surviving RID must be exactly rid_b={page=20,slot=0}, NOT rid_a.
    EXPECT_EQ(found->value().page_id, 20u);
    EXPECT_EQ(found->value().slot_id, 0u);
}

/// Symmetric: remove rid_b first; rid_a must survive with its exact values.
TEST(BTreeRidRemove, RemoveSecondOfTwoDuplicates) {
    auto tree = make_test_index(8, 8, false);

    KeyType key = make_key(99);
    RID rid_a = make_rid(5, 3);
    RID rid_b = make_rid(7, 1);

    ASSERT_TRUE(tree.insert(key, rid_a).has_value());
    ASSERT_TRUE(tree.insert(key, rid_b).has_value());

    auto r = tree.remove(key, rid_b);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(*r);
    EXPECT_EQ(tree.size(), 1u);

    auto found = tree.search(key);
    ASSERT_TRUE(found.has_value());
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ(found->value().page_id, 5u);
    EXPECT_EQ(found->value().slot_id, 3u);
}

/// Remove the last duplicate of a key — the key must disappear from the index.
TEST(BTreeRidRemove, RemoveLastDuplicateCleansUpKey) {
    auto tree = make_test_index(8, 8, false);

    KeyType key = make_key(7);
    RID rid = make_rid(3, 2);

    ASSERT_TRUE(tree.insert(key, rid).has_value());
    ASSERT_EQ(tree.size(), 1u);

    auto r = tree.remove(key, rid);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(*r);
    EXPECT_EQ(tree.size(), 0u);
    EXPECT_TRUE(tree.empty());

    // Search must now return nullopt.
    auto found = tree.search(key);
    ASSERT_TRUE(found.has_value());
    EXPECT_FALSE(found->has_value());
}

/// remove(key, rid) for a non-existent RID is a no-op — returns false,
/// leaves size unchanged, and the existing entry is still findable.
TEST(BTreeRidRemove, NonExistentRidIsNoOp) {
    auto tree = make_test_index(8, 8, false);

    KeyType key = make_key(55);
    RID rid_real = make_rid(11, 0);
    RID rid_ghost = make_rid(99, 9); // Never inserted.

    ASSERT_TRUE(tree.insert(key, rid_real).has_value());

    auto r = tree.remove(key, rid_ghost);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(*r); // Not found.
    EXPECT_EQ(tree.size(), 1u);

    // The real entry must still be present.
    auto found = tree.search(key);
    ASSERT_TRUE(found.has_value());
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ(found->value().page_id, 11u);
    EXPECT_EQ(found->value().slot_id, 0u);
}

/// remove(key, rid) on a key that does not exist at all returns false.
TEST(BTreeRidRemove, NonExistentKeyReturnsNotFound) {
    auto tree = make_test_index(8, 8, false);

    ASSERT_TRUE(tree.insert(make_key(10), make_rid(1)).has_value());

    auto r = tree.remove(make_key(999), make_rid(1));
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(*r);
    EXPECT_EQ(tree.size(), 1u);
}

/// remove(key, rid) on an empty tree returns false without crashing.
TEST(BTreeRidRemove, EmptyTreeReturnsNotFound) {
    auto tree = make_test_index(8, 8, false);
    auto r = tree.remove(make_key(1), make_rid(1));
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(*r);
}

/// Verify that duplicates spread across a leaf split are handled correctly.
/// With leaf_max=4 and many duplicates, some entries spill to the next leaf.
/// The RID-qualified remove must walk sibling leaves to find the target.
TEST(BTreeRidRemove, DuplicatesSpanningLeafBoundary) {
    // leaf_max=4 so that 5 same-key entries force a split.
    auto tree = make_test_index(4, 4, false);

    KeyType key = make_key(100);
    // Insert 6 entries for the same key — guaranteed to span at least two leaves.
    std::vector<RID> rids;
    for (uint32_t i = 1; i <= 6; ++i) {
        RID r{i, static_cast<uint16_t>(i)};
        rids.push_back(r);
        ASSERT_TRUE(tree.insert(key, r).has_value()) << "insert i=" << i;
    }
    ASSERT_EQ(tree.size(), 6u);

    // Remove the entry for RID {3, 3} — which may be on a non-first leaf.
    RID target = make_rid(3, 3);
    auto rm = tree.remove(key, target);
    ASSERT_TRUE(rm.has_value()) << rm.error().message;
    EXPECT_TRUE(*rm);
    EXPECT_EQ(tree.size(), 5u);

    // Use a range scan to collect all remaining entries and verify:
    // - {3,3} is gone
    // - all other 5 RIDs are still present
    auto scan_result = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan_result.has_value()) << scan_result.error().message;
    auto entries = collect_scan(*scan_result);
    ASSERT_TRUE(entries.has_value()) << entries.error().message;

    ASSERT_EQ(entries->size(), 5u);

    bool found_target = false;
    for (auto& [k, r] : *entries) {
        if (r == target) {
            found_target = true;
        }
    }
    EXPECT_FALSE(found_target) << "Removed RID {3,3} must not appear in scan";

    // Verify each non-removed RID is present exactly once.
    for (auto& expected_rid : rids) {
        if (expected_rid == target) {
            continue;
        }
        int count = 0;
        for (auto& [k, r] : *entries) {
            if (r == expected_rid) {
                ++count;
            }
        }
        EXPECT_EQ(count, 1) << "RID {" << expected_rid.page_id << "," << expected_rid.slot_id
                            << "} should appear exactly once";
    }
}

/// Regression for GDB-833 reviewer fix: find_leaf() routes key>=separator to
/// the RIGHT child, so for a duplicate-key run spanning multiple leaves it
/// always lands on the RIGHTMOST leaf.  A right-only walk then silently misses
/// entries on LEFT sibling leaves.
///
/// BTreeLeafNode::insert uses lower_bound (strict-less comparison), so equal
/// keys are inserted at position 0 (prepend semantics).
/// With leaf_max=4 and 8 same-key inserts {page=1..8, slot=0}, two splits:
///   after 5th insert: leaf0=[{5,0},{4,0}]  leaf1=[{3,0},{2,0},{1,0}]
///   after 7th insert: leaf1=[{7,0},{6,0}]  leaf2=[{3,0},{2,0},{1,0}]
///   8th insert -> leaf2=[{8,0},{3,0},{2,0},{1,0}]
/// Final: leaf0=[{5,0},{4,0}]  leaf1=[{7,0},{6,0}]  leaf2=[{8,0},{3,0},{2,0},{1,0}]
/// find_leaf(key) -> leaf2 (rightmost).
/// RIDs {4,0} and {5,0} sit on leaf0 (leftmost) - unreachable by right-only walk.
///
/// This test removes {4,0} (on the leftmost leaf) and asserts:
///   - remove returns true  (found + deleted)
///   - {4,0} is gone from a full scan
///   - all 7 hardcoded surviving RIDs {1,2,3,5,6,7,8} are still present
/// This test MUST fail against right-only-walk code and pass with the left-walk fix.
TEST(BTreeRidRemove, RemoveFromLeftmostLeafInDuplicateRun) {
    // leaf_max=4 so 8 same-key entries span exactly 3 leaves after two splits.
    auto tree = make_test_index(/*leaf_max=*/4, /*internal_max=*/4, /*is_unique=*/false);

    KeyType key = make_key(50);
    // Insert pages 1..8 in order.  Due to lower_bound prepend semantics,
    // {4,0} and {5,0} end up on leaf0 (leftmost) after two splits.
    const uint32_t N = 8;
    for (uint32_t i = 1; i <= N; ++i) {
        ASSERT_TRUE(tree.insert(key, make_rid(i, 0)).has_value()) << "insert i=" << i;
    }
    ASSERT_EQ(tree.size(), static_cast<uint64_t>(N));

    // {4,0} sits on leaf0 (the leftmost leaf).
    // Right-only code returns false here; the left-walk fix must return true.
    RID target = make_rid(4, 0);
    auto rm = tree.remove(key, target);
    ASSERT_TRUE(rm.has_value()) << rm.error().message;
    EXPECT_TRUE(*rm) << "remove(key, {4,0}) must return true; entry is on the leftmost leaf";
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(N - 1));

    // Full scan: collect all surviving entries.
    auto scan_result = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan_result.has_value()) << scan_result.error().message;
    auto entries = collect_scan(*scan_result);
    ASSERT_TRUE(entries.has_value()) << entries.error().message;
    ASSERT_EQ(entries->size(), static_cast<size_t>(N - 1));

    // {4,0} must be absent.
    for (auto& [k, r] : *entries) {
        EXPECT_FALSE(r.page_id == 4u && r.slot_id == 0u)
            << "Removed RID {4,0} must not appear in scan";
    }

    // Each of the 7 surviving RIDs must appear exactly once (hardcoded values).
    const uint32_t survivors[] = {1u, 2u, 3u, 5u, 6u, 7u, 8u};
    for (uint32_t p : survivors) {
        int count = 0;
        for (auto& [k, r] : *entries) {
            if (r.page_id == p && r.slot_id == 0u) {
                ++count;
            }
        }
        EXPECT_EQ(count, 1) << "RID {" << p << ",0} must survive exactly once";
    }
}

/// Unique-tree callers: key-only remove(key) still works correctly.
/// This guards against accidentally breaking the existing unique-tree path.
TEST(BTreeRidRemove, UniqueTreeKeyOnlyRemoveStillWorks) {
    auto tree = make_test_index(8, 8, /*is_unique=*/true);

    ASSERT_TRUE(tree.insert(make_key(1), make_rid(10)).has_value());
    ASSERT_TRUE(tree.insert(make_key(2), make_rid(20)).has_value());
    ASSERT_TRUE(tree.insert(make_key(3), make_rid(30)).has_value());

    auto r = tree.remove(make_key(2));
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(*r);
    EXPECT_EQ(tree.size(), 2u);

    auto found2 = tree.search(make_key(2));
    ASSERT_TRUE(found2.has_value());
    EXPECT_FALSE(found2->has_value()) << "key=2 should be gone";

    auto found1 = tree.search(make_key(1));
    ASSERT_TRUE(found1.has_value());
    ASSERT_TRUE(found1->has_value());
    EXPECT_EQ(found1->value().page_id, 10u);

    auto found3 = tree.search(make_key(3));
    ASSERT_TRUE(found3.has_value());
    ASSERT_TRUE(found3->has_value());
    EXPECT_EQ(found3->value().page_id, 30u);
}

/// Three duplicates for the same key: remove the middle one by RID.
/// Before: {page=1},{page=2},{page=3}. Remove {page=2}. After: {page=1},{page=3}.
TEST(BTreeRidRemove, RemoveMiddleDuplicateOfThree) {
    auto tree = make_test_index(8, 8, false);

    KeyType key = make_key(77);
    RID rid1 = make_rid(1, 0);
    RID rid2 = make_rid(2, 0);
    RID rid3 = make_rid(3, 0);

    ASSERT_TRUE(tree.insert(key, rid1).has_value());
    ASSERT_TRUE(tree.insert(key, rid2).has_value());
    ASSERT_TRUE(tree.insert(key, rid3).has_value());
    ASSERT_EQ(tree.size(), 3u);

    auto r = tree.remove(key, rid2);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(*r);
    ASSERT_EQ(tree.size(), 2u);

    // Collect all remaining via range scan.
    auto scan_result = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan_result.has_value());
    auto entries = collect_scan(*scan_result);
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 2u);

    // rid2 must be absent.
    for (auto& [k, r2] : *entries) {
        EXPECT_NE(r2.page_id, 2u) << "rid2={page=2} must be absent after removal";
    }

    // rid1 and rid3 must be present.
    bool has_rid1 = false;
    bool has_rid3 = false;
    for (auto& [k, r2] : *entries) {
        if (r2.page_id == 1u) {
            has_rid1 = true;
        }
        if (r2.page_id == 3u) {
            has_rid3 = true;
        }
    }
    EXPECT_TRUE(has_rid1) << "rid1={page=1} must survive";
    EXPECT_TRUE(has_rid3) << "rid3={page=3} must survive";
}
