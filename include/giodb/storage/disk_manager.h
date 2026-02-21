#pragma once

#include "giodb/common/result.h"
#include "giodb/storage/page.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace giodb {

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

/// Magic number identifying GioDB database files.
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
    /// If direct_io is true, bypasses the OS page cache (F_NOCACHE on macOS,
    /// O_DIRECT on Linux).
    [[nodiscard]] Result<FileId> create_file(const std::filesystem::path& path,
                                             bool direct_io = false);

    /// Open an existing database file, validating magic and version.
    /// If direct_io is true, bypasses the OS page cache.
    [[nodiscard]] Result<FileId> open_file(const std::filesystem::path& path,
                                           bool direct_io = false);

    /// Close a file handle.
    [[nodiscard]] Result<void> close_file(FileId file_id);

    /// Read page from disk with CRC32C verification.
    /// page_id must be >= 1 and < file page count.
    [[nodiscard]] Result<void> read_page(FileId file_id, PageId page_id, Page& page);

    /// Write page to disk. Computes CRC32C and stores in page checksum field.
    /// page_id must be >= 1 and < file page count.
    [[nodiscard]] Result<void> write_page(FileId file_id, PageId page_id, Page& page);

    /// Allocate a new page, extending the file if needed. Returns page ID >= 1.
    [[nodiscard]] Result<PageId> allocate_page(FileId file_id);

    /// Return total allocated page count (including header page 0).
    [[nodiscard]] Result<uint32_t> file_page_count(FileId file_id) const;

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
    };

    [[nodiscard]] Result<OpenFile*> get_open_file(FileId file_id);
    [[nodiscard]] Result<const OpenFile*> get_open_file(FileId file_id) const;
    [[nodiscard]] Result<void> write_file_header(OpenFile& file);
    [[nodiscard]] Result<void> read_file_header(OpenFile& file);
    [[nodiscard]] Result<void> ensure_file_size(OpenFile& file, uint32_t needed_pages);

    std::vector<OpenFile> files_;
    FileId next_file_id_ = 0;
};

} // namespace giodb
