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
// --- Anti-pattern fixed (GDB-840) -------------------------------------------
//
// ORIGINAL BUG: Test 1 (shrink) had an inert torn-read assertion. The writer
// called patterned_tuple(80, tag++) every iteration -- a CONSTANT size of 80.
// Because 80 <= 80 (the initial slot length after the first write), the
// update_tuple() in-place path never moved the slot's offset field; only the
// tag byte changed. So bytes[0] (== 80) always equalled bytes.size() (== 80),
// making the torn-read assertion a pure identity check that could never fail
// even if the latch were removed entirely. The single 100->80 shrink happened
// before the reader thread started, so it was unobservable.
//
// FIX: The writer now alternates between two well-separated sizes (SIZE_A=100
// and SIZE_B=60) so that (a) the shrink path fires on every other write,
// (b) the valid-state set is {100, 60} -- any other size is proof of a torn
// read -- and (c) transition counters assert we actually exercised the
// contended shrink path many times during the run, not zero times. The
// assertion is now non-trivial on EVERY sample taken by the reader.

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

// Fill helper -- produces a recognizable byte pattern with known length so
// readers can verify the copy is internally self-consistent.
//
// Invariant: bytes[0] encodes the tuple length (mod 256). For the sizes used
// in these tests (all < 256) this is exact. A reader that observes
// bytes[0] != bytes.size() has witnessed a torn read: it holds the length
// field from one version and the data bytes from another.
std::vector<uint8_t> patterned_tuple(size_t len, uint8_t tag) {
    std::vector<uint8_t> out(len, tag);
    // Encode the length in the first byte so readers can double-check.
    out[0] = static_cast<uint8_t>(len & 0xFF);
    return out;
}

} // namespace

// -- 1. Read races write SHRINKING a tuple -----------------------------------
//
// The writer alternates between two distinct sizes (SIZE_A=100 and SIZE_B=60)
// so that the shrink path (new_len < entry.length -> in-place overwrite with
// trailing zeroing + slot-length update) fires on every other write.
//
// Valid-state set for any reader snapshot: {SIZE_A, SIZE_B}.
// Any other size means the reader observed a half-committed slot update.
//
// The transition counters assert the test actually drove the contended path --
// it cannot pass by coincidence if the latch protocol regresses (e.g. the
// latch is removed: torn sizes would appear, failing the valid-set check).

TEST(PageConcurrentLatch, GetTupleReadsStableSnapshotWhileWriterShrinks) {
    static constexpr size_t SIZE_A = 100;
    static constexpr size_t SIZE_B = 60;
    // Run many iterations so a regression is reliably caught even on a fast
    // machine where one thread might otherwise never interleave.
    static constexpr int NUM_READER_ITERATIONS = 200'000;

    Page page(1, PageType::DATA);

    // Seed with SIZE_A so the first writer iteration is a shrink (SIZE_A->SIZE_B).
    auto initial = patterned_tuple(SIZE_A, 0xAA);
    auto slot_r = page.insert_tuple(initial);
    ASSERT_TRUE(slot_r.has_value());
    SlotId slot = *slot_r;

    std::atomic<bool> stop{false};
    std::atomic<size_t> writes{0};
    // Per-size observation counters.
    std::atomic<size_t> obs_a{0};
    std::atomic<size_t> obs_b{0};
    std::atomic<size_t> torn{0};

    // Writer alternates SIZE_A <-> SIZE_B continuously. Both sizes are less
    // than (or equal to) the initial slot length, exercising the in-place
    // shrink path on every iteration. The tag byte varies so the payload is
    // not constant -- a reader could not cache a valid answer.
    std::thread writer([&]() {
        uint8_t tag = 0x01;
        bool use_a = false; // start with SIZE_B so the very first write shrinks
        while (!stop.load(std::memory_order_relaxed)) {
            size_t len = use_a ? SIZE_A : SIZE_B;
            use_a = !use_a;
            auto t = patterned_tuple(len, tag++);
            (void)page.update_tuple(slot, t);
            writes.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Reader: sample NUM_READER_ITERATIONS times. Every sample must belong to
    // the valid-state set {SIZE_A, SIZE_B}. The length-byte invariant gives a
    // self-checking cross-field consistency check within a single snapshot
    // (bytes[0] must equal bytes.size()), catching any partial write where the
    // slot length was updated but the data bytes were not yet fully written (or
    // vice-versa).
    std::thread reader([&]() {
        for (int i = 0; i < NUM_READER_ITERATIONS && !stop.load(std::memory_order_relaxed); ++i) {
            auto r = page.get_tuple(slot);
            if (!r.has_value()) {
                // Transient NOT_FOUND not expected (slot is never deleted), but
                // skip rather than fail -- the transition-count guard below
                // will catch if we never actually saw anything.
                continue;
            }
            const auto& bytes = *r;
            ASSERT_FALSE(bytes.empty());

            // Self-consistency within the snapshot: length byte must match the
            // vector size. A violation means two fields were read from
            // different instants -- a torn read.
            ASSERT_EQ(static_cast<size_t>(bytes[0]), bytes.size())
                << "torn read: length byte (" << static_cast<int>(bytes[0])
                << ") does not match vector size (" << bytes.size() << ")";

            // Valid-set membership: the size must be one of the two the writer
            // is producing. Any other size is impossible without a torn read.
            const bool is_a = (bytes.size() == SIZE_A);
            const bool is_b = (bytes.size() == SIZE_B);
            if (is_a) {
                obs_a.fetch_add(1, std::memory_order_relaxed);
            } else if (is_b) {
                obs_b.fetch_add(1, std::memory_order_relaxed);
            } else {
                torn.fetch_add(1, std::memory_order_relaxed);
                FAIL() << "observer saw invalid size " << bytes.size()
                       << " -- not in valid-state set {" << SIZE_A << ", " << SIZE_B
                       << "}; torn read detected";
            }
        }
    });

    // Give the writer a head start so transitions are already happening when
    // the reader loop begins its NUM_READER_ITERATIONS.
    std::this_thread::sleep_for(500ms);
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    reader.join();

    // Sanity: no torn reads reported via the counter.
    EXPECT_EQ(torn.load(), 0u);

    // Transition coverage: the reader must have observed both sizes, proving
    // the test actually exercised the contended shrink path and did not just
    // observe a single quiescent state for the entire run.
    EXPECT_GT(obs_a.load(), 100u) << "reader never observed SIZE_A=" << SIZE_A
                                  << "; shrink path may not have been exercised";
    EXPECT_GT(obs_b.load(), 100u) << "reader never observed SIZE_B=" << SIZE_B
                                  << "; shrink path may not have been exercised";

    // Writer throughput sanity.
    EXPECT_GT(writes.load(), 100u);
}

// -- 2. Read races write GROWING a tuple (reallocates in the page) ----------
//
// Valid-state set: {40, 60, 80, 50}. Both the length-byte invariant and the
// set-membership check are asserted on every reader sample. A distinct-sizes
// counter asserts we observed multiple sizes (grow path exercised).

TEST(PageConcurrentLatch, GetTupleReadsStableSnapshotWhileWriterGrows) {
    static constexpr std::array<size_t, 4> VALID_SIZES = {40, 60, 80, 50};
    static constexpr int NUM_READER_ITERATIONS = 200'000;

    Page page(2, PageType::DATA);

    auto initial = patterned_tuple(40, 0xCC);
    auto slot_r = page.insert_tuple(initial);
    ASSERT_TRUE(slot_r.has_value());
    SlotId slot = *slot_r;

    std::atomic<bool> stop{false};
    // Per-size observation counters indexed by VALID_SIZES.
    std::array<std::atomic<size_t>, 4> obs{};
    for (auto& a : obs) {
        a.store(0);
    }
    std::atomic<size_t> torn{0};

    std::thread writer([&]() {
        size_t i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            auto len = VALID_SIZES[i % VALID_SIZES.size()];
            ++i;
            auto t = patterned_tuple(len, static_cast<uint8_t>(i));
            (void)page.update_tuple(slot, t);
        }
    });

    std::thread reader([&]() {
        for (int i = 0; i < NUM_READER_ITERATIONS && !stop.load(std::memory_order_relaxed); ++i) {
            auto r = page.get_tuple(slot);
            if (!r.has_value()) {
                continue;
            }
            const auto& bytes = *r;
            ASSERT_FALSE(bytes.empty());

            // Self-consistency: length byte must match snapshot size.
            ASSERT_EQ(static_cast<size_t>(bytes[0]), bytes.size())
                << "torn read: length byte mismatches vector size";

            // Valid-set membership.
            bool found = false;
            for (size_t idx = 0; idx < VALID_SIZES.size(); ++idx) {
                if (bytes.size() == VALID_SIZES[idx]) {
                    obs[idx].fetch_add(1, std::memory_order_relaxed);
                    found = true;
                    break;
                }
            }
            if (!found) {
                torn.fetch_add(1, std::memory_order_relaxed);
                FAIL() << "observer saw invalid size " << bytes.size()
                       << " -- not in valid-state set; torn read detected";
            }
        }
    });

    std::this_thread::sleep_for(500ms);
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    reader.join();

    EXPECT_EQ(torn.load(), 0u);

    // Transition coverage: reader must have observed at least two distinct
    // sizes so we know the grow path was exercised under contention.
    size_t distinct_seen = 0;
    for (const auto& a : obs) {
        if (a.load() > 0) {
            ++distinct_seen;
        }
    }
    EXPECT_GE(distinct_seen, 2u) << "reader observed fewer than 2 distinct sizes; grow path "
                                    "may not have been exercised under contention";
}

// -- 3. Two readers and a writer -- multiple readers must coexist safely -----
//
// Fixed-size tuples (32 bytes) so every slot always holds exactly 32 bytes.
// Valid-state set: {32}. Both the exact-size check and a throughput counter
// guard against a vacuous run.

TEST(PageConcurrentLatch, MultipleReadersDoNotBlockEachOther) {
    static constexpr size_t TUPLE_SIZE = 32;
    static constexpr int NUM_READER_ITERATIONS = 200'000;

    Page page(3, PageType::DATA);

    // Insert 8 tuples so readers have multiple slots to exercise.
    std::vector<SlotId> slots;
    for (int i = 0; i < 8; ++i) {
        auto s = page.insert_tuple(patterned_tuple(TUPLE_SIZE, static_cast<uint8_t>(i)));
        ASSERT_TRUE(s.has_value());
        slots.push_back(*s);
    }

    std::atomic<bool> stop{false};
    std::atomic<size_t> reads{0};
    std::atomic<size_t> torn{0};

    auto reader_loop = [&]() {
        for (int i = 0; i < NUM_READER_ITERATIONS && !stop.load(std::memory_order_relaxed); ++i) {
            for (SlotId s : slots) {
                auto r = page.get_tuple(s);
                if (r.has_value()) {
                    const auto& bytes = *r;
                    // Size must be exactly TUPLE_SIZE on every read; the writer
                    // never changes the size, so any deviation is a torn read.
                    if (bytes.size() != TUPLE_SIZE) {
                        torn.fetch_add(1, std::memory_order_relaxed);
                        FAIL() << "torn read: expected size " << TUPLE_SIZE << " but got "
                               << bytes.size();
                    }
                    // Self-consistency: length byte == snapshot size.
                    ASSERT_EQ(static_cast<size_t>(bytes[0]), bytes.size())
                        << "torn read: length byte mismatches vector size";
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
            (void)page.update_tuple(s, patterned_tuple(TUPLE_SIZE, static_cast<uint8_t>(++i)));
        }
    });

    std::this_thread::sleep_for(500ms);
    stop.store(true, std::memory_order_relaxed);
    r1.join();
    r2.join();
    writer.join();

    EXPECT_EQ(torn.load(), 0u);
    EXPECT_GT(reads.load(), 1000u)
        << "too few reads; multi-reader path was not meaningfully exercised";
}

// -- 4. Compact races with get_tuple -- compact rewrites every slot entry ----
//
// Valid-state set: {50} (live slots always hold 50-byte tuples).
// A compaction counter asserts compact() ran many times so the test is not
// vacuous.

TEST(PageConcurrentLatch, CompactDoesNotTearConcurrentReaders) {
    static constexpr size_t TUPLE_SIZE = 50;
    static constexpr int NUM_READER_ITERATIONS = 100'000;

    Page page(4, PageType::DATA);

    std::vector<SlotId> slots;
    for (int i = 0; i < 10; ++i) {
        auto s = page.insert_tuple(patterned_tuple(TUPLE_SIZE, 0x55));
        ASSERT_TRUE(s.has_value());
        slots.push_back(*s);
    }
    // Delete every other slot to create fragmentation so compact actually
    // moves bytes.
    std::vector<SlotId> live_slots;
    for (size_t i = 0; i < slots.size(); i += 2) {
        (void)page.delete_tuple(slots[i]);
    }
    for (size_t i = 1; i < slots.size(); i += 2) {
        live_slots.push_back(slots[i]);
    }

    std::atomic<bool> stop{false};
    std::atomic<size_t> reads{0};
    std::atomic<size_t> compactions{0};
    std::atomic<size_t> torn{0};

    std::thread compactor([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            page.compact();
            compactions.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread reader([&]() {
        for (int i = 0; i < NUM_READER_ITERATIONS && !stop.load(std::memory_order_relaxed); ++i) {
            for (SlotId s : live_slots) {
                auto r = page.get_tuple(s);
                if (r.has_value()) {
                    if (r->size() != TUPLE_SIZE) {
                        torn.fetch_add(1, std::memory_order_relaxed);
                        FAIL() << "compact torn read: expected " << TUPLE_SIZE << " bytes, got "
                               << r->size();
                    }
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    });

    std::this_thread::sleep_for(500ms);
    stop.store(true, std::memory_order_relaxed);
    compactor.join();
    reader.join();

    EXPECT_EQ(torn.load(), 0u);
    EXPECT_GT(reads.load(), 100u) << "reader observed too few tuples; compact test may be vacuous";
    EXPECT_GT(compactions.load(), 100u)
        << "compactor ran too few times; contended compact path not exercised";
}
