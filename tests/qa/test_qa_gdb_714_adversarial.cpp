/// QA adversarial tests for GDB-714: MVCC tuple headers (xmin/xmax) in
/// TableHeap storage (file format v2, per-heap WAL, recovery handler).
///
/// Storage-format changes are data-loss class. These tests attack:
///  - persistence: interleaved DML churn, slot reuse, forced page compaction,
///    flush + close + reopen, byte-exact verification of payloads and headers;
///  - page-capacity boundaries with the 24-byte header overhead (8140 user
///    bytes fits exactly; 8141 must fail cleanly without corruption);
///  - mixed-mode abuse (raw heap over an MVCC file and vice versa — the heap
///    mode is not persisted; behavior must be non-crashing and is documented);
///  - the v1/v2/v3 file-format gate, including truncated/garbage files and
///    the SQL-level StorageManager open path;
///  - WAL recovery: partial-flush convergence, double replay onto a fresh
///    file, interleaved frozen + committed + uncommitted transactions,
///    corrupted/lying/swapped payloads, header-only tuple images;
///  - concurrency smoke: parallel inserts must not produce torn headers;
///  - SQL end-to-end: multi-table churn across engine restart, oversized-row
///    rejection without table damage, ANALYZE / VACUUM on MVCC tables.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/page.h"
#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_recovery.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/table_wal.h"
#include "sixseven/txn/mvcc_tuple.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "test_qa_helpers.h"

using namespace sixseven;

namespace {

std::vector<uint8_t> adv_bytes(size_t size, uint8_t fill) {
    return std::vector<uint8_t>(size, fill);
}

/// Compose a hashable key from a RID.
uint64_t rid_key(RID rid) {
    return (static_cast<uint64_t>(rid.page_id) << 16) | rid.slot_id;
}

/// Maximum user payload for an MVCC heap tuple:
/// 8192 page - 24 page header - 4 slot entry - 24 MVCC header = 8140.
constexpr size_t kMaxMvccUserPayload =
    page_size - page_header_size - slot_entry_size - mvcc_header_size;

/// Maximum on-page image size: 8192 - 24 - 4 = 8164.
constexpr size_t kMaxImageSize = page_size - page_header_size - slot_entry_size;

/// Overwrite the format-version field (header offset 4) of a database file.
/// Version validation precedes checksum verification, so the checksum does
/// not need recomputing.
void patch_file_version(const std::filesystem::path& path, uint32_t version) {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f.is_open());
    f.seekp(static_cast<std::streamoff>(fh_version_offset));
    f.write(reinterpret_cast<const char*>(&version), sizeof(uint32_t));
    ASSERT_TRUE(f.good());
}

/// Expected state of one tuple for model-based verification.
struct ExpectedTuple {
    std::vector<uint8_t> payload;
    txn_id_t xmin = frozen_txn_id;
};

} // namespace

// =============================================================================
// Storage-level adversarial fixture (direct MVCC heap over one file)
// =============================================================================

class QA_GDB714_StorageAdversarial : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb714_adv_heap.db";
        std::error_code ec;
        std::filesystem::remove(path_, ec);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;
        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
        heap_ = std::make_unique<TableHeap>(
            *bpm_, dm_, file_id_, TableHeapOptions{.mvcc_headers = true});
    }

    void TearDown() override {
        heap_.reset();
        bpm_.reset();
        (void)dm_.close_file(file_id_);
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    /// Flush all dirty pages, close the file, and reopen it from disk with a
    /// fresh buffer pool and heap. Anything that survives this round trip is
    /// genuinely persisted (the open path also re-validates the v2 header).
    void flush_close_reopen() {
        ASSERT_TRUE(bpm_->flush_all().has_value());
        heap_.reset();
        bpm_.reset();
        ASSERT_TRUE(dm_.close_file(file_id_).has_value());

        auto fid = dm_.open_file(path_);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;
        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
        heap_ = std::make_unique<TableHeap>(
            *bpm_, dm_, file_id_, TableHeapOptions{.mvcc_headers = true});
    }

    /// Scan the heap and verify it matches the model exactly: same live RIDs,
    /// same user bytes, same xmin, xmax == 0 everywhere.
    void verify_model(const std::unordered_map<uint64_t, ExpectedTuple>& model) {
        auto it = heap_->begin();
        ASSERT_TRUE(it.has_value()) << it.error().message;

        size_t seen = 0;
        for (;;) {
            auto row_result = it->next();
            ASSERT_TRUE(row_result.has_value()) << "scan error: " << row_result.error().message;
            if (!row_result->has_value()) { break; }
            auto& [row_rid, row_data] = **row_result;
            auto key = rid_key(row_rid);
            auto expected = model.find(key);
            ASSERT_NE(expected, model.end())
                << "unexpected live tuple at page " << row_rid.page_id << " slot "
                << row_rid.slot_id;
            EXPECT_EQ(row_data, expected->second.payload)
                << "payload mismatch at page " << row_rid.page_id << " slot "
                << row_rid.slot_id;

            auto header = heap_->get_tuple_header(row_rid);
            ASSERT_TRUE(header.has_value()) << header.error().message;
            EXPECT_EQ(header->xmin, expected->second.xmin)
                << "xmin mismatch at page " << row_rid.page_id << " slot " << row_rid.slot_id;
            EXPECT_EQ(header->xmax, invalid_txn_id);
            ++seen;
        }
        EXPECT_EQ(seen, model.size());
        EXPECT_EQ(heap_->row_count(), model.size());
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<TableHeap> heap_;
};

// -----------------------------------------------------------------------------
// Persistence under churn
// -----------------------------------------------------------------------------

TEST_F(QA_GDB714_StorageAdversarial, ChurnSlotReuseThenReopenVerifiesEveryByte) {
    std::unordered_map<uint64_t, ExpectedTuple> model;
    std::vector<RID> rids;

    // Phase 1: 240 inserts with distinct sizes, fills, and explicit xmins.
    for (uint32_t i = 0; i < 240; ++i) {
        auto payload = adv_bytes(20 + (i % 50), static_cast<uint8_t>(i & 0xFF));
        txn_id_t xmin = 100 + i;
        auto rid = heap_->insert_tuple(payload, xmin);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
        rids.push_back(*rid);
        model[rid_key(*rid)] = {payload, xmin};
    }

    // Phase 2: delete every 3rd tuple.
    for (uint32_t i = 0; i < 240; i += 3) {
        ASSERT_TRUE(heap_->delete_tuple(rids[i]).has_value());
        model.erase(rid_key(rids[i]));
    }

    // Phase 3: grow-update surviving tuples with i % 4 == 1 (may force
    // page compaction); the pre-existing header must be preserved.
    for (uint32_t i = 1; i < 240; i += 4) {
        auto key = rid_key(rids[i]);
        if (model.find(key) == model.end()) {
            continue; // Deleted in phase 2.
        }
        auto payload = adv_bytes(60 + (i % 30), static_cast<uint8_t>(0xE0 ^ (i & 0xFF)));
        ASSERT_TRUE(heap_->update_tuple(rids[i], payload).has_value());
        model[key].payload = payload; // xmin unchanged.
    }

    // Phase 4: 80 fresh inserts (some land in freed space).
    for (uint32_t j = 0; j < 80; ++j) {
        auto payload = adv_bytes(24 + (j % 40), static_cast<uint8_t>(0xA0 + (j & 0x0F)));
        txn_id_t xmin = 1000 + j;
        auto rid = heap_->insert_tuple(payload, xmin);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
        model[rid_key(*rid)] = {payload, xmin};
    }

    verify_model(model);
    flush_close_reopen();
    verify_model(model); // Every byte and every header survived the disk.
}

TEST_F(QA_GDB714_StorageAdversarial, SlotReuseOnSamePageKeepsHeadersDistinct) {
    // Fill page 1 with single-byte tuples (25-byte on-page images).
    std::unordered_map<uint64_t, ExpectedTuple> model;
    std::vector<RID> rids;
    for (uint32_t i = 0; i < 100; ++i) {
        auto payload = adv_bytes(1, static_cast<uint8_t>(i));
        auto rid = heap_->insert_tuple(payload, /*xmin=*/i + 1);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
        ASSERT_EQ(rid->page_id, 1u); // All on the first data page.
        rids.push_back(*rid);
        model[rid_key(*rid)] = {payload, i + 1};
    }

    // Free a band of slots in the middle of the page.
    for (uint32_t i = 10; i < 60; ++i) {
        ASSERT_TRUE(heap_->delete_tuple(rids[i]).has_value());
        model.erase(rid_key(rids[i]));
    }

    // Reinsert: the last-insert hint still points at page 1, so
    // Page::insert_tuple reuses the freed slots — MVCC images included.
    for (uint32_t j = 0; j < 30; ++j) {
        auto payload = adv_bytes(1, static_cast<uint8_t>(0xF0 - j));
        auto rid = heap_->insert_tuple(payload, /*xmin=*/5000 + j);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
        EXPECT_EQ(rid->page_id, 1u);
        EXPECT_GE(rid->slot_id, 10u); // Freed band gets reused first.
        EXPECT_LT(rid->slot_id, 60u);
        model[rid_key(*rid)] = {payload, 5000 + j};
    }

    verify_model(model);
    flush_close_reopen();
    verify_model(model);
}

TEST_F(QA_GDB714_StorageAdversarial, UpdateGrowthForcesCompactWithHeadersPreserved) {
    // Three large tuples nearly fill page 1:
    // images 3024 + 3024 + 2024 = 8072 bytes + 12 slot bytes = 8084 of 8168.
    auto r1 = heap_->insert_tuple(adv_bytes(3000, 0x11), /*xmin=*/1);
    auto r2 = heap_->insert_tuple(adv_bytes(3000, 0x22), /*xmin=*/2);
    auto r3 = heap_->insert_tuple(adv_bytes(2000, 0x33), /*xmin=*/3);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r3.has_value());
    ASSERT_EQ(r3->page_id, r1->page_id);

    // Delete the middle tuple: 3024 bytes free but fragmented.
    ASSERT_TRUE(heap_->delete_tuple(*r2).has_value());

    // Grow r1 to 3060 user bytes (image 3084). Contiguous free space is only
    // ~84 bytes, so this must take the compact-and-retry path inside
    // TableHeap::update_tuple.
    auto grown = adv_bytes(3060, 0x44);
    ASSERT_TRUE(heap_->update_tuple(*r1, grown).has_value());

    auto data1 = heap_->get_tuple(*r1);
    ASSERT_TRUE(data1.has_value()) << data1.error().message;
    EXPECT_EQ(*data1, grown);

    auto h1 = heap_->get_tuple_header(*r1);
    ASSERT_TRUE(h1.has_value());
    EXPECT_EQ(h1->xmin, 1u); // Header survived compaction + relocation.
    EXPECT_EQ(h1->xmax, invalid_txn_id);

    // The untouched neighbor survived compaction byte-for-byte.
    auto data3 = heap_->get_tuple(*r3);
    ASSERT_TRUE(data3.has_value());
    EXPECT_EQ(*data3, adv_bytes(2000, 0x33));
    auto h3 = heap_->get_tuple_header(*r3);
    ASSERT_TRUE(h3.has_value());
    EXPECT_EQ(h3->xmin, 3u);

    flush_close_reopen();
    auto persisted = heap_->get_tuple(*r1);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(*persisted, grown);
}

// -----------------------------------------------------------------------------
// Page-capacity boundaries with the 24-byte header overhead
// -----------------------------------------------------------------------------

TEST_F(QA_GDB714_StorageAdversarial, MaxUserPayloadFitsExactlyAndPersists) {
    // 8140 user bytes -> 8164-byte image + 4-byte slot = exactly 8168 (the
    // whole data region of an empty page).
    auto payload = adv_bytes(kMaxMvccUserPayload, 0x7E);
    auto rid = heap_->insert_tuple(payload, /*xmin=*/77);
    ASSERT_TRUE(rid.has_value()) << rid.error().message;

    // A second max-size tuple cannot share the page.
    auto rid2 = heap_->insert_tuple(payload, /*xmin=*/78);
    ASSERT_TRUE(rid2.has_value()) << rid2.error().message;
    EXPECT_NE(rid2->page_id, rid->page_id);

    flush_close_reopen();

    auto data = heap_->get_tuple(*rid);
    ASSERT_TRUE(data.has_value()) << data.error().message;
    EXPECT_EQ(data->size(), kMaxMvccUserPayload);
    EXPECT_EQ(*data, payload);
    auto header = heap_->get_tuple_header(*rid);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->xmin, 77u);
}

TEST_F(QA_GDB714_StorageAdversarial, OneByteOverMaxRejectedCleanlyHeapUsable) {
    auto oversize = adv_bytes(kMaxMvccUserPayload + 1, 0x5A); // image = 8165
    auto rid = heap_->insert_tuple(oversize);
    ASSERT_FALSE(rid.has_value());
    EXPECT_EQ(rid.error().code, StatusCode::INVALID_ARGUMENT);

    // Batch path pre-validates and reports the offending index.
    std::vector<std::span<const uint8_t>> batch{std::span<const uint8_t>(oversize)};
    auto batch_result = heap_->insert_batch(batch);
    ASSERT_FALSE(batch_result.has_value());
    EXPECT_EQ(batch_result.error().code, StatusCode::INVALID_ARGUMENT);

    // The failed inserts left no live rows and the heap still works.
    EXPECT_EQ(heap_->row_count(), 0u);
    auto ok_rid = heap_->insert_tuple(adv_bytes(16, 0x01));
    ASSERT_TRUE(ok_rid.has_value()) << ok_rid.error().message;
    auto data = heap_->get_tuple(*ok_rid);
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(*data, adv_bytes(16, 0x01));
}

TEST_F(QA_GDB714_StorageAdversarial, OversizeUpdateFailsOriginalAndHeaderIntact) {
    auto original = adv_bytes(128, 0x3C);
    auto rid = heap_->insert_tuple(original, /*xmin=*/55);
    ASSERT_TRUE(rid.has_value());

    auto update = heap_->update_tuple(*rid, adv_bytes(kMaxMvccUserPayload + 1, 0xFF));
    ASSERT_FALSE(update.has_value());
    EXPECT_EQ(update.error().code, StatusCode::INVALID_ARGUMENT);

    auto data = heap_->get_tuple(*rid);
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(*data, original);
    auto header = heap_->get_tuple_header(*rid);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->xmin, 55u);
    EXPECT_EQ(header->xmax, invalid_txn_id);
}

// -----------------------------------------------------------------------------
// Recovery-primitive guard rails
// -----------------------------------------------------------------------------

TEST_F(QA_GDB714_StorageAdversarial, RestoreRawTupleRejectsHeaderPageAndOversize) {
    auto image = adv_bytes(mvcc_header_size + 8, 0x01);

    auto into_header = heap_->restore_raw_tuple(RID{0, 0}, image);
    ASSERT_FALSE(into_header.has_value());
    EXPECT_EQ(into_header.error().code, StatusCode::INVALID_ARGUMENT);

    auto oversize = heap_->restore_raw_tuple(RID{1, 0}, adv_bytes(kMaxImageSize + 1, 0x02));
    ASSERT_FALSE(oversize.has_value());
    EXPECT_EQ(oversize.error().code, StatusCode::INVALID_ARGUMENT);

    // Idempotent no-op delete far beyond the allocated file.
    EXPECT_TRUE(heap_->delete_raw_tuple(RID{9999, 5}).has_value());
    EXPECT_EQ(heap_->row_count(), 0u);
}

// -----------------------------------------------------------------------------
// Mixed-mode abuse: heap mode is not persisted in the file
// -----------------------------------------------------------------------------

TEST_F(QA_GDB714_StorageAdversarial, RawHeapOverMvccFileDoesNotCrash) {
    // Write through the MVCC heap, then misread via a raw (headerless) heap.
    auto user = adv_bytes(2, 0xAB);
    auto rid = heap_->insert_tuple(user, /*xmin=*/7);
    ASSERT_TRUE(rid.has_value());

    TableHeap raw_view(*bpm_, dm_, file_id_); // mvcc_headers = false
    EXPECT_FALSE(raw_view.mvcc_headers());

    // The raw view exposes header bytes as if they were user data: 24 + 2.
    auto raw = raw_view.get_tuple(*rid);
    ASSERT_TRUE(raw.has_value()) << raw.error().message;
    ASSERT_EQ(raw->size(), mvcc_header_size + user.size());
    txn_id_t leaked_xmin = 0;
    std::memcpy(&leaked_xmin, raw->data(), sizeof(txn_id_t));
    EXPECT_EQ(leaked_xmin, 7u); // First 8 bytes are the xmin stamp.

    // Header access through a raw heap is cleanly refused.
    auto header = raw_view.get_tuple_header(*rid);
    ASSERT_FALSE(header.has_value());
    EXPECT_EQ(header.error().code, StatusCode::NOT_IMPLEMENTED);
}

TEST_F(QA_GDB714_StorageAdversarial, MvccHeapOverRawFileDoesNotCrash) {
    // Write through a raw heap, then misread via an MVCC heap.
    TableHeap raw_heap(*bpm_, dm_, file_id_);
    auto small = raw_heap.insert_tuple(adv_bytes(10, 0x10)); // < 24 bytes
    auto large = raw_heap.insert_tuple(adv_bytes(40, 0x20)); // > 24 bytes
    ASSERT_TRUE(small.has_value());
    ASSERT_TRUE(large.has_value());

    TableHeap mvcc_view(*bpm_, dm_, file_id_, TableHeapOptions{.mvcc_headers = true});

    // A raw tuple shorter than the header is rejected as corrupt, not read
    // out of bounds.
    auto too_small = mvcc_view.get_tuple(*small);
    ASSERT_FALSE(too_small.has_value());
    EXPECT_EQ(too_small.error().code, StatusCode::INTERNAL_ERROR);

    // A longer raw tuple is mis-split: 24 garbage bytes are read as the
    // "header". Since GDB-747 the visibility filter sees the garbage xmax
    // (0x2020...) as a deleted version, so the read degrades to NOT_FOUND
    // instead of returning a mis-split payload. Documented hazard — the heap
    // mode is not persisted in the file (accepted follow-up). Must not crash.
    auto missplit = mvcc_view.get_tuple(*large);
    ASSERT_FALSE(missplit.has_value());
    EXPECT_EQ(missplit.error().code, StatusCode::NOT_FOUND);

    // Scanning through the MVCC view must not crash either: the short tuple
    // degrades to an empty payload; the long one is filtered out by the
    // garbage-xmax visibility check.
    auto it = mvcc_view.begin();
    ASSERT_TRUE(it.has_value());
    size_t rows = 0;
    for (;;) {
        auto row_result = it->next();
        ASSERT_TRUE(row_result.has_value()) << "scan error: " << row_result.error().message;
        if (!row_result->has_value()) { break; }
        EXPECT_LE((*row_result)->second.size(), 40u - mvcc_header_size);
        ++rows;
    }
    EXPECT_EQ(rows, 1u);
}

// -----------------------------------------------------------------------------
// Concurrency smoke: parallel inserts must not tear headers or payloads
// -----------------------------------------------------------------------------

TEST_F(QA_GDB714_StorageAdversarial, ConcurrentInsertsProduceNoTornHeaders) {
    // Single-page workload: 1 seed + 120 inserts x 60-byte images = 7320 of
    // 8168 data bytes, so the last-insert-page hint never changes and the
    // pre-existing hint race (GDB-1234, DISABLED_ConcurrentInsertsAcrossPages)
    // is not triggered. This isolates what GDB-714 owns: concurrent writers of
    // MVCC images on one page must be serialized by the page latch with no
    // torn headers or payloads.
    constexpr size_t kThreads = 2;
    constexpr size_t kPerThread = 60;

    auto seed = heap_->insert_tuple(adv_bytes(32, 0x55));
    ASSERT_TRUE(seed.has_value()) << seed.error().message;

    auto worker = [&](uint8_t fill) {
        for (size_t i = 0; i < kPerThread; ++i) {
            auto rid = heap_->insert_tuple(adv_bytes(32, fill));
            EXPECT_TRUE(rid.has_value()) << rid.error().message;
            if (rid) {
                EXPECT_EQ(rid->page_id, 1u);
            }
        }
    };

    std::thread t1(worker, static_cast<uint8_t>(0x11));
    std::thread t2(worker, static_cast<uint8_t>(0x99));
    t1.join();
    t2.join();

    EXPECT_EQ(heap_->row_count(), kThreads * kPerThread + 1);

    auto it = heap_->begin();
    ASSERT_TRUE(it.has_value());
    size_t count_a = 0;
    size_t count_b = 0;
    size_t count_seed = 0;
    for (;;) {
        auto row_result = it->next();
        ASSERT_TRUE(row_result.has_value()) << "scan error: " << row_result.error().message;
        if (!row_result->has_value()) { break; }
        auto& [row_rid, row_data] = **row_result;
        ASSERT_EQ(row_data.size(), 32u);
        // Payload must be uniformly one fill byte — a mix would be a torn write.
        uint8_t first = row_data[0];
        ASSERT_TRUE(first == 0x11 || first == 0x99 || first == 0x55) << static_cast<int>(first);
        for (uint8_t b : row_data) {
            ASSERT_EQ(b, first);
        }
        (first == 0x11 ? count_a : (first == 0x99 ? count_b : count_seed))++;

        auto header = heap_->get_tuple_header(row_rid);
        ASSERT_TRUE(header.has_value()) << header.error().message;
        EXPECT_EQ(header->xmin, frozen_txn_id);
        EXPECT_EQ(header->xmax, invalid_txn_id);
    }
    EXPECT_EQ(count_a, kPerThread);
    EXPECT_EQ(count_b, kPerThread);
    EXPECT_EQ(count_seed, 1u);
}

/// GDB-1234: TableHeap::insert_tuple used to re-read the shared
/// last_insert_page_ hint after the page mutation, so a concurrent insert
/// that advanced the hint made the first thread unpin the WRONG page
/// ("page N is not pinned" error + a leaked pin on the real page) and could
/// return a RID pointing at the wrong page. Multi-page concurrent inserts
/// tripped it readily. Fixed by capturing the hint into a local once per
/// operation (insert_tuple and insert_batch) and using that local
/// consistently for fetch/RID/unpin; last_insert_page_ is now
/// std::atomic<PageId>. Re-enabled as a regression guard.
TEST_F(QA_GDB714_StorageAdversarial, ConcurrentInsertsAcrossPages) {
    constexpr size_t kThreads = 2;
    constexpr size_t kPerThread = 300;

    auto worker = [&](uint8_t fill) {
        for (size_t i = 0; i < kPerThread; ++i) {
            auto rid = heap_->insert_tuple(adv_bytes(32, fill));
            EXPECT_TRUE(rid.has_value()) << rid.error().message;
            if (rid) {
                // The returned RID must actually contain the tuple.
                auto data = heap_->get_tuple(*rid);
                EXPECT_TRUE(data.has_value()) << data.error().message;
            }
        }
    };

    std::thread t1(worker, static_cast<uint8_t>(0x11));
    std::thread t2(worker, static_cast<uint8_t>(0x99));
    t1.join();
    t2.join();

    EXPECT_EQ(heap_->row_count(), kThreads * kPerThread);
}

// =============================================================================
// File format version gate (GDB-730)
// =============================================================================

class QA_GDB714_VersionGate : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb714_adv_gate";
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::filesystem::path dir_;
};

TEST_F(QA_GDB714_VersionGate, FutureVersionRejectedWithUpgradeGuidance) {
    auto path = dir_ / "future_v3.db";
    DiskManager dm;
    auto fid = dm.create_file(path);
    ASSERT_TRUE(fid.has_value());
    ASSERT_TRUE(dm.close_file(*fid).has_value());

    patch_file_version(path, 3);

    auto open_result = dm.open_file(path);
    ASSERT_FALSE(open_result.has_value());
    EXPECT_EQ(open_result.error().code, StatusCode::IO_ERROR);
    EXPECT_NE(open_result.error().message.find("newer"), std::string::npos)
        << open_result.error().message;
    EXPECT_NE(open_result.error().message.find("upgrade the binary"), std::string::npos)
        << open_result.error().message;
}

TEST_F(QA_GDB714_VersionGate, MaxVersionValueRejectedNotCrash) {
    auto path = dir_ / "version_max.db";
    DiskManager dm;
    auto fid = dm.create_file(path);
    ASSERT_TRUE(fid.has_value());
    ASSERT_TRUE(dm.close_file(*fid).has_value());

    patch_file_version(path, 0xFFFFFFFFu);

    auto open_result = dm.open_file(path);
    ASSERT_FALSE(open_result.has_value());
    EXPECT_EQ(open_result.error().code, StatusCode::IO_ERROR);
}

TEST_F(QA_GDB714_VersionGate, TruncatedAndEmptyFilesRejectedNotCrash) {
    DiskManager dm;

    // 100 bytes of garbage — shorter than the header page.
    auto truncated = dir_ / "truncated.db";
    {
        std::ofstream f(truncated, std::ios::binary);
        std::vector<char> junk(100, 0x42);
        f.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }
    auto r1 = dm.open_file(truncated);
    ASSERT_FALSE(r1.has_value());

    // Zero-byte file.
    auto empty = dir_ / "empty.db";
    { std::ofstream f(empty, std::ios::binary); }
    auto r2 = dm.open_file(empty);
    ASSERT_FALSE(r2.has_value());
}

// =============================================================================
// WAL recovery adversarial (per-heap WAL, GDB-736)
// =============================================================================

class QA_GDB714_WalAdversarial : public ::testing::Test {
protected:
    static constexpr uint32_t kTableId = 42;

    void SetUp() override {
        base_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb714_adv_wal";
        std::filesystem::remove_all(base_dir_);
        std::filesystem::create_directories(base_dir_);
        wal_dir_ = base_dir_ / "wal";

        auto fid = dm_.create_file(base_dir_ / "primary.db", false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        primary_fid_ = *fid;
        primary_bpm_ = std::make_unique<BufferPoolManager>(dm_, primary_fid_, 64);
        primary_heap_ = std::make_unique<TableHeap>(
            *primary_bpm_, dm_, primary_fid_, TableHeapOptions{.mvcc_headers = true});

        WalWriterOptions opts;
        opts.enable_group_commit = false;
        wal_ = std::make_unique<WalWriter>(wal_dir_, opts);
        ASSERT_TRUE(wal_->open().has_value());
        primary_heap_->attach_wal(wal_.get(), kTableId);
    }

    void TearDown() override {
        if (wal_) {
            (void)wal_->close();
        }
        recovered_heap_.reset();
        recovered_bpm_.reset();
        primary_heap_.reset();
        primary_bpm_.reset();
        std::error_code ec;
        std::filesystem::remove_all(base_dir_, ec);
    }

    void close_wal() {
        if (!wal_) {
            return;
        }
        ASSERT_TRUE(wal_->flush().has_value());
        ASSERT_TRUE(wal_->close().has_value());
        wal_.reset();
        primary_heap_->attach_wal(nullptr, 0);
    }

    /// Append a transaction-control or hand-crafted record to the WAL.
    void append_record(WalRecordType type,
                       txn_id_t txn,
                       uint32_t page_id = 0,
                       uint16_t slot_id = 0,
                       std::vector<uint8_t> data = {}) {
        WalRecord record;
        record.type = type;
        record.txn_id = txn;
        record.table_id = kTableId;
        record.page_id = page_id;
        record.slot_id = slot_id;
        record.data = std::move(data);
        auto lsn = wal_->append(record);
        ASSERT_TRUE(lsn.has_value()) << lsn.error().message;
    }

    /// Fresh empty recovery target (simulates data pages lost in a crash).
    void make_recovery_target() {
        auto fid = dm_.create_file(base_dir_ / "recovered.db", false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        recovered_fid_ = *fid;
        recovered_bpm_ = std::make_unique<BufferPoolManager>(dm_, recovered_fid_, 64);
        recovered_heap_ = std::make_unique<TableHeap>(
            *recovered_bpm_, dm_, recovered_fid_, TableHeapOptions{.mvcc_headers = true});
    }

    Result<RecoveryStats> run_recovery(TableHeap* target) {
        TableHeapRecoveryHandler handler;
        handler.register_table(kTableId, target);
        WalRecovery recovery(wal_dir_, handler);
        return recovery.recover();
    }

    /// Raw on-page image of a tuple (header + user data), or empty if dead.
    std::vector<uint8_t> raw_image(BufferPoolManager& bpm, RID rid) {
        auto page_result = bpm.fetch_page(rid.page_id);
        if (!page_result) {
            return {};
        }
        auto raw = (*page_result)->get_tuple(rid.slot_id);
        (void)bpm.unpin_page(rid.page_id, false);
        return raw ? std::move(*raw) : std::vector<uint8_t>{};
    }

    std::filesystem::path base_dir_;
    std::filesystem::path wal_dir_;
    DiskManager dm_;
    FileId primary_fid_ = 0;
    FileId recovered_fid_ = 0;
    std::unique_ptr<BufferPoolManager> primary_bpm_;
    std::unique_ptr<BufferPoolManager> recovered_bpm_;
    std::unique_ptr<TableHeap> primary_heap_;
    std::unique_ptr<TableHeap> recovered_heap_;
    std::unique_ptr<WalWriter> wal_;
};

TEST_F(QA_GDB714_WalAdversarial, RedoAfterPartialFlushConvergesToPrimaryState) {
    // Autocommit (frozen) operations: always redone by recovery.
    auto ra = primary_heap_->insert_tuple(adv_bytes(64, 0x11));
    auto rb = primary_heap_->insert_tuple(adv_bytes(72, 0x22));
    auto rc = primary_heap_->insert_tuple(adv_bytes(80, 0x33));
    ASSERT_TRUE(ra.has_value());
    ASSERT_TRUE(rb.has_value());
    ASSERT_TRUE(rc.has_value());
    ASSERT_TRUE(primary_heap_->update_tuple(*rb, adv_bytes(90, 0x44)).has_value());
    ASSERT_TRUE(primary_heap_->delete_tuple(*rc).has_value());

    auto image_a = raw_image(*primary_bpm_, *ra);
    ASSERT_FALSE(image_a.empty());

    close_wal();
    make_recovery_target();

    // Simulate a partial flush: tuple A's page state already reached disk
    // before the crash, B and C's mutations did not.
    ASSERT_TRUE(recovered_heap_->restore_raw_tuple(*ra, image_a).has_value());
    ASSERT_EQ(recovered_heap_->row_count(), 1u);

    auto stats = run_recovery(recovered_heap_.get());
    ASSERT_TRUE(stats.has_value()) << stats.error().message;

    // Convergence: byte-identical to the primary, no double-counted rows.
    EXPECT_EQ(raw_image(*recovered_bpm_, *ra), raw_image(*primary_bpm_, *ra));
    EXPECT_EQ(raw_image(*recovered_bpm_, *rb), raw_image(*primary_bpm_, *rb));
    EXPECT_TRUE(raw_image(*recovered_bpm_, *rc).empty());
    EXPECT_EQ(recovered_heap_->row_count(), 2u);

    auto hb = recovered_heap_->get_tuple_header(*rb);
    ASSERT_TRUE(hb.has_value());
    EXPECT_EQ(hb->xmin, frozen_txn_id); // Update preserved the original header.
    EXPECT_EQ(hb->xmax, invalid_txn_id);
}

TEST_F(QA_GDB714_WalAdversarial, DoubleReplayOntoFreshFileIsIdentical) {
    // Mix committed explicit transactions (with real xmins) and a frozen
    // delete. Explicit-txn DML records are only redone because the COMMIT
    // markers are present in the stream.
    append_record(WalRecordType::BEGIN, 9);
    auto r1 = primary_heap_->insert_tuple(adv_bytes(40, 0x01), /*xmin=*/9);
    ASSERT_TRUE(r1.has_value());
    append_record(WalRecordType::COMMIT, 9);

    append_record(WalRecordType::BEGIN, 10);
    auto r2 = primary_heap_->insert_tuple(adv_bytes(48, 0x02), /*xmin=*/10);
    ASSERT_TRUE(r2.has_value());
    append_record(WalRecordType::COMMIT, 10);

    ASSERT_TRUE(primary_heap_->delete_tuple(*r2, /*xmax=*/frozen_txn_id).has_value());

    close_wal();
    make_recovery_target();

    auto first = run_recovery(recovered_heap_.get());
    ASSERT_TRUE(first.has_value()) << first.error().message;
    auto second = run_recovery(recovered_heap_.get());
    ASSERT_TRUE(second.has_value()) << second.error().message;

    EXPECT_EQ(raw_image(*recovered_bpm_, *r1), raw_image(*primary_bpm_, *r1));
    EXPECT_TRUE(raw_image(*recovered_bpm_, *r2).empty());
    EXPECT_EQ(recovered_heap_->row_count(), 1u); // Not double-counted.

    auto h1 = recovered_heap_->get_tuple_header(*r1);
    ASSERT_TRUE(h1.has_value());
    EXPECT_EQ(h1->xmin, 9u);
    EXPECT_EQ(h1->xmax, invalid_txn_id);
}

TEST_F(QA_GDB714_WalAdversarial, InterleavedFrozenCommittedAndUncommittedTxns) {
    // Frozen autocommit insert.
    auto rf = primary_heap_->insert_tuple(adv_bytes(32, 0xF1));
    ASSERT_TRUE(rf.has_value());

    // Committed explicit transaction 5.
    append_record(WalRecordType::BEGIN, 5);
    auto rc = primary_heap_->insert_tuple(adv_bytes(32, 0xC5), /*xmin=*/5);
    ASSERT_TRUE(rc.has_value());
    append_record(WalRecordType::COMMIT, 5);

    // Uncommitted explicit transaction 6: insert never committed.
    append_record(WalRecordType::BEGIN, 6);
    auto ru = primary_heap_->insert_tuple(adv_bytes(32, 0xA6), /*xmin=*/6);
    ASSERT_TRUE(ru.has_value());

    // Uncommitted explicit transaction 7: deletes the frozen row.
    append_record(WalRecordType::BEGIN, 7);
    ASSERT_TRUE(primary_heap_->delete_tuple(*rf, /*xmax=*/7).has_value());

    close_wal();
    make_recovery_target();

    auto stats = run_recovery(recovered_heap_.get());
    ASSERT_TRUE(stats.has_value()) << stats.error().message;
    EXPECT_GE(stats->records_undone, 2u); // txn 6 insert + txn 7 delete.

    // Frozen row: the uncommitted delete was rolled back with xmax cleared.
    auto hf = recovered_heap_->get_tuple_header(*rf);
    ASSERT_TRUE(hf.has_value()) << hf.error().message;
    EXPECT_EQ(hf->xmin, frozen_txn_id);
    EXPECT_EQ(hf->xmax, invalid_txn_id);
    auto df = recovered_heap_->get_tuple(*rf);
    ASSERT_TRUE(df.has_value());
    EXPECT_EQ(*df, adv_bytes(32, 0xF1));

    // Committed txn 5 row present with its real xmin.
    auto hc = recovered_heap_->get_tuple_header(*rc);
    ASSERT_TRUE(hc.has_value());
    EXPECT_EQ(hc->xmin, 5u);

    // Uncommitted txn 6 row absent.
    EXPECT_FALSE(recovered_heap_->get_tuple(*ru).has_value());

    EXPECT_EQ(recovered_heap_->row_count(), 2u);
}

TEST_F(QA_GDB714_WalAdversarial, GarbagePayloadFailsRecoveryCleanly) {
    append_record(WalRecordType::INSERT, frozen_txn_id, 1, 0, {0x01, 0x02, 0x03});
    close_wal();
    make_recovery_target();

    auto stats = run_recovery(recovered_heap_.get());
    ASSERT_FALSE(stats.has_value());
    EXPECT_EQ(stats.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(recovered_heap_->row_count(), 0u);
}

TEST_F(QA_GDB714_WalAdversarial, LyingLengthFieldFailsRecoveryCleanly) {
    // before_len = 0, after_len = 0xFFFFFF00 with no image bytes following:
    // the length field lies about the payload size.
    std::vector<uint8_t> lying(2 * sizeof(uint32_t), 0);
    uint32_t huge = 0xFFFFFF00u;
    std::memcpy(lying.data() + sizeof(uint32_t), &huge, sizeof(uint32_t));

    append_record(WalRecordType::INSERT, frozen_txn_id, 1, 0, std::move(lying));
    close_wal();
    make_recovery_target();

    auto stats = run_recovery(recovered_heap_.get());
    ASSERT_FALSE(stats.has_value());
    EXPECT_EQ(stats.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(recovered_heap_->row_count(), 0u);
}

TEST_F(QA_GDB714_WalAdversarial, SwappedImagesOnInsertRejected) {
    // An INSERT record whose payload has the images swapped (before filled,
    // after empty) must be rejected — redo requires an after-image.
    auto image = adv_bytes(mvcc_header_size + 16, 0x77);
    append_record(
        WalRecordType::INSERT, frozen_txn_id, 1, 0, serialize_table_wal_payload(image, {}));
    close_wal();
    make_recovery_target();

    auto stats = run_recovery(recovered_heap_.get());
    ASSERT_FALSE(stats.has_value());
    EXPECT_EQ(stats.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(stats.error().message.find("after-image"), std::string::npos)
        << stats.error().message;
}

TEST_F(QA_GDB714_WalAdversarial, HeaderOnlyImageRedoDoesNotCrashReads) {
    // A 24-byte image (MVCC header, zero user bytes) cannot be produced by
    // TableHeap::insert_tuple, but a WAL stream could carry one. Redo accepts
    // it; reads must degrade cleanly instead of crashing.
    MvccTupleHeader header;
    header.xmin = 9;
    header.xmax = invalid_txn_id;
    std::vector<uint8_t> image(mvcc_header_size);
    serialize_mvcc_header(header, image.data());

    append_record(
        WalRecordType::INSERT, frozen_txn_id, 1, 0, serialize_table_wal_payload({}, image));
    close_wal();
    make_recovery_target();

    auto stats = run_recovery(recovered_heap_.get());
    ASSERT_TRUE(stats.has_value()) << stats.error().message;
    EXPECT_EQ(recovered_heap_->row_count(), 1u);

    RID rid{1, 0};
    auto data = recovered_heap_->get_tuple(rid);
    ASSERT_FALSE(data.has_value()); // "smaller than MVCC header" guard.
    EXPECT_EQ(data.error().code, StatusCode::INTERNAL_ERROR);

    auto hdr = recovered_heap_->get_tuple_header(rid);
    ASSERT_TRUE(hdr.has_value()) << hdr.error().message;
    EXPECT_EQ(hdr->xmin, 9u);

    // Scan degrades to an empty payload for the header-only tuple.
    auto it = recovered_heap_->begin();
    ASSERT_TRUE(it.has_value());
    auto row = it->next();
    ASSERT_TRUE(row.has_value()) << "next() error: " << row.error().message;
    ASSERT_TRUE(row->has_value()) << "expected a row, got end-of-scan";
    EXPECT_TRUE((**row).second.empty());
    auto end = it->next();
    ASSERT_TRUE(end.has_value()) << "next() error: " << end.error().message;
    EXPECT_FALSE(end->has_value());
}

// =============================================================================
// SQL end-to-end adversarial (StorageManager-backed MVCC tables)
// =============================================================================

class QA_GDB714_SqlAdversarial : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb714_adv_sql";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::error_code ec;
        std::filesystem::remove_all(data_dir_, ec);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value())
            << "exec failed for: " << sql
            << " :: " << (result ? std::string{} : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    /// Restart engine + storage manager (flushes everything) and reopen the
    /// given tables from their persisted v2 files.
    void restart_and_reopen(const std::vector<std::string>& tables) {
        engine_.reset();
        storage_.reset();
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
        for (const auto& name : tables) {
            auto schema = catalog_.get_table(default_database_id, name);
            ASSERT_TRUE(schema.has_value()) << name;
            ASSERT_TRUE(storage_->open_table_storage(default_database_id, schema->table_id, *schema)
                            .has_value())
                << name;
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::filesystem::path data_dir_;
};

TEST_F(QA_GDB714_SqlAdversarial, MultiTableInterleavedDmlSurvivesRestart) {
    exec_ok("CREATE TABLE adv_a (id INT, v VARCHAR)");
    exec_ok("CREATE TABLE adv_b (id INT, v VARCHAR)");
    exec_ok("CREATE TABLE adv_c (id INT, v VARCHAR)");

    // Interleave inserts across the three tables.
    for (int i = 0; i < 30; ++i) {
        std::string table = (i % 3 == 0) ? "adv_a" : (i % 3 == 1) ? "adv_b" : "adv_c";
        exec_ok("INSERT INTO " + table + " VALUES (" + std::to_string(i) + ", 'row-" +
                std::to_string(i) + "')");
    }

    exec_ok("UPDATE adv_a SET v = 'patched' WHERE id = 12");
    exec_ok("DELETE FROM adv_b WHERE id = 4");
    exec_ok("DELETE FROM adv_b WHERE id = 16");
    exec_ok("INSERT INTO adv_b VALUES (100, 'reborn')");

    restart_and_reopen({"adv_a", "adv_b", "adv_c"});

    auto qa = exec_ok("SELECT id, v FROM adv_a ORDER BY id");
    ASSERT_EQ(qa.rows.size(), 10u);
    auto qa_patched = exec_ok("SELECT v FROM adv_a WHERE id = 12");
    ASSERT_EQ(qa_patched.rows.size(), 1u);
    EXPECT_EQ(qa_patched.rows[0][0].as_string(), "patched");

    auto qb = exec_ok("SELECT id FROM adv_b ORDER BY id");
    ASSERT_EQ(qb.rows.size(), 9u); // 10 - 2 deleted + 1 inserted.
    EXPECT_EQ(qb.rows[8][0].as_int32(), 100);

    auto qc = exec_ok("SELECT id FROM adv_c");
    EXPECT_EQ(qc.rows.size(), 10u);
}

TEST_F(QA_GDB714_SqlAdversarial, NearPageMaxRowRoundTripsOversizedRejected) {
    exec_ok("CREATE TABLE adv_wide (id INT, payload VARCHAR)");

    // ~8000 user bytes fits comfortably under the 8140-byte MVCC-mode cap.
    std::string big(8000, 'x');
    exec_ok("INSERT INTO adv_wide VALUES (1, '" + big + "')");

    // An oversized row must fail cleanly, not corrupt the page.
    std::string oversized(8200, 'y');
    auto fail = engine_->execute("INSERT INTO adv_wide VALUES (2, '" + oversized + "')");
    ASSERT_FALSE(fail.has_value()) << "8200-byte row unexpectedly fit in an 8K page";

    // Table unharmed: the wide row is intact and new inserts work.
    auto qr = exec_ok("SELECT id, payload FROM adv_wide ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), big);

    exec_ok("INSERT INTO adv_wide VALUES (3, 'small')");
    auto qr2 = exec_ok("SELECT id FROM adv_wide ORDER BY id");
    ASSERT_EQ(qr2.rows.size(), 2u);
    EXPECT_EQ(qr2.rows[1][0].as_int32(), 3);

    // The wide row also survives a restart.
    restart_and_reopen({"adv_wide"});
    auto qr3 = exec_ok("SELECT payload FROM adv_wide WHERE id = 1");
    ASSERT_EQ(qr3.rows.size(), 1u);
    EXPECT_EQ(qr3.rows[0][0].as_string(), big);
}

TEST_F(QA_GDB714_SqlAdversarial, AnalyzeAndVacuumOnMvccTablesLeaveDataIntact) {
    exec_ok("CREATE TABLE adv_stats (id INT, name VARCHAR, qty INT)");
    for (int i = 0; i < 50; ++i) {
        exec_ok("INSERT INTO adv_stats VALUES (" + std::to_string(i) + ", 'name-" +
                std::to_string(i) + "', " + std::to_string(i * 10) + ")");
    }
    exec_ok("DELETE FROM adv_stats WHERE id = 7");

    // ANALYZE scans MVCC tuples through the header-stripping iterator; it
    // must not misread header bytes as row data or fail on the v2 layout.
    exec_ok("ANALYZE adv_stats");
    exec_ok("ANALYZE");

    // VACUUM stays a validated no-op on MVCC tables (GDB-1230 / GDB-711).
    exec_ok("VACUUM adv_stats");

    auto qr = exec_ok("SELECT id, name, qty FROM adv_stats ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 49u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "name-0");
    EXPECT_EQ(qr.rows[48][2].as_int32(), 490);
}

TEST_F(QA_GDB714_SqlAdversarial, V1TableFileRejectedAtStorageManagerOpen) {
    exec_ok("CREATE TABLE adv_legacy (id INT)");
    exec_ok("INSERT INTO adv_legacy VALUES (1)");

    auto schema = catalog_.get_table(default_database_id, "adv_legacy");
    ASSERT_TRUE(schema.has_value());
    auto table_id = schema->table_id;

    auto file_path = data_dir_ / "databases" / std::to_string(default_database_id) / "tables" /
                     ("table_" + std::to_string(table_id) + ".db");

    // Close everything so the file is flushed and unlocked, then stamp the
    // legacy version into the header.
    engine_.reset();
    storage_.reset();
    ASSERT_TRUE(std::filesystem::exists(file_path)) << file_path.string();
    patch_file_version(file_path, 1);

    storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
    engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);

    auto opened = storage_->open_table_storage(default_database_id, table_id, *schema);
    ASSERT_FALSE(opened.has_value());
    EXPECT_NE(opened.error().message.find("rebuild required"), std::string::npos)
        << opened.error().message;
}
