/// @file test_qa_gdb_849.cpp
/// @brief Adversarial QA tests for GDB-849: page compaction / free-space accounting.
///
/// Every expected value is derived from the page layout constants:
///   page_size        = 8192
///   page_header_size = 24
///   slot_entry_size  = 4  (2 x uint16_t)
///   fresh page free_space() = page_size - page_header_size - slot_entry_size
///                           = 8192 - 24 - 4 = 8164
///
/// compact() repacks live tuples by their slot.length, so shrink-in-place slack
/// (the gap between data_offset and the tuple's actual start) is reclaimed.
/// None of the assertions below use >= alone; all are EXPECT_EQ on exact values.

#include "sixseven/storage/page.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace sixseven;

namespace {

// ---------------------------------------------------------------------------
// Layout constants (must match page.h / page.cpp exactly)
// ---------------------------------------------------------------------------
static constexpr size_t HEADER = page_header_size; // 24
static constexpr size_t SLOT = slot_entry_size;    // 4
static constexpr size_t PAGE = page_size;          // 8192

// free_space() on a page with N slots and data_offset D:
//   raw = D - (HEADER + N*SLOT)
//   free_space = (raw >= SLOT) ? raw - SLOT : 0
//
// Fresh page (N=0, D=8192): free_space = 8192 - 24 - 4 = 8164
static constexpr size_t FRESH_FREE = PAGE - HEADER - SLOT; // 8164

static std::vector<uint8_t> make_bytes(size_t len, uint8_t fill) {
    return std::vector<uint8_t>(len, fill);
}

} // namespace

// ============================================================================
// GDB-849: multiple shrink-in-place updates → compact reclaims exact sum
// ============================================================================

// Three tuples, each shrunk by a different amount. compact() must reclaim
// exactly the total slack: (500-100) + (300-50) + (400-200) = 400+250+200 = 850.
TEST(QA_Page_GDB849, MultipleShrinksTotalSlackReclaimed) {
    Page page(1, PageType::DATA);

    // slot 0: 500 bytes
    auto r0 = page.insert_tuple(make_bytes(500, 0x01));
    ASSERT_TRUE(r0.has_value());

    // slot 1: 300 bytes
    auto r1 = page.insert_tuple(make_bytes(300, 0x02));
    ASSERT_TRUE(r1.has_value());

    // slot 2: 400 bytes
    auto r2 = page.insert_tuple(make_bytes(400, 0x03));
    ASSERT_TRUE(r2.has_value());

    // Shrink slot 0: 500 → 100, slack = 400
    ASSERT_TRUE(page.update_tuple(0, make_bytes(100, 0xAA)).has_value());
    // Shrink slot 1: 300 → 50, slack = 250
    ASSERT_TRUE(page.update_tuple(1, make_bytes(50, 0xBB)).has_value());
    // Shrink slot 2: 400 → 200, slack = 200
    ASSERT_TRUE(page.update_tuple(2, make_bytes(200, 0xCC)).has_value());

    size_t before = page.free_space();
    page.compact();
    size_t after = page.free_space();

    // Total slack reclaimed = 400 + 250 + 200 = 850
    EXPECT_EQ(after, before + 850u) << "compact must reclaim exactly 850 bytes of shrink slack";

    // Verify tuple contents are intact after compact.
    auto t0 = page.get_tuple(0);
    ASSERT_TRUE(t0.has_value());
    ASSERT_EQ(t0->size(), 100u);
    EXPECT_EQ((*t0)[0], 0xAA);

    auto t1 = page.get_tuple(1);
    ASSERT_TRUE(t1.has_value());
    ASSERT_EQ(t1->size(), 50u);
    EXPECT_EQ((*t1)[0], 0xBB);

    auto t2 = page.get_tuple(2);
    ASSERT_TRUE(t2.has_value());
    ASSERT_EQ(t2->size(), 200u);
    EXPECT_EQ((*t2)[0], 0xCC);
}

// ============================================================================
// GDB-849: shrink + delete mix — exact bytes reclaimed, slot tombstones valid
// ============================================================================

// Insert 4 tuples. Shrink slot 1 (300→80, slack=220). Delete slot 2 (400 bytes).
// compact() must reclaim 220 (shrink slack) + 400 (deleted tuple body) = 620 bytes.
// After compact, slot 1 returns NOT_FOUND, slot 2 deleted still returns NOT_FOUND.
TEST(QA_Page_GDB849, ShrinkAndDeleteMixExactReclaim) {
    Page page(1, PageType::DATA);

    // slot 0: 200 bytes
    ASSERT_TRUE(page.insert_tuple(make_bytes(200, 0x10)).has_value());
    // slot 1: 300 bytes (will be shrunk)
    ASSERT_TRUE(page.insert_tuple(make_bytes(300, 0x20)).has_value());
    // slot 2: 400 bytes (will be deleted)
    ASSERT_TRUE(page.insert_tuple(make_bytes(400, 0x30)).has_value());
    // slot 3: 150 bytes
    ASSERT_TRUE(page.insert_tuple(make_bytes(150, 0x40)).has_value());

    // Shrink slot 1: 300 → 80
    ASSERT_TRUE(page.update_tuple(1, make_bytes(80, 0x21)).has_value());
    // Delete slot 2
    ASSERT_TRUE(page.delete_tuple(2).has_value());

    size_t before = page.free_space();
    page.compact();
    size_t after = page.free_space();

    // Reclaimed: (300-80) + 400 = 220 + 400 = 620
    EXPECT_EQ(after, before + 620u)
        << "compact must reclaim exactly 620 bytes (shrink slack + deleted tuple)";

    // Slot 0 and 3 still live.
    auto t0 = page.get_tuple(0);
    ASSERT_TRUE(t0.has_value());
    ASSERT_EQ(t0->size(), 200u);
    EXPECT_EQ((*t0)[0], 0x10);

    auto t1 = page.get_tuple(1);
    ASSERT_TRUE(t1.has_value());
    ASSERT_EQ(t1->size(), 80u);
    EXPECT_EQ((*t1)[0], 0x21);

    // slot 2 must be deleted (tombstone preserved).
    auto t2 = page.get_tuple(2);
    EXPECT_FALSE(t2.has_value());

    auto t3 = page.get_tuple(3);
    ASSERT_TRUE(t3.has_value());
    ASSERT_EQ(t3->size(), 150u);
    EXPECT_EQ((*t3)[0], 0x40);
}

// ============================================================================
// GDB-849: grow update (tuple gets bigger) then compact — free_space exact
// ============================================================================

// Insert two tuples. Grow slot 0 from 100 to 300.
// update_tuple grow-relocate: zeros old body, sets slot to {0,0}, then allocates
// new block at data_offset bottom. The old 100-byte region becomes an unreferenced
// gap ABOVE data_offset. compact() sees it and reclaims exactly 100 bytes.
TEST(QA_Page_GDB849, GrowThenCompactReclaimsOldBodySlack) {
    Page page(1, PageType::DATA);

    // slot 0: 100 bytes
    ASSERT_TRUE(page.insert_tuple(make_bytes(100, 0x01)).has_value());
    // slot 1: 200 bytes
    ASSERT_TRUE(page.insert_tuple(make_bytes(200, 0x02)).has_value());

    // Grow slot 0: 100 → 300 (old 100-byte region is orphaned above data_offset)
    ASSERT_TRUE(page.update_tuple(0, make_bytes(300, 0x03)).has_value());

    size_t before = page.free_space();
    page.compact();
    size_t after = page.free_space();

    // compact() repacks by slot.length: slot 0 body (300) + slot 1 body (200) = 500 bytes.
    // The orphaned 100-byte region (old slot 0) is reclaimed → after == before + 100.
    EXPECT_EQ(after, before + 100u)
        << "compact after grow-relocate must reclaim exactly the old tuple size (100 bytes)";

    // Verify data integrity.
    auto t0 = page.get_tuple(0);
    ASSERT_TRUE(t0.has_value());
    ASSERT_EQ(t0->size(), 300u);
    EXPECT_EQ((*t0)[0], 0x03);

    auto t1 = page.get_tuple(1);
    ASSERT_TRUE(t1.has_value());
    ASSERT_EQ(t1->size(), 200u);
    EXPECT_EQ((*t1)[0], 0x02);
}

// ============================================================================
// GDB-849: compact with NO fragmentation is a true no-op (exact equality)
// ============================================================================

// Insert three same-sized tuples; no shrinks, no deletes. compact() cannot
// reclaim any bytes so after == before exactly.
TEST(QA_Page_GDB849, CompactNoFragmentationIsExactNoOp) {
    Page page(1, PageType::DATA);

    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(page.insert_tuple(make_bytes(200, static_cast<uint8_t>(i + 1))).has_value());
    }

    size_t before = page.free_space();
    page.compact();
    size_t after = page.free_space();

    EXPECT_EQ(after, before) << "compact with no fragmentation must not change free_space";

    // Data must be intact.
    for (int i = 0; i < 3; ++i) {
        auto t = page.get_tuple(static_cast<SlotId>(i));
        ASSERT_TRUE(t.has_value()) << "slot " << i << " must still be live";
        ASSERT_EQ(t->size(), 200u);
        EXPECT_EQ((*t)[0], static_cast<uint8_t>(i + 1));
    }
}

// ============================================================================
// GDB-849: compact twice in a row — second compact is idempotent (after2 == after1)
// ============================================================================

TEST(QA_Page_GDB849, CompactTwiceIsIdempotent) {
    Page page(1, PageType::DATA);

    ASSERT_TRUE(page.insert_tuple(make_bytes(500, 0xAA)).has_value());
    ASSERT_TRUE(page.insert_tuple(make_bytes(500, 0xBB)).has_value());
    ASSERT_TRUE(page.insert_tuple(make_bytes(500, 0xCC)).has_value());

    // Shrink slot 1: 500 → 100, slack = 400
    ASSERT_TRUE(page.update_tuple(1, make_bytes(100, 0xDD)).has_value());

    page.compact();
    size_t after1 = page.free_space();

    page.compact();
    size_t after2 = page.free_space();

    EXPECT_EQ(after2, after1) << "second compact must be a no-op: free_space must not change";

    // Data still intact after double compact.
    auto t0 = page.get_tuple(0);
    ASSERT_TRUE(t0.has_value());
    EXPECT_EQ(t0->size(), 500u);
    EXPECT_EQ((*t0)[0], 0xAA);

    auto t1 = page.get_tuple(1);
    ASSERT_TRUE(t1.has_value());
    EXPECT_EQ(t1->size(), 100u);
    EXPECT_EQ((*t1)[0], 0xDD);

    auto t2 = page.get_tuple(2);
    ASSERT_TRUE(t2.has_value());
    EXPECT_EQ(t2->size(), 500u);
    EXPECT_EQ((*t2)[0], 0xCC);
}

// ============================================================================
// GDB-849: tuple contents and byte patterns exact after compact
// ============================================================================

// Inserts tuples with unique fill bytes, deletes some, shrinks another, then
// reads every byte of every surviving tuple to confirm compaction didn't
// corrupt or mix data from adjacent tuples.
TEST(QA_Page_GDB849, TupleContentsExactAfterCompact) {
    Page page(1, PageType::DATA);

    // slot 0: 100 bytes of 0x11
    ASSERT_TRUE(page.insert_tuple(make_bytes(100, 0x11)).has_value());
    // slot 1: 200 bytes of 0x22
    ASSERT_TRUE(page.insert_tuple(make_bytes(200, 0x22)).has_value());
    // slot 2: 150 bytes of 0x33
    ASSERT_TRUE(page.insert_tuple(make_bytes(150, 0x33)).has_value());
    // slot 3: 300 bytes of 0x44
    ASSERT_TRUE(page.insert_tuple(make_bytes(300, 0x44)).has_value());
    // slot 4: 50 bytes of 0x55
    ASSERT_TRUE(page.insert_tuple(make_bytes(50, 0x55)).has_value());

    // Delete slot 1.
    ASSERT_TRUE(page.delete_tuple(1).has_value());
    // Shrink slot 3: 300 → 60
    ASSERT_TRUE(page.update_tuple(3, make_bytes(60, 0x66)).has_value());

    page.compact();

    // slot 0: every byte must be 0x11
    {
        auto t = page.get_tuple(0);
        ASSERT_TRUE(t.has_value());
        ASSERT_EQ(t->size(), 100u);
        for (size_t i = 0; i < t->size(); ++i) {
            EXPECT_EQ((*t)[i], 0x11u) << "slot 0 byte " << i << " corrupted";
        }
    }

    // slot 1: deleted
    EXPECT_FALSE(page.get_tuple(1).has_value());

    // slot 2: every byte must be 0x33
    {
        auto t = page.get_tuple(2);
        ASSERT_TRUE(t.has_value());
        ASSERT_EQ(t->size(), 150u);
        for (size_t i = 0; i < t->size(); ++i) {
            EXPECT_EQ((*t)[i], 0x33u) << "slot 2 byte " << i << " corrupted";
        }
    }

    // slot 3: shrunk to 60, every byte must be 0x66
    {
        auto t = page.get_tuple(3);
        ASSERT_TRUE(t.has_value());
        ASSERT_EQ(t->size(), 60u);
        for (size_t i = 0; i < t->size(); ++i) {
            EXPECT_EQ((*t)[i], 0x66u) << "slot 3 byte " << i << " corrupted";
        }
    }

    // slot 4: every byte must be 0x55
    {
        auto t = page.get_tuple(4);
        ASSERT_TRUE(t.has_value());
        ASSERT_EQ(t->size(), 50u);
        for (size_t i = 0; i < t->size(); ++i) {
            EXPECT_EQ((*t)[i], 0x55u) << "slot 4 byte " << i << " corrupted";
        }
    }
}

// ============================================================================
// GDB-849: boundary — compact on empty page (no tuples at all)
// ============================================================================

// An empty page has free_space() = FRESH_FREE = 8164.
// compact() must not alter that and must not corrupt the header.
TEST(QA_Page_GDB849, CompactEmptyPage) {
    Page page(1, PageType::DATA);

    // Fresh page free space.
    size_t before = page.free_space();
    EXPECT_EQ(before, FRESH_FREE);

    page.compact();

    size_t after = page.free_space();
    EXPECT_EQ(after, FRESH_FREE) << "compact on empty page must leave free_space unchanged";
    EXPECT_EQ(page.slot_count(), 0u);
    EXPECT_EQ(page.page_id(), 1u);
}

// ============================================================================
// GDB-849: boundary — compact on a full page (zero free space)
// ============================================================================

// Fill the page as much as possible. compact() with no fragmentation must
// leave free_space at 0 (or the same small residual, exactly).
TEST(QA_Page_GDB849, CompactFullPage) {
    Page page(1, PageType::DATA);

    // Fill with 200-byte tuples until we can't fit more.
    int count = 0;
    while (true) {
        auto r = page.insert_tuple(make_bytes(200, static_cast<uint8_t>(count % 256)));
        if (!r.has_value()) {
            break;
        }
        ++count;
    }
    ASSERT_GT(count, 0);

    size_t before = page.free_space();
    page.compact(); // no fragmentation — true no-op
    size_t after = page.free_space();

    EXPECT_EQ(after, before)
        << "compact on a full unfragmented page must leave free_space exactly the same";

    // Verify all tuples still readable.
    for (int i = 0; i < count; ++i) {
        auto t = page.get_tuple(static_cast<SlotId>(i));
        ASSERT_TRUE(t.has_value()) << "slot " << i << " unreadable after compact";
        ASSERT_EQ(t->size(), 200u);
        EXPECT_EQ((*t)[0], static_cast<uint8_t>(i % 256));
    }
}

// ============================================================================
// GDB-849: boundary — compact on a page with exactly one tuple
// ============================================================================

// Single 500-byte tuple, shrunk to 100. compact() reclaims exactly 400 bytes.
// After compact the page still has exactly one live tuple.
TEST(QA_Page_GDB849, CompactSingleTuple) {
    Page page(1, PageType::DATA);

    // Insert single 500-byte tuple.
    auto r = page.insert_tuple(make_bytes(500, 0xAB));
    ASSERT_TRUE(r.has_value());

    // Shrink it to 100 bytes.
    ASSERT_TRUE(page.update_tuple(0, make_bytes(100, 0xCD)).has_value());

    size_t before = page.free_space();
    page.compact();
    size_t after = page.free_space();

    EXPECT_EQ(after, before + 400u)
        << "compact on single shrunk tuple must reclaim exactly 400 bytes";

    EXPECT_EQ(page.slot_count(), 1u);
    auto t = page.get_tuple(0);
    ASSERT_TRUE(t.has_value());
    ASSERT_EQ(t->size(), 100u);
    EXPECT_EQ((*t)[0], 0xCD);
}

// ============================================================================
// GDB-849: free_space monotonicity — compact never reduces free_space
// ============================================================================

// Insert and shrink multiple tuples. free_space after compact must be strictly
// >= free_space before, and must equal before + total_slack exactly.
TEST(QA_Page_GDB849, FreeSpaceMonotonicity) {
    Page page(1, PageType::DATA);

    // Insert 5 tuples of varying sizes.
    ASSERT_TRUE(page.insert_tuple(make_bytes(100, 0x01)).has_value());
    ASSERT_TRUE(page.insert_tuple(make_bytes(200, 0x02)).has_value());
    ASSERT_TRUE(page.insert_tuple(make_bytes(300, 0x03)).has_value());
    ASSERT_TRUE(page.insert_tuple(make_bytes(250, 0x04)).has_value());
    ASSERT_TRUE(page.insert_tuple(make_bytes(175, 0x05)).has_value());

    // Shrink slot 0: 100 → 10, slack = 90
    ASSERT_TRUE(page.update_tuple(0, make_bytes(10, 0x11)).has_value());
    // Shrink slot 2: 300 → 50, slack = 250
    ASSERT_TRUE(page.update_tuple(2, make_bytes(50, 0x33)).has_value());
    // Shrink slot 4: 175 → 75, slack = 100
    ASSERT_TRUE(page.update_tuple(4, make_bytes(75, 0x55)).has_value());

    size_t before = page.free_space();
    page.compact();
    size_t after = page.free_space();

    // Total slack = 90 + 250 + 100 = 440
    EXPECT_EQ(after, before + 440u)
        << "free_space after compact must equal before + 440 (total shrink slack)";
    // And specifically it must not decrease.
    EXPECT_GE(after, before) << "free_space must be monotonically non-decreasing after compact";
}

// ============================================================================
// GDB-849: RID→slot mapping valid after compact (RIDs don't shift)
// ============================================================================

// compact() must preserve RID (slot index) → data mapping. Each slot keeps its
// original slot ID even after the physical data bytes move.
TEST(QA_Page_GDB849, RidSlotMappingValidAfterCompact) {
    Page page(1, PageType::DATA);

    // Insert with unique fill per slot so we can verify which data is at which RID.
    for (int i = 0; i < 6; ++i) {
        auto r = page.insert_tuple(make_bytes(100, static_cast<uint8_t>(i + 1)));
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(*r, static_cast<SlotId>(i));
    }

    // Delete slots 1 and 4 (non-contiguous).
    ASSERT_TRUE(page.delete_tuple(1).has_value());
    ASSERT_TRUE(page.delete_tuple(4).has_value());

    // Shrink slot 2: 100 → 30
    ASSERT_TRUE(page.update_tuple(2, make_bytes(30, 0x22)).has_value());

    page.compact();

    // Slot 0: fill = 0x01, 100 bytes
    {
        auto t = page.get_tuple(0);
        ASSERT_TRUE(t.has_value()) << "slot 0 must survive compact";
        ASSERT_EQ(t->size(), 100u);
        EXPECT_EQ((*t)[0], 0x01u);
    }

    // Slot 1: deleted
    EXPECT_FALSE(page.get_tuple(1).has_value());

    // Slot 2: shrunk, fill = 0x22, 30 bytes
    {
        auto t = page.get_tuple(2);
        ASSERT_TRUE(t.has_value()) << "slot 2 must survive compact";
        ASSERT_EQ(t->size(), 30u);
        EXPECT_EQ((*t)[0], 0x22u);
    }

    // Slot 3: fill = 0x04, 100 bytes
    {
        auto t = page.get_tuple(3);
        ASSERT_TRUE(t.has_value()) << "slot 3 must survive compact";
        ASSERT_EQ(t->size(), 100u);
        EXPECT_EQ((*t)[0], 0x04u);
    }

    // Slot 4: deleted
    EXPECT_FALSE(page.get_tuple(4).has_value());

    // Slot 5: fill = 0x06, 100 bytes
    {
        auto t = page.get_tuple(5);
        ASSERT_TRUE(t.has_value()) << "slot 5 must survive compact";
        ASSERT_EQ(t->size(), 100u);
        EXPECT_EQ((*t)[0], 0x06u);
    }
}

// ============================================================================
// GDB-849: compact reclaims EXACT 400 bytes for the original ticket scenario
// ============================================================================

// This is the direct mutation-grade regression for GDB-849:
// three 500-byte tuples, slot 1 shrunk to 100. Any compact() that skips
// repacking the shrunk slot, or that does nothing at all, produces
// after == before and the EXPECT_EQ will catch it.
TEST(QA_Page_GDB849, OriginalTicketScenarioExact400Reclaimed) {
    Page page(1, PageType::DATA);

    for (int i = 0; i < 3; ++i) {
        auto r = page.insert_tuple(make_bytes(500, static_cast<uint8_t>(i + 1)));
        ASSERT_TRUE(r.has_value()) << "Insert " << i << " failed";
    }

    ASSERT_TRUE(page.update_tuple(1, make_bytes(100, 0xFF)).has_value());

    size_t before = page.free_space();
    page.compact();
    size_t after = page.free_space();

    // old_len(500) - new_len(100) = 400 bytes of slack, reclaimed deterministically.
    EXPECT_EQ(after, before + 400u)
        << "compact must reclaim exactly 400 bytes; a no-op compact reclaims 0 and fails here";

    // All live tuples readable with correct content.
    auto t0 = page.get_tuple(0);
    ASSERT_TRUE(t0.has_value());
    EXPECT_EQ(t0->size(), 500u);
    EXPECT_EQ((*t0)[0], 0x01u);

    auto t1 = page.get_tuple(1);
    ASSERT_TRUE(t1.has_value());
    EXPECT_EQ(t1->size(), 100u);
    EXPECT_EQ((*t1)[0], 0xFFu);

    auto t2 = page.get_tuple(2);
    ASSERT_TRUE(t2.has_value());
    EXPECT_EQ(t2->size(), 500u);
    EXPECT_EQ((*t2)[0], 0x03u);
}

// ============================================================================
// GDB-849: compact a page where all slots are deleted (all-deleted boundary)
// ============================================================================

// All slots deleted means compact() should reset data_offset to page_size and
// free_space should recover to the maximum for N slots (slot directory intact).
TEST(QA_Page_GDB849, CompactAllDeletedSlotsRecovery) {
    Page page(1, PageType::DATA);

    // Insert 4 tuples of 300 bytes.
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(page.insert_tuple(make_bytes(300, static_cast<uint8_t>(i + 1))).has_value());
    }

    // Delete all.
    for (SlotId s = 0; s < 4; ++s) {
        ASSERT_TRUE(page.delete_tuple(s).has_value());
    }

    page.compact();

    // After compacting all-deleted: data_offset should be page_size.
    // free_space() = page_size - slot_directory_end - slot_entry_size
    //              = 8192 - (24 + 4*4) - 4
    //              = 8192 - 40 - 4 = 8148
    size_t expected_free = PAGE - (HEADER + 4 * SLOT) - SLOT;
    EXPECT_EQ(page.free_space(), expected_free)
        << "compact on all-deleted page must restore maximum free space";

    // All slots must still be deleted.
    for (SlotId s = 0; s < 4; ++s) {
        EXPECT_FALSE(page.get_tuple(s).has_value())
            << "slot " << s << " should still be deleted after compact";
    }

    // Must be able to insert fresh tuples after compacting all-deleted.
    auto r = page.insert_tuple(make_bytes(600, 0xEE));
    ASSERT_TRUE(r.has_value()) << "insert after all-deleted compact must succeed";
    auto t = page.get_tuple(*r);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->size(), 600u);
    EXPECT_EQ((*t)[0], 0xEEu);
}

// ============================================================================
// GDB-849: post-compact insert uses reclaimed space (regression for space acct)
// ============================================================================

// After compacting shrink slack, the page must actually be able to accept a
// new insert of exactly the reclaimed size. If free_space accounting is wrong,
// the insert will fail even though the space was declared as reclaimed.
TEST(QA_Page_GDB849, PostCompactInsertUsesReclaimedSpace) {
    Page page(1, PageType::DATA);

    // Insert two 500-byte tuples.
    ASSERT_TRUE(page.insert_tuple(make_bytes(500, 0xAA)).has_value());
    ASSERT_TRUE(page.insert_tuple(make_bytes(500, 0xBB)).has_value());

    // Fill remaining free space with a large tuple, leaving almost no room.
    size_t remaining = page.free_space();
    // Keep room for exactly one more slot (SLOT bytes) — fill the rest.
    size_t filler_size = remaining > SLOT ? remaining - SLOT : 0;
    if (filler_size > 0) {
        ASSERT_TRUE(page.insert_tuple(make_bytes(filler_size, 0xCC)).has_value());
    }

    // Now shrink slot 0: 500 → 100, creating 400 bytes of slack.
    ASSERT_TRUE(page.update_tuple(0, make_bytes(100, 0xDD)).has_value());

    // Page is still "full" by raw accounting (data_offset hasn't moved for shrink).
    // compact() must reclaim the 400 bytes.
    page.compact();

    // After compact, inserting up to 400 bytes must succeed.
    // (Insert a 200-byte tuple to confirm the space is truly available.)
    auto r = page.insert_tuple(make_bytes(200, 0xEE));
    ASSERT_TRUE(r.has_value())
        << "insert of 200 bytes must succeed after compact reclaimed 400 bytes of shrink slack";
    auto t = page.get_tuple(*r);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->size(), 200u);
    EXPECT_EQ((*t)[0], 0xEEu);
}
