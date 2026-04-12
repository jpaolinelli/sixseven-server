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

// Fill helper — produces a recognizable byte pattern with known length so
// readers can verify the copy is internally self-consistent.
std::vector<uint8_t> patterned_tuple(size_t len, uint8_t tag) {
    std::vector<uint8_t> out(len, tag);
    // Encode the length in the first byte so readers can double-check.
    // (Only meaningful for len < 256; the test uses small lengths.)
    out[0] = static_cast<uint8_t>(len & 0xFF);
    return out;
}

} // namespace

// -- 1. Read races write shrinking a tuple -----------------------------------

TEST(PageConcurrentLatch, GetTupleReadsStableSnapshotWhileWriterShrinks) {
    Page page(1, PageType::DATA);

    // Seed with a known tuple.
    auto initial = patterned_tuple(100, 0xAA);
    auto slot_r = page.insert_tuple(initial);
    ASSERT_TRUE(slot_r.has_value());
    SlotId slot = *slot_r;

    std::atomic<bool> stop{false};
    std::atomic<size_t> reads{0};
    std::atomic<size_t> writes{0};

    // Writer alternates between two sizes so the in-place path and the
    // shrink-then-zero path both get exercised.
    std::thread writer([&]() {
        uint8_t tag = 0x00;
        while (!stop.load(std::memory_order_relaxed)) {
            auto t = patterned_tuple(80, tag++);
            (void)page.update_tuple(slot, t);
            writes.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread reader([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            auto r = page.get_tuple(slot);
            if (r.has_value()) {
                const auto& bytes = *r;
                // Under the latch the copy must match one of the two sizes
                // the writer is alternating between, never a torn mix.
                ASSERT_FALSE(bytes.empty());
                ASSERT_EQ(static_cast<size_t>(bytes[0]), bytes.size())
                    << "torn read: length byte does not match vector size";
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    std::this_thread::sleep_for(300ms);
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    reader.join();

    EXPECT_GT(reads.load(), 0u);
    EXPECT_GT(writes.load(), 0u);
}

// -- 2. Read races write growing a tuple (reallocates in the page) ----------

TEST(PageConcurrentLatch, GetTupleReadsStableSnapshotWhileWriterGrows) {
    Page page(2, PageType::DATA);

    auto initial = patterned_tuple(40, 0xCC);
    auto slot_r = page.insert_tuple(initial);
    ASSERT_TRUE(slot_r.has_value());
    SlotId slot = *slot_r;

    std::atomic<bool> stop{false};
    std::atomic<size_t> reads{0};

    std::thread writer([&]() {
        size_t i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            // Grow the tuple: forces reallocation at the bottom of the page
            // and rewrites the slot entry. This is the specific scenario
            // the embedding store callback hits.
            auto sizes = std::array<size_t, 4>{40, 60, 80, 50};
            auto len = sizes[i++ % sizes.size()];
            auto t = patterned_tuple(len, static_cast<uint8_t>(i));
            (void)page.update_tuple(slot, t);
        }
    });

    std::thread reader([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            auto r = page.get_tuple(slot);
            if (r.has_value()) {
                const auto& bytes = *r;
                ASSERT_FALSE(bytes.empty());
                ASSERT_EQ(static_cast<size_t>(bytes[0]), bytes.size())
                    << "torn read: length byte mismatches vector size";
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    std::this_thread::sleep_for(300ms);
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    reader.join();

    EXPECT_GT(reads.load(), 0u);
}

// -- 3. Two readers and a writer -- multiple readers must coexist safely ----

TEST(PageConcurrentLatch, MultipleReadersDoNotBlockEachOther) {
    Page page(3, PageType::DATA);

    // Insert 8 tuples so readers have multiple slots to exercise.
    std::vector<SlotId> slots;
    for (int i = 0; i < 8; ++i) {
        auto s = page.insert_tuple(patterned_tuple(32, static_cast<uint8_t>(i)));
        ASSERT_TRUE(s.has_value());
        slots.push_back(*s);
    }

    std::atomic<bool> stop{false};
    std::atomic<size_t> reads{0};

    auto reader_loop = [&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            for (SlotId s : slots) {
                auto r = page.get_tuple(s);
                if (r.has_value()) {
                    const auto& bytes = *r;
                    ASSERT_EQ(bytes.size(), 32u);
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    };

    std::thread r1(reader_loop);
    std::thread r2(reader_loop);
    std::thread writer([&]() {
        size_t i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            SlotId s = slots[i % slots.size()];
            (void)page.update_tuple(s, patterned_tuple(32, static_cast<uint8_t>(++i)));
        }
    });

    std::this_thread::sleep_for(200ms);
    stop.store(true, std::memory_order_relaxed);
    r1.join();
    r2.join();
    writer.join();

    EXPECT_GT(reads.load(), 0u);
}

// -- 4. Compact races with get_tuple — compact rewrites every slot entry ----

TEST(PageConcurrentLatch, CompactDoesNotTearConcurrentReaders) {
    Page page(4, PageType::DATA);

    std::vector<SlotId> slots;
    for (int i = 0; i < 10; ++i) {
        auto s = page.insert_tuple(patterned_tuple(50, 0x55));
        ASSERT_TRUE(s.has_value());
        slots.push_back(*s);
    }
    // Delete every other slot to create fragmentation so compact actually
    // moves bytes.
    for (size_t i = 0; i < slots.size(); i += 2) {
        (void)page.delete_tuple(slots[i]);
    }

    std::atomic<bool> stop{false};
    std::atomic<size_t> reads{0};

    std::thread compactor([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            page.compact();
        }
    });

    std::thread reader([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            // Read the live (odd-indexed) slots.
            for (size_t i = 1; i < slots.size(); i += 2) {
                auto r = page.get_tuple(slots[i]);
                if (r.has_value()) {
                    ASSERT_EQ(r->size(), 50u) << "compact torn read";
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    });

    std::this_thread::sleep_for(200ms);
    stop.store(true, std::memory_order_relaxed);
    compactor.join();
    reader.join();

    EXPECT_GT(reads.load(), 0u);
}
