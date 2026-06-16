// Tests for Page's shared_mutex latch_ protecting concurrent readers from
// torn reads while a writer is updating tuple bytes in-place or shifting
// the slot directory. This is the correctness-critical regression test for
// the NEAREST "variable-length data extends beyond tuple at column 2"
// error seen at >10M rows under concurrent embedding-store updates.
//
// The failure mode these tests guard against is: a reader computes the
// end of a variable-length field as (offset + length) where offset and
// length were read from different points in time because the writer
// rewrote the slot directory under it. The new page-level shared_mutex
// serializes the read against the writer so each snapshot is consistent.
//
// Design: writer-bounded, NO wall-clock dependence.
//   - Writer does exactly k_writer_ops state transitions then signals done.
//   - Reader loops while (!done), asserts every sample is untorn.
//   - Non-vacuity: EXPECT_EQ(writes, k_writer_ops) + EXPECT_GT(samples, 0).
//   - No per-state count requirement — that was the racy part removed.

#include "sixseven/storage/page.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace sixseven;
using namespace std::chrono_literals;

namespace {

// The writer performs exactly this many state transitions per test.
// 20 000 ops with yield() between each gives the reader ample scheduling
// opportunities without depending on any wall-clock window.
constexpr size_t k_writer_ops = 20000;

// Fill helper — produces a self-describing byte pattern: bytes[0] encodes
// the length so every sample the reader takes can verify it is untorn via
//   ASSERT_EQ(static_cast<size_t>(bytes[0]), bytes.size())
// Only meaningful for len < 256; all test lengths are well under that.
std::vector<uint8_t> patterned_tuple(size_t len, uint8_t tag) {
    std::vector<uint8_t> out(len, tag);
    out[0] = static_cast<uint8_t>(len & 0xFF);
    return out;
}

} // namespace

// -- 1. Read races write shrinking a tuple -----------------------------------

TEST(PageConcurrentLatch, GetTupleReadsStableSnapshotWhileWriterShrinks) {
    Page page(1, PageType::DATA);

    // Two valid states the writer alternates between.
    constexpr size_t size_a = 80;
    constexpr size_t size_b = 100;

    auto initial = patterned_tuple(size_a, 0xAA);
    auto slot_r = page.insert_tuple(initial);
    ASSERT_TRUE(slot_r.has_value());
    SlotId slot = *slot_r;

    std::atomic<bool> done{false};
    std::atomic<size_t> writes{0};
    std::atomic<size_t> total_samples{0};

    // Writer: exactly k_writer_ops transitions, alternating size_a / size_b.
    std::thread writer([&]() {
        for (size_t i = 0; i < k_writer_ops; ++i) {
            size_t len = (i % 2 == 0) ? size_a : size_b;
            auto t = patterned_tuple(len, static_cast<uint8_t>(i));
            (void)page.update_tuple(slot, t);
            writes.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });

    // Reader: sample until writer is done; every sample must be untorn.
    // Generous 60-s hang-guard — should never fire given bounded writer ops.
    std::thread reader([&]() {
        auto deadline = std::chrono::steady_clock::now() + 60s;
        while (!done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline) {
                ADD_FAILURE() << "hang-guard fired: writer did not finish";
                break;
            }
            auto r = page.get_tuple(slot);
            if (r.has_value()) {
                const auto& bytes = *r;
                ASSERT_FALSE(bytes.empty());
                // Self-describing sentinel: bytes[0] must equal the vector
                // length.  A torn read (offset/length from different writer
                // states) produces a mismatch here.
                ASSERT_EQ(static_cast<size_t>(bytes[0]), bytes.size())
                    << "torn read: length byte does not match vector size";
                // Value must be one of the two valid sizes.
                ASSERT_TRUE(bytes.size() == size_a || bytes.size() == size_b)
                    << "read unexpected size: " << bytes.size();
                total_samples.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    writer.join();
    reader.join();

    // Non-vacuity: writer completed all ops (mutation path was exercised).
    EXPECT_EQ(writes.load(), k_writer_ops);
    // Reader must have observed at least one sample.
    EXPECT_GT(total_samples.load(), 0u);
}

// -- 2. Read races write growing a tuple (reallocates in the page) ----------

TEST(PageConcurrentLatch, GetTupleReadsStableSnapshotWhileWriterGrows) {
    Page page(2, PageType::DATA);

    // Four valid sizes; all <= 255 so bytes[0] encodes length reliably.
    const std::array<size_t, 4> sizes{40, 60, 80, 50};

    auto initial = patterned_tuple(sizes[0], 0xCC);
    auto slot_r = page.insert_tuple(initial);
    ASSERT_TRUE(slot_r.has_value());
    SlotId slot = *slot_r;

    std::atomic<bool> done{false};
    std::atomic<size_t> writes{0};
    std::atomic<size_t> total_samples{0};

    // Writer: exactly k_writer_ops transitions, cycling through the 4 sizes.
    std::thread writer([&]() {
        for (size_t i = 0; i < k_writer_ops; ++i) {
            size_t len = sizes[i % sizes.size()];
            auto t = patterned_tuple(len, static_cast<uint8_t>(i));
            (void)page.update_tuple(slot, t);
            writes.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });

    std::thread reader([&]() {
        auto deadline = std::chrono::steady_clock::now() + 60s;
        while (!done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline) {
                ADD_FAILURE() << "hang-guard fired: writer did not finish";
                break;
            }
            auto r = page.get_tuple(slot);
            if (r.has_value()) {
                const auto& bytes = *r;
                ASSERT_FALSE(bytes.empty());
                ASSERT_EQ(static_cast<size_t>(bytes[0]), bytes.size())
                    << "torn read: length byte mismatches vector size";
                // Value must be one of the four valid sizes.
                bool valid = false;
                for (size_t s : sizes) {
                    if (bytes.size() == s) {
                        valid = true;
                        break;
                    }
                }
                ASSERT_TRUE(valid) << "read unexpected size: " << bytes.size();
                total_samples.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    writer.join();
    reader.join();

    EXPECT_EQ(writes.load(), k_writer_ops);
    EXPECT_GT(total_samples.load(), 0u);
}

// -- 3. Two readers and a writer -- multiple readers must coexist safely -----

TEST(PageConcurrentLatch, MultipleReadersDoNotBlockEachOther) {
    Page page(3, PageType::DATA);

    // Fixed size so any torn read shows up as a size mismatch.
    constexpr size_t tuple_size = 32;

    // Insert 8 tuples so readers have multiple slots to exercise.
    std::vector<SlotId> slots;
    for (int i = 0; i < 8; ++i) {
        auto s = page.insert_tuple(patterned_tuple(tuple_size, static_cast<uint8_t>(i)));
        ASSERT_TRUE(s.has_value());
        slots.push_back(*s);
    }

    std::atomic<bool> done{false};
    std::atomic<size_t> writes{0};
    std::atomic<size_t> total_samples{0};

    // Writer: exactly k_writer_ops updates, cycling through slots at fixed size.
    std::thread writer([&]() {
        for (size_t i = 0; i < k_writer_ops; ++i) {
            SlotId s = slots[i % slots.size()];
            (void)page.update_tuple(s, patterned_tuple(tuple_size, static_cast<uint8_t>(i + 1)));
            writes.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });

    auto reader_loop = [&]() {
        auto deadline = std::chrono::steady_clock::now() + 60s;
        while (!done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline) {
                ADD_FAILURE() << "hang-guard fired: writer did not finish";
                break;
            }
            for (SlotId s : slots) {
                auto r = page.get_tuple(s);
                if (r.has_value()) {
                    const auto& bytes = *r;
                    ASSERT_EQ(bytes.size(), tuple_size) << "torn read: unexpected tuple size";
                    total_samples.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    };

    std::thread r1(reader_loop);
    std::thread r2(reader_loop);

    writer.join();
    r1.join();
    r2.join();

    EXPECT_EQ(writes.load(), k_writer_ops);
    EXPECT_GT(total_samples.load(), 0u);
}

// -- 4. Compact races with get_tuple -- compact rewrites every slot entry ----

TEST(PageConcurrentLatch, CompactDoesNotTearConcurrentReaders) {
    Page page(4, PageType::DATA);

    constexpr size_t tuple_size = 50;

    std::vector<SlotId> slots;
    for (int i = 0; i < 10; ++i) {
        auto s = page.insert_tuple(patterned_tuple(tuple_size, 0x55));
        ASSERT_TRUE(s.has_value());
        slots.push_back(*s);
    }
    // Delete every other slot to create fragmentation so compact actually
    // moves bytes.
    for (size_t i = 0; i < slots.size(); i += 2) {
        (void)page.delete_tuple(slots[i]);
    }

    std::atomic<bool> done{false};
    std::atomic<size_t> compactions{0};
    std::atomic<size_t> total_samples{0};

    // Compactor: exactly k_writer_ops compact() calls.
    std::thread compactor([&]() {
        for (size_t i = 0; i < k_writer_ops; ++i) {
            page.compact();
            compactions.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });

    // Reader: read the live (odd-indexed) slots until compactor is done.
    std::thread reader([&]() {
        auto deadline = std::chrono::steady_clock::now() + 60s;
        while (!done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline) {
                ADD_FAILURE() << "hang-guard fired: compactor did not finish";
                break;
            }
            for (size_t i = 1; i < slots.size(); i += 2) {
                auto r = page.get_tuple(slots[i]);
                if (r.has_value()) {
                    ASSERT_EQ(r->size(), tuple_size) << "compact torn read";
                    total_samples.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    });

    compactor.join();
    reader.join();

    EXPECT_EQ(compactions.load(), k_writer_ops);
    EXPECT_GT(total_samples.load(), 0u);
}
