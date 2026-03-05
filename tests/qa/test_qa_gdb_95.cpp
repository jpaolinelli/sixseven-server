/// QA adversarial tests for GDB-95: B+ tree delete with merge/redistribute.
/// Tests redistribution, merging, root shrink, and tree integrity after deletes.

#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <random>
#include <set>
#include <vector>

#include "test_btree_helpers.h"

using namespace sixseven;
using namespace sixseven::test;

// =============================================================================
// Delete from Empty/Singleton
// =============================================================================

TEST(QA_GDB95_Empty, DeleteFromEmptyTree) {
    auto tree = make_test_index(4, 4);
    auto del = tree.remove(make_key(42));
    ASSERT_TRUE(del.has_value());
    EXPECT_FALSE(*del);
    EXPECT_TRUE(tree.empty());
}

TEST(QA_GDB95_Empty, DeleteOnlyKey) {
    auto tree = make_test_index(4, 4);
    (void)tree.insert(make_key(42), make_rid(1));
    EXPECT_EQ(tree.size(), 1u);

    auto del = tree.remove(make_key(42));
    ASSERT_TRUE(del.has_value());
    EXPECT_TRUE(*del);
    EXPECT_TRUE(tree.empty());
    EXPECT_EQ(tree.size(), 0u);

    // Tree should be usable after clearing
    auto s = tree.search(make_key(42));
    ASSERT_TRUE(s.has_value());
    EXPECT_FALSE(s->has_value());
}

TEST(QA_GDB95_Empty, DeleteAndReinsert) {
    auto tree = make_test_index(4, 4);
    (void)tree.insert(make_key(42), make_rid(1));
    (void)tree.remove(make_key(42));
    EXPECT_TRUE(tree.empty());

    // Reinsert into cleared tree
    auto ins = tree.insert(make_key(42), make_rid(2));
    ASSERT_TRUE(ins.has_value());
    EXPECT_EQ(tree.size(), 1u);

    auto s = tree.search(make_key(42));
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
    EXPECT_EQ(s->value().page_id, 2u);
}

// =============================================================================
// Delete Without Underflow (Root Leaf)
// =============================================================================

TEST(QA_GDB95_NoUnderflow, DeleteFromRootLeaf) {
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 4; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Delete one key - root leaf should not underflow
    (void)tree.remove(make_key(20));
    EXPECT_EQ(tree.size(), 3u);

    for (int k : {10, 30, 40}) {
        auto s = tree.search(make_key(k));
        ASSERT_TRUE(s.has_value());
        EXPECT_TRUE(s->has_value());
    }

    auto s = tree.search(make_key(20));
    ASSERT_TRUE(s.has_value());
    EXPECT_FALSE(s->has_value());
}

// =============================================================================
// Delete All Keys (Various Orders)
// =============================================================================

TEST(QA_GDB95_DeleteAll, AscendingOrder) {
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 20; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    for (int i = 1; i <= 20; ++i) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value()) << "delete failed at key " << i;
        EXPECT_TRUE(*del) << "key " << i << " not found";
    }
    EXPECT_TRUE(tree.empty());
}

TEST(QA_GDB95_DeleteAll, DescendingOrder) {
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 20; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    for (int i = 20; i >= 1; --i) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value()) << "delete failed at key " << i;
        EXPECT_TRUE(*del) << "key " << i << " not found";
    }
    EXPECT_TRUE(tree.empty());
}

TEST(QA_GDB95_DeleteAll, RandomOrder) {
    auto tree = make_test_index(4, 4);

    std::vector<int> keys(30);
    std::iota(keys.begin(), keys.end(), 1);

    for (int k : keys) {
        (void)tree.insert(make_key(k), make_rid(static_cast<uint32_t>(k)));
    }

    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);

    for (int k : keys) {
        auto del = tree.remove(make_key(k));
        ASSERT_TRUE(del.has_value()) << "delete failed at key " << k;
        EXPECT_TRUE(*del) << "key " << k << " not found";
    }
    EXPECT_TRUE(tree.empty());
}

TEST(QA_GDB95_DeleteAll, SmallCapacityRandomOrder) {
    auto tree = make_test_index(2, 2);

    std::vector<int> keys(30);
    std::iota(keys.begin(), keys.end(), 1);

    for (int k : keys) {
        (void)tree.insert(make_key(k), make_rid(static_cast<uint32_t>(k)));
    }

    std::mt19937 rng(99);
    std::shuffle(keys.begin(), keys.end(), rng);

    for (int k : keys) {
        auto del = tree.remove(make_key(k));
        ASSERT_TRUE(del.has_value()) << "delete failed at key " << k;
        EXPECT_TRUE(*del) << "key " << k << " not found";
    }
    EXPECT_TRUE(tree.empty());
}

// =============================================================================
// Delete with Verification of Remaining Keys
// =============================================================================

TEST(QA_GDB95_Verify, DeleteEveryOtherKey) {
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 20; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    // Delete even keys
    for (int i = 2; i <= 20; i += 2) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value());
        EXPECT_TRUE(*del);
    }
    EXPECT_EQ(tree.size(), 10u);

    // Odd keys should remain
    for (int i = 1; i <= 20; i += 2) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i << " should be present";
    }

    // Even keys should be gone
    for (int i = 2; i <= 20; i += 2) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        EXPECT_FALSE(s->has_value()) << "key " << i << " should be deleted";
    }

    // Range scan should be sorted and correct
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 10u);

    for (size_t i = 1; i < entries->size(); ++i) {
        EXPECT_LT(key_val((*entries)[i - 1].first), key_val((*entries)[i].first));
    }
}

TEST(QA_GDB95_Verify, DeleteFirstHalf) {
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 20; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Delete keys 10-100
    for (int i = 1; i <= 10; ++i) {
        (void)tree.remove(make_key(i * 10));
    }
    EXPECT_EQ(tree.size(), 10u);

    // Keys 110-200 should remain
    for (int i = 11; i <= 20; ++i) {
        auto s = tree.search(make_key(i * 10));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i * 10 << " missing";
    }
}

TEST(QA_GDB95_Verify, DeleteSecondHalf) {
    auto tree = make_test_index(4, 4);

    for (int i = 1; i <= 20; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Delete keys 110-200
    for (int i = 11; i <= 20; ++i) {
        (void)tree.remove(make_key(i * 10));
    }
    EXPECT_EQ(tree.size(), 10u);

    // Keys 10-100 should remain
    for (int i = 1; i <= 10; ++i) {
        auto s = tree.search(make_key(i * 10));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i * 10 << " missing";
    }
}

// =============================================================================
// Delete Non-Existent Keys
// =============================================================================

TEST(QA_GDB95_NotFound, DeleteMissingKey) {
    auto tree = make_test_index(4, 4);
    for (int i = 0; i < 10; i += 2) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    // Delete odd keys (not present)
    for (int i = 1; i < 10; i += 2) {
        auto del = tree.remove(make_key(i));
        ASSERT_TRUE(del.has_value());
        EXPECT_FALSE(*del);
    }
    EXPECT_EQ(tree.size(), 5u);
}

TEST(QA_GDB95_NotFound, DeleteSameKeyTwice) {
    auto tree = make_test_index(4, 4);
    (void)tree.insert(make_key(42), make_rid(1));

    auto del1 = tree.remove(make_key(42));
    ASSERT_TRUE(del1.has_value());
    EXPECT_TRUE(*del1);

    auto del2 = tree.remove(make_key(42));
    ASSERT_TRUE(del2.has_value());
    EXPECT_FALSE(*del2);
}

// =============================================================================
// Root Shrink
// =============================================================================

TEST(QA_GDB95_RootShrink, ShrinkToSingleLeaf) {
    auto tree = make_test_index(4, 4);

    // Insert enough for multi-level tree
    for (int i = 1; i <= 10; ++i) {
        (void)tree.insert(make_key(i * 10), make_rid(static_cast<uint32_t>(i)));
    }

    // Delete enough to force root shrink back to single leaf
    for (int i = 1; i <= 8; ++i) {
        auto del = tree.remove(make_key(i * 10));
        ASSERT_TRUE(del.has_value());
        EXPECT_TRUE(*del);
    }
    EXPECT_EQ(tree.size(), 2u);

    // Remaining keys should work
    auto s1 = tree.search(make_key(90));
    ASSERT_TRUE(s1.has_value());
    EXPECT_TRUE(s1->has_value());

    auto s2 = tree.search(make_key(100));
    ASSERT_TRUE(s2.has_value());
    EXPECT_TRUE(s2->has_value());
}

TEST(QA_GDB95_RootShrink, ShrinkAndGrow) {
    auto tree = make_test_index(3, 3);

    // Grow tree
    for (int i = 1; i <= 15; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    // Shrink tree
    for (int i = 1; i <= 13; ++i) {
        (void)tree.remove(make_key(i));
    }
    EXPECT_EQ(tree.size(), 2u);

    // Grow again
    for (int i = 1; i <= 15; ++i) {
        (void)tree.insert(make_key(i + 100), make_rid(static_cast<uint32_t>(i + 100)));
    }
    EXPECT_EQ(tree.size(), 17u);

    // All remaining should be searchable
    for (int i = 14; i <= 15; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value());
    }
    for (int i = 101; i <= 115; ++i) {
        auto s = tree.search(make_key(i));
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i << " not found";
    }
}

// =============================================================================
// Interleaved Insert/Delete
// =============================================================================

TEST(QA_GDB95_Interleave, InsertDeleteAlternating) {
    auto tree = make_test_index(4, 4);

    // Insert 5, delete 2, insert 5, delete 2, ...
    int next_key = 1;
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 5; ++i) {
            (void)tree.insert(make_key(next_key), make_rid(static_cast<uint32_t>(next_key)));
            ++next_key;
        }
        // Delete the first 2 keys inserted in this round
        int start = round * 5 + 1;
        (void)tree.remove(make_key(start));
        (void)tree.remove(make_key(start + 1));
    }

    // Verify size: 5 rounds * (5 inserts - 2 deletes) = 15
    EXPECT_EQ(tree.size(), 15u);

    // Range scan should be sorted
    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 15u);

    for (size_t i = 1; i < entries->size(); ++i) {
        EXPECT_LT(key_val((*entries)[i - 1].first), key_val((*entries)[i].first));
    }
}

// =============================================================================
// Stress: Large Scale Delete
// =============================================================================

TEST(QA_GDB95_Stress, InsertThenDeleteAll) {
    auto tree = make_test_index(5, 5);

    std::vector<int> keys(200);
    std::iota(keys.begin(), keys.end(), 1);

    // Insert all
    for (int k : keys) {
        (void)tree.insert(make_key(k), make_rid(static_cast<uint32_t>(k)));
    }
    EXPECT_EQ(tree.size(), 200u);

    // Delete all in random order
    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);

    for (size_t i = 0; i < keys.size(); ++i) {
        auto del = tree.remove(make_key(keys[i]));
        ASSERT_TRUE(del.has_value()) << "delete failed at key " << keys[i];
        EXPECT_TRUE(*del) << "key " << keys[i] << " not found";

        // Verify remaining keys periodically
        if (i % 50 == 49) {
            std::set<int> remaining(keys.begin() + i + 1, keys.end());
            for (int r : remaining) {
                auto s = tree.search(make_key(r));
                ASSERT_TRUE(s.has_value());
                ASSERT_TRUE(s->has_value()) << "key " << r << " missing after deleting " << i + 1;
            }
        }
    }
    EXPECT_TRUE(tree.empty());
}

TEST(QA_GDB95_Stress, SmallCapacityInsertDeleteRepeat) {
    auto tree = make_test_index(2, 2);

    std::mt19937 rng(123);

    // Repeated cycles of insert/delete
    for (int cycle = 0; cycle < 3; ++cycle) {
        std::vector<int> keys(50);
        std::iota(keys.begin(), keys.end(), cycle * 100);
        std::shuffle(keys.begin(), keys.end(), rng);

        for (int k : keys) {
            auto ins = tree.insert(make_key(k), make_rid(static_cast<uint32_t>(k)));
            ASSERT_TRUE(ins.has_value()) << "cycle " << cycle << " insert " << k;
        }

        // Delete half
        for (size_t i = 0; i < 25; ++i) {
            auto del = tree.remove(make_key(keys[i]));
            ASSERT_TRUE(del.has_value());
            EXPECT_TRUE(*del);
        }

        // Verify remaining
        for (size_t i = 25; i < 50; ++i) {
            auto s = tree.search(make_key(keys[i]));
            ASSERT_TRUE(s.has_value());
            ASSERT_TRUE(s->has_value()) << "cycle " << cycle << " key " << keys[i] << " missing";
        }
    }
}

TEST(QA_GDB95_Stress, DeleteAllOneByOneVerifyScanEachTime) {
    auto tree = make_test_index(3, 3);

    const int n = 20;
    for (int i = 1; i <= n; ++i) {
        (void)tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i)));
    }

    for (int del_key = 1; del_key <= n; ++del_key) {
        auto del = tree.remove(make_key(del_key));
        ASSERT_TRUE(del.has_value());
        EXPECT_TRUE(*del);

        // Full scan should be sorted and have correct count
        if (del_key < n) {
            auto scan = tree.range_scan(std::nullopt, std::nullopt);
            ASSERT_TRUE(scan.has_value());
            auto entries = collect_scan(*scan);
            ASSERT_TRUE(entries.has_value());
            EXPECT_EQ(entries->size(), static_cast<size_t>(n - del_key))
                << "wrong count after deleting key " << del_key;

            for (size_t i = 1; i < entries->size(); ++i) {
                EXPECT_LT(key_val((*entries)[i - 1].first), key_val((*entries)[i].first))
                    << "not sorted after deleting key " << del_key;
            }
        }
    }
    EXPECT_TRUE(tree.empty());
}
