#include "sixseven/storage/page.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <vector>

using namespace sixseven;

// -- Helper: create a tuple of N bytes filled with a pattern byte -----------

static std::vector<uint8_t> make_tuple(size_t len, uint8_t fill = 0xAA) {
    return std::vector<uint8_t>(len, fill);
}

// -- Page construction --------------------------------------------------------

TEST(Page, Construction) {
    Page page(42, PageType::DATA);
    EXPECT_EQ(page.page_id(), 42u);
    EXPECT_EQ(page.page_type(), PageType::DATA);
    EXPECT_EQ(page.slot_count(), 0u);
    EXPECT_EQ(page.lsn(), 0u);
    EXPECT_EQ(page.checksum(), 0u);
}

TEST(Page, PageTypeEnum) {
    Page page(0, PageType::BTREE_LEAF);
    EXPECT_EQ(page.page_type(), PageType::BTREE_LEAF);

    page.set_page_type(PageType::OVERFLOW_PAGE);
    EXPECT_EQ(page.page_type(), PageType::OVERFLOW_PAGE);
}

TEST(Page, HeaderSetters) {
    Page page(0, PageType::DATA);
    page.set_page_id(100);
    EXPECT_EQ(page.page_id(), 100u);

    page.set_lsn(12345678ULL);
    EXPECT_EQ(page.lsn(), 12345678ULL);

    page.set_checksum(0xDEADBEEF);
    EXPECT_EQ(page.checksum(), 0xDEADBEEF);
}

TEST(Page, ConstructFromRawBytes) {
    Page original(99, PageType::BTREE_INTERNAL);
    original.set_lsn(42);
    original.set_checksum(123);

    auto tuple = make_tuple(10, 0xBB);
    auto result = original.insert_tuple(tuple);
    ASSERT_TRUE(result.has_value());

    // Reconstruct from raw bytes.
    Page copy(original.raw());
    EXPECT_EQ(copy.page_id(), 99u);
    EXPECT_EQ(copy.page_type(), PageType::BTREE_INTERNAL);
    EXPECT_EQ(copy.slot_count(), 1u);
    EXPECT_EQ(copy.lsn(), 42u);
    EXPECT_EQ(copy.checksum(), 123u);

    auto read = copy.get_tuple(0);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->size(), 10u);
    EXPECT_EQ((*read)[0], 0xBB);
}

// -- Insert and get -----------------------------------------------------------

TEST(Page, InsertAndGetTuple) {
    Page page(0, PageType::DATA);
    auto tuple = make_tuple(100, 0x42);

    auto result = page.insert_tuple(tuple);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0u); // First slot

    EXPECT_EQ(page.slot_count(), 1u);

    auto read = page.get_tuple(0);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->size(), 100u);
    for (uint8_t b : *read) {
        EXPECT_EQ(b, 0x42);
    }
}

TEST(Page, InsertMultipleTuples) {
    Page page(0, PageType::DATA);

    for (int i = 0; i < 10; ++i) {
        auto tuple = make_tuple(50, static_cast<uint8_t>(i));
        auto result = page.insert_tuple(tuple);
        ASSERT_TRUE(result.has_value()) << "Failed to insert tuple " << i;
        EXPECT_EQ(*result, static_cast<SlotId>(i));
    }

    EXPECT_EQ(page.slot_count(), 10u);

    // Verify all tuples are readable.
    for (int i = 0; i < 10; ++i) {
        auto read = page.get_tuple(static_cast<SlotId>(i));
        ASSERT_TRUE(read.has_value());
        EXPECT_EQ(read->size(), 50u);
        EXPECT_EQ((*read)[0], static_cast<uint8_t>(i));
    }
}

TEST(Page, InsertUntilFull) {
    Page page(0, PageType::DATA);
    int count = 0;

    // Insert 100-byte tuples until the page is full.
    while (true) {
        auto tuple = make_tuple(100);
        auto result = page.insert_tuple(tuple);
        if (!result.has_value()) {
            break;
        }
        ++count;
    }

    EXPECT_GT(count, 0);
    // Verify all inserted tuples are still readable.
    for (int i = 0; i < count; ++i) {
        auto read = page.get_tuple(static_cast<SlotId>(i));
        ASSERT_TRUE(read.has_value());
        EXPECT_EQ(read->size(), 100u);
    }
}

TEST(Page, InsertEmptyTupleFails) {
    Page page(0, PageType::DATA);
    std::vector<uint8_t> empty;
    auto result = page.insert_tuple(empty);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// -- Get errors ---------------------------------------------------------------

TEST(Page, GetInvalidSlotId) {
    Page page(0, PageType::DATA);
    auto result = page.get_tuple(0);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(Page, GetDeletedSlot) {
    Page page(0, PageType::DATA);
    auto tuple = make_tuple(10);
    auto insert_result = page.insert_tuple(tuple);
    ASSERT_TRUE(insert_result.has_value());

    auto del_result = page.delete_tuple(0);
    ASSERT_TRUE(del_result.has_value());

    auto get_result = page.get_tuple(0);
    EXPECT_FALSE(get_result.has_value());
    EXPECT_EQ(get_result.error().code, StatusCode::NOT_FOUND);
}

// -- Delete -------------------------------------------------------------------

TEST(Page, DeleteTuple) {
    Page page(0, PageType::DATA);
    auto t1 = make_tuple(20, 0x11);
    auto t2 = make_tuple(30, 0x22);
    auto t3 = make_tuple(40, 0x33);

    ASSERT_TRUE(page.insert_tuple(t1).has_value());
    ASSERT_TRUE(page.insert_tuple(t2).has_value());
    ASSERT_TRUE(page.insert_tuple(t3).has_value());

    // Delete the middle tuple.
    auto result = page.delete_tuple(1);
    ASSERT_TRUE(result.has_value());

    // Slot 1 is deleted.
    auto get1 = page.get_tuple(1);
    EXPECT_FALSE(get1.has_value());

    // Slots 0 and 2 still valid.
    auto get0 = page.get_tuple(0);
    ASSERT_TRUE(get0.has_value());
    EXPECT_EQ((*get0)[0], 0x11);

    auto get2 = page.get_tuple(2);
    ASSERT_TRUE(get2.has_value());
    EXPECT_EQ((*get2)[0], 0x33);
}

TEST(Page, DeleteInvalidSlotId) {
    Page page(0, PageType::DATA);
    auto result = page.delete_tuple(0);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(Page, DeleteAlreadyDeletedSlot) {
    Page page(0, PageType::DATA);
    ASSERT_TRUE(page.insert_tuple(make_tuple(10)).has_value());
    ASSERT_TRUE(page.delete_tuple(0).has_value());

    auto result = page.delete_tuple(0);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// -- Update -------------------------------------------------------------------

TEST(Page, UpdateInPlace) {
    Page page(0, PageType::DATA);
    auto t1 = make_tuple(50, 0x11);
    ASSERT_TRUE(page.insert_tuple(t1).has_value());

    // Update with same size — should update in place.
    auto t2 = make_tuple(50, 0x22);
    auto result = page.update_tuple(0, t2);
    ASSERT_TRUE(result.has_value());

    auto read = page.get_tuple(0);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->size(), 50u);
    EXPECT_EQ((*read)[0], 0x22);
}

TEST(Page, UpdateShrink) {
    Page page(0, PageType::DATA);
    auto t1 = make_tuple(100, 0x11);
    ASSERT_TRUE(page.insert_tuple(t1).has_value());

    // Update with smaller data.
    auto t2 = make_tuple(30, 0x22);
    auto result = page.update_tuple(0, t2);
    ASSERT_TRUE(result.has_value());

    auto read = page.get_tuple(0);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->size(), 30u);
    EXPECT_EQ((*read)[0], 0x22);
}

TEST(Page, UpdateGrow) {
    Page page(0, PageType::DATA);
    auto t1 = make_tuple(30, 0x11);
    ASSERT_TRUE(page.insert_tuple(t1).has_value());

    // Update with larger data — triggers delete+insert.
    auto t2 = make_tuple(100, 0x22);
    auto result = page.update_tuple(0, t2);
    ASSERT_TRUE(result.has_value());

    auto read = page.get_tuple(0);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->size(), 100u);
    EXPECT_EQ((*read)[0], 0x22);
}

TEST(Page, UpdateDeletedSlotFails) {
    Page page(0, PageType::DATA);
    ASSERT_TRUE(page.insert_tuple(make_tuple(10)).has_value());
    ASSERT_TRUE(page.delete_tuple(0).has_value());

    auto result = page.update_tuple(0, make_tuple(10));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(Page, UpdateEmptyDataFails) {
    Page page(0, PageType::DATA);
    ASSERT_TRUE(page.insert_tuple(make_tuple(10)).has_value());

    auto result = page.update_tuple(0, {});
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(Page, UpdateInvalidSlotFails) {
    Page page(0, PageType::DATA);
    auto result = page.update_tuple(0, make_tuple(10));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// -- Free space ---------------------------------------------------------------

TEST(Page, FreeSpaceNewPage) {
    Page page(0, PageType::DATA);
    // Total usable = page_size - page_header_size = 8168
    // free_space() reserves slot_entry_size for a new entry.
    EXPECT_EQ(page.free_space(), page_size - page_header_size - slot_entry_size);
}

TEST(Page, FreeSpaceDecreases) {
    Page page(0, PageType::DATA);
    size_t initial = page.free_space();

    auto tuple = make_tuple(100);
    ASSERT_TRUE(page.insert_tuple(tuple).has_value());

    // After insert: lost 100 bytes (tuple) + 4 bytes (slot entry).
    // free_space() also reserves 4 for next slot entry.
    size_t after = page.free_space();
    EXPECT_EQ(after, initial - 100 - slot_entry_size);
}

// -- Compaction ---------------------------------------------------------------

TEST(Page, CompactionRecoversFreeSpace) {
    Page page(0, PageType::DATA);

    // Insert 3 tuples.
    ASSERT_TRUE(page.insert_tuple(make_tuple(100, 0x11)).has_value());
    ASSERT_TRUE(page.insert_tuple(make_tuple(100, 0x22)).has_value());
    ASSERT_TRUE(page.insert_tuple(make_tuple(100, 0x33)).has_value());

    size_t before_delete = page.free_space();

    // Delete middle tuple.
    ASSERT_TRUE(page.delete_tuple(1).has_value());

    // free_space() doesn't increase after delete (fragmented).
    size_t after_delete = page.free_space();
    EXPECT_EQ(after_delete, before_delete); // Same: gap isn't reclaimed yet.

    // Compact.
    page.compact();

    size_t after_compact = page.free_space();
    // After compaction, we should have recovered the 100 bytes from tuple 1.
    EXPECT_GT(after_compact, after_delete);
    EXPECT_EQ(after_compact, before_delete + 100);

    // Verify remaining tuples are intact.
    auto get0 = page.get_tuple(0);
    ASSERT_TRUE(get0.has_value());
    EXPECT_EQ(get0->size(), 100u);
    EXPECT_EQ((*get0)[0], 0x11);

    auto get2 = page.get_tuple(2);
    ASSERT_TRUE(get2.has_value());
    EXPECT_EQ(get2->size(), 100u);
    EXPECT_EQ((*get2)[0], 0x33);
}

TEST(Page, CompactionEmptyPage) {
    Page page(0, PageType::DATA);
    page.compact(); // Should not crash.
    EXPECT_EQ(page.free_space(), page_size - page_header_size - slot_entry_size);
}

TEST(Page, CompactionAllDeleted) {
    Page page(0, PageType::DATA);
    ASSERT_TRUE(page.insert_tuple(make_tuple(100, 0x11)).has_value());
    ASSERT_TRUE(page.insert_tuple(make_tuple(100, 0x22)).has_value());
    ASSERT_TRUE(page.delete_tuple(0).has_value());
    ASSERT_TRUE(page.delete_tuple(1).has_value());

    page.compact();

    // Slot count is still 2 (deleted slots remain in directory),
    // but data space is fully reclaimed.
    EXPECT_EQ(page.slot_count(), 2u);
}

// -- Slot reuse ---------------------------------------------------------------

TEST(Page, DeletedSlotReuse) {
    Page page(0, PageType::DATA);

    ASSERT_TRUE(page.insert_tuple(make_tuple(20, 0x11)).has_value()); // slot 0
    ASSERT_TRUE(page.insert_tuple(make_tuple(20, 0x22)).has_value()); // slot 1
    ASSERT_TRUE(page.insert_tuple(make_tuple(20, 0x33)).has_value()); // slot 2

    ASSERT_TRUE(page.delete_tuple(1).has_value()); // delete slot 1

    // Inserting should reuse slot 1.
    auto result = page.insert_tuple(make_tuple(20, 0x44));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1u);

    auto read = page.get_tuple(1);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ((*read)[0], 0x44);
}

// -- Varied tuple sizes -------------------------------------------------------

TEST(Page, VariedTupleSizes) {
    Page page(0, PageType::DATA);

    auto t1 = make_tuple(1, 0x01);    // minimum
    auto t2 = make_tuple(500, 0x02);  // medium
    auto t3 = make_tuple(2000, 0x03); // large
    auto t4 = make_tuple(10, 0x04);   // small

    ASSERT_TRUE(page.insert_tuple(t1).has_value());
    ASSERT_TRUE(page.insert_tuple(t2).has_value());
    ASSERT_TRUE(page.insert_tuple(t3).has_value());
    ASSERT_TRUE(page.insert_tuple(t4).has_value());

    for (SlotId i = 0; i < 4; ++i) {
        auto read = page.get_tuple(i);
        ASSERT_TRUE(read.has_value());
        EXPECT_EQ((*read)[0], static_cast<uint8_t>(i + 1));
    }

    EXPECT_EQ(page.get_tuple(0)->size(), 1u);
    EXPECT_EQ(page.get_tuple(1)->size(), 500u);
    EXPECT_EQ(page.get_tuple(2)->size(), 2000u);
    EXPECT_EQ(page.get_tuple(3)->size(), 10u);
}

// -- Large tuple that nearly fills the page -----------------------------------

TEST(Page, LargeTupleNearlyFillsPage) {
    Page page(0, PageType::DATA);
    size_t max_tuple = page.free_space();

    auto big = make_tuple(max_tuple, 0xFF);
    auto result = page.insert_tuple(big);
    ASSERT_TRUE(result.has_value());

    auto read = page.get_tuple(0);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->size(), max_tuple);

    // No more space for another tuple.
    auto result2 = page.insert_tuple(make_tuple(1));
    EXPECT_FALSE(result2.has_value());
}

// -- Compaction then insert ---------------------------------------------------

TEST(Page, CompactThenInsert) {
    Page page(0, PageType::DATA);

    // Fill with several tuples.
    for (int i = 0; i < 20; ++i) {
        auto r = page.insert_tuple(make_tuple(200, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << "Insert " << i << " failed";
    }

    // Delete half of them.
    for (int i = 0; i < 20; i += 2) {
        ASSERT_TRUE(page.delete_tuple(static_cast<SlotId>(i)).has_value());
    }

    // Compact to reclaim space.
    page.compact();

    // Insert more tuples into reclaimed space.
    int inserted = 0;
    for (int i = 0; i < 10; ++i) {
        auto r = page.insert_tuple(make_tuple(200, 0xEE));
        if (r.has_value()) {
            ++inserted;
        }
    }
    EXPECT_GT(inserted, 0);
}
