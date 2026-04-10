#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"

#include <benchmark/benchmark.h>

#include <filesystem>
#include <random>
#include <thread>

using namespace sixseven;

/// Benchmark concurrent fetch_page + unpin_page throughput.
/// Range(0) = number of threads.
/// Pre-creates pages so the benchmark measures cache hit throughput under
/// contention, isolating the locking overhead.
static void BM_BufferPoolConcurrentFetchUnpin(benchmark::State& state) {
    const auto num_threads = static_cast<int>(state.range(0));
    constexpr uint32_t pool_size = 256;
    constexpr uint32_t num_pages = 128; // all fit in pool -> pure hit path

    // Setup: create file and pre-populate pages (once per iteration).
    for (auto _ : state) {
        state.PauseTiming();

        auto path = std::filesystem::temp_directory_path() / "bench_bpm_concurrent.db";
        std::filesystem::remove(path);

        DiskManager dm;
        auto fid = dm.create_file(path, false, true);
        if (!fid.has_value()) {
            state.SkipWithError(fid.error().message.c_str());
            return;
        }
        FileId file_id = *fid;
        BufferPoolManager bpm(dm, file_id, pool_size);

        // Pre-create pages so they're all cached.
        std::vector<PageId> page_ids;
        page_ids.reserve(num_pages);
        for (uint32_t i = 0; i < num_pages; ++i) {
            auto p = bpm.new_page();
            if (!p.has_value()) {
                state.SkipWithError(p.error().message.c_str());
                return;
            }
            page_ids.push_back((*p)->page_id());
            (void)bpm.unpin_page((*p)->page_id(), false);
        }

        constexpr int64_t ops_per_thread = 10000;
        std::atomic<int64_t> total_ops{0};

        state.ResumeTiming();

        // Launch worker threads.
        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(num_threads));
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&, t]() {
                std::mt19937 rng(static_cast<uint32_t>(t * 42 + 7));
                std::uniform_int_distribution<uint32_t> dist(0, num_pages - 1);
                int64_t ops = 0;
                for (int64_t i = 0; i < ops_per_thread; ++i) {
                    PageId pid = page_ids[dist(rng)];
                    auto page = bpm.fetch_page(pid);
                    if (page.has_value()) {
                        (void)bpm.unpin_page(pid, false);
                        ++ops;
                    }
                }
                total_ops.fetch_add(ops, std::memory_order_relaxed);
            });
        }
        for (auto& th : threads) {
            th.join();
        }

        state.PauseTiming();
        (void)bpm.flush_all();
        (void)dm.close_file(file_id);
        std::filesystem::remove(path);
        state.ResumeTiming();

        state.SetItemsProcessed(total_ops.load(std::memory_order_relaxed));
    }

    state.counters["threads"] = benchmark::Counter(
        static_cast<double>(num_threads), benchmark::Counter::kDefaults);
}

BENCHMARK(BM_BufferPoolConcurrentFetchUnpin)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3);

/// Benchmark concurrent mixed workload (fetch + new_page + flush) with eviction.
/// Uses a small pool to force eviction contention.
static void BM_BufferPoolConcurrentMixed(benchmark::State& state) {
    const auto num_threads = static_cast<int>(state.range(0));
    constexpr uint32_t pool_size = 32;
    constexpr uint32_t initial_pages = 64; // more pages than pool -> eviction

    for (auto _ : state) {
        state.PauseTiming();

        auto path = std::filesystem::temp_directory_path() / "bench_bpm_mixed.db";
        std::filesystem::remove(path);

        DiskManager dm;
        auto fid = dm.create_file(path, false, true);
        if (!fid.has_value()) {
            state.SkipWithError(fid.error().message.c_str());
            return;
        }
        FileId file_id = *fid;
        BufferPoolManager bpm(dm, file_id, pool_size);

        // Pre-create pages.
        std::vector<PageId> page_ids;
        page_ids.reserve(initial_pages);
        for (uint32_t i = 0; i < initial_pages; ++i) {
            auto p = bpm.new_page();
            if (!p.has_value()) {
                state.SkipWithError(p.error().message.c_str());
                return;
            }
            page_ids.push_back((*p)->page_id());
            (void)bpm.unpin_page((*p)->page_id(), false);
        }

        constexpr int64_t ops_per_thread = 5000;
        std::atomic<int64_t> total_ops{0};

        state.ResumeTiming();

        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(num_threads));
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&, t]() {
                std::mt19937 rng(static_cast<uint32_t>(t * 31 + 13));
                std::uniform_int_distribution<uint32_t> dist(0, initial_pages - 1);
                std::uniform_int_distribution<int> op_dist(0, 9);
                int64_t ops = 0;
                for (int64_t i = 0; i < ops_per_thread; ++i) {
                    int op = op_dist(rng);
                    if (op < 7) {
                        // 70%: fetch + unpin (may trigger eviction)
                        PageId pid = page_ids[dist(rng)];
                        auto page = bpm.fetch_page(pid);
                        if (page.has_value()) {
                            (void)bpm.unpin_page(pid, op < 3); // 30% dirty
                            ++ops;
                        }
                    } else if (op < 9) {
                        // 20%: flush a random page
                        PageId pid = page_ids[dist(rng)];
                        (void)bpm.flush_page(pid);
                        ++ops;
                    } else {
                        // 10%: new_page + unpin
                        auto page = bpm.new_page();
                        if (page.has_value()) {
                            (void)bpm.unpin_page((*page)->page_id(), false);
                            ++ops;
                        }
                    }
                }
                total_ops.fetch_add(ops, std::memory_order_relaxed);
            });
        }
        for (auto& th : threads) {
            th.join();
        }

        state.PauseTiming();
        (void)bpm.flush_all();
        (void)dm.close_file(file_id);
        std::filesystem::remove(path);
        state.ResumeTiming();

        state.SetItemsProcessed(total_ops.load(std::memory_order_relaxed));
    }

    state.counters["threads"] = benchmark::Counter(
        static_cast<double>(num_threads), benchmark::Counter::kDefaults);
}

BENCHMARK(BM_BufferPoolConcurrentMixed)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3);
