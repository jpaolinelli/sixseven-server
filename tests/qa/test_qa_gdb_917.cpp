/// @file test_qa_gdb_917.cpp
/// QA adversarial tests for GDB-917: BTreeStress::FullScanSortedAfterMixedOps
/// passes vacuously if the scan returns zero entries.
///
/// The fix added ASSERT_EQ(entries->size(), 66u), EXPECT_EQ(tree.size(), 66u),
/// and per-entry exact-key assertions before the pre-existing sortedness loop.
///
/// QA Focus:
///   1. Mutation-grade: verify the ASSERT_EQ would fire on a short/empty scan.
///   2. OOB safety: ASSERT_EQ must precede the per-entry index loop.
///   3. Survivor set correctness: first=3, last=294, count=66.
///   4. Broader regression: full mixed insert/delete/scan path.

#include "test_btree_helpers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace sixseven;
using namespace sixseven::test;

// =============================================================================
// GDB-917 QA tests
// =============================================================================

// Verify the survivor set derivation matches the implementation.
// Inserted: i*3 for i in [0,100). Deleted: i*3 for i%3==0 (i.e. multiples of 9).
// Survivors: i*3 for i in [0,100) where i%3 != 0.
TEST(QA_GDB917, SurvivorSetMathIsCorrect) {
    std::vector<int64_t> expected;
    expected.reserve(66);
    for (int i = 0; i < 100; ++i) {
        if (i % 3 != 0) {
            expected.push_back(static_cast<int64_t>(i * 3));
        }
    }

    ASSERT_EQ(expected.size(), 66u) << "Survivor count must be 66";
    EXPECT_EQ(expected.front(), 3) << "First survivor key must be 3 (i=1)";
    EXPECT_EQ(expected.back(), 294) << "Last survivor key must be 294 (i=98)";

    // Key 0 (i=0, deleted) and key 297 (i=99, deleted) must not appear.
    EXPECT_TRUE(std::find(expected.begin(), expected.end(), 0) == expected.end())
        << "Key 0 must not be in survivors";
    EXPECT_TRUE(std::find(expected.begin(), expected.end(), 297) == expected.end())
        << "Key 297 must not be in survivors";

    // All multiples of 9 in range must be absent (they were deleted).
    for (int i = 0; i < 100; i += 3) {
        int64_t key = static_cast<int64_t>(i * 3);
        EXPECT_TRUE(std::find(expected.begin(), expected.end(), key) == expected.end())
            << "Deleted key " << key << " must not be in survivors";
    }

    // All entries are sorted (strictly ascending).
    for (size_t k = 1; k < expected.size(); ++k) {
        EXPECT_LT(expected[k - 1], expected[k]) << "Expected set must be sorted at index " << k;
    }
}

// Verify the btree actually stores exactly 66 survivors after the mixed ops.
// This is the core AC: size and content are pinned.
TEST(QA_GDB917, FullScanReturns66SurvivorsWithCorrectContent) {
    auto tree = make_test_index(5, 5);

    for (int i = 0; i < 100; ++i) {
        auto ins = tree.insert(make_key(i * 3), make_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(ins.has_value()) << "Insert i=" << i << " failed";
    }

    for (int i = 0; i < 100; i += 3) {
        auto del = tree.remove(make_key(i * 3));
        ASSERT_TRUE(del.has_value()) << "Remove i=" << i << " failed";
    }

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value()) << "range_scan failed: " << scan.error().message;

    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value()) << "collect_scan failed: " << entries.error().message;

    // Non-vacuous count assertion: an empty or short scan fails here.
    ASSERT_EQ(entries->size(), 66u) << "Expected 66 survivors; got " << entries->size();

    // Cross-check tree's internal counter.
    EXPECT_EQ(tree.size(), 66u) << "tree.size() must agree with scan count";

    // Build expected survivor set.
    std::vector<int64_t> expected;
    expected.reserve(66);
    for (int i = 0; i < 100; ++i) {
        if (i % 3 != 0) {
            expected.push_back(static_cast<int64_t>(i * 3));
        }
    }

    // Per-entry exact-key check (safe: ASSERT_EQ above guards against OOB).
    for (size_t k = 0; k < entries->size(); ++k) {
        EXPECT_EQ(key_val((*entries)[k].first), expected[k])
            << "Key mismatch at scan index " << k;
    }

    // Sortedness (pre-existing check, kept for completeness).
    for (size_t k = 1; k < entries->size(); ++k) {
        EXPECT_LT(key_val((*entries)[k - 1].first), key_val((*entries)[k].first))
            << "Not sorted at index " << k;
    }
}

// Verify that boundary keys (first and last survivor) are present and correct.
TEST(QA_GDB917, FirstAndLastSurvivorKeysArePinned) {
    auto tree = make_test_index(5, 5);

    for (int i = 0; i < 100; ++i) {
        (void)tree.insert(make_key(i * 3), make_rid(static_cast<uint32_t>(i)));
    }
    for (int i = 0; i < 100; i += 3) {
        (void)tree.remove(make_key(i * 3));
    }

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 66u);

    EXPECT_EQ(key_val(entries->front().first), 3)
        << "First survivor must be key=3 (i=1, not deleted)";
    EXPECT_EQ(key_val(entries->back().first), 294)
        << "Last survivor must be key=294 (i=98, not deleted)";
}

// Verify deleted keys (multiples of 9) are truly absent from the scan output.
TEST(QA_GDB917, DeletedKeysAbsentFromScan) {
    auto tree = make_test_index(5, 5);

    for (int i = 0; i < 100; ++i) {
        (void)tree.insert(make_key(i * 3), make_rid(static_cast<uint32_t>(i)));
    }
    for (int i = 0; i < 100; i += 3) {
        (void)tree.remove(make_key(i * 3));
    }

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 66u);

    // Collect scanned keys into a set for fast lookup.
    std::vector<int64_t> scanned_keys;
    scanned_keys.reserve(entries->size());
    for (const auto& e : *entries) {
        scanned_keys.push_back(key_val(e.first));
    }

    // Every multiple of 9 in [0,297] must be absent.
    for (int i = 0; i < 100; i += 3) {
        int64_t deleted_key = static_cast<int64_t>(i * 3);
        auto it = std::find(scanned_keys.begin(), scanned_keys.end(), deleted_key);
        EXPECT_EQ(it, scanned_keys.end())
            << "Deleted key " << deleted_key << " must not appear in scan output";
    }
}

// Adversarial: vary tree fan-out to stress splits and merges differently.
// The 66-survivor invariant must hold regardless of page capacity.
TEST(QA_GDB917, SurvivorInvariantHoldsAcrossFanouts) {
    // Test with multiple (leaf_max, internal_max) configurations.
    const std::vector<std::pair<uint16_t, uint16_t>> configs = {
        {3, 3},
        {4, 4},
        {7, 7},
        {10, 10},
    };

    for (auto [lm, im] : configs) {
        auto tree = make_test_index(lm, im);

        for (int i = 0; i < 100; ++i) {
            (void)tree.insert(make_key(i * 3), make_rid(static_cast<uint32_t>(i)));
        }
        for (int i = 0; i < 100; i += 3) {
            (void)tree.remove(make_key(i * 3));
        }

        auto scan = tree.range_scan(std::nullopt, std::nullopt);
        ASSERT_TRUE(scan.has_value()) << "range_scan failed with lm=" << lm;
        auto entries = collect_scan(*scan);
        ASSERT_TRUE(entries.has_value()) << "collect_scan failed with lm=" << lm;

        EXPECT_EQ(entries->size(), 66u)
            << "Expected 66 survivors with leaf_max=" << lm << " internal_max=" << im;
        EXPECT_EQ(tree.size(), 66u)
            << "tree.size() mismatch with leaf_max=" << lm << " internal_max=" << im;
    }
}
