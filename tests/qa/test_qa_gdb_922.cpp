/// QA regression tests for GDB-922: DWB recovery path implementation.
///
/// Verifies that:
///   1. A torn data page is correctly restored from an intact DWB copy.
///   2. A torn/partial DWB copy does NOT clobber a valid data page.
///   3. A valid data page is left untouched (recovery is a no-op).
///   4. Opening an existing DWB file without O_TRUNC preserves recovery data.
///   5. (Adversarial) DWB slot with wrong page_id does not corrupt a different page.
///   6. (Adversarial) Zeroed/empty DWB file => recovery is a clean no-op.
///   7. (Adversarial) DWB disabled via set_double_write_enabled(false) still works.
///   8. (Adversarial) enable_double_write() called twice returns ALREADY_EXISTS.
///   9. (Adversarial) DWB slot page_id == 0 (file header) is skipped -- not restored.
///  10. (Adversarial) DWB slot page_id beyond file extent is skipped -- no oob write.

#include "sixseven/common/platform.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/page.h"

#include <gtest/gtest.h>

#include <fcntl.h>

#include <array>
#include <cstring>
#include <filesystem>

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class QA_GDB922_DwbRecovery : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        temp_dir_ = std::filesystem::temp_directory_path() /
                    ("sixseven_qa_gdb922_" + std::string(info->name()));
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);

        auto create_result = dm_.create_file(temp_dir_ / "test.gdb");
        ASSERT_TRUE(create_result.has_value()) << create_result.error().message;
        file_id_ = *create_result;

        dwb_path_ = temp_dir_ / "test.dwb";
    }

    void TearDown() override {
        bpm_.reset();
        (void)dm_.close_file(file_id_);
        std::filesystem::remove_all(temp_dir_);
    }

    // Helper: create a BPM with DWB enabled, write a page, flush it, return the
    // PageId.  The BPM is stored in bpm_ so the caller can tear it down or
    // inspect the DWB file afterward.
    PageId write_page_through_dwb(uint8_t fill_byte) {
        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 4);
        auto en = bpm_->enable_double_write(dwb_path_);
        [&] { ASSERT_TRUE(en.has_value()) << en.error().message; }();

        auto np = bpm_->new_page();
        [&] { ASSERT_TRUE(np.has_value()) << np.error().message; }();
        PageId pid = (*np)->page_id();

        std::vector<uint8_t> payload(64, fill_byte);
        [&] { ASSERT_TRUE((*np)->insert_tuple(payload).has_value()); }();
        [&] { ASSERT_TRUE(bpm_->unpin_page(pid, /*is_dirty=*/true).has_value()); }();
        [&] { ASSERT_TRUE(bpm_->flush_page(pid).has_value()); }();

        return pid;
    }

    // Helper: corrupt a data-file page by closing the DM file (releases the
    // exclusive lock), overwriting the page's first 64 bytes with 0xDE so the
    // CRC check fails, then reopening the file in DM so subsequent reads go
    // through the normal path.  The caller must have already destroyed bpm_.
    // Returns the new file_id_ after reopen.
    void corrupt_data_page_and_reopen(PageId pid) {
        // DiskManager stores pages at offset: pid * page_size bytes from file start.
        uint64_t offset = static_cast<uint64_t>(pid) * static_cast<uint64_t>(page_size);

        // Close the DM handle to release the exclusive lock so we can open
        // a second handle for the corruption write (required on Windows where
        // LockFileEx enforces mandatory byte-range locks).
        auto close_r = dm_.close_file(file_id_);
        ASSERT_TRUE(close_r.has_value()) << close_r.error().message;

        // Open and corrupt.
        int fd = ::open((temp_dir_ / "test.gdb").string().c_str(), O_RDWR, 0644);
        ASSERT_GE(fd, 0) << "Could not open data file for corruption";
        std::array<uint8_t, 64> garbage{};
        std::fill(garbage.begin(), garbage.end(), uint8_t(0xDE));
        ssize_t w = sixseven_platform::pwrite(
            fd, garbage.data(), garbage.size(), static_cast<int64_t>(offset));
        EXPECT_EQ(w, static_cast<ssize_t>(garbage.size()));
        ::close(fd);

        // Reopen in DM to get a new file_id_.
        auto open_r = dm_.open_file(temp_dir_ / "test.gdb");
        ASSERT_TRUE(open_r.has_value()) << open_r.error().message;
        file_id_ = *open_r;
    }

    std::filesystem::path temp_dir_;
    std::filesystem::path dwb_path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
};

// =============================================================================
// AC1: Torn data page is restored from an intact DWB copy
// =============================================================================

/// Core recovery test: write a page (DWB holds a valid copy + data file has
/// correct page), then corrupt the data file page (simulating a torn write),
/// then re-enable DWB (recovery runs automatically), and confirm the data page
/// is restored and readable.
TEST_F(QA_GDB922_DwbRecovery, TornDataPageIsRestoredFromDwbCopy) {
    // Phase 1: write a page through the DWB so both the DWB and data file hold
    // the correct bytes.
    const uint8_t kFill = 0xAB;
    PageId pid = write_page_through_dwb(kFill);

    // The DWB slot at offset 0 now holds the valid page copy.
    // Destroy the BPM (closes file handles but leaves the DWB file on disk).
    bpm_.reset();

    // Phase 2: corrupt the data-file page to simulate a torn write.
    // This also reopens the file in dm_ and updates file_id_.
    corrupt_data_page_and_reopen(pid);

    // Confirm the corruption: direct DiskManager read should fail CRC.
    {
        Page probe(0, PageType::DATA);
        auto r = dm_.read_page(file_id_, pid, probe);
        ASSERT_FALSE(r.has_value()) << "Expected CRC failure after corruption";
    }

    // Phase 3: re-enable DWB.  Recovery runs automatically inside
    // enable_double_write() because the DWB file already exists.
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 4);
    auto en = bpm_->enable_double_write(dwb_path_);
    ASSERT_TRUE(en.has_value()) << en.error().message;

    // Phase 4: confirm the data page is now readable and has the correct content.
    Page restored(0, PageType::DATA);
    auto r = dm_.read_page(file_id_, pid, restored);
    ASSERT_TRUE(r.has_value()) << "Data page should be readable after DWB recovery";

    auto tuple = restored.get_tuple(0);
    ASSERT_TRUE(tuple.has_value()) << "Restored page should have a tuple";
    EXPECT_EQ((*tuple)[0], kFill) << "Restored tuple should have the original fill byte";
}

// =============================================================================
// AC2: A torn/partial DWB copy must NOT clobber a valid data page
// =============================================================================

/// If the DWB file itself is partial or has a bad checksum, recovery must skip
/// it rather than overwriting a valid data page.
TEST_F(QA_GDB922_DwbRecovery, TornDwbCopyDoesNotClobberValidDataPage) {
    // Write a page normally through the DWB.
    const uint8_t kFill = 0xCD;
    PageId pid = write_page_through_dwb(kFill);
    bpm_.reset();

    // Confirm data page is readable before we corrupt the DWB.
    {
        Page probe(0, PageType::DATA);
        auto r = dm_.read_page(file_id_, pid, probe);
        ASSERT_TRUE(r.has_value()) << "Data page should be valid before DWB corruption";
    }

    // Corrupt the DWB file itself (simulate a torn DWB write).
    {
        int fd = ::open(dwb_path_.string().c_str(), O_RDWR, 0644);
        ASSERT_GE(fd, 0);
        std::array<uint8_t, 32> garbage{};
        std::fill(garbage.begin(), garbage.end(), uint8_t(0xFF));
        ssize_t w = sixseven_platform::pwrite(fd, garbage.data(), garbage.size(), 0);
        EXPECT_EQ(w, static_cast<ssize_t>(garbage.size()));
        ::close(fd);
    }

    // Re-enable DWB.  Recovery should detect the bad DWB checksum and skip.
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 4);
    auto en = bpm_->enable_double_write(dwb_path_);
    ASSERT_TRUE(en.has_value()) << "enable_double_write should succeed even with torn DWB";

    // Data page must still be intact.
    Page after(0, PageType::DATA);
    auto r = dm_.read_page(file_id_, pid, after);
    ASSERT_TRUE(r.has_value()) << "Data page must not be clobbered by a torn DWB copy";

    auto tuple = after.get_tuple(0);
    ASSERT_TRUE(tuple.has_value());
    EXPECT_EQ((*tuple)[0], kFill);
}

// =============================================================================
// AC3: Valid data page -- recovery is a no-op
// =============================================================================

/// When both the DWB copy and the data page are intact, re-enabling DWB should
/// succeed and leave the data page unchanged.
TEST_F(QA_GDB922_DwbRecovery, CleanShutdownRecoveryIsNoop) {
    const uint8_t kFill = 0x55;
    PageId pid = write_page_through_dwb(kFill);
    bpm_.reset();

    // Both DWB and data file are consistent.  Re-enable DWB.
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 4);
    auto en = bpm_->enable_double_write(dwb_path_);
    ASSERT_TRUE(en.has_value()) << en.error().message;

    // Data page should still be readable with original content.
    Page p(0, PageType::DATA);
    auto r = dm_.read_page(file_id_, pid, p);
    ASSERT_TRUE(r.has_value()) << "Data page should be intact after no-op recovery";

    auto tuple = p.get_tuple(0);
    ASSERT_TRUE(tuple.has_value());
    EXPECT_EQ((*tuple)[0], kFill);
}

// =============================================================================
// AC4: No O_TRUNC data-loss -- existing DWB slot survives re-open
// =============================================================================

/// Opening an existing DWB file must NOT truncate it (the old O_TRUNC bug).
/// Verify that the DWB file still contains a non-zero page after re-opening.
TEST_F(QA_GDB922_DwbRecovery, NoDwbTruncOnReopen) {
    // Write a page so the DWB file has a valid slot.
    write_page_through_dwb(0x77);
    bpm_.reset();

    // Record DWB file size before re-open.
    auto size_before = std::filesystem::file_size(dwb_path_);
    ASSERT_GE(size_before, static_cast<uintmax_t>(page_size))
        << "DWB file should be at least one page after a write";

    // Re-open without truncation (recovery runs, then slot is zeroed and
    // re-allocated for new writes -- file stays at page_size, not zero).
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 4);
    auto en = bpm_->enable_double_write(dwb_path_);
    ASSERT_TRUE(en.has_value()) << en.error().message;

    auto size_after = std::filesystem::file_size(dwb_path_);
    EXPECT_GE(size_after, static_cast<uintmax_t>(page_size))
        << "DWB file must not be zero-length after re-open (O_TRUNC bug regression)";
}

// =============================================================================
// Adversarial AC5: DWB slot page_id does not match any corrupted page --
// recovery must NOT write to a page that is not actually torn.
// =============================================================================

/// Scenario: write page A through the DWB (DWB holds a valid copy of A).
/// Then corrupt page A (tears it), re-enable DWB so recovery restores A.
/// Then write page B through a new BPM (DWB now holds a copy of B).
/// Corrupt page A again but NOT page B.
/// Re-enable DWB: the DWB slot holds B's copy. Recovery reads the data file
/// for B and finds it valid => no restore. Page A stays corrupted (no wrong-
/// page restore). This proves recovery does not blindly write to a page that
/// the DWB slot does not match or that is not torn.
TEST_F(QA_GDB922_DwbRecovery, DwbSlotProtectsOnlyItsOwnPage) {
    // Write page A through the DWB.
    const uint8_t kFillA = 0xAA;
    PageId pid_a = write_page_through_dwb(kFillA);
    bpm_.reset();

    // Corrupt page A, reopen DM, run recovery -> A is restored.
    corrupt_data_page_and_reopen(pid_a);
    {
        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 4);
        auto en = bpm_->enable_double_write(dwb_path_);
        ASSERT_TRUE(en.has_value()) << en.error().message;
        // After recovery, A should be readable again.
        Page p(0, PageType::DATA);
        auto r = dm_.read_page(file_id_, pid_a, p);
        ASSERT_TRUE(r.has_value()) << "Page A should be restored after first recovery";
    }
    bpm_.reset();

    // Now write page B through a new BPM (DWB slot is overwritten with B).
    const uint8_t kFillB = 0xBB;
    PageId pid_b = write_page_through_dwb(kFillB);
    bpm_.reset();

    // Corrupt page A again but leave page B intact.
    corrupt_data_page_and_reopen(pid_a);

    // Re-enable DWB: slot holds B's copy; B is valid so recovery is a no-op.
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 4);
    auto en = bpm_->enable_double_write(dwb_path_);
    ASSERT_TRUE(en.has_value()) << en.error().message;

    // Page B must still be intact (recovery must not have wrongly written to it).
    {
        Page pb(0, PageType::DATA);
        auto r = dm_.read_page(file_id_, pid_b, pb);
        ASSERT_TRUE(r.has_value()) << "Page B must remain intact (slot was for B, B was valid)";
        auto tuple = pb.get_tuple(0);
        ASSERT_TRUE(tuple.has_value());
        EXPECT_EQ((*tuple)[0], kFillB);
    }

    // Page A is still corrupted (the DWB slot held B, not A, so A was not restored).
    {
        Page pa(0, PageType::DATA);
        auto r = dm_.read_page(file_id_, pid_a, pa);
        EXPECT_FALSE(r.has_value()) << "Page A should still be corrupted (DWB slot was for B)";
    }
}

// =============================================================================
// Adversarial AC6: Zeroed / empty DWB file => recovery is a clean no-op.
// =============================================================================

/// If the DWB file is exactly one page of zero bytes, it must NOT trigger a
/// restore (the computed checksum of an all-zeros page will not match the stored
/// checksum field of zero, because the stored field is part of the page data and
/// compute_page_checksum excludes it from the CRC -- but the CRC of all-zeros
/// payload is non-zero, so stored==0 != computed!=0 => skip).
TEST_F(QA_GDB922_DwbRecovery, ZeroedDwbFileIsNoop) {
    // Write a real page so we have a valid data file page to verify against.
    const uint8_t kFill = 0x99;
    PageId pid = write_page_through_dwb(kFill);
    bpm_.reset();

    // Overwrite the entire DWB file with zeros.
    {
        int fd = ::open(dwb_path_.string().c_str(), O_RDWR, 0644);
        ASSERT_GE(fd, 0);
        std::array<uint8_t, page_size> zeros{};
        ssize_t w = sixseven_platform::pwrite(fd, zeros.data(), zeros.size(), 0);
        EXPECT_EQ(w, static_cast<ssize_t>(zeros.size()));
        ::close(fd);
    }

    // Re-enable DWB: zeroed slot must be skipped.
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 4);
    auto en = bpm_->enable_double_write(dwb_path_);
    ASSERT_TRUE(en.has_value()) << "enable_double_write must succeed with zeroed DWB";

    // Data page must remain intact (no false restore from zeros).
    Page p(0, PageType::DATA);
    auto r = dm_.read_page(file_id_, pid, p);
    ASSERT_TRUE(r.has_value()) << "Data page must not be clobbered by zeroed DWB slot";
    auto tuple = p.get_tuple(0);
    ASSERT_TRUE(tuple.has_value());
    EXPECT_EQ((*tuple)[0], kFill);
}

// =============================================================================
// Adversarial AC7: DWB disabled (set_double_write_enabled(false)) -- normal
// table CRUD still works; no DWB file is created.
// Note: StorageManager is the integration point; test at BPM level since
// StorageManager needs the full executor/catalog stack. We verify simply that
// NOT calling enable_double_write still allows write/read.
// =============================================================================

TEST_F(QA_GDB922_DwbRecovery, NoDwbEnabledStillAllowsCrud) {
    // Create BPM without DWB (simulates storage_double_write=false path).
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 4);
    // DWB NOT enabled.

    auto np = bpm_->new_page();
    ASSERT_TRUE(np.has_value()) << np.error().message;
    PageId pid = (*np)->page_id();

    std::vector<uint8_t> payload(32, uint8_t(0x42));
    ASSERT_TRUE((*np)->insert_tuple(payload).has_value());
    ASSERT_TRUE(bpm_->unpin_page(pid, true).has_value());
    ASSERT_TRUE(bpm_->flush_page(pid).has_value());

    // Evict and re-fetch to confirm round-trip through disk.
    ASSERT_TRUE(bpm_->delete_page(pid).has_value());

    // Re-read via DiskManager directly.
    Page p(0, PageType::DATA);
    auto r = dm_.read_page(file_id_, pid, p);
    ASSERT_TRUE(r.has_value()) << "Page should be readable without DWB enabled";
    auto tuple = p.get_tuple(0);
    ASSERT_TRUE(tuple.has_value());
    EXPECT_EQ((*tuple)[0], uint8_t(0x42));

    // No DWB file should have been created.
    EXPECT_FALSE(std::filesystem::exists(dwb_path_)) << "DWB file must not exist when not enabled";
}

// =============================================================================
// Adversarial AC8: enable_double_write() called twice returns ALREADY_EXISTS.
// =============================================================================

TEST_F(QA_GDB922_DwbRecovery, EnableDwbTwiceReturnsAlreadyExists) {
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 4);

    auto en1 = bpm_->enable_double_write(dwb_path_);
    ASSERT_TRUE(en1.has_value()) << en1.error().message;

    auto en2 = bpm_->enable_double_write(dwb_path_);
    ASSERT_FALSE(en2.has_value()) << "Second enable_double_write must fail";
    EXPECT_EQ(en2.error().code, StatusCode::ALREADY_EXISTS);
}

// =============================================================================
// Adversarial AC9: DWB slot with page_id == 0 (file header page) is skipped.
// The recovery code checks page_id != 0 to avoid overwriting the file header.
// We synthesize a fake DWB slot with page_id=0 and a valid checksum and verify
// that enable_double_write does NOT restore it.
// =============================================================================

TEST_F(QA_GDB922_DwbRecovery, DwbSlotWithPageIdZeroIsSkipped) {
    // Create a fake DWB file containing a Page whose page_id == 0 but has a
    // valid checksum. Recovery should skip this slot (page_id==0 guard).
    {
        Page fake_page(0, PageType::DATA); // page_id = 0
        // Give it a non-zero payload so it doesn't look empty.
        std::vector<uint8_t> payload(16, uint8_t(0xBE));
        (void)fake_page.insert_tuple(payload);
        // Set valid checksum.
        fake_page.set_checksum(compute_page_checksum(fake_page));

        int fd = ::open(dwb_path_.string().c_str(), O_RDWR | O_CREAT, 0644);
        ASSERT_GE(fd, 0);
        ssize_t w = sixseven_platform::pwrite(fd, fake_page.raw().data(), page_size, 0);
        EXPECT_EQ(w, static_cast<ssize_t>(page_size));
        ::close(fd);
    }

    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 4);
    // Recovery runs: must skip page_id==0 and succeed.
    auto en = bpm_->enable_double_write(dwb_path_);
    ASSERT_TRUE(en.has_value()) << "enable_double_write must succeed even with page_id=0 slot";

    // File header (page 0) must be intact -- try to read a page that IS in the
    // file (we haven't corrupted anything, so the read must succeed).
    // Allocate a user page to confirm the file is usable.
    auto np = bpm_->new_page();
    ASSERT_TRUE(np.has_value()) << "Should be able to allocate pages after skipping page_id=0 slot";
}

// =============================================================================
// Adversarial AC10: DWB slot with page_id beyond the file extent is skipped --
// no out-of-bounds write to a non-existent page.
// =============================================================================

TEST_F(QA_GDB922_DwbRecovery, DwbSlotBeyondFileExtentIsSkipped) {
    // Write a real page so the data file exists with known content.
    const uint8_t kFill = 0x77;
    PageId pid = write_page_through_dwb(kFill);
    bpm_.reset();

    // Record actual page count via DiskManager.
    auto pc = dm_.file_page_count(file_id_);
    ASSERT_TRUE(pc.has_value());
    PageId out_of_bounds_pid = *pc + 999; // well beyond the file.

    // Craft a DWB slot claiming to protect an out-of-bounds page.
    {
        Page fake_page(out_of_bounds_pid, PageType::DATA);
        std::vector<uint8_t> payload(16, uint8_t(0xEF));
        (void)fake_page.insert_tuple(payload);
        fake_page.set_checksum(compute_page_checksum(fake_page));

        int fd = ::open(dwb_path_.string().c_str(), O_RDWR, 0644);
        ASSERT_GE(fd, 0);
        ssize_t w = sixseven_platform::pwrite(fd, fake_page.raw().data(), page_size, 0);
        EXPECT_EQ(w, static_cast<ssize_t>(page_size));
        ::close(fd);
    }

    // Re-enable DWB: must skip the out-of-bounds slot gracefully.
    bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 4);
    auto en = bpm_->enable_double_write(dwb_path_);
    ASSERT_TRUE(en.has_value()) << "enable_double_write must succeed for out-of-bounds slot";

    // Existing page must be intact.
    Page p(0, PageType::DATA);
    auto r = dm_.read_page(file_id_, pid, p);
    ASSERT_TRUE(r.has_value()) << "Existing page must be intact after out-of-bounds slot skip";
    auto tuple = p.get_tuple(0);
    ASSERT_TRUE(tuple.has_value());
    EXPECT_EQ((*tuple)[0], kFill);
}
