#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"

#include <benchmark/benchmark.h>

#include <filesystem>
#include <string>

using namespace sixseven;

static const Schema users_schema({
    {"id", TypeId::INT32},
    {"username", TypeId::STRING},
    {"email", TypeId::STRING},
    {"age", TypeId::UINT8},
    {"active", TypeId::BOOL},
});

/// Pre-serialize a batch of tuples for the given row count.
static std::vector<std::vector<uint8_t>> make_tuples(int64_t count) {
    std::vector<std::vector<uint8_t>> tuples;
    tuples.reserve(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i) {
        std::vector<Value> values = {
            Value(static_cast<int32_t>(i)),
            Value(std::string{"user_" + std::to_string(i)}),
            Value(std::string{"user" + std::to_string(i) + "@example.com"}),
            Value(static_cast<uint8_t>(25)),
            Value(true),
        };
        auto buf = TupleSerializer::serialize(values, users_schema);
        tuples.push_back(std::move(*buf));
    }
    return tuples;
}

/// Benchmark INSERT throughput at a given pool size.
/// Range parameter: number of rows to insert per iteration.
/// Arg parameter (via Args): pool_size in frames.
static void BM_InsertThroughput(benchmark::State& state) {
    auto pool_size = static_cast<uint32_t>(state.range(0));
    const int64_t num_rows = state.range(1);

    auto tuples = make_tuples(num_rows);

    for (auto _ : state) {
        state.PauseTiming();
        auto path = std::filesystem::temp_directory_path() / "bench_insert.db";
        std::filesystem::remove(path);

        DiskManager dm;
        auto fid = dm.create_file(path, false, true);
        if (!fid.has_value()) {
            state.SkipWithError(fid.error().message.c_str());
            return;
        }
        FileId file_id = *fid;
        BufferPoolManager bpm(dm, file_id, pool_size);
        TableHeap heap(bpm, dm, file_id);
        state.ResumeTiming();

        for (int64_t i = 0; i < num_rows; ++i) {
            auto rid = heap.insert_tuple(tuples[static_cast<size_t>(i)]);
            if (!rid.has_value()) {
                state.SkipWithError(rid.error().message.c_str());
                return;
            }
        }

        state.PauseTiming();
        (void)bpm.flush_all();
        (void)dm.close_file(file_id);
        std::filesystem::remove(path);
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * num_rows);
    state.counters["rows"] = benchmark::Counter(
        static_cast<double>(num_rows), benchmark::Counter::kDefaults);
    state.counters["pool_frames"] = benchmark::Counter(
        static_cast<double>(pool_size), benchmark::Counter::kDefaults);
}

// Old default: 256 frames (2MB), New default: 32768 frames (256MB).
// Row counts: 1000, 5000, 10000.
BENCHMARK(BM_InsertThroughput)
    ->Args({256, 1000})
    ->Args({256, 5000})
    ->Args({256, 10000})
    ->Args({32768, 1000})
    ->Args({32768, 5000})
    ->Args({32768, 10000})
    ->Unit(benchmark::kMillisecond);
