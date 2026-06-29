// End-to-end regression test for the torn-read bug that caused
// "Error: variable-length data extends beyond tuple at column 2" on
// NEAREST queries under heavy concurrent ingest at 10M+ rows.
//
// The real scenario: NEAREST brute-force scan iterates the heap while
// the embedding store callback rewrites individual rows in place
// (growing them by ~1.5 KB after the embedding is computed). Without
// page-level latching, the scanner saw half-written slot directories
// or variable-length offset tables and crashed on
// `offset + length > data.size()`.
//
// This test reproduces the race at the TableHeap level by driving the
// same update/scan pattern through public APIs.

#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <random>
#include <thread>
#include <vector>

using namespace sixseven;
using namespace std::chrono_literals;

namespace {

// Test "tuple" layout: 2-byte length prefix followed by that many bytes
// of a recognizable pattern. Readers validate that the length-prefix
// matches the byte count they received, which is impossible to satisfy
// if the bytes were torn mid-write.
constexpr size_t kPrefix = sizeof(uint16_t);

std::vector<uint8_t> make_payload(uint16_t len, uint8_t tag) {
    std::vector<uint8_t> out(kPrefix + len);
    std::memcpy(out.data(), &len, sizeof(uint16_t));
    std::fill(out.begin() + kPrefix, out.end(), tag);
    return out;
}

bool payload_is_consistent(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < kPrefix) return false;
    uint16_t len = 0;
    std::memcpy(&len, bytes.data(), sizeof(uint16_t));
    if (kPrefix + static_cast<size_t>(len) != bytes.size()) return false;
    if (bytes.size() == kPrefix) return true;
    uint8_t tag = bytes[kPrefix];
    for (size_t i = kPrefix; i < bytes.size(); ++i) {
        if (bytes[i] != tag) return false;
    }
    return true;
}

} // namespace

class ConcurrentHeapScanUpdate : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                "sixseven_test_concurrent_heap_scan_update.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;

        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
    }

    void TearDown() override {
        bpm_.reset();
        (void)dm_.close_file(file_id_);
        std::filesystem::remove(path_);
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
};

TEST_F(ConcurrentHeapScanUpdate, ScannerNeverObservesTornTuple) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Seed the heap with N tuples at a known size. The writer will then
    // grow/shrink each one at random, matching the embedding store's
    // "insert then rewrite bigger" pattern.
    constexpr int kRows = 400;
    constexpr uint16_t kInitialLen = 64;

    std::vector<RID> rids;
    rids.reserve(kRows);
    for (int i = 0; i < kRows; ++i) {
        auto payload = make_payload(kInitialLen, static_cast<uint8_t>(i & 0xFF));
        auto rid = heap.insert_tuple(payload);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
        rids.push_back(*rid);
    }

    std::atomic<bool> stop{false};
    std::atomic<size_t> reads{0};
    std::atomic<size_t> updates{0};
    std::atomic<size_t> successful_updates{0};
    std::atomic<size_t> torn_reads{0};

    // Writer: cycle through rids, rewriting each with a different length
    // and tag. This exercises both the in-place path (new <= old length)
    // and the reallocation path (new > old length).
    std::thread writer([&]() {
        std::mt19937 rng(0xC0FFEE);
        std::uniform_int_distribution<uint16_t> len_dist(16, 200);
        size_t idx = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            RID rid = rids[idx % rids.size()];
            uint16_t new_len = len_dist(rng);
            uint8_t tag = static_cast<uint8_t>((idx + 13) & 0xFF);
            auto payload = make_payload(new_len, tag);
            auto r = heap.update_tuple(rid, payload);
            // It is legitimate for update_tuple to fail with
            // INVALID_ARGUMENT when the page is full — just skip and keep
            // going. What is NOT legitimate is a torn read on the scanner
            // side, which the reader thread asserts on.
            updates.fetch_add(1, std::memory_order_relaxed);
            if (r.has_value()) {
                successful_updates.fetch_add(1, std::memory_order_relaxed);
            }
            ++idx;
        }
    });

    // Two scanner threads to make sure multiple concurrent readers also
    // cooperate with the writer.
    auto scanner_loop = [&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            auto it = heap.begin();
            if (!it.has_value()) continue;
            for (;;) {
                auto row_result = it->next();
                if (!row_result || !row_result->has_value()) {
                    break;
                }
                const auto& bytes = (*row_result)->second;
                if (!payload_is_consistent(bytes)) {
                    torn_reads.fetch_add(1, std::memory_order_relaxed);
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::thread s1(scanner_loop);
    std::thread s2(scanner_loop);

    std::this_thread::sleep_for(500ms);
    stop.store(true, std::memory_order_relaxed);

    writer.join();
    s1.join();
    s2.join();

    EXPECT_GT(reads.load(), 0u) << "scanner never read anything";
    EXPECT_GT(updates.load(), 0u) << "writer never updated anything";
    // Vacuity guard: at least one update must actually SUCCEED, not merely be
    // attempted. If update_tuple regressed to fail on every call, zero
    // concurrent rewrites would occur, torn_reads would be trivially 0, and the
    // torn-read check below would pass vacuously (GDB-967).
    EXPECT_GT(successful_updates.load(), 0u) << "writer never updated anything successfully";
    EXPECT_EQ(torn_reads.load(), 0u)
        << "observed torn reads — the page latch is not protecting scanners";
}
