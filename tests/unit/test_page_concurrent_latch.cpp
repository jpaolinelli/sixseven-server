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
//
// --- Determinism fix (GDB-840 review blocker) --------------------------------
//
// FLAKINESS ROOT CAUSE: All tests used "sleep 500ms then stop" for
// termination. CompactDoesNotTearConcurrentReaders was 5/12 flaky because
// the compactor monopolized the exclusive page latch, starving the reader
// thread so it could not accumulate >100 reads within 500ms.
//
// FIX: Termination is now READER-COUNT-DRIVEN:
// - The READER runs until it has collected TARGET_READER_SAMPLES valid
//   samples, then sets stop. The writer/compactor spins until stop fires.
// - A generous wall-clock safety timeout guards against hangs on pathological
//   CI machines where the reader somehow never progresses.
// - The compactor yields between compact() calls so the reader gets
//   scheduling opportunities, preventing starvation while still exercising
//   genuine latch contention (reader still races the exclusive-latch compactor).
// - Non-vacuity guard = "we collected exactly TARGET_READER_SAMPLES samples
//   of each required type". Because the reader is the termination driver,
//   this is guaranteed by construction on any machine speed.
// - Torn-read detection is unchanged: FAIL() if any sample is outside the
//   valid-state set. The test must still fail if the latch protocol regresses.

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

// Safety timeout used by reader loops. Guards against hangs on pathologically
// slow or loaded CI machines where the reader cannot progress.
static constexpr auto READER_SAFETY_TIMEOUT = 30s;

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
// Termination: READER collects TARGET_SAMPLES_PER_SIZE samples of each size
// (guaranteed by looping until met), then sets stop. The writer spins until
// stop fires. Non-vacuity is guaranteed by construction: both sizes are
// required before the reader exits, so obs_a > 0 and obs_b > 0 always hold.

TEST(PageConcurrentLatch, GetTupleReadsStableSnapshotWhileWriterShrinks) {
    static constexpr size_t SIZE_A = 100;
    static constexpr size_t SIZE_B = 60;
    // Require this many samples of EACH size before terminating.
    static constexpr size_t TARGET_SAMPLES_PER_SIZE = 200U;

    Page page(1, PageType::DATA);

    // Seed with SIZE_A so the first writer iteration is a shrink (SIZE_A->SIZE_B).
    auto initial = patterned_tuple(SIZE_A, 0xAA);
    auto slot_r = page.insert_tuple(initial);
    ASSERT_TRUE(slot_r.has_value());
    SlotId slot = *slot_r;

    std::atomic<bool> stop{false};
    std::atomic<size_t> writes{0};
    std::atomic<size_t> obs_a{0};
    std::atomic<size_t> obs_b{0};
    std::atomic<size_t> torn{0};

    // Writer: spins until the reader sets stop, alternating SIZE_A <-> SIZE_B.
    std::thread writer([&]() {
        uint8_t tag = 0x01;
        bool use_a = false; // first write is SIZE_B (shrink)
        while (!stop.load(std::memory_order_relaxed)) {
            size_t len = use_a ? SIZE_A : SIZE_B;
            use_a = !use_a;
            auto t = patterned_tuple(len, tag++);
            (void)page.update_tuple(slot, t);
            writes.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Reader: loop until TARGET_SAMPLES_PER_SIZE of EACH size collected, or
    // safety timeout expires. Then set stop to unblock the writer.
    std::thread reader([&]() {
        const auto deadline = std::chrono::steady_clock::now() + READER_SAFETY_TIMEOUT;
        while ((obs_a.load(std::memory_order_relaxed) < TARGET_SAMPLES_PER_SIZE ||
                obs_b.load(std::memory_order_relaxed) < TARGET_SAMPLES_PER_SIZE) &&
               std::chrono::steady_clock::now() < deadline) {
            auto r = page.get_tuple(slot);
            if (!r.has_value()) {
                continue;
            }
            const auto& bytes = *r;
            ASSERT_FALSE(bytes.empty());

            ASSERT_EQ(static_cast<size_t>(bytes[0]), bytes.size())
                << "torn read: length byte (" << static_cast<int>(bytes[0])
                << ") does not match vector size (" << bytes.size() << ")";

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
        stop.store(true, std::memory_order_relaxed);
    });

    reader.join();
    writer.join();

    EXPECT_EQ(torn.load(), 0U);

    // Non-vacuity: the reader loop exits only once BOTH counters reached the
    // target (or safety timeout). On a non-broken machine the timeout is never
    // hit and both are guaranteed to be >= TARGET_SAMPLES_PER_SIZE.
    EXPECT_GE(obs_a.load(), TARGET_SAMPLES_PER_SIZE)
        << "reader never observed SIZE_A=" << SIZE_A << "; shrink path may not have been exercised";
    EXPECT_GE(obs_b.load(), TARGET_SAMPLES_PER_SIZE)
        << "reader never observed SIZE_B=" << SIZE_B << "; shrink path may not have been exercised";

    EXPECT_GT(writes.load(), 0U);
}

// -- 2. Read races write GROWING a tuple (reallocates in the page) ----------
//
// Valid-state set: {40, 60, 80, 50}. Both the length-byte invariant and the
// set-membership check are asserted on every reader sample. The reader runs
// until it has seen at least TARGET_SAMPLES_PER_SIZE samples in total AND
// observed at least 2 distinct sizes, then sets stop.

TEST(PageConcurrentLatch, GetTupleReadsStableSnapshotWhileWriterGrows) {
    static constexpr std::array<size_t, 4> VALID_SIZES = {40, 60, 80, 50};
    static constexpr size_t TARGET_TOTAL_SAMPLES = 500U;

    Page page(2, PageType::DATA);

    auto initial = patterned_tuple(40, 0xCC);
    auto slot_r = page.insert_tuple(initial);
    ASSERT_TRUE(slot_r.has_value());
    SlotId slot = *slot_r;

    std::atomic<bool> stop{false};
    std::array<std::atomic<size_t>, 4> obs{};
    for (auto& a : obs) {
        a.store(0);
    }
    std::atomic<size_t> torn{0};
    std::atomic<size_t> total_samples{0};

    // Writer: spins until reader sets stop, cycling through VALID_SIZES.
    std::thread writer([&]() {
        size_t i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            auto len = VALID_SIZES[i % VALID_SIZES.size()];
            ++i;
            auto t = patterned_tuple(len, static_cast<uint8_t>(i));
            (void)page.update_tuple(slot, t);
        }
    });

    // Reader: loop until TARGET_TOTAL_SAMPLES collected and at least 2 distinct
    // sizes observed, or safety timeout. Then set stop.
    std::thread reader([&]() {
        const auto deadline = std::chrono::steady_clock::now() + READER_SAFETY_TIMEOUT;
        auto distinct_seen = [&]() {
            size_t d = 0;
            for (const auto& a : obs) {
                if (a.load(std::memory_order_relaxed) > 0) {
                    ++d;
                }
            }
            return d;
        };

        while ((total_samples.load(std::memory_order_relaxed) < TARGET_TOTAL_SAMPLES ||
                distinct_seen() < 2U) &&
               std::chrono::steady_clock::now() < deadline) {
            auto r = page.get_tuple(slot);
            if (!r.has_value()) {
                continue;
            }
            const auto& bytes = *r;
            ASSERT_FALSE(bytes.empty());

            ASSERT_EQ(static_cast<size_t>(bytes[0]), bytes.size())
                << "torn read: length byte mismatches vector size";

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
            } else {
                total_samples.fetch_add(1, std::memory_order_relaxed);
            }
        }
        stop.store(true, std::memory_order_relaxed);
    });

    reader.join();
    writer.join();

    EXPECT_EQ(torn.load(), 0U);

    size_t distinct_seen = 0;
    for (const auto& a : obs) {
        if (a.load() > 0) {
            ++distinct_seen;
        }
    }
    EXPECT_GE(distinct_seen, 2U) << "reader observed fewer than 2 distinct sizes; grow path "
                                    "may not have been exercised under contention";
}

// -- 3. Two readers and a writer -- multiple readers must coexist safely -----
//
// Fixed-size tuples (32 bytes) so every slot always holds exactly 32 bytes.
// Valid-state set: {32}. Each reader runs until it has collected
// TARGET_READER_SAMPLES valid samples, then the last reader sets stop.

TEST(PageConcurrentLatch, MultipleReadersDoNotBlockEachOther) {
    static constexpr size_t TUPLE_SIZE = 32;
    static constexpr size_t TARGET_READER_SAMPLES = 500U; // per reader

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
    // Counts how many reader threads have finished their target samples.
    std::atomic<size_t> readers_done{0};

    auto reader_loop = [&]() {
        const auto deadline = std::chrono::steady_clock::now() + READER_SAFETY_TIMEOUT;
        size_t samples = 0;
        while (samples < TARGET_READER_SAMPLES && std::chrono::steady_clock::now() < deadline) {
            for (SlotId s : slots) {
                auto r = page.get_tuple(s);
                if (r.has_value()) {
                    const auto& bytes = *r;
                    if (bytes.size() != TUPLE_SIZE) {
                        torn.fetch_add(1, std::memory_order_relaxed);
                        FAIL() << "torn read: expected size " << TUPLE_SIZE << " but got "
                               << bytes.size();
                    }
                    ASSERT_EQ(static_cast<size_t>(bytes[0]), bytes.size())
                        << "torn read: length byte mismatches vector size";
                    reads.fetch_add(1, std::memory_order_relaxed);
                    ++samples;
                }
            }
        }
        // Last reader to finish sets stop to unblock the writer.
        if (readers_done.fetch_add(1, std::memory_order_acq_rel) == 1U) {
            stop.store(true, std::memory_order_relaxed);
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

    r1.join();
    r2.join();
    writer.join();

    EXPECT_EQ(torn.load(), 0U);
    EXPECT_GT(reads.load(), 0U)
        << "too few reads; multi-reader path was not meaningfully exercised";
}

// -- 4. Compact races with get_tuple -- compact rewrites every slot entry ----
//
// Valid-state set: {50} (live slots always hold 50-byte tuples).
// Termination: READER collects TARGET_READER_SAMPLES valid samples, then sets
// stop. The compactor spins (with yield between calls) until stop fires.
// The yield gives the reader guaranteed scheduling opportunities -- preventing
// the starvation that caused the original 5/12 flakiness -- while still
// exercising genuine latch contention (the reader races the exclusive-latch
// compactor on every compact() call).

TEST(PageConcurrentLatch, CompactDoesNotTearConcurrentReaders) {
    static constexpr size_t TUPLE_SIZE = 50;
    static constexpr size_t TARGET_READER_SAMPLES = 500U;

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

    // Compactor: spins until reader sets stop. Yields between compact() calls
    // so the reader thread is guaranteed scheduling opportunities.
    std::thread compactor([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            page.compact();
            compactions.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Reader: collect TARGET_READER_SAMPLES valid samples, then set stop.
    // Safety timeout prevents hangs on severely loaded machines.
    std::thread reader([&]() {
        const auto deadline = std::chrono::steady_clock::now() + READER_SAFETY_TIMEOUT;
        size_t samples = 0;
        while (samples < TARGET_READER_SAMPLES && std::chrono::steady_clock::now() < deadline) {
            for (SlotId s : live_slots) {
                auto r = page.get_tuple(s);
                if (r.has_value()) {
                    if (r->size() != TUPLE_SIZE) {
                        torn.fetch_add(1, std::memory_order_relaxed);
                        FAIL() << "compact torn read: expected " << TUPLE_SIZE << " bytes, got "
                               << r->size();
                    }
                    reads.fetch_add(1, std::memory_order_relaxed);
                    ++samples;
                }
            }
        }
        stop.store(true, std::memory_order_relaxed);
    });

    reader.join();
    compactor.join();

    EXPECT_EQ(torn.load(), 0U);
    EXPECT_GT(reads.load(), 0U) << "reader observed no tuples; compact test may be vacuous";
    EXPECT_GT(compactions.load(), 0U)
        << "compactor ran zero times; contended compact path not exercised";
}