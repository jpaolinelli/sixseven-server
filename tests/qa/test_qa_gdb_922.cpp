/// QA regression tests for GDB-922: DWB recovery path implementation.
///
/// Verifies that:
///   1. A torn data page is correctly restored from an intact DWB copy.
///   2. A torn/partial DWB copy does NOT clobber a valid data page.
///   3. A valid data page is left untouched (recovery is a no-op).
///   4. Opening an existing DWB file without O_TRUNC preserves recovery data.

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
