/// QA adversarial tests for GDB-620: LRU-K eviction via indexed priority queue
///
/// The implementer replaced the O(n) linear scan in `LRUKReplacer::evict()`
/// with an `std::set`-based indexed priority queue keyed on backward
/// K-distance. The risk surface is heap-consistency: every state-change
/// operation (`record_access`, `set_evictable`, `remove`, `evict`) must keep
/// the set of heap entries in lockstep with the conceptual evictable set, or
/// the priority queue will hand out the wrong victim — silently.
///
/// These tests aim to break the heap invariant from every angle:
///   1. Differential tests against a linear-scan reference (skip predicate,
///      K=1 / K=3, large random workloads).
///   2. Boundary cases: 1-frame pool, set_evictable without record_access,
///      remove() at head/middle/non-evictable, idempotent toggle.
///   3. Skip predicate edge cases: all-skipped, none-skipped, alternating.
///   4. Frame reuse after eviction (eviction must fully reset frame state).
///   5. End-to-end through `BufferPoolManager` to verify the priority queue
///      drives the same eviction order as the buffer pool used to.
///   6. Large pool stress (32K frames) to confirm there is no hidden quadratic
///      behavior the differential tests would never reach.

#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <random>
#include <vector>

using namespace sixseven;

namespace {

// =============================================================================
// Linear-scan reference implementation (oracle for differential tests).
// =============================================================================
//
// This mirrors the algorithm that lived in `LRUKReplacer::evict()` before this
// ticket. The differential tests below treat the reference as authoritative
// and assert that the priority-queue implementation makes identical choices on
// the same trace.

struct RefFrameInfo {
    std::vector<uint64_t> history;
    bool evictable = false;
};

struct RefReplacer {
    uint32_t k;
    uint64_t current_timestamp = 0;
    std::vector<RefFrameInfo> frames;

    RefReplacer(uint32_t num_frames, uint32_t k_in) : k(k_in), frames(num_frames) {}

    void record_access(FrameId frame_id) {
        auto& info = frames[frame_id];
        info.history.push_back(++current_timestamp);
        if (info.history.size() > k) {
            info.history.erase(info.history.begin());
        }
    }

    void set_evictable(FrameId frame_id, bool evictable) {
        frames[frame_id].evictable = evictable;
    }

    void remove(FrameId frame_id) {
        frames[frame_id].history.clear();
        frames[frame_id].evictable = false;
    }

    /// Linear-scan eviction with optional skip predicate (two-pass:
    /// non-skipped frames preferred, skipped frames as fallback).
    std::optional<FrameId> evict_with_skip(const std::function<bool(FrameId)>& skip) {
        auto find_best = [&](bool want_skipped) -> std::optional<FrameId> {
            FrameId victim = 0;
            bool found = false;
            bool victim_is_inf = false;
            uint64_t victim_oldest_access = std::numeric_limits<uint64_t>::max();
            uint64_t victim_k_distance = 0;

            for (FrameId i = 0; i < static_cast<FrameId>(frames.size()); ++i) {
                const auto& info = frames[i];
                if (!info.evictable || info.history.empty()) {
                    continue;
                }
                if (skip) {
                    const bool is_skipped = skip(i);
                    if (is_skipped != want_skipped) {
                        continue;
                    }
                }
                const bool is_inf = info.history.size() < k;
                if (is_inf) {
                    if (!found || !victim_is_inf || info.history.front() < victim_oldest_access) {
                        victim = i;
                        found = true;
                        victim_is_inf = true;
                        victim_oldest_access = info.history.front();
                    }
                } else if (!victim_is_inf) {
                    const uint64_t k_distance = current_timestamp - info.history.front();
                    if (!found || k_distance > victim_k_distance) {
                        victim = i;
                        found = true;
                        victim_k_distance = k_distance;
                    }
                }
            }
            if (!found) {
                return std::nullopt;
            }
            return victim;
        };

        auto v = find_best(/*want_skipped=*/false);
        if (!v && skip) {
            v = find_best(/*want_skipped=*/true);
        }
        if (!v) {
            return std::nullopt;
        }
        frames[*v].history.clear();
        frames[*v].evictable = false;
        return v;
    }

    std::optional<FrameId> evict() {
        return evict_with_skip(nullptr);
    }
};

} // namespace

// =============================================================================
// 1. Boundary & invariant tests
// =============================================================================

/// Single-frame pool: the only frame is recorded, made evictable, evicted,
/// then re-recorded. Heap state must reset cleanly between cycles.
TEST(QA_GDB620_LRUK, SingleFrameLifecycleRepeats) {
    LRUKReplacer pq(/*num_frames=*/1, /*k=*/2);
    for (int cycle = 0; cycle < 5; ++cycle) {
        pq.record_access(0);
        pq.set_evictable(0, true);
        EXPECT_EQ(pq.size(), 1u);
        auto victim = pq.evict();
        ASSERT_TRUE(victim.has_value()) << "cycle " << cycle;
        EXPECT_EQ(*victim, 0u);
        EXPECT_EQ(pq.size(), 0u);
        // Second evict on empty replacer must fail with NOT_FOUND.
        auto miss = pq.evict();
        ASSERT_FALSE(miss.has_value());
        EXPECT_EQ(miss.error().code, StatusCode::NOT_FOUND);
    }
}

/// `set_evictable(true)` on a frame that has never been accessed must NOT
/// add anything to the heap. Then `evict()` must report NOT_FOUND because the
/// frame has no access history to compute a priority key from. (This matches
/// the legacy linear-scan behaviour, which also skipped history-empty frames.)
TEST(QA_GDB620_LRUK, EvictableWithoutAccessNotEvicted) {
    LRUKReplacer pq(4, 2);
    pq.set_evictable(0, true);
    pq.set_evictable(1, true);
    auto result = pq.evict();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);

    // Now record an access on frame 1: it should become evictable.
    pq.record_access(1);
    auto v = pq.evict();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 1u);
}

/// `set_evictable(true)` is idempotent: calling it twice must not corrupt the
/// heap or double-count `size()`. Likewise for `set_evictable(false)`.
TEST(QA_GDB620_LRUK, SetEvictableIdempotent) {
    LRUKReplacer pq(4, 2);
    for (FrameId i = 0; i < 4; ++i) {
        pq.record_access(i);
    }
    pq.set_evictable(0, true);
    pq.set_evictable(0, true); // no-op
    pq.set_evictable(0, true); // no-op
    EXPECT_EQ(pq.size(), 1u);

    pq.set_evictable(0, false);
    pq.set_evictable(0, false); // no-op
    EXPECT_EQ(pq.size(), 0u);

    pq.set_evictable(1, true);
    pq.set_evictable(2, true);
    EXPECT_EQ(pq.size(), 2u);

    auto victim = pq.evict();
    ASSERT_TRUE(victim.has_value());
    EXPECT_EQ(*victim, 1u); // frame 1 was accessed before frame 2.
}

/// `remove()` on the head of the heap must rebalance correctly: the next
/// `evict()` must pick what was the second-priority frame, not the removed one.
TEST(QA_GDB620_LRUK, RemoveHeadOfHeap) {
    LRUKReplacer pq(4, 2);
    for (FrameId i = 0; i < 4; ++i) {
        pq.record_access(i);
        pq.set_evictable(i, true);
    }
    // Frame 0 is the head (oldest single access → infinite distance).
    pq.remove(0);
    EXPECT_EQ(pq.size(), 3u);
    auto v = pq.evict();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 1u);
}

/// `remove()` from the middle of the heap must not disturb the priority of
/// other frames.
TEST(QA_GDB620_LRUK, RemoveMiddleOfHeap) {
    LRUKReplacer pq(4, 2);
    for (FrameId i = 0; i < 4; ++i) {
        pq.record_access(i);
        pq.set_evictable(i, true);
    }
    pq.remove(2);
    EXPECT_EQ(pq.size(), 3u);
    EXPECT_EQ(*pq.evict(), 0u);
    EXPECT_EQ(*pq.evict(), 1u);
    EXPECT_EQ(*pq.evict(), 3u);
    EXPECT_EQ(pq.size(), 0u);
}

/// `remove()` on a non-evictable frame must not change `size()` and must not
/// crash from a stray heap erase.
TEST(QA_GDB620_LRUK, RemoveNonEvictableFrameNoSizeChange) {
    LRUKReplacer pq(4, 2);
    pq.record_access(0);
    pq.record_access(1);
    pq.set_evictable(0, true); // only frame 0 is evictable
    EXPECT_EQ(pq.size(), 1u);

    pq.remove(1); // non-evictable, but has history
    EXPECT_EQ(pq.size(), 1u);

    pq.remove(0); // evictable
    EXPECT_EQ(pq.size(), 0u);
    auto miss = pq.evict();
    ASSERT_FALSE(miss.has_value());
}

/// After eviction, the same frame_id can be re-recorded and re-evicted —
/// `evict()` must clear all per-frame state.
TEST(QA_GDB620_LRUK, FrameReuseAfterEviction) {
    LRUKReplacer pq(2, 2);
    pq.record_access(0);
    pq.record_access(0); // 2 accesses → finite K-distance
    pq.set_evictable(0, true);
    auto v1 = pq.evict();
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, 0u);
    EXPECT_EQ(pq.size(), 0u);

    // Re-record only once → infinite K-distance (size=1 < K=2).
    pq.record_access(0);
    pq.set_evictable(0, true);
    EXPECT_EQ(pq.size(), 1u);
    auto v2 = pq.evict();
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, 0u);
}

// =============================================================================
// 2. Skip predicate edge cases
// =============================================================================

/// `evict(skip)` where `skip` returns false for everything must behave
/// identically to `evict()`.
TEST(QA_GDB620_LRUK, EvictSkipNoneEqualsPlainEvict) {
    LRUKReplacer pq(4, 2);
    for (FrameId i = 0; i < 4; ++i) {
        pq.record_access(i);
        pq.set_evictable(i, true);
    }
    auto v = pq.evict([](FrameId) { return false; });
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 0u);
    EXPECT_EQ(*pq.evict([](FrameId) { return false; }), 1u);
    EXPECT_EQ(*pq.evict([](FrameId) { return false; }), 2u);
    EXPECT_EQ(*pq.evict([](FrameId) { return false; }), 3u);
}

/// `evict(skip)` where every evictable frame is skipped must still return a
/// victim (the highest-priority skipped frame), not NOT_FOUND.
TEST(QA_GDB620_LRUK, EvictAllSkippedFallsBackToHead) {
    LRUKReplacer pq(4, 2);
    for (FrameId i = 0; i < 4; ++i) {
        pq.record_access(i);
        pq.set_evictable(i, true);
    }
    auto v = pq.evict([](FrameId) { return true; });
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 0u); // head: oldest first access
    EXPECT_EQ(pq.size(), 3u);
    EXPECT_EQ(*pq.evict([](FrameId) { return true; }), 1u);
}

/// Alternating skip predicate: skip even frame_ids. Pass 1 must walk the heap
/// in priority order and pick the first odd-id frame. The skipped frames must
/// remain in the heap untouched and be evictable in subsequent calls.
TEST(QA_GDB620_LRUK, EvictSkipAlternating) {
    LRUKReplacer pq(8, 2);
    for (FrameId i = 0; i < 8; ++i) {
        pq.record_access(i);
        pq.set_evictable(i, true);
    }
    auto skip_even = [](FrameId fid) { return (fid % 2) == 0; };

    // Heap order is by oldest first access: 0, 1, 2, 3, 4, 5, 6, 7.
    // Skipping even frames, the first non-skipped is frame 1.
    auto v1 = pq.evict(skip_even);
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, 1u);

    auto v2 = pq.evict(skip_even);
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, 3u);

    EXPECT_EQ(pq.size(), 6u);
    // Even frames remain in the heap and are still evictable via plain evict().
    EXPECT_EQ(*pq.evict(), 0u);
    EXPECT_EQ(*pq.evict(), 2u);
    EXPECT_EQ(*pq.evict(), 4u);
}

// =============================================================================
// 3. Differential tests against the linear-scan reference
// =============================================================================

/// Apply a long pseudo-random trace including a `skip` predicate that
/// fluctuates per-frame, comparing the priority queue against the reference
/// implementation. This is the broadest invariant check: any heap-consistency
/// bug or skip-pass divergence will surface as a victim mismatch.
TEST(QA_GDB620_LRUK, RandomTraceWithSkipMatchesReference) {
    constexpr uint32_t num_frames = 64;
    constexpr uint32_t k = 2;
    constexpr int num_ops = 5000;

    std::mt19937 rng(0xDEADBEEF);
    std::uniform_int_distribution<int> action_dist(0, 9);
    std::uniform_int_distribution<FrameId> frame_dist(0, num_frames - 1);

    LRUKReplacer pq(num_frames, k);
    RefReplacer ref(num_frames, k);

    // Per-frame "dirty" flag — used by the skip predicate. Toggled randomly
    // alongside access/evictable operations to exercise interleaved state.
    std::vector<bool> dirty(num_frames, false);
    std::vector<bool> is_evictable(num_frames, false);

    auto skip = [&](FrameId fid) { return dirty[fid]; };

    int evict_count = 0;
    int success_count = 0;
    for (int i = 0; i < num_ops; ++i) {
        const int action = action_dist(rng);
        if (action < 4) {
            // 40% record_access
            const FrameId fid = frame_dist(rng);
            pq.record_access(fid);
            ref.record_access(fid);
        } else if (action < 7) {
            // 30% toggle evictable
            const FrameId fid = frame_dist(rng);
            const bool flag = !is_evictable[fid];
            is_evictable[fid] = flag;
            pq.set_evictable(fid, flag);
            ref.set_evictable(fid, flag);
        } else if (action < 8) {
            // 10% toggle dirty (only flips skip predicate output)
            const FrameId fid = frame_dist(rng);
            dirty[fid] = !dirty[fid];
        } else {
            // 20% evict with skip
            ++evict_count;
            auto pq_result = pq.evict(skip);
            auto ref_result = ref.evict_with_skip(skip);
            ASSERT_EQ(pq_result.has_value(), ref_result.has_value())
                << "op " << i << " disagreement on success/failure";
            if (pq_result.has_value()) {
                ASSERT_EQ(*pq_result, *ref_result) << "op " << i << " picked different victim";
                is_evictable[*pq_result] = false;
                dirty[*pq_result] = false;
                ++success_count;
            }
        }
    }
    // Sanity: the trace must actually exercise the evict path with a non-trivial
    // number of successes; otherwise the assertion above would never fire.
    ASSERT_GT(evict_count, 100);
    ASSERT_GT(success_count, 50);
}

/// K=3 differential test. Most existing tests target K=2 (the default); K=3
/// stresses the boundary between "infinite K-distance" (history.size() < K)
/// and "finite K-distance" with a different threshold.
TEST(QA_GDB620_LRUK, RandomTraceK3MatchesReference) {
    constexpr uint32_t num_frames = 32;
    constexpr uint32_t k = 3;
    constexpr int num_ops = 3000;

    std::mt19937 rng(0xCAFEF00D);
    std::uniform_int_distribution<int> action_dist(0, 9);
    std::uniform_int_distribution<FrameId> frame_dist(0, num_frames - 1);

    LRUKReplacer pq(num_frames, k);
    RefReplacer ref(num_frames, k);

    std::vector<bool> is_evictable(num_frames, false);
    for (int i = 0; i < num_ops; ++i) {
        const int action = action_dist(rng);
        if (action < 5) {
            const FrameId fid = frame_dist(rng);
            pq.record_access(fid);
            ref.record_access(fid);
        } else if (action < 8) {
            const FrameId fid = frame_dist(rng);
            const bool flag = !is_evictable[fid];
            is_evictable[fid] = flag;
            pq.set_evictable(fid, flag);
            ref.set_evictable(fid, flag);
        } else {
            auto pq_result = pq.evict();
            auto ref_result = ref.evict();
            ASSERT_EQ(pq_result.has_value(), ref_result.has_value()) << "op " << i;
            if (pq_result.has_value()) {
                ASSERT_EQ(*pq_result, *ref_result) << "op " << i;
                is_evictable[*pq_result] = false;
            }
        }
    }
}

/// K=1 (plain LRU) differential test with `remove()` calls mixed in. The
/// implementer's tests cover K=1 access-only; this exercises the
/// remove-from-heap path with K=1 priority keys.
TEST(QA_GDB620_LRUK, RandomTraceK1WithRemoveMatchesReference) {
    constexpr uint32_t num_frames = 32;
    constexpr uint32_t k = 1;
    constexpr int num_ops = 3000;

    std::mt19937 rng(0xBADC0FFEU);
    std::uniform_int_distribution<int> action_dist(0, 9);
    std::uniform_int_distribution<FrameId> frame_dist(0, num_frames - 1);

    LRUKReplacer pq(num_frames, k);
    RefReplacer ref(num_frames, k);

    std::vector<bool> is_evictable(num_frames, false);
    for (int i = 0; i < num_ops; ++i) {
        const int action = action_dist(rng);
        if (action < 5) {
            const FrameId fid = frame_dist(rng);
            pq.record_access(fid);
            ref.record_access(fid);
        } else if (action < 7) {
            const FrameId fid = frame_dist(rng);
            const bool flag = !is_evictable[fid];
            is_evictable[fid] = flag;
            pq.set_evictable(fid, flag);
            ref.set_evictable(fid, flag);
        } else if (action < 8) {
            // 10% remove
            const FrameId fid = frame_dist(rng);
            pq.remove(fid);
            ref.remove(fid);
            is_evictable[fid] = false;
        } else {
            auto pq_result = pq.evict();
            auto ref_result = ref.evict();
            ASSERT_EQ(pq_result.has_value(), ref_result.has_value()) << "op " << i;
            if (pq_result.has_value()) {
                ASSERT_EQ(*pq_result, *ref_result) << "op " << i;
                is_evictable[*pq_result] = false;
            }
        }
    }
}

// =============================================================================
// 4. Stress: large pool
// =============================================================================

/// Verify there is no hidden quadratic behaviour at default pool size by
/// running a long trace at the configured `default_pool_size` (32K frames)
/// and asserting reference parity. This also acts as a smoke test for the
/// O(log n) eviction path.
TEST(QA_GDB620_LRUK, LargePoolDifferentialAtDefaultSize) {
    constexpr uint32_t num_frames = default_pool_size;
    constexpr uint32_t k = 2;
    constexpr int num_ops = 20000;

    std::mt19937 rng(0xFEEDFACE);
    std::uniform_int_distribution<int> action_dist(0, 9);
    std::uniform_int_distribution<FrameId> frame_dist(0, num_frames - 1);

    LRUKReplacer pq(num_frames, k);
    RefReplacer ref(num_frames, k);

    std::vector<bool> is_evictable(num_frames, false);

    int eviction_successes = 0;
    for (int i = 0; i < num_ops; ++i) {
        const int action = action_dist(rng);
        if (action < 6) {
            const FrameId fid = frame_dist(rng);
            pq.record_access(fid);
            ref.record_access(fid);
        } else if (action < 9) {
            const FrameId fid = frame_dist(rng);
            const bool flag = !is_evictable[fid];
            is_evictable[fid] = flag;
            pq.set_evictable(fid, flag);
            ref.set_evictable(fid, flag);
        } else {
            auto pq_result = pq.evict();
            auto ref_result = ref.evict();
            ASSERT_EQ(pq_result.has_value(), ref_result.has_value()) << "op " << i;
            if (pq_result.has_value()) {
                ASSERT_EQ(*pq_result, *ref_result) << "op " << i;
                is_evictable[*pq_result] = false;
                ++eviction_successes;
            }
        }
    }
    ASSERT_GT(eviction_successes, 50);
}

// =============================================================================
// 5. End-to-end through BufferPoolManager
// =============================================================================

class QA_GDB620_BufferPool : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        temp_dir_ = std::filesystem::temp_directory_path() /
                    ("sixseven_qa_gdb620_" + std::string(info->name()));
        std::filesystem::create_directories(temp_dir_);

        auto create_result = dm_.create_file(temp_dir_ / "test.gdb");
        ASSERT_TRUE(create_result.has_value()) << create_result.error().message;
        file_id_ = *create_result;
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    DiskManager dm_;
    FileId file_id_ = 0;
    std::filesystem::path temp_dir_;
};

/// Drive a small buffer pool through more pages than it can hold. After the
/// pool is saturated, the next fetch must evict in LRU-K order. With K=2 and
/// fewer than K accesses per page, the eviction order is FIFO by first access.
TEST_F(QA_GDB620_BufferPool, FifoEvictionOrderForSinglyAccessedPages) {
    constexpr uint32_t pool_size = 4;
    BufferPoolManager bpm(dm_, file_id_, pool_size);

    // Allocate 8 pages (2x pool size), unpinning each immediately so it
    // becomes evictable. Mark dirty so freshly-initialized pages are flushed
    // to disk on eviction (otherwise the on-disk page never gets written).
    // After this loop the pool holds the most-recent 4 pages (4..7) —
    // pages 0..3 must have been evicted in order.
    std::vector<PageId> page_ids;
    for (int i = 0; i < 8; ++i) {
        auto p = bpm.new_page();
        ASSERT_TRUE(p.has_value());
        page_ids.push_back((*p)->page_id());
        ASSERT_TRUE(bpm.unpin_page(page_ids.back(), /*is_dirty=*/true).has_value());
    }

    const uint64_t initial_misses = bpm.miss_count();

    // Fetching the most-recent 4 pages should be cache hits.
    for (int i = 4; i < 8; ++i) {
        auto p = bpm.fetch_page(page_ids[i]);
        ASSERT_TRUE(p.has_value()) << "page " << i << ": " << p.error().message;
        ASSERT_TRUE(bpm.unpin_page(page_ids[i], false).has_value());
    }
    EXPECT_EQ(bpm.miss_count(), initial_misses); // all hits

    // Fetching the older 4 pages should be misses (they were evicted).
    for (int i = 0; i < 4; ++i) {
        auto p = bpm.fetch_page(page_ids[i]);
        ASSERT_TRUE(p.has_value()) << "page " << i << ": " << p.error().message;
        ASSERT_TRUE(bpm.unpin_page(page_ids[i], false).has_value());
    }
    EXPECT_GE(bpm.miss_count(), initial_misses + 4u);
}

/// LRU-K (K=2) must protect frequently-accessed pages from a sequential scan.
/// We fetch one page twice (giving it a finite K-distance) and then walk
/// many other pages once. The hot page must remain in the pool.
TEST_F(QA_GDB620_BufferPool, LRUKProtectsHotPageFromScan) {
    constexpr uint32_t pool_size = 4;
    BufferPoolManager bpm(dm_, file_id_, pool_size);

    // Hot page: two accesses → finite K-distance.
    auto hot = bpm.new_page();
    ASSERT_TRUE(hot.has_value());
    PageId hot_id = (*hot)->page_id();
    ASSERT_TRUE(bpm.unpin_page(hot_id, false).has_value());
    auto hot2 = bpm.fetch_page(hot_id);
    ASSERT_TRUE(hot2.has_value());
    ASSERT_TRUE(bpm.unpin_page(hot_id, false).has_value());

    // Sequential scan: fill the pool with single-access "cold" pages.
    std::vector<PageId> cold_ids;
    for (int i = 0; i < 16; ++i) {
        auto p = bpm.new_page();
        ASSERT_TRUE(p.has_value());
        cold_ids.push_back((*p)->page_id());
        ASSERT_TRUE(bpm.unpin_page(cold_ids.back(), false).has_value());
    }

    // The hot page must still be a cache hit; cold pages will have been
    // evicted in turn (each had infinite K-distance and lost to other infinite
    // pages by oldest-first-access).
    const uint64_t hits_before = bpm.hit_count();
    auto hot3 = bpm.fetch_page(hot_id);
    ASSERT_TRUE(hot3.has_value());
    EXPECT_EQ(bpm.hit_count(), hits_before + 1)
        << "hot page should have survived the scan thanks to LRU-K";
    ASSERT_TRUE(bpm.unpin_page(hot_id, false).has_value());
}

/// Stress: drive the buffer pool through thousands of fetch/unpin operations
/// across a working set larger than the pool. Asserts no errors and that
/// hits/misses sum to the operation count — i.e. no fetch silently dropped
/// because the heap handed out a stale or duplicated victim.
TEST_F(QA_GDB620_BufferPool, FetchUnpinStressAccountingMatches) {
    constexpr uint32_t pool_size = 16;
    constexpr int num_pages = 64;
    constexpr int num_ops = 4000;

    BufferPoolManager bpm(dm_, file_id_, pool_size);

    std::vector<PageId> page_ids;
    for (int i = 0; i < num_pages; ++i) {
        auto p = bpm.new_page();
        ASSERT_TRUE(p.has_value());
        page_ids.push_back((*p)->page_id());
        // Mark dirty so the freshly-allocated page is flushed to disk before
        // eviction; otherwise re-fetching it from disk fails on checksum.
        ASSERT_TRUE(bpm.unpin_page(page_ids.back(), /*is_dirty=*/true).has_value());
    }

    const uint64_t hits_before = bpm.hit_count();
    const uint64_t misses_before = bpm.miss_count();

    std::mt19937 rng(0x1234);
    std::uniform_int_distribution<int> idx_dist(0, num_pages - 1);
    for (int i = 0; i < num_ops; ++i) {
        const int idx = idx_dist(rng);
        auto p = bpm.fetch_page(page_ids[idx]);
        ASSERT_TRUE(p.has_value()) << "fetch " << i << " of page " << page_ids[idx];
        ASSERT_TRUE(bpm.unpin_page(page_ids[idx], (i % 3) == 0).has_value());
    }

    const uint64_t total_delta =
        (bpm.hit_count() - hits_before) + (bpm.miss_count() - misses_before);
    EXPECT_EQ(total_delta, static_cast<uint64_t>(num_ops));
}
