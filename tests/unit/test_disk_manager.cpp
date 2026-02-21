#include "giodb/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace giodb;

// -- Test fixture with temp directory -----------------------------------------

class DiskManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        temp_dir_ = std::filesystem::temp_directory_path() /
                    ("giodb_test_" + std::string(info->test_suite_name()) + "_" + info->name());
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override { std::filesystem::remove_all(temp_dir_); }

    [[nodiscard]] std::filesystem::path test_file(const std::string& name = "test.gdb") const {
        return temp_dir_ / name;
    }

    std::filesystem::path temp_dir_;
    DiskManager dm_;
};

// -- CRC32C tests -------------------------------------------------------------

TEST(CRC32C, EmptyBuffer) {
    uint32_t result = crc32c(nullptr, 0);
    // CRC32C of empty buffer is 0x00000000.
    EXPECT_EQ(result, 0x00000000u);
}

TEST(CRC32C, KnownVector) {
    // CRC32C of "123456789" is 0xE3069283 (well-known test vector).
    const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    uint32_t result = crc32c(data, sizeof(data));
    EXPECT_EQ(result, 0xE3069283u);
}

TEST(CRC32C, AllZeros) {
    std::array<uint8_t, 32> zeros{};
    uint32_t result = crc32c(zeros.data(), zeros.size());
    EXPECT_NE(result, 0u); // CRC32C of zeros should not be zero.
}

TEST(CRC32C, SingleByte) {
    uint8_t byte = 0x42;
    uint32_t result = crc32c(&byte, 1);
    EXPECT_NE(result, 0u);
}

TEST(CRC32C, DifferentDataDifferentChecksum) {
    std::array<uint8_t, 16> a{};
    std::array<uint8_t, 16> b{};
    a.fill(0xAA);
    b.fill(0xBB);
    EXPECT_NE(crc32c(a.data(), a.size()), crc32c(b.data(), b.size()));
}

// -- compute_page_checksum tests ----------------------------------------------

TEST(PageChecksum, ConsistentChecksum) {
    Page page(1, PageType::DATA);
    auto data = std::vector<uint8_t>(100, 0x42);
    auto insert_result = page.insert_tuple(data);
    ASSERT_TRUE(insert_result.has_value());

    uint32_t cksum1 = compute_page_checksum(page);
    uint32_t cksum2 = compute_page_checksum(page);
    EXPECT_EQ(cksum1, cksum2);
}

TEST(PageChecksum, DifferentPagesDifferentChecksum) {
    Page page_a(1, PageType::DATA);
    Page page_b(2, PageType::DATA);

    auto data = std::vector<uint8_t>(50, 0x11);
    ASSERT_TRUE(page_a.insert_tuple(data).has_value());

    // page_b has no tuple data — should produce a different checksum.
    EXPECT_NE(compute_page_checksum(page_a), compute_page_checksum(page_b));
}

TEST(PageChecksum, ChecksumFieldIgnored) {
    Page page(1, PageType::DATA);
    auto data = std::vector<uint8_t>(100, 0x42);
    ASSERT_TRUE(page.insert_tuple(data).has_value());

    // Setting different checksum values shouldn't change the computed checksum.
    page.set_checksum(0);
    uint32_t cksum_a = compute_page_checksum(page);

    page.set_checksum(0xDEADBEEF);
    uint32_t cksum_b = compute_page_checksum(page);

    EXPECT_EQ(cksum_a, cksum_b);
}

TEST(PageChecksum, UsesPublicChecksumOffset) {
    // Verify that Page::checksum_offset matches the expected value (20 bytes).
    EXPECT_EQ(Page::checksum_offset, 20u);
}

// -- DiskManager: create and open files ---------------------------------------

TEST_F(DiskManagerTest, CreateFile) {
    auto result = dm_.create_file(test_file());
    ASSERT_TRUE(result.has_value());

    FileId fid = *result;

    // File should exist on disk.
    EXPECT_TRUE(std::filesystem::exists(test_file()));

    // File size should be at least 1MB (128 pages pre-allocated).
    auto file_size = std::filesystem::file_size(test_file());
    EXPECT_GE(file_size, static_cast<uintmax_t>(file_growth_pages) * page_size);

    // Page count should be 1 (just the header).
    auto pc = dm_.file_page_count(fid);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(*pc, 1u);
}

TEST_F(DiskManagerTest, CreateExistingFileFailsWithoutOverwrite) {
    // First create succeeds.
    auto result1 = dm_.create_file(test_file());
    ASSERT_TRUE(result1.has_value());
    ASSERT_TRUE(dm_.close_file(*result1).has_value());

    // Second create without overwrite should fail with ALREADY_EXISTS.
    auto result2 = dm_.create_file(test_file());
    ASSERT_FALSE(result2.has_value());
    EXPECT_EQ(result2.error().code, StatusCode::ALREADY_EXISTS);
}

TEST_F(DiskManagerTest, CreateExistingFileSucceedsWithOverwrite) {
    // First create.
    auto result1 = dm_.create_file(test_file());
    ASSERT_TRUE(result1.has_value());
    FileId fid1 = *result1;

    // Allocate some pages.
    ASSERT_TRUE(dm_.allocate_page(fid1).has_value());
    ASSERT_TRUE(dm_.allocate_page(fid1).has_value());
    ASSERT_TRUE(dm_.close_file(fid1).has_value());

    // Overwrite should succeed and reset page count.
    auto result2 = dm_.create_file(test_file(), /*direct_io=*/false, /*overwrite=*/true);
    ASSERT_TRUE(result2.has_value());
    FileId fid2 = *result2;

    auto pc = dm_.file_page_count(fid2);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(*pc, 1u); // Reset to just header.
}

TEST_F(DiskManagerTest, OpenExistingFile) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid1 = *create_result;

    // Allocate some pages.
    ASSERT_TRUE(dm_.allocate_page(fid1).has_value());
    ASSERT_TRUE(dm_.allocate_page(fid1).has_value());

    auto close_result = dm_.close_file(fid1);
    ASSERT_TRUE(close_result.has_value());

    // Reopen and verify page count persisted.
    auto open_result = dm_.open_file(test_file());
    ASSERT_TRUE(open_result.has_value());
    FileId fid2 = *open_result;

    auto pc = dm_.file_page_count(fid2);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(*pc, 3u); // header + 2 allocated pages.
}

TEST_F(DiskManagerTest, OpenNonExistentFileFails) {
    auto result = dm_.open_file(temp_dir_ / "nonexistent.gdb");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST_F(DiskManagerTest, OpenFileWithBadMagicFails) {
    // Write a file with wrong magic.
    std::array<uint8_t, page_size> header{};
    uint32_t bad_magic = 0xDEADBEEF;
    std::memcpy(&header[fh_magic_offset], &bad_magic, sizeof(uint32_t));

    {
        std::ofstream ofs(test_file(), std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(header.data()), page_size);
    }

    auto result = dm_.open_file(test_file());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST_F(DiskManagerTest, OpenFileWithVersionMismatchFails) {
    // Write a valid file header but with wrong version number.
    std::array<uint8_t, page_size> header{};
    std::memcpy(&header[fh_magic_offset], &file_magic, sizeof(uint32_t));
    uint32_t bad_version = 99;
    std::memcpy(&header[fh_version_offset], &bad_version, sizeof(uint32_t));
    uint32_t pc = 1;
    std::memcpy(&header[fh_page_count_offset], &pc, sizeof(uint32_t));

    // Compute a valid checksum for this (bad-version) header so we reach
    // the version check rather than failing on checksum.
    uint32_t cksum = crc32c(header.data(), page_size);
    // Re-compute with skip region like the real code does.
    // We need to zero the checksum field, compute, then write.
    std::memset(&header[fh_checksum_offset], 0, sizeof(uint32_t));
    // Use crc32c on the whole buffer with the checksum field zeroed.
    cksum = crc32c(header.data(), page_size);
    std::memcpy(&header[fh_checksum_offset], &cksum, sizeof(uint32_t));

    {
        std::ofstream ofs(test_file(), std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(header.data()), page_size);
    }

    auto result = dm_.open_file(test_file());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST_F(DiskManagerTest, OpenShortFileFails) {
    // Write a file that is shorter than one page (incomplete header).
    {
        std::ofstream ofs(test_file(), std::ios::binary);
        std::array<uint8_t, 64> short_data{};
        ofs.write(reinterpret_cast<const char*>(short_data.data()), short_data.size());
    }

    auto result = dm_.open_file(test_file());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

// -- DiskManager: file locking ------------------------------------------------

TEST_F(DiskManagerTest, FileLockingPreventsDoubleOpen) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    // File is still open and locked.

    // A second DiskManager trying to open the same file should fail.
    DiskManager dm2;
    auto open_result = dm2.open_file(test_file());
    ASSERT_FALSE(open_result.has_value());
    EXPECT_EQ(open_result.error().code, StatusCode::IO_ERROR);
}

TEST_F(DiskManagerTest, FileLockReleasedAfterClose) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    ASSERT_TRUE(dm_.close_file(*create_result).has_value());

    // After closing, another DiskManager should be able to open it.
    DiskManager dm2;
    auto open_result = dm2.open_file(test_file());
    ASSERT_TRUE(open_result.has_value());
}

// -- DiskManager: close -------------------------------------------------------

TEST_F(DiskManagerTest, CloseFile) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    auto close_result = dm_.close_file(fid);
    ASSERT_TRUE(close_result.has_value());

    // Operations on closed file should fail.
    auto pc = dm_.file_page_count(fid);
    ASSERT_FALSE(pc.has_value());
    EXPECT_EQ(pc.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(DiskManagerTest, CloseInvalidFileIdFails) {
    auto result = dm_.close_file(999);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// -- DiskManager: FileId reuse ------------------------------------------------

TEST_F(DiskManagerTest, FileIdReusedAfterClose) {
    auto result1 = dm_.create_file(test_file("file1.gdb"));
    ASSERT_TRUE(result1.has_value());
    FileId fid1 = *result1;

    ASSERT_TRUE(dm_.close_file(fid1).has_value());

    // The next create should reuse fid1's slot.
    auto result2 = dm_.create_file(test_file("file2.gdb"));
    ASSERT_TRUE(result2.has_value());
    FileId fid2 = *result2;

    EXPECT_EQ(fid1, fid2);
}

// -- DiskManager: allocate_page -----------------------------------------------

TEST_F(DiskManagerTest, AllocatePageReturnsIncrementingIds) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    auto p1 = dm_.allocate_page(fid);
    ASSERT_TRUE(p1.has_value());
    EXPECT_EQ(*p1, 1u); // First user page.

    auto p2 = dm_.allocate_page(fid);
    ASSERT_TRUE(p2.has_value());
    EXPECT_EQ(*p2, 2u);

    auto p3 = dm_.allocate_page(fid);
    ASSERT_TRUE(p3.has_value());
    EXPECT_EQ(*p3, 3u);

    auto pc = dm_.file_page_count(fid);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(*pc, 4u); // header + 3 pages.
}

TEST_F(DiskManagerTest, AllocatePageExtendsFile) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    // Allocate more than 128 pages to force file extension beyond 1MB.
    for (uint32_t i = 0; i < 200; ++i) {
        auto result = dm_.allocate_page(fid);
        ASSERT_TRUE(result.has_value()) << "Failed to allocate page " << i;
    }

    auto pc = dm_.file_page_count(fid);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(*pc, 201u); // header + 200 pages.

    // File should have grown to at least 201 pages (rounded up to 1MB chunks).
    auto file_sz = std::filesystem::file_size(test_file());
    EXPECT_GE(file_sz, 201u * page_size);
    // Should be aligned to 1MB chunks.
    EXPECT_EQ(file_sz % (static_cast<uintmax_t>(file_growth_pages) * page_size), 0u);
}

// -- DiskManager: bulk allocate_pages -----------------------------------------

TEST_F(DiskManagerTest, BulkAllocatePagesReturnsFirstId) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    // Bulk-allocate 10 pages.
    auto result = dm_.allocate_pages(fid, 10);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1u); // First user page.

    auto pc = dm_.file_page_count(fid);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(*pc, 11u); // header + 10 pages.

    // All 10 pages should be writable.
    for (PageId pid = 1; pid <= 10; ++pid) {
        Page page(pid, PageType::DATA);
        auto tuple = std::vector<uint8_t>(50, static_cast<uint8_t>(pid));
        ASSERT_TRUE(page.insert_tuple(tuple).has_value());
        auto write_result = dm_.write_page(fid, pid, page);
        ASSERT_TRUE(write_result.has_value()) << "Failed to write page " << pid;
    }
}

TEST_F(DiskManagerTest, BulkAllocatePagesZeroCountFails) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    auto result = dm_.allocate_pages(fid, 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(DiskManagerTest, BulkAllocatePagesCombinedWithSingle) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    // Allocate one page, then bulk-allocate 5 more.
    auto p1 = dm_.allocate_page(fid);
    ASSERT_TRUE(p1.has_value());
    EXPECT_EQ(*p1, 1u);

    auto p2 = dm_.allocate_pages(fid, 5);
    ASSERT_TRUE(p2.has_value());
    EXPECT_EQ(*p2, 2u); // Continues from where single allocation left off.

    auto pc = dm_.file_page_count(fid);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(*pc, 7u); // header + 1 + 5.
}

// -- DiskManager: write and read pages ----------------------------------------

TEST_F(DiskManagerTest, WriteAndReadPage) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    auto page_id_result = dm_.allocate_page(fid);
    ASSERT_TRUE(page_id_result.has_value());
    PageId pid = *page_id_result;

    // Create a page with some data.
    Page write_page(pid, PageType::DATA);
    auto tuple = std::vector<uint8_t>(200, 0x42);
    ASSERT_TRUE(write_page.insert_tuple(tuple).has_value());

    // Write to disk.
    auto write_result = dm_.write_page(fid, pid, write_page);
    ASSERT_TRUE(write_result.has_value());

    // Read back.
    Page read_page(0, PageType::DATA);
    auto read_result = dm_.read_page(fid, pid, read_page);
    ASSERT_TRUE(read_result.has_value());

    // Verify data matches.
    EXPECT_EQ(read_page.page_id(), pid);
    EXPECT_EQ(read_page.page_type(), PageType::DATA);
    EXPECT_EQ(read_page.slot_count(), 1u);

    auto get_result = read_page.get_tuple(0);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ(get_result->size(), 200u);
    for (uint8_t b : *get_result) {
        EXPECT_EQ(b, 0x42);
    }
}

TEST_F(DiskManagerTest, WriteAndReadMultiplePages) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    // Allocate and write 10 pages.
    for (uint32_t i = 0; i < 10; ++i) {
        auto pid_result = dm_.allocate_page(fid);
        ASSERT_TRUE(pid_result.has_value());
        PageId pid = *pid_result;

        Page page(pid, PageType::DATA);
        auto tuple = std::vector<uint8_t>(50, static_cast<uint8_t>(i));
        ASSERT_TRUE(page.insert_tuple(tuple).has_value());

        auto write_result = dm_.write_page(fid, pid, page);
        ASSERT_TRUE(write_result.has_value());
    }

    // Read them all back and verify.
    for (uint32_t i = 0; i < 10; ++i) {
        PageId pid = i + 1;
        Page page(0, PageType::DATA);
        auto read_result = dm_.read_page(fid, pid, page);
        ASSERT_TRUE(read_result.has_value()) << "Failed to read page " << pid;

        EXPECT_EQ(page.page_id(), pid);

        auto get_result = page.get_tuple(0);
        ASSERT_TRUE(get_result.has_value());
        EXPECT_EQ((*get_result)[0], static_cast<uint8_t>(i));
    }
}

TEST_F(DiskManagerTest, WritePageSetsChecksum) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    auto pid_result = dm_.allocate_page(fid);
    ASSERT_TRUE(pid_result.has_value());
    PageId pid = *pid_result;

    Page page(pid, PageType::DATA);
    auto tuple = std::vector<uint8_t>(100, 0xAA);
    ASSERT_TRUE(page.insert_tuple(tuple).has_value());

    // Checksum should be 0 before write.
    EXPECT_EQ(page.checksum(), 0u);

    auto write_result = dm_.write_page(fid, pid, page);
    ASSERT_TRUE(write_result.has_value());

    // After write, checksum should be set.
    EXPECT_NE(page.checksum(), 0u);
    EXPECT_EQ(page.checksum(), compute_page_checksum(page));
}

TEST_F(DiskManagerTest, PersistAcrossCloseReopen) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    // Write a page.
    auto pid_result = dm_.allocate_page(fid);
    ASSERT_TRUE(pid_result.has_value());
    PageId pid = *pid_result;

    Page write_page(pid, PageType::BTREE_LEAF);
    auto tuple = std::vector<uint8_t>(300, 0xBB);
    ASSERT_TRUE(write_page.insert_tuple(tuple).has_value());
    ASSERT_TRUE(dm_.write_page(fid, pid, write_page).has_value());

    // Close.
    ASSERT_TRUE(dm_.close_file(fid).has_value());

    // Reopen and verify.
    auto open_result = dm_.open_file(test_file());
    ASSERT_TRUE(open_result.has_value());
    FileId fid2 = *open_result;

    Page read_page(0, PageType::DATA);
    auto read_result = dm_.read_page(fid2, pid, read_page);
    ASSERT_TRUE(read_result.has_value());

    EXPECT_EQ(read_page.page_type(), PageType::BTREE_LEAF);
    auto get_result = read_page.get_tuple(0);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ(get_result->size(), 300u);
    EXPECT_EQ((*get_result)[0], 0xBB);
}

// -- DiskManager: checksum verification (corruption detection) ----------------

TEST_F(DiskManagerTest, CorruptedPageDetected) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    auto pid_result = dm_.allocate_page(fid);
    ASSERT_TRUE(pid_result.has_value());
    PageId pid = *pid_result;

    // Write a valid page.
    Page page(pid, PageType::DATA);
    auto tuple = std::vector<uint8_t>(100, 0xCC);
    ASSERT_TRUE(page.insert_tuple(tuple).has_value());
    ASSERT_TRUE(dm_.write_page(fid, pid, page).has_value());

    // Close the file.
    ASSERT_TRUE(dm_.close_file(fid).has_value());

    // Corrupt a byte in the middle of the page on disk.
    {
        std::fstream fs(test_file(), std::ios::in | std::ios::out | std::ios::binary);
        auto corrupt_offset =
            static_cast<std::streamoff>(pid) * static_cast<std::streamoff>(page_size) + 100;
        fs.seekp(corrupt_offset);
        char bad_byte = 0xFF;
        fs.write(&bad_byte, 1);
    }

    // Reopen and try to read — should fail with checksum error.
    auto open_result = dm_.open_file(test_file());
    ASSERT_TRUE(open_result.has_value());
    FileId fid2 = *open_result;

    Page read_page(0, PageType::DATA);
    auto read_result = dm_.read_page(fid2, pid, read_page);
    ASSERT_FALSE(read_result.has_value());
    EXPECT_EQ(read_result.error().code, StatusCode::IO_ERROR);
}

// -- DiskManager: error cases -------------------------------------------------

TEST_F(DiskManagerTest, ReadPageZeroFails) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    Page page(0, PageType::DATA);
    auto result = dm_.read_page(fid, 0, page);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(DiskManagerTest, WritePageZeroFails) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    Page page(0, PageType::DATA);
    auto result = dm_.write_page(fid, 0, page);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(DiskManagerTest, ReadUnallocatedPageFails) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    // Only header exists, page 1 not allocated.
    Page page(0, PageType::DATA);
    auto result = dm_.read_page(fid, 1, page);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(DiskManagerTest, WriteUnallocatedPageFails) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    Page page(5, PageType::DATA);
    auto result = dm_.write_page(fid, 5, page);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(DiskManagerTest, InvalidFileIdFails) {
    Page page(0, PageType::DATA);

    auto read_result = dm_.read_page(999, 1, page);
    ASSERT_FALSE(read_result.has_value());
    EXPECT_EQ(read_result.error().code, StatusCode::INVALID_ARGUMENT);

    auto write_result = dm_.write_page(999, 1, page);
    ASSERT_FALSE(write_result.has_value());
    EXPECT_EQ(write_result.error().code, StatusCode::INVALID_ARGUMENT);

    auto alloc_result = dm_.allocate_page(999);
    ASSERT_FALSE(alloc_result.has_value());
    EXPECT_EQ(alloc_result.error().code, StatusCode::INVALID_ARGUMENT);
}

// -- DiskManager: multiple files ----------------------------------------------

TEST_F(DiskManagerTest, MultipleFiles) {
    auto fid1_result = dm_.create_file(test_file("file1.gdb"));
    ASSERT_TRUE(fid1_result.has_value());
    FileId fid1 = *fid1_result;

    auto fid2_result = dm_.create_file(test_file("file2.gdb"));
    ASSERT_TRUE(fid2_result.has_value());
    FileId fid2 = *fid2_result;

    EXPECT_NE(fid1, fid2);

    // Allocate and write different data to each file.
    auto pid1_result = dm_.allocate_page(fid1);
    ASSERT_TRUE(pid1_result.has_value());
    auto pid2_result = dm_.allocate_page(fid2);
    ASSERT_TRUE(pid2_result.has_value());

    Page page1(*pid1_result, PageType::DATA);
    auto tuple1 = std::vector<uint8_t>(64, 0x11);
    ASSERT_TRUE(page1.insert_tuple(tuple1).has_value());
    ASSERT_TRUE(dm_.write_page(fid1, *pid1_result, page1).has_value());

    Page page2(*pid2_result, PageType::DATA);
    auto tuple2 = std::vector<uint8_t>(64, 0x22);
    ASSERT_TRUE(page2.insert_tuple(tuple2).has_value());
    ASSERT_TRUE(dm_.write_page(fid2, *pid2_result, page2).has_value());

    // Read back and verify independence.
    Page read1(0, PageType::DATA);
    ASSERT_TRUE(dm_.read_page(fid1, *pid1_result, read1).has_value());
    auto get1 = read1.get_tuple(0);
    ASSERT_TRUE(get1.has_value());
    EXPECT_EQ((*get1)[0], 0x11);

    Page read2(0, PageType::DATA);
    ASSERT_TRUE(dm_.read_page(fid2, *pid2_result, read2).has_value());
    auto get2 = read2.get_tuple(0);
    ASSERT_TRUE(get2.has_value());
    EXPECT_EQ((*get2)[0], 0x22);
}

// -- DiskManager: file header integrity ---------------------------------------

TEST_F(DiskManagerTest, CorruptedFileHeaderDetected) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    ASSERT_TRUE(dm_.close_file(*create_result).has_value());

    // Corrupt a byte in the header's page_count field.
    {
        std::fstream fs(test_file(), std::ios::in | std::ios::out | std::ios::binary);
        fs.seekp(static_cast<std::streamoff>(fh_page_count_offset));
        char bad_byte = 0xFF;
        fs.write(&bad_byte, 1);
    }

    // Opening should fail with checksum error.
    auto open_result = dm_.open_file(test_file());
    ASSERT_FALSE(open_result.has_value());
    EXPECT_EQ(open_result.error().code, StatusCode::IO_ERROR);
}

// -- DiskManager: sync_file and sync_data -------------------------------------

TEST_F(DiskManagerTest, SyncFileSucceeds) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    // Write a page then sync.
    auto pid_result = dm_.allocate_page(fid);
    ASSERT_TRUE(pid_result.has_value());

    Page page(*pid_result, PageType::DATA);
    auto tuple = std::vector<uint8_t>(100, 0xDD);
    ASSERT_TRUE(page.insert_tuple(tuple).has_value());
    ASSERT_TRUE(dm_.write_page(fid, *pid_result, page).has_value());

    // sync_file (full sync) should succeed.
    auto sync_result = dm_.sync_file(fid);
    ASSERT_TRUE(sync_result.has_value());
}

TEST_F(DiskManagerTest, SyncDataSucceeds) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    // Write a page then sync data.
    auto pid_result = dm_.allocate_page(fid);
    ASSERT_TRUE(pid_result.has_value());

    Page page(*pid_result, PageType::DATA);
    auto tuple = std::vector<uint8_t>(100, 0xEE);
    ASSERT_TRUE(page.insert_tuple(tuple).has_value());
    ASSERT_TRUE(dm_.write_page(fid, *pid_result, page).has_value());

    // sync_data (data-only sync) should succeed.
    auto sync_result = dm_.sync_data(fid);
    ASSERT_TRUE(sync_result.has_value());
}

TEST_F(DiskManagerTest, SyncInvalidFileIdFails) {
    auto result1 = dm_.sync_file(999);
    ASSERT_FALSE(result1.has_value());
    EXPECT_EQ(result1.error().code, StatusCode::INVALID_ARGUMENT);

    auto result2 = dm_.sync_data(999);
    ASSERT_FALSE(result2.has_value());
    EXPECT_EQ(result2.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(DiskManagerTest, SyncClosedFileFails) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;
    ASSERT_TRUE(dm_.close_file(fid).has_value());

    auto result1 = dm_.sync_file(fid);
    ASSERT_FALSE(result1.has_value());
    EXPECT_EQ(result1.error().code, StatusCode::INVALID_ARGUMENT);

    auto result2 = dm_.sync_data(fid);
    ASSERT_FALSE(result2.has_value());
    EXPECT_EQ(result2.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(DiskManagerTest, DataPersistsAfterSync) {
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    auto pid_result = dm_.allocate_page(fid);
    ASSERT_TRUE(pid_result.has_value());
    PageId pid = *pid_result;

    Page page(pid, PageType::DATA);
    auto tuple = std::vector<uint8_t>(200, 0x77);
    ASSERT_TRUE(page.insert_tuple(tuple).has_value());
    ASSERT_TRUE(dm_.write_page(fid, pid, page).has_value());

    // Sync then close.
    ASSERT_TRUE(dm_.sync_file(fid).has_value());
    ASSERT_TRUE(dm_.close_file(fid).has_value());

    // Reopen and verify data persisted.
    auto open_result = dm_.open_file(test_file());
    ASSERT_TRUE(open_result.has_value());
    FileId fid2 = *open_result;

    Page read_page(0, PageType::DATA);
    auto read_result = dm_.read_page(fid2, pid, read_page);
    ASSERT_TRUE(read_result.has_value());

    auto get_result = read_page.get_tuple(0);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ(get_result->size(), 200u);
    EXPECT_EQ((*get_result)[0], 0x77);
}

// -- DiskManager: Direct I/O --------------------------------------------------

TEST_F(DiskManagerTest, DirectIOCreateAndWrite) {
    auto create_result = dm_.create_file(test_file(), /*direct_io=*/true);
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    auto pid_result = dm_.allocate_page(fid);
    ASSERT_TRUE(pid_result.has_value());
    PageId pid = *pid_result;

    Page page(pid, PageType::DATA);
    auto tuple = std::vector<uint8_t>(150, 0x55);
    ASSERT_TRUE(page.insert_tuple(tuple).has_value());

    auto write_result = dm_.write_page(fid, pid, page);
    ASSERT_TRUE(write_result.has_value());

    // Read back and verify.
    Page read_page(0, PageType::DATA);
    auto read_result = dm_.read_page(fid, pid, read_page);
    ASSERT_TRUE(read_result.has_value());

    auto get_result = read_page.get_tuple(0);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ(get_result->size(), 150u);
    EXPECT_EQ((*get_result)[0], 0x55);
}

TEST_F(DiskManagerTest, DirectIOOpenExisting) {
    // Create a file without direct I/O.
    auto create_result = dm_.create_file(test_file());
    ASSERT_TRUE(create_result.has_value());
    FileId fid1 = *create_result;

    auto pid_result = dm_.allocate_page(fid1);
    ASSERT_TRUE(pid_result.has_value());
    PageId pid = *pid_result;

    Page page(pid, PageType::DATA);
    auto tuple = std::vector<uint8_t>(80, 0x33);
    ASSERT_TRUE(page.insert_tuple(tuple).has_value());
    ASSERT_TRUE(dm_.write_page(fid1, pid, page).has_value());
    ASSERT_TRUE(dm_.close_file(fid1).has_value());

    // Reopen with direct I/O.
    auto open_result = dm_.open_file(test_file(), /*direct_io=*/true);
    ASSERT_TRUE(open_result.has_value());
    FileId fid2 = *open_result;

    Page read_page(0, PageType::DATA);
    auto read_result = dm_.read_page(fid2, pid, read_page);
    ASSERT_TRUE(read_result.has_value());

    auto get_result = read_page.get_tuple(0);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ((*get_result)[0], 0x33);
}

TEST_F(DiskManagerTest, DirectIOWithSync) {
    auto create_result = dm_.create_file(test_file(), /*direct_io=*/true);
    ASSERT_TRUE(create_result.has_value());
    FileId fid = *create_result;

    auto pid_result = dm_.allocate_page(fid);
    ASSERT_TRUE(pid_result.has_value());

    Page page(*pid_result, PageType::DATA);
    auto tuple = std::vector<uint8_t>(100, 0x99);
    ASSERT_TRUE(page.insert_tuple(tuple).has_value());
    ASSERT_TRUE(dm_.write_page(fid, *pid_result, page).has_value());

    // Both sync methods should work with direct I/O.
    ASSERT_TRUE(dm_.sync_file(fid).has_value());
    ASSERT_TRUE(dm_.sync_data(fid).has_value());
}
