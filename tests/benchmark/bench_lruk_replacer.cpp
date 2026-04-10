#include "sixseven/storage/buffer_pool.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <vector>

using namespace sixseven;

namespace {

/// Reference linear-scan implementation of LRU-K eviction.
///
/// This is the algorithm that lived in `LRUKReplacer::evict()` before GDB-620
/// replaced it with an `std::set`-based indexed priority queue. We keep a copy
/// in this benchmark file so we can compare the old O(n) approach to the new
/// O(log n) approach at large pool sizes (32K frames after GDB-613).
class LinearLRUKReplacer {
public:
    LinearLRUKReplacer(uint32_t num_frames, uint32_t k) : k_(k), frames_(num_frames) {}

    void record_access(FrameId frame_id) {
        auto& info = frames_[frame_id];
        info.history.push_back(++current_timestamp_);
        if (info.history.size() > k_) {
            info.history.erase(info.history.begin());
        }
    }

    void set_evictable(FrameId frame_id, bool evictable) {
        auto& info = frames_[frame_id];
        if (info.evictable == evictable) {
            return;
        }
        info.evictable = evictable;
        if (evictable) {
            ++evictable_count_;
        } else {
            --evictable_count_;
        }
    }

    std::optional<FrameId> evict() {
        if (evictable_count_ == 0) {
            return std::nullopt;
        }
        FrameId victim = 0;
        bool found = false;
        bool victim_is_inf = false;
        // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
        uint64_t victim_oldest_access = std::numeric_limits<uint64_t>::max();
        uint64_t victim_k_distance = 0;

        for (FrameId i = 0; i < static_cast<FrameId>(frames_.size()); ++i) {
            const auto& info = frames_[i];
            if (!info.evictable || info.history.empty()) {
                continue;
            }
            const bool is_inf = info.history.size() < k_;
            if (is_inf) {
                if (!found || !victim_is_inf || info.history.front() < victim_oldest_access) {
                    victim = i;
                    found = true;
                    victim_is_inf = true;
                    victim_oldest_access = info.history.front();
                }
            } else if (!victim_is_inf) {
                const uint64_t k_distance = current_timestamp_ - info.history.front();
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
        frames_[victim].history.clear();
        frames_[victim].evictable = false;
        --evictable_count_;
        return victim;
    }

private:
    struct FrameInfo {
        std::vector<uint64_t> history;
        bool evictable = false;
    };

    uint32_t k_;
    uint64_t current_timestamp_ = 0;
    uint32_t evictable_count_ = 0;
    std::vector<FrameInfo> frames_;
};

/// Build a populated linear replacer where every frame has K=2 accesses and
/// is marked evictable. Mirrors the steady-state of a fully warm buffer pool.
LinearLRUKReplacer make_linear(uint32_t num_frames) {
    LinearLRUKReplacer r(num_frames, 2);
    for (uint32_t i = 0; i < num_frames; ++i) {
        r.record_access(i);
    }
    for (uint32_t i = 0; i < num_frames; ++i) {
        r.record_access(i);
    }
    for (uint32_t i = 0; i < num_frames; ++i) {
        r.set_evictable(i, true);
    }
    return r;
}

LRUKReplacer make_pq(uint32_t num_frames) {
    LRUKReplacer r(num_frames, 2);
    for (uint32_t i = 0; i < num_frames; ++i) {
        r.record_access(i);
    }
    for (uint32_t i = 0; i < num_frames; ++i) {
        r.record_access(i);
    }
    for (uint32_t i = 0; i < num_frames; ++i) {
        r.set_evictable(i, true);
    }
    return r;
}

} // namespace

// -- Pure eviction throughput at the default 32K pool size --------------------
//
// Each iteration evicts one frame from a pre-populated replacer. The setup is
// excluded from timing via PauseTiming/ResumeTiming so we measure the cost of
// `evict()` alone, not the cost of rebuilding the replacer.

static void BM_LRUKLinearEvict(benchmark::State& state) {
    const auto num_frames = static_cast<uint32_t>(state.range(0));
    auto r = make_linear(num_frames);
    for (auto _ : state) {
        if (r.evict() == std::nullopt) {
            state.PauseTiming();
            r = make_linear(num_frames);
            state.ResumeTiming();
        }
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["frames"] =
        benchmark::Counter(static_cast<double>(num_frames), benchmark::Counter::kDefaults);
}

static void BM_LRUKPriorityQueueEvict(benchmark::State& state) {
    const auto num_frames = static_cast<uint32_t>(state.range(0));
    auto r = make_pq(num_frames);
    for (auto _ : state) {
        if (!r.evict().has_value()) {
            state.PauseTiming();
            r = make_pq(num_frames);
            state.ResumeTiming();
        }
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["frames"] =
        benchmark::Counter(static_cast<double>(num_frames), benchmark::Counter::kDefaults);
}

BENCHMARK(BM_LRUKLinearEvict)->Arg(1024)->Arg(8192)->Arg(32768)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_LRUKPriorityQueueEvict)
    ->Arg(1024)
    ->Arg(8192)
    ->Arg(32768)
    ->Unit(benchmark::kMicrosecond);

// -- Mixed access + eviction churn --------------------------------------------
//
// Closer to the real BPM workload: accesses on random frames interleaved with
// eviction + re-insertion. With 32K frames the linear scan dominates; the PQ
// version should be roughly constant in the access:evict ratio.

template <typename Replacer>
void run_mixed_workload(benchmark::State& state, Replacer make_replacer) {
    const auto num_frames = static_cast<uint32_t>(state.range(0));
    auto r = make_replacer(num_frames);
    std::mt19937 rng(0xBEEF);
    std::uniform_int_distribution<uint32_t> frame_dist(0, num_frames - 1);

    for (auto _ : state) {
        // 4 random accesses, then one eviction + re-insertion to keep the
        // replacer at steady state.
        for (int i = 0; i < 4; ++i) {
            r.record_access(frame_dist(rng));
        }
        auto victim = r.evict();
        if (!victim.has_value()) {
            state.PauseTiming();
            r = make_replacer(num_frames);
            state.ResumeTiming();
            continue;
        }
        r.record_access(*victim);
        r.record_access(*victim);
        r.set_evictable(*victim, true);
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["frames"] =
        benchmark::Counter(static_cast<double>(num_frames), benchmark::Counter::kDefaults);
}

static void BM_LRUKLinearMixed(benchmark::State& state) {
    run_mixed_workload(state, make_linear);
}

static void BM_LRUKPriorityQueueMixed(benchmark::State& state) {
    run_mixed_workload(state, make_pq);
}

BENCHMARK(BM_LRUKLinearMixed)->Arg(1024)->Arg(8192)->Arg(32768)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_LRUKPriorityQueueMixed)
    ->Arg(1024)
    ->Arg(8192)
    ->Arg(32768)
    ->Unit(benchmark::kMicrosecond);
