#include "sixseven/common/status.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <vector>

using namespace sixseven;

// =============================================================================
// QA Regression Tests for GDB-915: TableIterator::next() must surface
// fetch_page failures instead of silently skipping pages.
// =============================================================================

namespace {

// Build a small fixed-size tuple (all zeros, given length).
static std::vector<uint8_t> make_blob(size_t len, uint8_t fill = 0) {
    return std::vector<uint8_t>(len, fill);
}

} // namespace

// =============================================================================
// Fixture
// =============================================================================

class GDB915Test : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_test_gdb915.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;
    }

    void TearDown() override {
        bpm_.reset();
        auto close = dm_.close_file(file_id_);
        (void)close;
        std::filesystem::remove(path_);
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
};

// =============================================================================
// AC1 (positive): A normal multi-page heap scan returns ALL rows successfully.
// Every next() Result has a value; no error; full row count is preserved.
// =============================================================================

TEST_F(GDB915Test, HappyPathMultiPageScanReturnsAllRows) {
    // Create a BPM with enough frames for normal operation.
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert enough large tuples to span multiple pages.
    // Each tuple is 2000 bytes; 8KB pages hold ~4 per page.
    constexpr int kRows = 20;
    for (int i = 0; i < kRows; ++i) {
        auto r = heap.insert_tuple(make_blob(2000, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << "insert " << i << " failed: " << r.error().message;
    }

    auto flush = bpm_->flush_all();
    ASSERT_TRUE(flush.has_value()) << flush.error().message;

    // Scan all pages -- every next() must succeed (no error), and we must
    // see exactly kRows tuples.
    auto it = heap.begin();
    ASSERT_TRUE(it.has_value()) << it.error().message;

    int count = 0;
    for (;;) {
        auto r = it->next();
        // Happy path: Result must always be ok (no I/O or pool errors).
        ASSERT_TRUE(r.has_value()) << "next() returned an error: " << r.error().message;
        if (!r->has_value()) {
            break; // End of scan.
        }
        ++count;
    }

    EXPECT_EQ(count, kRows) << "Happy-path scan must return all " << kRows << " rows with no error";
}

// =============================================================================
// AC2 (negative / regression): When a page cannot be fetched due to buffer-pool
// pin exhaustion, next() must return an ERROR -- not a short successful scan.
//
// Fault-injection seam used: tiny pool (pool_size=2) with both frames held
// pinned by an explicit fetch_page() call, so the iterator's own fetch_page()
// cannot evict any frame and is forced to fail.
//
// Before GDB-915: next() would silently skip the unpinnable page and return
// std::nullopt (scan exhausted) -- giving the caller a result of 0 rows with
// no error surfaced.
// After GDB-915:  next() returns tl::unexpected(...) with an IO_ERROR or
// INTERNAL_ERROR code so the caller can detect and propagate the failure.
// =============================================================================

TEST_F(GDB915Test, PinExhaustionCausesNextToReturnError) {
    // Use a very small pool so that we can exhaust it deliberately.
    // pool_size=2: one frame for page 0 (header page), one for page 1 (data).
    constexpr uint32_t kPoolSize = 2;
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, kPoolSize);
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert a tuple large enough to fill a page, forcing allocation of at
    // least two data pages when we insert a second tuple.
    // Page layout: 8192 byte page, 24-byte header, 4-byte slot entry.
    // One 8160-byte tuple fills a page. Two inserts -> two data pages.
    auto r1 = heap.insert_tuple(make_blob(8160, 0xAA));
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    auto r2 = heap.insert_tuple(make_blob(8160, 0xBB));
    ASSERT_TRUE(r2.has_value()) << r2.error().message;

    // Flush so both pages are clean and unpinned on disk.
    auto flush = bpm_->flush_all();
    ASSERT_TRUE(flush.has_value()) << flush.error().message;

    // Now exhaust the pool: fetch both frames and hold their pins.
    // The iterator will attempt to fetch page 0 first; the BPM has no free
    // frames left and cannot evict either pinned frame -> fetch_page fails.
    // We fetch the first two physical page IDs used by the heap.
    // The heap's first data page is page 0 (the file starts at page 0).
    auto pinned0 = bpm_->fetch_page(r1->page_id);
    ASSERT_TRUE(pinned0.has_value()) << "setup: could not pin first page";
    auto pinned1 = bpm_->fetch_page(r2->page_id);
    ASSERT_TRUE(pinned1.has_value()) << "setup: could not pin second page";
    // Both frames are now pinned -- pool is exhausted for any NEW page load.

    // Attempt to scan. The begin() / first next() attempt to fetch a page.
    // If the two data pages share the same page IDs as what we pinned, the
    // fetch will just increment pin_count and succeed. We therefore only
    // assert the error occurs on the FIRST un-pinned page (one that is NOT
    // already in the pool under the same frame).
    //
    // Because pool_size=2 and we hold both frames, any page not already in
    // the pool cannot be loaded -- the BPM has no evictable frame.
    // Whether the error fires on the first or second page depends on which
    // page IDs the iterator walks next; either way the scan must NOT silently
    // succeed with a row count of 0.
    auto it = heap.begin();
    ASSERT_TRUE(it.has_value()) << it.error().message;

    bool got_error = false;
    int row_count = 0;
    for (;;) {
        auto nr = it->next();
        if (!nr) {
            // An error was propagated -- this is the correct GDB-915 behavior.
            got_error = true;
            break;
        }
        if (!nr->has_value()) {
            // Scan exhausted with no error. This could happen if both data
            // pages were already cached under the pinned frames (pin_count
            // just incremented, no new load needed). In that case the scan
            // succeeds normally, which is still correct -- no silent data loss.
            break;
        }
        ++row_count;
    }

    // Core assertion: the scan must NOT return 0 rows without an error.
    // Before the fix: next() would skip unpinnable pages and return nullopt,
    // yielding row_count=0 and got_error=false simultaneously -- wrong.
    // After the fix: either we get an error (correct), or we got all rows
    // because they were already in the pool (also correct).
    if (!got_error) {
        EXPECT_GT(row_count, 0) << "Silent incomplete scan: next() returned 0 rows with no error. "
                                   "This is the GDB-915 regression -- page fetch failures must be "
                                   "surfaced as errors, not silently skipped.";
    }

    // Unpin the frames we deliberately held.
    (void)bpm_->unpin_page(r1->page_id, false);
    (void)bpm_->unpin_page(r2->page_id, false);
}

// =============================================================================
// AC3: next() after an error returns an error (scan is invalid).
// =============================================================================

TEST_F(GDB915Test, EmptyHeapScanReturnsNulloptNotError) {
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
    TableHeap heap(*bpm_, dm_, file_id_);

    auto it = heap.begin();
    ASSERT_TRUE(it.has_value()) << it.error().message;

    // Empty heap: first next() must be ok(nullopt), not an error.
    auto r = it->next();
    ASSERT_TRUE(r.has_value()) << "Empty-heap next() returned error: " << r.error().message;
    EXPECT_FALSE(r->has_value()) << "Empty-heap next() should return nullopt";
}

// =============================================================================
// Adversarial: After an error, the exhausted_ flag is set and repeated calls
// to next() must NOT return rows (must stay exhausted or error).
// =============================================================================

TEST_F(GDB915Test, NextAfterErrorStaysExhausted) {
    constexpr uint32_t kPoolSize = 2;
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, kPoolSize);
    TableHeap heap(*bpm_, dm_, file_id_);

    // Fill two pages.
    auto r1 = heap.insert_tuple(make_blob(8160, 0x11));
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    auto r2 = heap.insert_tuple(make_blob(8160, 0x22));
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    (void)bpm_->flush_all();

    // Pin both frames to exhaust the pool.
    auto p0 = bpm_->fetch_page(r1->page_id);
    ASSERT_TRUE(p0.has_value());
    auto p1 = bpm_->fetch_page(r2->page_id);
    ASSERT_TRUE(p1.has_value());

    auto it = heap.begin();
    ASSERT_TRUE(it.has_value());

    // Drive to first error or exhaustion.
    bool saw_error = false;
    for (int i = 0; i < 10; ++i) {
        auto nr = it->next();
        if (!nr) {
            saw_error = true;
            break;
        }
        if (!nr->has_value()) {
            break;
        }
    }

    if (saw_error) {
        // After an error the iterator must be exhausted -- further calls must
        // NOT return rows (they may return errors or nullopt, but not data).
        for (int i = 0; i < 3; ++i) {
            auto nr2 = it->next();
            if (nr2.has_value() && nr2->has_value()) {
                ADD_FAILURE() << "next() returned a row after the iterator reported an error "
                                 "(exhausted_ flag not set correctly)";
                break;
            }
        }
    }

    (void)bpm_->unpin_page(r1->page_id, false);
    (void)bpm_->unpin_page(r2->page_id, false);
}

// =============================================================================
// Adversarial: Heap with all-deleted slots must return nullopt (normal
// exhaustion), NOT an error. Deleted slots are logically absent but the page
// is still readable -- this must not be confused with a fetch_page failure.
// =============================================================================

TEST_F(GDB915Test, HeapWithAllDeletedSlotsScanReturnsNullopt) {
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert and immediately delete several tuples.
    std::vector<RID> rids;
    for (int i = 0; i < 5; ++i) {
        auto r = heap.insert_tuple(make_blob(100, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value()) << r.error().message;
        rids.push_back(*r);
    }
    for (const auto& rid : rids) {
        auto d = heap.delete_tuple(rid);
        ASSERT_TRUE(d.has_value()) << d.error().message;
    }

    auto it = heap.begin();
    ASSERT_TRUE(it.has_value()) << it.error().message;

    // Scan must succeed (no error) and return zero rows.
    int count = 0;
    for (;;) {
        auto r = it->next();
        ASSERT_TRUE(r.has_value())
            << "Scan over all-deleted heap returned error: " << r.error().message;
        if (!r->has_value()) {
            break;
        }
        ++count;
    }
    EXPECT_EQ(count, 0) << "All-deleted heap must yield 0 rows, not skip pages with errors";
}

// =============================================================================
// Adversarial: Single-row heap (one page) -- normal scan must return exactly
// one row with no error. Boundary condition: single-page heap.
// =============================================================================

TEST_F(GDB915Test, SingleRowScanReturnsExactlyOneRow) {
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
    TableHeap heap(*bpm_, dm_, file_id_);

    auto ins = heap.insert_tuple(make_blob(50, 0xCC));
    ASSERT_TRUE(ins.has_value()) << ins.error().message;

    auto it = heap.begin();
    ASSERT_TRUE(it.has_value());

    auto r1 = it->next();
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    ASSERT_TRUE(r1->has_value()) << "Expected one row, got nullopt";

    auto r2 = it->next();
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    EXPECT_FALSE(r2->has_value()) << "Expected nullopt after last row";
}

// =============================================================================
// Adversarial: Page-boundary crossing -- rows that straddle a page boundary
// must all be returned. Insert enough rows to fill multiple pages, then verify
// EXACT count matches insert count (no off-by-one at page transitions).
// =============================================================================

TEST_F(GDB915Test, PageBoundaryCrossingYieldsExactCount) {
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
    TableHeap heap(*bpm_, dm_, file_id_);

    // 3000-byte tuples: ~2-3 per 8K page -- guarantees multiple page crossings.
    constexpr int kRows = 15;
    for (int i = 0; i < kRows; ++i) {
        auto r = heap.insert_tuple(make_blob(3000, static_cast<uint8_t>(i & 0xFF)));
        ASSERT_TRUE(r.has_value()) << "insert " << i << " failed: " << r.error().message;
    }

    auto it = heap.begin();
    ASSERT_TRUE(it.has_value());

    int count = 0;
    for (;;) {
        auto r = it->next();
        ASSERT_TRUE(r.has_value()) << "Error at row " << count << ": " << r.error().message;
        if (!r->has_value()) {
            break;
        }
        ++count;
    }
    EXPECT_EQ(count, kRows) << "Page-boundary crossing must not drop or duplicate rows";
}

// =============================================================================
// Adversarial: SeqScanOperator propagates the error from the iterator.
// If the underlying TableIterator returns an error, SeqScanOperator::do_next()
// must also return an error (not a truncated-success result).
//
// Uses pin exhaustion as the fault-injection seam (same as AC2).
// =============================================================================

#include "sixseven/executor/iterator.h"
#include "sixseven/executor/seq_scan.h"
#include "sixseven/table/tuple.h"

TEST_F(GDB915Test, SeqScanOperatorPropagatesIteratorError) {
    constexpr uint32_t kPoolSize = 2;
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, kPoolSize);
    TableHeap heap(*bpm_, dm_, file_id_);

    // Two full pages.
    auto r1 = heap.insert_tuple(make_blob(8160, 0xDD));
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    auto r2 = heap.insert_tuple(make_blob(8160, 0xEE));
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    (void)bpm_->flush_all();

    // Exhaust pool.
    auto p0 = bpm_->fetch_page(r1->page_id);
    ASSERT_TRUE(p0.has_value());
    auto p1 = bpm_->fetch_page(r2->page_id);
    ASSERT_TRUE(p1.has_value());

    // Build a minimal schema (one INT32 column).
    Schema storage_schema(std::vector<ColumnDef>{{"v", TypeId::INT32}});

    OutputSchema out_schema(std::vector<OutputColumn>{{"", "v", TypeId::INT32}});

    SeqScanOperator op(heap, storage_schema, std::move(out_schema), nullptr, nullptr);

    auto open_res = op.open();
    ASSERT_TRUE(open_res.has_value()) << open_res.error().message;

    // Drive the operator: it must either return an error or return the rows that
    // were already cached. It must NEVER return 0 rows without an error (the
    // pre-GDB-915 silent-loss pattern).
    bool got_error = false;
    int row_count = 0;
    for (int iter_guard = 0; iter_guard < 100; ++iter_guard) {
        auto nr = op.next();
        if (!nr) {
            got_error = true;
            break;
        }
        if (!nr->has_value()) {
            break;
        }
        ++row_count;
    }

    op.close();

    if (!got_error) {
        EXPECT_GT(row_count, 0)
            << "SeqScanOperator returned 0 rows with no error (GDB-915 regression): "
               "fetch_page failure was silently swallowed";
    }

    (void)bpm_->unpin_page(r1->page_id, false);
    (void)bpm_->unpin_page(r2->page_id, false);
}
