/// Unit tests for the MVCC tuple-header storage layout (GDB-714 / GDB-725):
/// TableHeap in MVCC mode transparently prepends/strips MvccTupleHeader,
/// persists xmin/xmax across reopen, and exposes recovery primitives
/// (Page::restore_tuple, TableHeap::restore_raw_tuple / delete_raw_tuple).

#include "sixseven/common/status.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/page.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/txn/mvcc.h"
#include "sixseven/txn/mvcc_tuple.h"
#include "sixseven/txn/txn_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

using namespace sixseven;

namespace {

std::vector<uint8_t> make_bytes(size_t size, uint8_t fill) {
    return std::vector<uint8_t>(size, fill);
}

} // namespace

// =============================================================================
// Fixture
// =============================================================================

class TableHeapMvccTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_test_heap_mvcc.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;
        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
    }

    void TearDown() override {
        bpm_.reset();
        (void)dm_.close_file(file_id_);
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    TableHeap make_mvcc_heap() {
        return TableHeap(*bpm_, dm_, file_id_, TableHeapOptions{.mvcc_headers = true});
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
};

// =============================================================================
// Header stamping and transparency
// =============================================================================

TEST_F(TableHeapMvccTest, InsertStampsExplicitXmin) {
    auto heap = make_mvcc_heap();
    auto rid = heap.insert_tuple(make_bytes(40, 0xAA), /*xmin=*/42);
    ASSERT_TRUE(rid.has_value()) << rid.error().message;

    auto header = heap.get_tuple_header(*rid);
    ASSERT_TRUE(header.has_value()) << header.error().message;
    EXPECT_EQ(header->xmin, 42u);
    EXPECT_EQ(header->xmax, invalid_txn_id);
    EXPECT_FALSE(header->has_next_version());
}

TEST_F(TableHeapMvccTest, InsertDefaultsToFrozenXmin) {
    auto heap = make_mvcc_heap();
    auto rid = heap.insert_tuple(make_bytes(10, 0xBB));
    ASSERT_TRUE(rid.has_value()) << rid.error().message;

    auto header = heap.get_tuple_header(*rid);
    ASSERT_TRUE(header.has_value()) << header.error().message;
    EXPECT_EQ(header->xmin, frozen_txn_id);
}

TEST_F(TableHeapMvccTest, GetTupleReturnsUserBytesOnly) {
    auto heap = make_mvcc_heap();
    auto user = make_bytes(33, 0xCC);
    auto rid = heap.insert_tuple(user);
    ASSERT_TRUE(rid.has_value());

    auto data = heap.get_tuple(*rid);
    ASSERT_TRUE(data.has_value()) << data.error().message;
    EXPECT_EQ(*data, user); // Size and content identical — header invisible.
}

TEST_F(TableHeapMvccTest, OnPageImageContainsHeaderPrefix) {
    auto heap = make_mvcc_heap();
    auto user = make_bytes(16, 0xDD);
    auto rid = heap.insert_tuple(user, /*xmin=*/7);
    ASSERT_TRUE(rid.has_value());

    // Inspect the raw on-page bytes via the Page API.
    auto page_result = bpm_->fetch_page(rid->page_id);
    ASSERT_TRUE(page_result.has_value());
    auto raw = (*page_result)->get_tuple(rid->slot_id);
    ASSERT_TRUE(raw.has_value());
    (void)bpm_->unpin_page(rid->page_id, false);

    ASSERT_EQ(raw->size(), mvcc_header_size + user.size());
    auto header = read_mvcc_header(*raw);
    EXPECT_EQ(header.xmin, 7u);
    EXPECT_EQ(header.xmax, invalid_txn_id);
    EXPECT_EQ(std::vector<uint8_t>(raw->begin() + static_cast<std::ptrdiff_t>(mvcc_header_size),
                                   raw->end()),
              user);
}

TEST_F(TableHeapMvccTest, UpdatePreservesHeaderAndReplacesData) {
    auto heap = make_mvcc_heap();
    auto rid = heap.insert_tuple(make_bytes(20, 0x01), /*xmin=*/9);
    ASSERT_TRUE(rid.has_value());

    auto new_data = make_bytes(50, 0x02);
    ASSERT_TRUE(heap.update_tuple(*rid, new_data).has_value());

    auto data = heap.get_tuple(*rid);
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(*data, new_data);

    auto header = heap.get_tuple_header(*rid);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->xmin, 9u); // Preserved across in-place update.
    EXPECT_EQ(header->xmax, invalid_txn_id);
}

TEST_F(TableHeapMvccTest, BatchInsertStampsAllHeaders) {
    auto heap = make_mvcc_heap();
    std::vector<std::vector<uint8_t>> rows;
    std::vector<std::span<const uint8_t>> spans;
    for (uint8_t i = 0; i < 5; ++i) {
        rows.push_back(make_bytes(30, i));
    }
    for (const auto& r : rows) {
        spans.emplace_back(r);
    }

    auto rids = heap.insert_batch(spans, /*xmin=*/11);
    ASSERT_TRUE(rids.has_value()) << rids.error().message;
    ASSERT_EQ(rids->size(), 5u);

    for (size_t i = 0; i < rids->size(); ++i) {
        auto header = heap.get_tuple_header((*rids)[i]);
        ASSERT_TRUE(header.has_value()) << header.error().message;
        EXPECT_EQ(header->xmin, 11u);

        auto data = heap.get_tuple((*rids)[i]);
        ASSERT_TRUE(data.has_value());
        EXPECT_EQ(*data, rows[i]);
    }
    EXPECT_EQ(heap.row_count(), 5u);
}

TEST_F(TableHeapMvccTest, IteratorStripsHeaders) {
    auto heap = make_mvcc_heap();
    for (uint8_t i = 1; i <= 3; ++i) {
        ASSERT_TRUE(heap.insert_tuple(make_bytes(25, i)).has_value());
    }

    auto it = heap.begin();
    ASSERT_TRUE(it.has_value());

    uint8_t expected = 1;
    for (;;) {
        auto row_result = it->next();
        ASSERT_TRUE(row_result.has_value()) << row_result.error().message;
        if (!row_result->has_value()) {
            break;
        }
        EXPECT_EQ((*row_result)->second, make_bytes(25, expected));
        ++expected;
    }
    EXPECT_EQ(expected, 4);
}

TEST_F(TableHeapMvccTest, DeletePhysicallyRemovesSlot) {
    auto heap = make_mvcc_heap();
    auto rid = heap.insert_tuple(make_bytes(20, 0xE1));
    ASSERT_TRUE(rid.has_value());
    EXPECT_EQ(heap.row_count(), 1u);

    ASSERT_TRUE(heap.delete_tuple(*rid, /*xmax=*/55).has_value());
    EXPECT_EQ(heap.row_count(), 0u);

    auto get = heap.get_tuple(*rid);
    ASSERT_FALSE(get.has_value());
    EXPECT_EQ(get.error().code, StatusCode::NOT_FOUND);

    // Double delete still fails like the legacy layout.
    EXPECT_FALSE(heap.delete_tuple(*rid).has_value());
}

TEST_F(TableHeapMvccTest, MaxUserPayloadShrinksByHeaderSize) {
    auto heap = make_mvcc_heap();

    // Legacy max image = 8164 bytes; the 24-byte header leaves 8140 for user data.
    constexpr size_t max_user = page_size - page_header_size - slot_entry_size - mvcc_header_size;
    auto fits = heap.insert_tuple(make_bytes(max_user, 0x77));
    ASSERT_TRUE(fits.has_value()) << fits.error().message;

    auto too_big = heap.insert_tuple(make_bytes(max_user + 1, 0x88));
    EXPECT_FALSE(too_big.has_value());
}

// GDB-714 review issue 1: a user payload near 64KB pushes the on-page image
// (payload + 24-byte header) past UINT16_MAX; the image length then truncated
// to a tiny value, passed the page space check, and persisted garbage while
// reporting success. The oversize guard in Page::insert_tuple/update_tuple
// must reject these cleanly instead.
TEST_F(TableHeapMvccTest, NearUint16MaxPayloadRejectedCleanly) {
    auto heap = make_mvcc_heap();

    // 65530 + 24 = 65554 -> previously truncated to 18; must now fail cleanly.
    auto oversize = heap.insert_tuple(make_bytes(65530, 0xAB));
    ASSERT_FALSE(oversize.has_value());
    EXPECT_EQ(oversize.error().code, StatusCode::INVALID_ARGUMENT);

    // The exact wrap boundary: image == UINT16_MAX + 1.
    auto wrap = heap.insert_tuple(make_bytes(65536 - mvcc_header_size, 0xCD));
    ASSERT_FALSE(wrap.has_value());
    EXPECT_EQ(wrap.error().code, StatusCode::INVALID_ARGUMENT);

    // The heap must remain fully usable: no phantom rows, normal inserts work.
    EXPECT_EQ(heap.row_count(), 0U);
    auto ok_insert = heap.insert_tuple(make_bytes(64, 0x11));
    ASSERT_TRUE(ok_insert.has_value()) << ok_insert.error().message;
    auto back = heap.get_tuple(*ok_insert);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, make_bytes(64, 0x11));
}

TEST_F(TableHeapMvccTest, NearUint16MaxUpdateRejectedAndOriginalIntact) {
    auto heap = make_mvcc_heap();

    auto rid = heap.insert_tuple(make_bytes(32, 0x22));
    ASSERT_TRUE(rid.has_value()) << rid.error().message;

    auto grown = heap.update_tuple(*rid, make_bytes(65530, 0xEF));
    ASSERT_FALSE(grown.has_value());
    EXPECT_EQ(grown.error().code, StatusCode::INVALID_ARGUMENT);

    // Original tuple bytes untouched by the failed update.
    auto back = heap.get_tuple(*rid);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, make_bytes(32, 0x22));
}

// Page-level guard holds for raw (non-MVCC) images too.
TEST(PageOversizeGuard, InsertAndUpdateRejectImagesBeyondUint16) {
    Page page(1, PageType::DATA);

    auto huge = page.insert_tuple(make_bytes(65536, 0x01));
    ASSERT_FALSE(huge.has_value());
    EXPECT_EQ(huge.error().code, StatusCode::INVALID_ARGUMENT);

    auto slot = page.insert_tuple(make_bytes(16, 0x02));
    ASSERT_TRUE(slot.has_value());
    auto grown = page.update_tuple(*slot, make_bytes(65560, 0x03));
    ASSERT_FALSE(grown.has_value());
    EXPECT_EQ(grown.error().code, StatusCode::INVALID_ARGUMENT);

    auto data = page.get_tuple(*slot);
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(*data, make_bytes(16, 0x02));
}

TEST_F(TableHeapMvccTest, HeadersPersistAcrossReopen) {
    std::vector<RID> rids;
    {
        auto heap = make_mvcc_heap();
        for (uint8_t i = 1; i <= 3; ++i) {
            auto rid = heap.insert_tuple(make_bytes(40, i), /*xmin=*/100 + i);
            ASSERT_TRUE(rid.has_value());
            rids.push_back(*rid);
        }
        ASSERT_TRUE(bpm_->flush_all().has_value());
    }

    // Close and reopen the file with fresh BPM + heap.
    bpm_.reset();
    ASSERT_TRUE(dm_.close_file(file_id_).has_value());
    auto fid = dm_.open_file(path_);
    ASSERT_TRUE(fid.has_value()) << fid.error().message;
    file_id_ = *fid;
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);

    auto heap = make_mvcc_heap();
    EXPECT_EQ(heap.row_count(), 3u);
    for (uint8_t i = 1; i <= 3; ++i) {
        auto header = heap.get_tuple_header(rids[i - 1u]);
        ASSERT_TRUE(header.has_value()) << header.error().message;
        EXPECT_EQ(header->xmin, 100u + i);
        EXPECT_EQ(header->xmax, invalid_txn_id);

        auto data = heap.get_tuple(rids[i - 1u]);
        ASSERT_TRUE(data.has_value());
        EXPECT_EQ(*data, make_bytes(40, i));
    }
}

// =============================================================================
// Legacy (raw) heaps are unchanged
// =============================================================================

TEST_F(TableHeapMvccTest, RawHeapHasNoHeaders) {
    TableHeap heap(*bpm_, dm_, file_id_); // Default options: no MVCC headers.
    auto user = make_bytes(12, 0xF1);
    auto rid = heap.insert_tuple(user);
    ASSERT_TRUE(rid.has_value());

    auto header = heap.get_tuple_header(*rid);
    ASSERT_FALSE(header.has_value());
    EXPECT_EQ(header.error().code, StatusCode::NOT_IMPLEMENTED);

    // On-page bytes are exactly the user bytes (no header prefix).
    auto page_result = bpm_->fetch_page(rid->page_id);
    ASSERT_TRUE(page_result.has_value());
    auto raw = (*page_result)->get_tuple(rid->slot_id);
    ASSERT_TRUE(raw.has_value());
    (void)bpm_->unpin_page(rid->page_id, false);
    EXPECT_EQ(*raw, user);

    EXPECT_FALSE(heap.mvcc_headers());
}

// =============================================================================
// Page::restore_tuple (recovery primitive)
// =============================================================================

TEST(PageRestoreTuple, OverwritesLiveSlot) {
    Page page(1, PageType::DATA);
    auto slot = page.insert_tuple(make_bytes(20, 0x01));
    ASSERT_TRUE(slot.has_value());

    // Shorter data in place.
    ASSERT_TRUE(page.restore_tuple(*slot, make_bytes(10, 0x02)).has_value());
    auto data = page.get_tuple(*slot);
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(*data, make_bytes(10, 0x02));

    // Longer data forces reallocation within the page.
    ASSERT_TRUE(page.restore_tuple(*slot, make_bytes(50, 0x03)).has_value());
    data = page.get_tuple(*slot);
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(*data, make_bytes(50, 0x03));
}

TEST(PageRestoreTuple, RevivesDeletedSlot) {
    Page page(1, PageType::DATA);
    auto slot = page.insert_tuple(make_bytes(20, 0x01));
    ASSERT_TRUE(slot.has_value());
    ASSERT_TRUE(page.delete_tuple(*slot).has_value());
    ASSERT_FALSE(page.is_slot_live(*slot));

    ASSERT_TRUE(page.restore_tuple(*slot, make_bytes(20, 0x04)).has_value());
    EXPECT_TRUE(page.is_slot_live(*slot));
    auto data = page.get_tuple(*slot);
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(*data, make_bytes(20, 0x04));
}

TEST(PageRestoreTuple, ExtendsDirectoryWithGapSlots) {
    Page page(1, PageType::DATA);
    EXPECT_EQ(page.slot_count(), 0u);

    // Restore directly into slot 3 of an empty page.
    ASSERT_TRUE(page.restore_tuple(3, make_bytes(30, 0x05)).has_value());
    EXPECT_EQ(page.slot_count(), 4u);
    EXPECT_TRUE(page.is_slot_live(3));

    // The gap slots are created deleted and can be revived later.
    for (SlotId s = 0; s < 3; ++s) {
        EXPECT_FALSE(page.is_slot_live(s));
    }
    ASSERT_TRUE(page.restore_tuple(1, make_bytes(8, 0x06)).has_value());
    EXPECT_TRUE(page.is_slot_live(1));

    auto data = page.get_tuple(3);
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(*data, make_bytes(30, 0x05));
}

TEST(PageRestoreTuple, RejectsEmptyAndOversized) {
    Page page(1, PageType::DATA);
    EXPECT_FALSE(page.restore_tuple(0, {}).has_value());

    auto oversized = make_bytes(page_size, 0x07);
    auto result = page.restore_tuple(0, oversized);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(PageRestoreTuple, FailsWhenPageFull) {
    Page page(1, PageType::DATA);
    // Fill the page almost completely.
    constexpr size_t big = page_size - page_header_size - slot_entry_size;
    ASSERT_TRUE(page.insert_tuple(make_bytes(big, 0x08)).has_value());

    auto result = page.restore_tuple(5, make_bytes(100, 0x09));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// TableHeap recovery primitives
// =============================================================================

TEST_F(TableHeapMvccTest, RestoreRawTupleAllocatesMissingPages) {
    auto heap = make_mvcc_heap();

    MvccTupleHeader header;
    header.xmin = 21;
    auto image = prepend_mvcc_header(header, make_bytes(40, 0x0A));

    RID rid{3, 5};
    ASSERT_TRUE(heap.restore_raw_tuple(rid, image).has_value());

    auto pc = heap.page_count();
    ASSERT_TRUE(pc.has_value());
    EXPECT_GE(*pc, 3u);

    auto data = heap.get_tuple(rid);
    ASSERT_TRUE(data.has_value()) << data.error().message;
    EXPECT_EQ(*data, make_bytes(40, 0x0A));
    EXPECT_EQ(heap.get_tuple_header(rid)->xmin, 21u);
    EXPECT_EQ(heap.row_count(), 1u);

    // Idempotent: restoring the same image again does not double-count.
    ASSERT_TRUE(heap.restore_raw_tuple(rid, image).has_value());
    EXPECT_EQ(heap.row_count(), 1u);
}

TEST_F(TableHeapMvccTest, DeleteRawTupleIsIdempotent) {
    auto heap = make_mvcc_heap();
    auto rid = heap.insert_tuple(make_bytes(15, 0x0B));
    ASSERT_TRUE(rid.has_value());
    EXPECT_EQ(heap.row_count(), 1u);

    ASSERT_TRUE(heap.delete_raw_tuple(*rid).has_value());
    EXPECT_EQ(heap.row_count(), 0u);

    // Re-deleting, deleting an unknown slot, and deleting on a page that
    // does not exist all succeed as no-ops.
    ASSERT_TRUE(heap.delete_raw_tuple(*rid).has_value());
    ASSERT_TRUE(heap.delete_raw_tuple(RID{rid->page_id, 999}).has_value());
    ASSERT_TRUE(heap.delete_raw_tuple(RID{42, 0}).has_value());
    EXPECT_EQ(heap.row_count(), 0u);
}

// =============================================================================
// Frozen txn id semantics (GDB-714)
// =============================================================================

TEST(FrozenTxnId, StatusIsCommitted) {
    TransactionManager txn_mgr;
    EXPECT_EQ(txn_mgr.get_status(frozen_txn_id), TransactionStatus::COMMITTED);
    // GDB-1242: unknown/unregistered real ids are treated as COMMITTED (the
    // cross-restart compromise), not ABORTED. is_registered() is the correct
    // predicate for "is this xid unknown".
    EXPECT_EQ(txn_mgr.get_status(123456), TransactionStatus::COMMITTED);
    EXPECT_FALSE(txn_mgr.is_registered(123456));
}

TEST(FrozenTxnId, FrozenTupleIsVisibleToFreshSnapshot) {
    TransactionManager txn_mgr;
    auto* txn = txn_mgr.begin().value();

    MvccTupleHeader header;
    header.xmin = frozen_txn_id;
    header.xmax = invalid_txn_id;
    EXPECT_TRUE(is_visible(header, txn->snapshot, txn_mgr, txn->txn_id));
}

TEST(FrozenTxnId, FrozenDeleteIsInvisibleAndDead) {
    TransactionManager txn_mgr;
    auto* txn = txn_mgr.begin().value();

    MvccTupleHeader header;
    header.xmin = frozen_txn_id;
    header.xmax = frozen_txn_id;
    EXPECT_FALSE(is_visible(header, txn->snapshot, txn_mgr, txn->txn_id));
    EXPECT_TRUE(is_dead(header, txn_mgr.xmin_horizon(), txn_mgr));
}

TEST(FrozenTxnId, FrozenLiveTupleIsNotDead) {
    TransactionManager txn_mgr;
    MvccTupleHeader header;
    header.xmin = frozen_txn_id;
    header.xmax = invalid_txn_id;
    EXPECT_FALSE(is_dead(header, txn_mgr.xmin_horizon(), txn_mgr));
}
