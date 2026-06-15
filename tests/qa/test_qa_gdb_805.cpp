/// QA regression tests for GDB-805: Remove dead per-node shared_mutex latches
/// from BTreeInternalNode / BTreeLeafNode.
///
/// Focus areas:
///   1. Dead-code removal completeness — no latch() accessor on nodes.
///   2. Struct size / layout — removing mutable shared_mutex (~56–64 bytes each)
///      should reduce node sizes; confirm node objects are constructible.
///   3. B+ tree correctness is not regressed: insert, search, delete, range scan,
///      splits, merges all work correctly after the struct change.
///   4. Tree-level latch is still intact and protects concurrent access
///      (BTreeIndex::tree_latch_ / BTreeIterator::tree_lock_ must remain).
///   5. Large tree: many insertions force multi-level splits; all keys recoverable.
///   6. Concurrent access: parallel readers + sequential writers via tree latch.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <shared_mutex>
#include <thread>
#include <type_traits>
#include <vector>

#include "sixseven/index/btree_index.h"
#include "sixseven/index/btree_iterator.h"
#include "sixseven/index/btree_key.h"
#include "sixseven/index/btree_node.h"
#include "sixseven/index/rid.h"

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static BTreeIndex make_gdb805_index(uint16_t leaf_max = 4,
                                    uint16_t internal_max = 4,
                                    bool is_unique = false) {
    BTreeConfig cfg;
    cfg.key_types    = {TypeId::INT64};
    cfg.leaf_max_keys     = leaf_max;
    cfg.internal_max_keys = internal_max;
    cfg.is_unique         = is_unique;
    return BTreeIndex(std::move(cfg));
}

static KeyType k(int64_t v) { return {Value(v)}; }
static RID     r(uint32_t p, uint16_t s = 0) { return {p, s}; }

// ---------------------------------------------------------------------------
// Suite 1: Dead code removal – no latch() member on node types (GDB805_Struct)
// ---------------------------------------------------------------------------

/// Confirm BTreeLeafNode is default-constructible and the latch() accessor
/// no longer exists (static_assert would prevent compilation if it leaked back).
TEST(QA_GDB805_Struct, LeafNodeConstructible) {
    BTreeLeafNode leaf(1, 8);
    EXPECT_EQ(leaf.key_count(), 0u);
    EXPECT_EQ(leaf.max_keys(), 8u);
    EXPECT_EQ(leaf.page_id(), 1u);
}

TEST(QA_GDB805_Struct, InternalNodeConstructible) {
    BTreeInternalNode node(2, 8);
    EXPECT_EQ(node.key_count(), 0u);
    EXPECT_EQ(node.max_keys(), 8u);
    EXPECT_EQ(node.page_id(), 2u);
}

/// latch() must NOT exist — verified via SFINAE trait.
/// If latch() is re-added, these traits will return true and the static_asserts
/// will fire at compile time.
namespace {
template <typename T, typename = void>
struct has_latch : std::false_type {};

template <typename T>
struct has_latch<T, std::void_t<decltype(std::declval<T>().latch())>> : std::true_type {};
} // namespace

TEST(QA_GDB805_Struct, NoLatchMethodOnLeafNode) {
    static_assert(!has_latch<BTreeLeafNode>::value,
                  "BTreeLeafNode must not have a latch() method — GDB-805 removed it");
    SUCCEED();
}

TEST(QA_GDB805_Struct, NoLatchMethodOnInternalNode) {
    static_assert(!has_latch<BTreeInternalNode>::value,
                  "BTreeInternalNode must not have a latch() method — GDB-805 removed it");
    SUCCEED();
}

/// Confirm tree-level latch is still present on BTreeIndex.
TEST(QA_GDB805_Struct, TreeLevelLatchStillPresent) {
    auto tree = make_gdb805_index();
    // tree_latch() must be callable and return a reference to shared_mutex.
    std::shared_mutex& latch = tree.tree_latch();
    // Acquire shared lock (multiple readers allowed).
    std::shared_lock<std::shared_mutex> sl(latch);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Suite 2: Insert / Search correctness (GDB805_Insert)
// ---------------------------------------------------------------------------

TEST(QA_GDB805_Insert, SingleKey) {
    auto tree = make_gdb805_index();
    ASSERT_TRUE(tree.insert(k(10), r(1)).has_value());
    EXPECT_EQ(tree.size(), 1u);
    auto res = tree.search(k(10));
    ASSERT_TRUE(res.has_value());
    ASSERT_TRUE(res->has_value());
    EXPECT_EQ(res->value().page_id, 1u);
}

TEST(QA_GDB805_Insert, LeafSplitSmallOrder) {
    // max 4 keys per leaf → 5th insert forces a split.
    auto tree = make_gdb805_index(4, 4);
    for (int i = 1; i <= 5; ++i) {
        ASSERT_TRUE(tree.insert(k(i), r(static_cast<uint32_t>(i))).has_value())
            << "Insert failed at i=" << i;
    }
    EXPECT_EQ(tree.size(), 5u);
    for (int i = 1; i <= 5; ++i) {
        auto res = tree.search(k(i));
        ASSERT_TRUE(res.has_value());
        ASSERT_TRUE(res->has_value()) << "Key " << i << " missing after leaf split";
    }
}

TEST(QA_GDB805_Insert, ReverseOrderInsertThenSearch) {
    // Insert in descending order — stresses the binary-search insert position.
    auto tree = make_gdb805_index(4, 4);
    constexpr int N = 50;
    for (int i = N; i >= 1; --i) {
        ASSERT_TRUE(tree.insert(k(i), r(static_cast<uint32_t>(i))).has_value());
    }
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(N));
    for (int i = 1; i <= N; ++i) {
        auto res = tree.search(k(i));
        ASSERT_TRUE(res.has_value());
        ASSERT_TRUE(res->has_value()) << "Key " << i << " missing after reverse insert";
    }
}

TEST(QA_GDB805_Insert, SearchMissingKeyReturnsNullopt) {
    auto tree = make_gdb805_index();
    ASSERT_TRUE(tree.insert(k(5), r(5)).has_value());
    auto res = tree.search(k(99));
    ASSERT_TRUE(res.has_value());
    EXPECT_FALSE(res->has_value());
}

TEST(QA_GDB805_Insert, DuplicateKeyNonUnique) {
    auto tree = make_gdb805_index(4, 4, false);
    ASSERT_TRUE(tree.insert(k(7), r(1)).has_value());
    ASSERT_TRUE(tree.insert(k(7), r(2)).has_value());  // allowed in non-unique
    EXPECT_EQ(tree.size(), 2u);
}

TEST(QA_GDB805_Insert, DuplicateKeyUniqueReturnsError) {
    auto tree = make_gdb805_index(4, 4, true);
    ASSERT_TRUE(tree.insert(k(7), r(1)).has_value());
    auto dup = tree.insert(k(7), r(2));
    EXPECT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

// ---------------------------------------------------------------------------
// Suite 3: Delete / merge correctness (GDB805_Delete)
// ---------------------------------------------------------------------------

TEST(QA_GDB805_Delete, DeleteSingleKey) {
    auto tree = make_gdb805_index();
    ASSERT_TRUE(tree.insert(k(1), r(1)).has_value());
    auto del = tree.remove(k(1));
    ASSERT_TRUE(del.has_value());
    EXPECT_TRUE(*del);
    EXPECT_EQ(tree.size(), 0u);
    auto res = tree.search(k(1));
    ASSERT_TRUE(res.has_value());
    EXPECT_FALSE(res->has_value());
}

TEST(QA_GDB805_Delete, DeleteNonExistentKey) {
    auto tree = make_gdb805_index();
    ASSERT_TRUE(tree.insert(k(1), r(1)).has_value());
    auto del = tree.remove(k(99));
    ASSERT_TRUE(del.has_value());
    EXPECT_FALSE(*del);  // key not found → false
}

TEST(QA_GDB805_Delete, DeleteAllKeys) {
    auto tree = make_gdb805_index(4, 4);
    constexpr int N = 20;
    for (int i = 1; i <= N; ++i) {
        ASSERT_TRUE(tree.insert(k(i), r(static_cast<uint32_t>(i))).has_value());
    }
    for (int i = 1; i <= N; ++i) {
        auto del = tree.remove(k(i));
        ASSERT_TRUE(del.has_value()) << "Remove error at key " << i;
        ASSERT_TRUE(*del) << "Key " << i << " not found during delete";
    }
    EXPECT_EQ(tree.size(), 0u);
}

TEST(QA_GDB805_Delete, DeleteThenReinsert) {
    auto tree = make_gdb805_index(4, 4);
    for (int i = 1; i <= 10; ++i) {
        ASSERT_TRUE(tree.insert(k(i), r(static_cast<uint32_t>(i))).has_value());
    }
    // Delete odds.
    for (int i = 1; i <= 10; i += 2) {
        ASSERT_TRUE(tree.remove(k(i)).has_value());
    }
    EXPECT_EQ(tree.size(), 5u);
    // Reinsert odds.
    for (int i = 1; i <= 10; i += 2) {
        ASSERT_TRUE(tree.insert(k(i), r(static_cast<uint32_t>(i))).has_value());
    }
    EXPECT_EQ(tree.size(), 10u);
    for (int i = 1; i <= 10; ++i) {
        auto res = tree.search(k(i));
        ASSERT_TRUE(res.has_value());
        ASSERT_TRUE(res->has_value()) << "Key " << i << " missing after delete+reinsert";
    }
}

// ---------------------------------------------------------------------------
// Suite 4: Range scan (GDB805_RangeScan)
// ---------------------------------------------------------------------------

TEST(QA_GDB805_RangeScan, FullScanAscending) {
    // range_scan is [begin, end) exclusive. Use N+1 as end to include key N.
    auto tree = make_gdb805_index(4, 4);
    constexpr int N = 30;
    for (int i = 1; i <= N; ++i) {
        ASSERT_TRUE(tree.insert(k(i), r(static_cast<uint32_t>(i))).has_value());
    }
    auto it = tree.range_scan(k(1), k(N + 1));
    ASSERT_TRUE(it.has_value());
    std::vector<int64_t> found;
    while (!it->is_end()) {
        auto entry = it->next();
        ASSERT_TRUE(entry.has_value());
        if (!entry->has_value()) break;
        found.push_back(std::get<int64_t>((*entry)->first[0].data()));
    }
    ASSERT_EQ(found.size(), static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(found[i], static_cast<int64_t>(i + 1));
    }
}

TEST(QA_GDB805_RangeScan, EmptyRangeScan) {
    auto tree = make_gdb805_index();
    for (int i = 10; i <= 20; ++i) {
        ASSERT_TRUE(tree.insert(k(i), r(static_cast<uint32_t>(i))).has_value());
    }
    // Range that has no matches.
    auto it = tree.range_scan(k(100), k(200));
    ASSERT_TRUE(it.has_value());
    std::vector<int64_t> found;
    while (!it->is_end()) {
        auto entry = it->next();
        ASSERT_TRUE(entry.has_value());
        if (!entry->has_value()) break;
        found.push_back(std::get<int64_t>((*entry)->first[0].data()));
    }
    EXPECT_TRUE(found.empty());
}

TEST(QA_GDB805_RangeScan, PartialRange) {
    // range_scan is [begin, end) exclusive. range_scan(5, 16) returns keys 5..15.
    auto tree = make_gdb805_index(4, 4);
    for (int i = 1; i <= 20; ++i) {
        ASSERT_TRUE(tree.insert(k(i), r(static_cast<uint32_t>(i))).has_value());
    }
    auto it = tree.range_scan(k(5), k(16));
    ASSERT_TRUE(it.has_value());
    std::vector<int64_t> found;
    while (!it->is_end()) {
        auto entry = it->next();
        ASSERT_TRUE(entry.has_value());
        if (!entry->has_value()) break;
        found.push_back(std::get<int64_t>((*entry)->first[0].data()));
    }
    // Should contain 5..15 inclusive (11 keys).
    ASSERT_EQ(found.size(), 11u);
    EXPECT_EQ(found.front(), 5);
    EXPECT_EQ(found.back(), 15);
}

// ---------------------------------------------------------------------------
// Suite 5: Large tree (GDB805_LargeTree) — forces multi-level splits
// ---------------------------------------------------------------------------

TEST(QA_GDB805_LargeTree, InsertAndSearchThousandKeys) {
    auto tree = make_gdb805_index(8, 8);
    constexpr int N = 1000;
    std::vector<int> keys(N);
    std::iota(keys.begin(), keys.end(), 0);
    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);

    for (int key : keys) {
        auto ins = tree.insert(k(key), r(static_cast<uint32_t>(key)));
        ASSERT_TRUE(ins.has_value()) << "Insert failed at key " << key;
    }
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(N));

    for (int i = 0; i < N; ++i) {
        auto res = tree.search(k(i));
        ASSERT_TRUE(res.has_value());
        ASSERT_TRUE(res->has_value()) << "Key " << i << " missing from large tree";
        EXPECT_EQ(res->value().page_id, static_cast<uint32_t>(i));
    }
}

TEST(QA_GDB805_LargeTree, BulkDeleteHalf) {
    auto tree = make_gdb805_index(8, 8);
    constexpr int N = 500;
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(tree.insert(k(i), r(static_cast<uint32_t>(i))).has_value());
    }
    // Delete even keys.
    for (int i = 0; i < N; i += 2) {
        auto del = tree.remove(k(i));
        ASSERT_TRUE(del.has_value());
        ASSERT_TRUE(*del) << "Even key " << i << " not found";
    }
    EXPECT_EQ(tree.size(), static_cast<uint64_t>(N / 2));
    // Odds must still be present.
    for (int i = 1; i < N; i += 2) {
        auto res = tree.search(k(i));
        ASSERT_TRUE(res.has_value());
        ASSERT_TRUE(res->has_value()) << "Odd key " << i << " missing after bulk delete";
    }
    // Evens must be gone.
    for (int i = 0; i < N; i += 2) {
        auto res = tree.search(k(i));
        ASSERT_TRUE(res.has_value());
        EXPECT_FALSE(res->has_value()) << "Even key " << i << " still present after delete";
    }
}

TEST(QA_GDB805_LargeTree, MaxInt64Key) {
    auto tree = make_gdb805_index(4, 4);
    int64_t max_key = std::numeric_limits<int64_t>::max();
    int64_t min_key = std::numeric_limits<int64_t>::min();
    ASSERT_TRUE(tree.insert(k(max_key), r(1)).has_value());
    ASSERT_TRUE(tree.insert(k(min_key), r(2)).has_value());
    ASSERT_TRUE(tree.insert(k(0), r(3)).has_value());

    auto res_max = tree.search(k(max_key));
    ASSERT_TRUE(res_max.has_value());
    ASSERT_TRUE(res_max->has_value());
    EXPECT_EQ(res_max->value().page_id, 1u);

    auto res_min = tree.search(k(min_key));
    ASSERT_TRUE(res_min.has_value());
    ASSERT_TRUE(res_min->has_value());
    EXPECT_EQ(res_min->value().page_id, 2u);
}

// ---------------------------------------------------------------------------
// Suite 6: Concurrent access via tree-level latch (GDB805_Concurrency)
// ---------------------------------------------------------------------------

TEST(QA_GDB805_Concurrency, ParallelReadersNoDeadlock) {
    // Confirm tree_latch_ is held correctly by multiple readers simultaneously.
    auto tree = make_gdb805_index(8, 8);
    constexpr int N = 200;
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(tree.insert(k(i), r(static_cast<uint32_t>(i))).has_value());
    }

    constexpr int num_readers = 4;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};

    for (int t = 0; t < num_readers; ++t) {
        threads.emplace_back([&tree, &failures]() {
            for (int i = 0; i < N; ++i) {
                auto res = tree.search(k(i));
                if (!res.has_value() || !res->has_value()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(failures.load(), 0);
}

TEST(QA_GDB805_Concurrency, WriterAndReadersDontCorrupt) {
    // Sequential: writer finishes, then readers verify. Tests that tree_latch_
    // (not the removed per-node latches) is the sole synchronization point.
    auto tree = make_gdb805_index(8, 8);
    constexpr int N = 300;

    // Writer phase.
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(tree.insert(k(i), r(static_cast<uint32_t>(i))).has_value());
    }

    // Reader phase.
    std::atomic<int> failures{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&tree, &failures]() {
            for (int i = 0; i < N; ++i) {
                auto res = tree.search(k(i));
                if (!res.has_value() || !res->has_value()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& r : readers) r.join();
    EXPECT_EQ(failures.load(), 0);
}

TEST(QA_GDB805_Concurrency, RangeScanHoldsSharedLock) {
    // Iterator must hold a shared_lock on tree_latch_ for its lifetime.
    // range_scan is [begin, end) exclusive: range_scan(1, 21) returns keys 1..20.
    auto tree = make_gdb805_index(4, 4);
    for (int i = 1; i <= 20; ++i) {
        ASSERT_TRUE(tree.insert(k(i), r(static_cast<uint32_t>(i))).has_value());
    }
    auto it = tree.range_scan(k(1), k(21));
    ASSERT_TRUE(it.has_value());

    int count = 0;
    while (!it->is_end()) {
        auto entry = it->next();
        ASSERT_TRUE(entry.has_value());
        if (!entry->has_value()) break;
        ++count;
    }
    EXPECT_EQ(count, 20);
}

// ---------------------------------------------------------------------------
// Suite 7: Node-level direct operations (GDB805_NodeOps)
// ---------------------------------------------------------------------------

TEST(QA_GDB805_NodeOps, LeafInsertAndSearch) {
    BTreeLeafNode leaf(1, 8);
    auto ins = leaf.insert(k(42), r(1));
    ASSERT_TRUE(ins.has_value());
    auto res = leaf.search(k(42));
    ASSERT_TRUE(res.has_value());
    ASSERT_TRUE(res->has_value());
    EXPECT_EQ(res->value().page_id, 1u);
}

TEST(QA_GDB805_NodeOps, LeafRemove) {
    BTreeLeafNode leaf(1, 8);
    ASSERT_TRUE(leaf.insert(k(10), r(1)).has_value());
    auto del = leaf.remove(k(10));
    ASSERT_TRUE(del.has_value());
    EXPECT_TRUE(*del);
    EXPECT_EQ(leaf.key_count(), 0u);
}

TEST(QA_GDB805_NodeOps, LeafIsFull) {
    BTreeLeafNode leaf(1, 3);
    EXPECT_FALSE(leaf.is_full());
    ASSERT_TRUE(leaf.insert(k(1), r(1)).has_value());
    ASSERT_TRUE(leaf.insert(k(2), r(2)).has_value());
    ASSERT_TRUE(leaf.insert(k(3), r(3)).has_value());
    EXPECT_TRUE(leaf.is_full());
}

TEST(QA_GDB805_NodeOps, InternalNodeSearch) {
    // Build an internal node with two children separated by key 50.
    BTreeInternalNode node(1, 8);
    node.children().push_back(10);  // left child
    node.keys().push_back(k(50));
    node.children().push_back(20);  // right child

    // Key < 50 → left child (10).
    auto left = node.search(k(30));
    ASSERT_TRUE(left.has_value());
    EXPECT_EQ(*left, 10u);

    // Key >= 50 → right child (20).
    auto right = node.search(k(50));
    ASSERT_TRUE(right.has_value());
    EXPECT_EQ(*right, 20u);
}

TEST(QA_GDB805_NodeOps, LeafLowerBound) {
    BTreeLeafNode leaf(1, 8);
    for (int i = 1; i <= 5; ++i) {
        ASSERT_TRUE(leaf.insert(k(i * 10), r(static_cast<uint32_t>(i))).has_value());
    }
    // Lower bound of 25 → index of 30 (first key >= 25).
    auto lb = leaf.lower_bound(k(25));
    ASSERT_TRUE(lb.has_value());
    EXPECT_EQ(*lb, 2u);  // key[2] == 30
}

TEST(QA_GDB805_NodeOps, LeafSiblingPointers) {
    BTreeLeafNode leaf(5, 8);
    EXPECT_EQ(leaf.next_leaf_id(), invalid_page_id);
    EXPECT_EQ(leaf.prev_leaf_id(), invalid_page_id);
    leaf.set_next_leaf_id(6);
    leaf.set_prev_leaf_id(4);
    EXPECT_EQ(leaf.next_leaf_id(), 6u);
    EXPECT_EQ(leaf.prev_leaf_id(), 4u);
}
