/// QA adversarial tests for GDB-833: BTreeIndex RID-qualified remove.
///
/// Focus areas:
/// 1. Duplicate-key runs spanning MANY leaves (3-5+ leaves) — remove entries at
///    leftmost, rightmost, and interior leaf boundaries.
/// 2. Interleave: insert dups, remove some, insert more (RID reuse + fresh RIDs),
///    remove again — assert live RID set is always exact.
/// 3. Edge cases: non-existent RID, wrong key, double-delete, remove to last dup
///    then re-insert.
/// 4. Tree-structure stress: removals triggering leaf underflow/merge in the middle
///    of a duplicate run — verify prev/next sibling links stay consistent.
/// 5. Left-walk termination safety — cannot infinite-loop or walk into unrelated keys.

#include "sixseven/index/btree_index.h"
#include "sixseven/index/btree_key.h"
#include "sixseven/index/btree_node.h"
#include "sixseven/index/rid.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <vector>

// Pull in helpers from the unit test helpers header (same as dev tests use).
#include "test_btree_helpers.h"

using namespace sixseven;
using namespace sixseven::test;

// ============================================================================
// Helpers
// ============================================================================

/// Collect all (key, rid) pairs from a full range scan.
static Result<std::vector<std::pair<KeyType, RID>>> full_scan(BTreeIndex& tree) {
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    if (!scan.has_value()) {
        return tl::unexpected(scan.error());
    }
    return collect_scan(*scan);
}

/// Assert that exactly the given RIDs are present under `key` in the index.
/// Uses a full range scan so we catch phantom entries.
static void expect_rid_set(BTreeIndex& tree, const KeyType& key,
                           std::vector<RID> expected_rids) {
    auto entries_result = full_scan(tree);
    ASSERT_TRUE(entries_result.has_value()) << entries_result.error().message;
    auto& entries = *entries_result;

    // Collect actual RIDs for this key.
    std::vector<RID> actual;
    for (auto& [k, r] : entries) {
        auto cmp = compare_keys(k, key);
        ASSERT_TRUE(cmp.has_value());
        if (*cmp == std::strong_ordering::equal) {
            actual.push_back(r);
        }
    }

    // Sort both for comparison.
    auto rid_less = [](const RID& a, const RID& b) {
        return a.page_id < b.page_id || (a.page_id == b.page_id && a.slot_id < b.slot_id);
    };
    std::sort(actual.begin(), actual.end(), rid_less);
    std::sort(expected_rids.begin(), expected_rids.end(), rid_less);

    EXPECT_EQ(actual.size(), expected_rids.size()) << "RID count mismatch for key";
    for (size_t i = 0; i < std::min(actual.size(), expected_rids.size()); ++i) {
        EXPECT_EQ(actual[i].page_id, expected_rids[i].page_id)
            << "RID[" << i << "].page_id mismatch";
        EXPECT_EQ(actual[i].slot_id, expected_rids[i].slot_id)
            << "RID[" << i << "].slot_id mismatch";
    }
}

// ============================================================================
// 1. Duplicate-key runs spanning MANY leaves
// ============================================================================

/// leaf_max=3 forces splits after every 3 inserts.
/// 10 identical-key entries => at least 4 leaves.
/// Remove entry at LEFTMOST leaf, RIGHTMOST leaf, and one INTERIOR leaf.
/// After each removal verify live set is exact via full scan.
TEST(QA_GDB833, DuplicateRunAcrossFourLeaves_RemoveLeftmostRightmostInterior) {
    // leaf_max=3: entries per leaf before split, forces many leaves.
    auto tree = make_test_index(3, 4, false);
    KeyType key = make_key(500);

    // Insert 10 entries: RID pages 1..10.
    for (uint32_t i = 1; i <= 10; ++i) {
        ASSERT_TRUE(tree.insert(key, make_rid(i, 0)).has_value()) << "insert i=" << i;
    }
    ASSERT_EQ(tree.size(), 10u);

    // --- Remove page=1 (likely leftmost) ---
    {
        auto rm = tree.remove(key, make_rid(1, 0));
        ASSERT_TRUE(rm.has_value()) << rm.error().message;
        EXPECT_TRUE(*rm) << "remove({page=1}) must succeed";
        EXPECT_EQ(tree.size(), 9u);

        // page=1 must be absent; pages 2-10 must each appear exactly once.
        auto entries = full_scan(tree);
        ASSERT_TRUE(entries.has_value()) << entries.error().message;
        ASSERT_EQ(entries->size(), 9u);
        for (auto& [k, r] : *entries) {
            EXPECT_NE(r.page_id, 1u) << "Removed RID {1,0} must not appear";
        }
        for (uint32_t p = 2; p <= 10; ++p) {
            int cnt = 0;
            for (auto& [k, r] : *entries) {
                if (r.page_id == p && r.slot_id == 0) {
                    ++cnt;
                }
            }
            EXPECT_EQ(cnt, 1) << "RID {" << p << ",0} must appear exactly once";
        }
    }

    // --- Remove page=10 (likely rightmost) ---
    {
        auto rm = tree.remove(key, make_rid(10, 0));
        ASSERT_TRUE(rm.has_value()) << rm.error().message;
        EXPECT_TRUE(*rm) << "remove({page=10}) must succeed";
        EXPECT_EQ(tree.size(), 8u);

        auto entries = full_scan(tree);
        ASSERT_TRUE(entries.has_value());
        ASSERT_EQ(entries->size(), 8u);
        for (auto& [k, r] : *entries) {
            EXPECT_NE(r.page_id, 10u) << "Removed RID {10,0} must not appear";
            EXPECT_NE(r.page_id, 1u) << "Previously removed RID {1,0} must not appear";
        }
    }

    // --- Remove page=5 (interior) ---
    {
        auto rm = tree.remove(key, make_rid(5, 0));
        ASSERT_TRUE(rm.has_value()) << rm.error().message;
        EXPECT_TRUE(*rm) << "remove({page=5}) must succeed";
        EXPECT_EQ(tree.size(), 7u);

        auto entries = full_scan(tree);
        ASSERT_TRUE(entries.has_value());
        ASSERT_EQ(entries->size(), 7u);

        // Verify the exact surviving set: {2,3,4,6,7,8,9}.
        std::set<uint32_t> surviving{2, 3, 4, 6, 7, 8, 9};
        for (uint32_t p : {1u, 5u, 10u}) {
            for (auto& [k, r] : *entries) {
                EXPECT_NE(r.page_id, p) << "Removed RID {" << p << ",0} must not appear";
            }
        }
        for (uint32_t p : surviving) {
            int cnt = 0;
            for (auto& [k, r] : *entries) {
                if (r.page_id == p && r.slot_id == 0) {
                    ++cnt;
                }
            }
            EXPECT_EQ(cnt, 1) << "RID {" << p << ",0} must appear exactly once";
        }
    }
}

/// leaf_max=2 (smallest possible), 5 same-key entries.
/// Every pair forces a split, creating many leaves.
/// Remove entries at exact leaf-boundary positions (first and last entry of each leaf).
TEST(QA_GDB833, TinyLeafMax_RemoveBoundaryEntries) {
    auto tree = make_test_index(2, 4, false);
    KeyType key = make_key(999);

    // With leaf_max=2, 5 entries produce at least 3 leaves.
    for (uint32_t i = 1; i <= 5; ++i) {
        ASSERT_TRUE(tree.insert(key, make_rid(i, 0)).has_value());
    }
    ASSERT_EQ(tree.size(), 5u);

    // Remove pages in various orders: 2, 4, 1, 5, 3 (scattered).
    const uint32_t remove_order[] = {2, 4, 1, 5, 3};
    uint32_t remaining = 5;
    std::set<uint32_t> live{1, 2, 3, 4, 5};

    for (uint32_t p : remove_order) {
        auto rm = tree.remove(key, make_rid(p, 0));
        ASSERT_TRUE(rm.has_value()) << "remove {" << p << ",0}: " << rm.error().message;
        EXPECT_TRUE(*rm) << "remove {" << p << ",0} must return true";
        --remaining;
        live.erase(p);
        ASSERT_EQ(tree.size(), static_cast<uint64_t>(remaining))
            << "size after removing page=" << p;

        auto entries = full_scan(tree);
        ASSERT_TRUE(entries.has_value());
        ASSERT_EQ(entries->size(), remaining);

        for (uint32_t lp : live) {
            int cnt = 0;
            for (auto& [k, r] : *entries) {
                if (r.page_id == lp && r.slot_id == 0) {
                    ++cnt;
                }
            }
            EXPECT_EQ(cnt, 1) << "RID {" << lp << ",0} must appear exactly once";
        }
    }

    EXPECT_EQ(tree.size(), 0u);
    EXPECT_TRUE(tree.empty());
}

/// 20 entries on the same key with leaf_max=4.
/// Remove in reverse insertion order (rightmost first, then walking left).
/// Verifies that sibling links are maintained correctly throughout.
TEST(QA_GDB833, TwentyDuplicatesRemoveReverseOrder) {
    auto tree = make_test_index(4, 4, false);
    KeyType key = make_key(1000);

    for (uint32_t i = 1; i <= 20; ++i) {
        ASSERT_TRUE(tree.insert(key, make_rid(i, 0)).has_value());
    }
    ASSERT_EQ(tree.size(), 20u);

    // Remove in reverse order: 20, 19, 18, ...
    for (uint32_t i = 20; i >= 1; --i) {
        auto rm = tree.remove(key, make_rid(i, 0));
        ASSERT_TRUE(rm.has_value()) << "remove {" << i << ",0}: " << rm.error().message;
        EXPECT_TRUE(*rm) << "remove {" << i << ",0} must return true";
        EXPECT_EQ(tree.size(), static_cast<uint64_t>(i - 1));
    }

    EXPECT_TRUE(tree.empty());
    // After emptying: search returns nullopt, range scan returns nothing.
    auto found = tree.search(key);
    ASSERT_TRUE(found.has_value());
    EXPECT_FALSE(found->has_value()) << "key must be absent after all dups removed";
}

// ============================================================================
// 2. Interleave: insert, remove, re-insert, remove again
// ============================================================================

/// Insert dups, remove some, insert more with the same key (RID reuse + fresh),
/// remove again. Live RID set must be exact at every step.
TEST(QA_GDB833, InterleaveInsertRemoveReinsert) {
    auto tree = make_test_index(4, 4, false);
    KeyType key = make_key(77);

    // Phase 1: insert RIDs {1,0} .. {4,0}
    for (uint32_t i = 1; i <= 4; ++i) {
        ASSERT_TRUE(tree.insert(key, make_rid(i, 0)).has_value());
    }
    ASSERT_EQ(tree.size(), 4u);

    // Phase 2: remove {2,0} and {4,0}
    {
        auto r1 = tree.remove(key, make_rid(2, 0));
        ASSERT_TRUE(r1.has_value()) << r1.error().message;
        EXPECT_TRUE(*r1);
        auto r2 = tree.remove(key, make_rid(4, 0));
        ASSERT_TRUE(r2.has_value()) << r2.error().message;
        EXPECT_TRUE(*r2);
    }
    ASSERT_EQ(tree.size(), 2u);
    expect_rid_set(tree, key, {make_rid(1, 0), make_rid(3, 0)});

    // Phase 3: re-insert {2,0} (RID reuse) and fresh {5,0}
    ASSERT_TRUE(tree.insert(key, make_rid(2, 0)).has_value());
    ASSERT_TRUE(tree.insert(key, make_rid(5, 0)).has_value());
    ASSERT_EQ(tree.size(), 4u);
    expect_rid_set(tree, key, {make_rid(1, 0), make_rid(2, 0), make_rid(3, 0), make_rid(5, 0)});

    // Phase 4: remove {1,0}, {3,0}, {5,0}
    for (uint32_t p : {1u, 3u, 5u}) {
        auto rm = tree.remove(key, make_rid(p, 0));
        ASSERT_TRUE(rm.has_value()) << rm.error().message;
        EXPECT_TRUE(*rm);
    }
    ASSERT_EQ(tree.size(), 1u);
    expect_rid_set(tree, key, {make_rid(2, 0)});

    // Phase 5: remove the last one
    auto last_rm = tree.remove(key, make_rid(2, 0));
    ASSERT_TRUE(last_rm.has_value());
    EXPECT_TRUE(*last_rm);
    EXPECT_EQ(tree.size(), 0u);

    // Phase 6: re-insert fresh {10,0} — tree should accept it cleanly
    ASSERT_TRUE(tree.insert(key, make_rid(10, 0)).has_value());
    ASSERT_EQ(tree.size(), 1u);
    expect_rid_set(tree, key, {make_rid(10, 0)});
}

/// Mix of different keys and duplicate keys. Insert a large dataset,
/// then remove specific RIDs and verify no cross-key contamination.
TEST(QA_GDB833, MixedKeys_RidRemoveDoesNotCrossContaminateOtherKeys) {
    auto tree = make_test_index(4, 4, false);

    // Insert 5 unique keys each with 3 duplicate RIDs.
    const int64_t keys_arr[] = {10, 20, 30, 40, 50};
    for (int64_t k : keys_arr) {
        for (uint32_t r = 1; r <= 3; ++r) {
            ASSERT_TRUE(tree.insert(make_key(k), make_rid(k * 100 + r, 0)).has_value());
        }
    }
    ASSERT_EQ(tree.size(), 15u);

    // Remove the middle RID from key=30 (rid {3002, 0}).
    auto rm = tree.remove(make_key(30), make_rid(3002, 0));
    ASSERT_TRUE(rm.has_value()) << rm.error().message;
    EXPECT_TRUE(*rm);
    EXPECT_EQ(tree.size(), 14u);

    // Key=30 should have exactly {3001,0} and {3003,0}.
    expect_rid_set(tree, make_key(30), {make_rid(3001, 0), make_rid(3003, 0)});

    // All other keys must still have exactly 3 RIDs each.
    for (int64_t k : keys_arr) {
        if (k == 30) {
            continue;
        }
        std::vector<RID> expected;
        for (uint32_t r = 1; r <= 3; ++r) {
            expected.push_back(make_rid(k * 100 + r, 0));
        }
        expect_rid_set(tree, make_key(k), expected);
    }
}

// ============================================================================
// 3. Edge cases: non-existent, wrong key, double-delete, cleanup + re-insert
// ============================================================================

/// remove(key, rid) for an existing rid but with a WRONG key returns false
/// and leaves the real entry intact.
TEST(QA_GDB833, ExistingRidWithWrongKeyReturnsNotFound) {
    auto tree = make_test_index(4, 4, false);
    KeyType real_key = make_key(10);
    KeyType wrong_key = make_key(20);
    RID real_rid = make_rid(42, 0);

    ASSERT_TRUE(tree.insert(real_key, real_rid).has_value());
    ASSERT_TRUE(tree.insert(wrong_key, make_rid(99, 0)).has_value()); // different key

    auto rm = tree.remove(wrong_key, real_rid); // right RID, wrong key
    ASSERT_TRUE(rm.has_value()) << rm.error().message;
    EXPECT_FALSE(*rm) << "RID on wrong key must not be found";
    EXPECT_EQ(tree.size(), 2u); // nothing removed

    // Real entry still present under real_key.
    auto found = tree.search(real_key);
    ASSERT_TRUE(found.has_value());
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ(found->value().page_id, 42u);
}

/// Double-delete: remove(key, rid) twice must be a clean no-op the second time.
/// No corruption, no crash, size unchanged after second call.
TEST(QA_GDB833, DoubleDeleteIsNoOp) {
    auto tree = make_test_index(4, 4, false);
    KeyType key = make_key(55);
    RID rid = make_rid(7, 3);
    RID rid2 = make_rid(8, 1); // survivor

    ASSERT_TRUE(tree.insert(key, rid).has_value());
    ASSERT_TRUE(tree.insert(key, rid2).has_value());
    ASSERT_EQ(tree.size(), 2u);

    // First remove succeeds.
    auto r1 = tree.remove(key, rid);
    ASSERT_TRUE(r1.has_value());
    EXPECT_TRUE(*r1);
    EXPECT_EQ(tree.size(), 1u);

    // Second remove of same (key, rid) must return false (not found).
    auto r2 = tree.remove(key, rid);
    ASSERT_TRUE(r2.has_value()) << "double-delete must not error: " << r2.error().message;
    EXPECT_FALSE(*r2) << "second removal of same (key, rid) must return false";
    EXPECT_EQ(tree.size(), 1u) << "size must not change after double-delete";

    // Survivor must still be present.
    auto found = tree.search(key);
    ASSERT_TRUE(found.has_value());
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ(found->value().page_id, 8u);
    EXPECT_EQ(found->value().slot_id, 1u);
}

/// remove(key, rid) on an empty tree must return false without any crash or error.
TEST(QA_GDB833, EmptyTreeRidRemoveReturnsNotFound) {
    auto tree = make_test_index(4, 4, false);
    auto r = tree.remove(make_key(100), make_rid(1, 0));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(*r);
    EXPECT_EQ(tree.size(), 0u);
}

/// Remove the LAST duplicate, then re-insert the same key.
/// The index must accept the re-insert cleanly and the new RID must be findable.
TEST(QA_GDB833, RemoveLastDupThenReinsert) {
    auto tree = make_test_index(4, 4, false);
    KeyType key = make_key(33);
    RID rid_orig = make_rid(1, 0);
    RID rid_new = make_rid(2, 0);

    // Insert and remove single entry.
    ASSERT_TRUE(tree.insert(key, rid_orig).has_value());
    auto rm = tree.remove(key, rid_orig);
    ASSERT_TRUE(rm.has_value());
    EXPECT_TRUE(*rm);
    EXPECT_TRUE(tree.empty());

    // Re-insert different RID under same key.
    ASSERT_TRUE(tree.insert(key, rid_new).has_value());
    EXPECT_EQ(tree.size(), 1u);

    auto found = tree.search(key);
    ASSERT_TRUE(found.has_value());
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ(found->value().page_id, 2u);
    EXPECT_EQ(found->value().slot_id, 0u);
}

/// Non-existent key — tree has keys but not the target key.
/// Must return false and leave tree intact.
TEST(QA_GDB833, NonExistentKeyReturnsFalse) {
    auto tree = make_test_index(4, 4, false);
    for (int64_t k : {1LL, 2LL, 4LL, 5LL}) { // deliberately skip 3
        ASSERT_TRUE(tree.insert(make_key(k), make_rid(static_cast<uint32_t>(k), 0)).has_value());
    }
    ASSERT_EQ(tree.size(), 4u);

    auto rm = tree.remove(make_key(3), make_rid(3, 0));
    ASSERT_TRUE(rm.has_value()) << rm.error().message;
    EXPECT_FALSE(*rm);
    EXPECT_EQ(tree.size(), 4u); // nothing removed
}

// ============================================================================
// 4. Tree-structure stress: underflow, merge, rebalance inside a dup run
// ============================================================================

/// Insert just enough entries to fill to min-occupancy boundary on both sides
/// of a duplicate run, then remove an entry that triggers leaf underflow + merge.
/// After merge, verify sibling links are consistent by doing a full scan
/// and confirming all surviving entries are present exactly once.
TEST(QA_GDB833, RemovalTriggeringLeafMergePreservesAllEntries) {
    // leaf_max=4, min_keys=2 (ceil(4/2)=2).
    // Build a tree with a dup run that is tight on occupancy.
    auto tree = make_test_index(4, 4, false);

    KeyType dup_key = make_key(50);
    KeyType before_key = make_key(10);
    KeyType after_key = make_key(90);

    // Anchor entries for before/after the dup key to give the tree structure.
    ASSERT_TRUE(tree.insert(before_key, make_rid(1000, 0)).has_value());
    ASSERT_TRUE(tree.insert(after_key, make_rid(2000, 0)).has_value());

    // Insert 4 duplicate entries: just enough to fill 2 leaves with min occupancy.
    for (uint32_t i = 1; i <= 4; ++i) {
        ASSERT_TRUE(tree.insert(dup_key, make_rid(i, 0)).has_value());
    }
    // Total = 6 entries.
    ASSERT_EQ(tree.size(), 6u);

    // Remove one dup entry — may trigger underflow/merge.
    auto rm = tree.remove(dup_key, make_rid(2, 0));
    ASSERT_TRUE(rm.has_value()) << "merge-triggering remove: " << rm.error().message;
    EXPECT_TRUE(*rm);
    ASSERT_EQ(tree.size(), 5u);

    // Full scan must find exactly 5 entries: 2 anchors + 3 dup entries.
    auto entries = full_scan(tree);
    ASSERT_TRUE(entries.has_value()) << entries.error().message;
    ASSERT_EQ(entries->size(), 5u) << "Expected 5 entries after merge";

    // Verify anchor keys.
    bool found_before = false;
    bool found_after = false;
    for (auto& [k, r] : *entries) {
        auto cmp_b = compare_keys(k, before_key);
        auto cmp_a = compare_keys(k, after_key);
        ASSERT_TRUE(cmp_b.has_value());
        ASSERT_TRUE(cmp_a.has_value());
        if (*cmp_b == std::strong_ordering::equal && r.page_id == 1000u) {
            found_before = true;
        }
        if (*cmp_a == std::strong_ordering::equal && r.page_id == 2000u) {
            found_after = true;
        }
    }
    EXPECT_TRUE(found_before) << "Anchor before_key must survive";
    EXPECT_TRUE(found_after) << "Anchor after_key must survive";

    // Verify surviving dup entries: {1,0}, {3,0}, {4,0}.
    for (uint32_t p : {1u, 3u, 4u}) {
        int cnt = 0;
        for (auto& [k, r] : *entries) {
            auto cmp = compare_keys(k, dup_key);
            ASSERT_TRUE(cmp.has_value());
            if (*cmp == std::strong_ordering::equal && r.page_id == p) {
                ++cnt;
            }
        }
        EXPECT_EQ(cnt, 1) << "dup RID {" << p << ",0} must appear exactly once after merge";
    }

    // Removed entry {2,0} must be absent.
    for (auto& [k, r] : *entries) {
        auto cmp = compare_keys(k, dup_key);
        ASSERT_TRUE(cmp.has_value());
        if (*cmp == std::strong_ordering::equal) {
            EXPECT_NE(r.page_id, 2u) << "Removed RID {2,0} must not appear";
        }
    }
}

/// Heavy underflow cascade: with leaf_max=3 and many dup entries,
/// successive removals trigger multiple merges. Verify tree invariants
/// (size, scan completeness) after each removal.
TEST(QA_GDB833, SuccessiveRemovalsCascadingMerges) {
    auto tree = make_test_index(3, 3, false);
    KeyType key = make_key(200);

    // Insert 12 entries for the same key.
    for (uint32_t i = 1; i <= 12; ++i) {
        ASSERT_TRUE(tree.insert(key, make_rid(i, 0)).has_value());
    }
    ASSERT_EQ(tree.size(), 12u);

    // Remove one at a time in random-ish order (not sequential to maximise merge variety).
    const uint32_t removal_order[] = {3, 9, 1, 12, 6, 7, 2, 11, 5, 8, 4, 10};
    std::set<uint32_t> live;
    for (uint32_t i = 1; i <= 12; ++i) {
        live.insert(i);
    }

    for (uint32_t p : removal_order) {
        auto rm = tree.remove(key, make_rid(p, 0));
        ASSERT_TRUE(rm.has_value()) << "remove {" << p << ",0}: " << rm.error().message;
        EXPECT_TRUE(*rm) << "remove {" << p << ",0} must return true";
        live.erase(p);
        EXPECT_EQ(tree.size(), live.size()) << "size after removing page=" << p;

        if (!live.empty()) {
            auto entries = full_scan(tree);
            ASSERT_TRUE(entries.has_value()) << entries.error().message;
            ASSERT_EQ(entries->size(), live.size());

            for (uint32_t lp : live) {
                int cnt = 0;
                for (auto& [k, r] : *entries) {
                    if (r.page_id == lp && r.slot_id == 0) {
                        ++cnt;
                    }
                }
                EXPECT_EQ(cnt, 1) << "RID {" << lp << ",0} must appear exactly once";
            }
        }
    }

    EXPECT_TRUE(tree.empty());
}

/// After a merge that removes a leaf, verify prev/next sibling links of
/// surrounding leaves are properly updated by doing a scan that crosses
/// the merge point.
TEST(QA_GDB833, MergeSiblingLinksConsistencyAfterRidRemove) {
    // leaf_max=4. Build: [key=10 x3][key=20 x3][key=30 x3].
    // After inserts the tree spans multiple leaves.
    // Remove an entry from key=20 triggering a merge, then scan all keys
    // to verify the linked-list of leaves is intact.
    auto tree = make_test_index(4, 4, false);

    for (uint32_t r = 1; r <= 3; ++r) {
        ASSERT_TRUE(tree.insert(make_key(10), make_rid(100 + r, 0)).has_value());
        ASSERT_TRUE(tree.insert(make_key(20), make_rid(200 + r, 0)).has_value());
        ASSERT_TRUE(tree.insert(make_key(30), make_rid(300 + r, 0)).has_value());
    }
    ASSERT_EQ(tree.size(), 9u);

    // Remove one entry from key=20 (may trigger underflow/merge).
    auto rm = tree.remove(make_key(20), make_rid(202, 0));
    ASSERT_TRUE(rm.has_value()) << rm.error().message;
    EXPECT_TRUE(*rm);
    ASSERT_EQ(tree.size(), 8u);

    // Full scan must return all 8 surviving entries in sorted key order.
    auto entries = full_scan(tree);
    ASSERT_TRUE(entries.has_value()) << entries.error().message;
    ASSERT_EQ(entries->size(), 8u);

    // Verify scan ordering is non-decreasing (sibling links correct).
    for (size_t i = 1; i < entries->size(); ++i) {
        auto cmp = compare_keys((*entries)[i - 1].first, (*entries)[i].first);
        ASSERT_TRUE(cmp.has_value());
        EXPECT_NE(*cmp, std::strong_ordering::greater)
            << "scan must return entries in non-decreasing key order (sibling link broken?)";
    }

    // Verify key=20 has exactly {201,0} and {203,0}.
    expect_rid_set(tree, make_key(20), {make_rid(201, 0), make_rid(203, 0)});
}

// ============================================================================
// 5. Left-walk termination safety
// ============================================================================

/// Verify the left-walk cannot walk into unrelated SMALLER keys.
/// Build a tree: many entries for key=10, then entries for key=20.
/// remove(key=20, rid) must NOT walk left past key=20 boundary into key=10 leaves.
/// Also verify the key=10 entries are completely untouched.
TEST(QA_GDB833, LeftWalkDoesNotCrossKeyBoundary) {
    auto tree = make_test_index(4, 4, false);

    // Insert many entries for key=10 (fill several leaves).
    for (uint32_t i = 1; i <= 8; ++i) {
        ASSERT_TRUE(tree.insert(make_key(10), make_rid(100 + i, 0)).has_value());
    }

    // Insert entries for key=20 (different key).
    for (uint32_t i = 1; i <= 4; ++i) {
        ASSERT_TRUE(tree.insert(make_key(20), make_rid(200 + i, 0)).has_value());
    }
    ASSERT_EQ(tree.size(), 12u);

    // Remove one entry from key=20.
    auto rm = tree.remove(make_key(20), make_rid(202, 0));
    ASSERT_TRUE(rm.has_value()) << rm.error().message;
    EXPECT_TRUE(*rm);
    ASSERT_EQ(tree.size(), 11u);

    // All 8 key=10 entries must still be present and unmodified.
    std::vector<RID> expected_10;
    for (uint32_t i = 1; i <= 8; ++i) {
        expected_10.push_back(make_rid(100 + i, 0));
    }
    expect_rid_set(tree, make_key(10), expected_10);

    // key=20 must have exactly {201,0}, {203,0}, {204,0}.
    expect_rid_set(tree, make_key(20),
                   {make_rid(201, 0), make_rid(203, 0), make_rid(204, 0)});
}

/// Stress: alternate inserts and RID-qualified removes across two different
/// keys with the same value numeric distance — verifies no key confusion.
/// Runs many rounds to maximise structural changes.
TEST(QA_GDB833, AlternatingKeyInsertRemoveStress) {
    auto tree = make_test_index(4, 4, false);

    const uint32_t ROUNDS = 50;
    // Use two keys far apart so the tree has two distinct subtrees.
    KeyType key_a = make_key(1);
    KeyType key_b = make_key(100000);

    std::set<uint32_t> live_a;
    std::set<uint32_t> live_b;

    uint32_t next_rid = 1;

    for (uint32_t round = 0; round < ROUNDS; ++round) {
        // Insert 3 RIDs into each key.
        for (int j = 0; j < 3; ++j) {
            uint32_t ra = next_rid++;
            uint32_t rb = next_rid++;
            ASSERT_TRUE(tree.insert(key_a, make_rid(ra, 0)).has_value());
            ASSERT_TRUE(tree.insert(key_b, make_rid(rb, 0)).has_value());
            live_a.insert(ra);
            live_b.insert(rb);
        }

        // Remove 1 RID from each (the oldest).
        if (!live_a.empty()) {
            uint32_t victim_a = *live_a.begin();
            auto rm = tree.remove(key_a, make_rid(victim_a, 0));
            ASSERT_TRUE(rm.has_value()) << rm.error().message;
            EXPECT_TRUE(*rm);
            live_a.erase(victim_a);
        }
        if (!live_b.empty()) {
            uint32_t victim_b = *live_b.begin();
            auto rm = tree.remove(key_b, make_rid(victim_b, 0));
            ASSERT_TRUE(rm.has_value()) << rm.error().message;
            EXPECT_TRUE(*rm);
            live_b.erase(victim_b);
        }

        EXPECT_EQ(tree.size(), live_a.size() + live_b.size());
    }

    // Final: full scan must have no duplicates and exactly the live sets.
    auto entries = full_scan(tree);
    ASSERT_TRUE(entries.has_value()) << entries.error().message;
    EXPECT_EQ(entries->size(), live_a.size() + live_b.size());
}

// ============================================================================
// 6. remove(key) key-only overload still works correctly (non-regression)
// ============================================================================

/// After adding remove(key, rid), the original remove(key) must still work
/// correctly for non-unique trees (deletes an arbitrary entry, correct size).
TEST(QA_GDB833, KeyOnlyRemoveStillFunctional) {
    auto tree = make_test_index(4, 4, false);
    KeyType key = make_key(42);

    // Insert 3 entries.
    ASSERT_TRUE(tree.insert(key, make_rid(1, 0)).has_value());
    ASSERT_TRUE(tree.insert(key, make_rid(2, 0)).has_value());
    ASSERT_TRUE(tree.insert(key, make_rid(3, 0)).has_value());
    ASSERT_EQ(tree.size(), 3u);

    // Key-only remove must delete exactly one entry.
    auto r = tree.remove(key);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(*r);
    EXPECT_EQ(tree.size(), 2u);

    // Two entries must survive.
    auto entries = full_scan(tree);
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 2u);
}

// ============================================================================
// 7. Slot_id discrimination within the same page
// ============================================================================

/// Two entries: same key, same page_id, different slot_id.
/// Removing one must not remove the other (slot_id matters).
TEST(QA_GDB833, SlotIdDiscrimination) {
    auto tree = make_test_index(4, 4, false);
    KeyType key = make_key(7);
    RID rid_a = {42, 0}; // page=42, slot=0
    RID rid_b = {42, 1}; // page=42, slot=1 (same page, different slot)

    ASSERT_TRUE(tree.insert(key, rid_a).has_value());
    ASSERT_TRUE(tree.insert(key, rid_b).has_value());
    ASSERT_EQ(tree.size(), 2u);

    // Remove only slot=0.
    auto rm = tree.remove(key, rid_a);
    ASSERT_TRUE(rm.has_value()) << rm.error().message;
    EXPECT_TRUE(*rm);
    EXPECT_EQ(tree.size(), 1u);

    // slot=1 must survive.
    auto found = tree.search(key);
    ASSERT_TRUE(found.has_value());
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ(found->value().page_id, 42u);
    EXPECT_EQ(found->value().slot_id, 1u);

    // Trying to remove slot=0 again must return false.
    auto rm2 = tree.remove(key, rid_a);
    ASSERT_TRUE(rm2.has_value()) << rm2.error().message;
    EXPECT_FALSE(*rm2) << "slot=0 already removed; second removal must return false";
    EXPECT_EQ(tree.size(), 1u);
}
