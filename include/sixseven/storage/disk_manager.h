#pragma once

#include "sixseven/common/result.h"
#include "sixseven/storage/page.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace sixseven {

/// File identifier for an open database file.
using FileId = uint32_t;

/// Page identifier within a database file.
/// Page 0 is the file header; user pages start at 1.
using PageId = uint32_t;

// -- CRC32C ------------------------------------------------------------------

/// Compute CRC32C (Castagnoli) checksum over a byte buffer.
uint32_t crc32c(const uint8_t* data, size_t length);

/// Compute CRC32C of a page, excluding its checksum field (offset 20, 4 bytes).
uint32_t compute_page_checksum(const Page& page);

// -- File format constants ---------------------------------------------------

/// Magic number identifying SixSevenDB database files.
static constexpr uint32_t file_magic = 0x47444201;

/// Current file format version.
static constexpr uint32_t file_format_version = 1;

/// Pages per 1MB growth chunk: 128 x 8KB = 1MB.
static constexpr uint32_t file_growth_pages = 128;

/// File header field offsets within page 0.
static constexpr size_t fh_magic_offset = 0;      // uint32_t
static constexpr size_t fh_version_offset = 4;    // uint32_t
static constexpr size_t fh_page_count_offset = 8; // uint32_t
static constexpr size_t fh_checksum_offset = 12;  // uint32_t
static constexpr size_t fh_ext_offset = 16;       // start of extension area

// -- DiskManager -------------------------------------------------------------

/// Manages reading and writing 8KB pages to/from database files on disk.
///
/// File layout:
/// ```
///   Page 0:  file header (magic, version, page_count, checksum)
///   Page 1+: user data pages
/// ```
///
/// Files grow in 1MB chunks (128 pages). Pages must be written with
/// write_page() before they can be read; reading an uninitialized page
/// returns a checksum verification error.
///
/// Thread safety: This class is NOT thread-safe. External synchronization
/// (e.g., a mutex) is required if a DiskManager instance is shared across
/// threads. In practice, the buffer pool layer provides this synchronization.
///
/// File locking: Each open file is protected by an exclusive advisory lock
/// (flock). This prevents multiple processes from opening the same database
/// file concurrently, which would lead to data corruption.
class DiskManager {
public:
    DiskManager() = default;
    ~DiskManager();

    // Non-copyable, non-movable (owns file descriptors).
    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;
    DiskManager(DiskManager&&) = delete;
    DiskManager& operator=(DiskManager&&) = delete;

    /// Create a new database file with a valid header. Pre-allocates 1MB.
    /// Fails with ALREADY_EXISTS if the file already exists and overwrite
    /// is false. If overwrite is true, the existing file is truncated.
    /// If direct_io is true, bypasses the OS page cache (F_NOCACHE on macOS,
    /// O_DIRECT on Linux).
    [[nodiscard]] Result<FileId>
    create_file(const std::filesystem::path& path, bool direct_io = false, bool overwrite = false);

    /// Open an existing database file, validating magic and version.
    /// If direct_io is true, bypasses the OS page cache.
    [[nodiscard]] Result<FileId> open_file(const std::filesystem::path& path,
                                           bool direct_io = false);

    /// Open an existing database file in read-only mode.
    /// Uses a shared advisory lock (LOCK_SH) instead of exclusive, allowing
    /// concurrent readers. Write operations (write_page, allocate_page) will
    /// fail on read-only files. Used by standby servers for user table access.
    [[nodiscard]] Result<FileId> open_file_readonly(const std::filesystem::path& path);

    /// Re-open a read-only file in read/write mode.
    /// Closes the read-only fd (releasing the shared lock), re-opens with O_RDWR,
    /// and acquires an exclusive advisory lock. Used during standby promotion to
    /// switch data files from read-only to writable without changing the FileId.
    [[nodiscard]] Result<void> reopen_file_readwrite(FileId file_id);

    /// Close a file handle. The FileId may be reused by future open/create calls.
    [[nodiscard]] Result<void> close_file(FileId file_id);

    /// Read page from disk with CRC32C verification.
    /// page_id must be >= 1 and < file page count.
    [[nodiscard]] Result<void> read_page(FileId file_id, PageId page_id, Page& page);

    /// Write page to disk. Computes CRC32C and stores in page checksum field.
    /// page_id must be >= 1 and < file page count.
    [[nodiscard]] Result<void> write_page(FileId file_id, PageId page_id, Page& page);

    /// Allocate a new page, extending the file if needed. Returns page ID >= 1.
    [[nodiscard]] Result<PageId> allocate_page(FileId file_id);

    /// Allocate multiple pages in a single operation. More efficient than
    /// calling allocate_page() in a loop because the file header is written
    /// only once. Returns the first page ID of the allocated range.
    /// Allocated pages are [first_id, first_id + count).
    [[nodiscard]] Result<PageId> allocate_pages(FileId file_id, uint32_t count);

    /// Return total allocated page count (including header page 0).
    [[nodiscard]] Result<uint32_t> file_page_count(FileId file_id) const;

    /// Read a uint64_t from the file header extension area (bytes 16+ of page 0).
    /// @param offset Byte offset relative to fh_ext_offset.
    [[nodiscard]] Result<uint64_t> read_header_ext_u64(FileId file_id, size_t offset);

    /// Write a uint64_t to the file header extension area and update the checksum.
    /// @param offset Byte offset relative to fh_ext_offset.
    [[nodiscard]] Result<void> write_header_ext_u64(FileId file_id, size_t offset, uint64_t value);

    // -- Crash safety ---------------------------------------------------------

    /// Full sync: flush all data and metadata to persistent storage.
    /// Uses F_FULLFSYNC on macOS (true durability) or fsync on Linux.
    /// Use for WAL writes where durability is critical.
    [[nodiscard]] Result<void> sync_file(FileId file_id);

    /// Data-only sync: flush data but may skip metadata updates.
    /// Uses fsync on macOS or fdatasync on Linux.
    /// Use for data files where metadata sync can be deferred.
    [[nodiscard]] Result<void> sync_data(FileId file_id);

private:
    struct OpenFile {
        int fd = -1;
        std::filesystem::path path;
        uint32_t page_count = 0; ///< Allocated pages including header page 0.
        bool direct_io = false;  ///< Whether OS page cache is bypassed.
        bool read_only = false;  ///< Whether the file was opened in read-only mode.
    };

    /// Acquire the next available FileId, reusing closed slots when possible.
    FileId acquire_file_id();

    [[nodiscard]] Result<OpenFile*> get_open_file(FileId file_id);
    [[nodiscard]] Result<const OpenFile*> get_open_file(FileId file_id) const;
    [[nodiscard]] Result<void> write_file_header(OpenFile& file);
    [[nodiscard]] Result<void> read_file_header(OpenFile& file);
    [[nodiscard]] Result<void> ensure_file_size(OpenFile& file, uint32_t needed_pages);

    std::vector<OpenFile> files_;
    std::vector<FileId> free_ids_; ///< Reusable FileId slots from closed files.
    FileId next_file_id_ = 0;
};

} // namespace sixseven
