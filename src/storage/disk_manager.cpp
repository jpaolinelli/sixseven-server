#include "giodb/storage/disk_manager.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>

// Hardware CRC32C support on ARM (Apple Silicon, ARMv8 with CRC extensions).
#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

namespace giodb {

// -- CRC32C implementation ----------------------------------------------------
// Uses the Castagnoli polynomial (bit-reversed: 0x82F63B78), standard for
// storage systems (iSCSI, ext4, Btrfs).
//
// When building on ARM with CRC extensions (e.g., Apple Silicon), the hardware
// __crc32c* intrinsics are used for significantly higher throughput.

namespace {

#if defined(__ARM_FEATURE_CRC32)

// -- Hardware CRC32C (ARM intrinsics) -----------------------------------------

/// Process a contiguous byte range using hardware CRC32C, accumulating into crc.
uint32_t crc32c_hw_accumulate(uint32_t crc, const uint8_t* data, size_t length) {
    // Process 8 bytes at a time.
    while (length >= 8) {
        uint64_t val = 0;
        std::memcpy(&val, data, sizeof(uint64_t));
        crc = __crc32cd(crc, val);
        data += 8;
        length -= 8;
    }
    // Process 4 bytes.
    if (length >= 4) {
        uint32_t val = 0;
        std::memcpy(&val, data, sizeof(uint32_t));
        crc = __crc32cw(crc, val);
        data += 4;
        length -= 4;
    }
    // Process remaining bytes one at a time.
    while (length > 0) {
        crc = __crc32cb(crc, *data);
        ++data;
        --length;
    }
    return crc;
}

/// Compute CRC32C over a buffer, treating bytes [skip_off, skip_off+skip_len) as zero.
uint32_t crc32c_skip_region(const uint8_t* data, size_t length, size_t skip_off, size_t skip_len) {
    uint32_t crc = 0xFFFFFFFF;
    // Segment before the skip region.
    crc = crc32c_hw_accumulate(crc, data, skip_off);
    // Skip region: feed zeros.
    std::array<uint8_t, 8> zeros{};
    size_t remaining = skip_len;
    while (remaining > 0) {
        size_t chunk = std::min(remaining, zeros.size());
        crc = crc32c_hw_accumulate(crc, zeros.data(), chunk);
        remaining -= chunk;
    }
    // Segment after the skip region.
    crc = crc32c_hw_accumulate(crc, data + skip_off + skip_len, length - skip_off - skip_len);
    return crc ^ 0xFFFFFFFF;
}

#else

// -- Software CRC32C (lookup table) -------------------------------------------

constexpr uint32_t crc32c_poly = 0x82F63B78;

constexpr std::array<uint32_t, 256> make_crc32c_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ crc32c_poly;
            } else {
                crc >>= 1;
            }
        }
        table[i] = crc;
    }
    return table;
}

constexpr auto crc32c_table = make_crc32c_table();

/// Compute CRC32C over a buffer, treating bytes [skip_off, skip_off+skip_len) as zero.
uint32_t crc32c_skip_region(const uint8_t* data, size_t length, size_t skip_off, size_t skip_len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        uint8_t byte = (i >= skip_off && i < skip_off + skip_len) ? 0 : data[i];
        crc = crc32c_table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

#endif // __ARM_FEATURE_CRC32

} // namespace

uint32_t crc32c(const uint8_t* data, size_t length) {
#if defined(__ARM_FEATURE_CRC32)
    uint32_t crc = 0xFFFFFFFF;
    crc = crc32c_hw_accumulate(crc, data, length);
    return crc ^ 0xFFFFFFFF;
#else
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc = crc32c_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
#endif
}

uint32_t compute_page_checksum(const Page& page) {
    return crc32c_skip_region(
        page.raw().data(), page_size, Page::checksum_offset, sizeof(uint32_t));
}

// -- DiskManager --------------------------------------------------------------

DiskManager::~DiskManager() {
    for (auto& file : files_) {
        if (file.fd >= 0) {
            ::close(file.fd);
            file.fd = -1;
        }
    }
}

FileId DiskManager::acquire_file_id() {
    if (!free_ids_.empty()) {
        FileId id = free_ids_.back();
        free_ids_.pop_back();
        return id;
    }
    FileId id = next_file_id_++;
    if (id >= files_.size()) {
        files_.resize(id + 1);
    }
    return id;
}

Result<FileId>
DiskManager::create_file(const std::filesystem::path& path, bool direct_io, bool overwrite) {
    int flags = O_RDWR | O_CREAT;
    if (overwrite) {
        flags |= O_TRUNC;
    } else {
        flags |= O_EXCL; // Fail if the file already exists.
    }
#if !defined(__APPLE__) && defined(O_DIRECT)
    if (direct_io) {
        flags |= O_DIRECT;
    }
#endif
    int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            return make_error(StatusCode::ALREADY_EXISTS, "file already exists: " + path.string());
        }
        return make_error(StatusCode::IO_ERROR,
                          "failed to create file: " + path.string() + ": " + std::strerror(errno));
    }

    // Acquire an exclusive advisory lock to prevent concurrent access.
    if (::flock(fd, LOCK_EX | LOCK_NB) < 0) {
        ::close(fd);
        return make_error(StatusCode::IO_ERROR,
                          "failed to lock file (already in use?): " + path.string());
    }

#ifdef __APPLE__
    // macOS: bypass OS page cache via F_NOCACHE.
    if (direct_io) {
        if (::fcntl(fd, F_NOCACHE, 1) < 0) {
            ::close(fd);
            return make_error(StatusCode::IO_ERROR,
                              "F_NOCACHE failed: " + std::string(std::strerror(errno)));
        }
    }
#endif

    FileId file_id = acquire_file_id();

    OpenFile& file = files_[file_id];
    file.fd = fd;
    file.path = path;
    file.page_count = 1; // Header page only.
    file.direct_io = direct_io;

    // Pre-allocate 1MB of space.
    auto extend_result = ensure_file_size(file, file_growth_pages);
    if (!extend_result) {
        ::close(fd);
        ::unlink(path.c_str());
        file.fd = -1;
        free_ids_.push_back(file_id);
        return tl::unexpected(extend_result.error());
    }

    // Write the file header to page 0.
    auto header_result = write_file_header(file);
    if (!header_result) {
        ::close(fd);
        ::unlink(path.c_str());
        file.fd = -1;
        free_ids_.push_back(file_id);
        return tl::unexpected(header_result.error());
    }

    return ok(file_id);
}

Result<FileId> DiskManager::open_file(const std::filesystem::path& path, bool direct_io) {
    int flags = O_RDWR;
#if !defined(__APPLE__) && defined(O_DIRECT)
    if (direct_io) {
        flags |= O_DIRECT;
    }
#endif
    int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to open file: " + path.string() + ": " + std::strerror(errno));
    }

    // Acquire an exclusive advisory lock to prevent concurrent access.
    if (::flock(fd, LOCK_EX | LOCK_NB) < 0) {
        ::close(fd);
        return make_error(StatusCode::IO_ERROR,
                          "failed to lock file (already in use?): " + path.string());
    }

#ifdef __APPLE__
    // macOS: bypass OS page cache via F_NOCACHE.
    if (direct_io) {
        if (::fcntl(fd, F_NOCACHE, 1) < 0) {
            ::close(fd);
            return make_error(StatusCode::IO_ERROR,
                              "F_NOCACHE failed: " + std::string(std::strerror(errno)));
        }
    }
#endif

    FileId file_id = acquire_file_id();

    OpenFile& file = files_[file_id];
    file.fd = fd;
    file.path = path;
    file.direct_io = direct_io;

    // Read and validate the file header.
    auto header_result = read_file_header(file);
    if (!header_result) {
        ::close(fd);
        file.fd = -1;
        free_ids_.push_back(file_id);
        return tl::unexpected(header_result.error());
    }

    return ok(file_id);
}

Result<void> DiskManager::close_file(FileId file_id) {
    auto file_result = get_open_file(file_id);
    if (!file_result) {
        return tl::unexpected(file_result.error());
    }
    OpenFile* file = *file_result;

    // flock is automatically released on close.
    if (::close(file->fd) < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to close file: " + file->path.string() + ": " +
                              std::strerror(errno));
    }

    file->fd = -1;
    free_ids_.push_back(file_id);
    return ok();
}

Result<void> DiskManager::read_page(FileId file_id, PageId page_id, Page& page) {
    auto file_result = get_open_file(file_id);
    if (!file_result) {
        return tl::unexpected(file_result.error());
    }
    OpenFile* file = *file_result;

    if (page_id == 0) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "page 0 is the file header and cannot be read as a data page");
    }
    if (page_id >= file->page_count) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "page ID " + std::to_string(page_id) + " out of range (file has " +
                              std::to_string(file->page_count) + " pages)");
    }

    auto offset = static_cast<off_t>(page_id) * static_cast<off_t>(page_size);
    ssize_t bytes_read = ::pread(file->fd, page.raw().data(), page_size, offset);
    if (bytes_read < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to read page " + std::to_string(page_id) + ": " +
                              std::strerror(errno));
    }
    if (static_cast<size_t>(bytes_read) != page_size) {
        return make_error(StatusCode::IO_ERROR,
                          "short read for page " + std::to_string(page_id) + ": expected " +
                              std::to_string(page_size) + " bytes, got " +
                              std::to_string(bytes_read));
    }

    // Verify CRC32C checksum.
    uint32_t stored = page.checksum();
    uint32_t computed = compute_page_checksum(page);
    if (stored != computed) {
        return make_error(StatusCode::IO_ERROR,
                          "checksum mismatch for page " + std::to_string(page_id) + ": stored=" +
                              std::to_string(stored) + " computed=" + std::to_string(computed));
    }

    return ok();
}

Result<void> DiskManager::write_page(FileId file_id, PageId page_id, Page& page) {
    auto file_result = get_open_file(file_id);
    if (!file_result) {
        return tl::unexpected(file_result.error());
    }
    OpenFile* file = *file_result;

    if (page_id == 0) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "page 0 is the file header and cannot be written as a data page");
    }
    if (page_id >= file->page_count) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "page ID " + std::to_string(page_id) + " out of range (file has " +
                              std::to_string(file->page_count) + " pages)");
    }

    // Compute and store CRC32C checksum.
    page.set_checksum(compute_page_checksum(page));

    auto offset = static_cast<off_t>(page_id) * static_cast<off_t>(page_size);
    ssize_t written = ::pwrite(file->fd, page.raw().data(), page_size, offset);
    if (written < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to write page " + std::to_string(page_id) + ": " +
                              std::strerror(errno));
    }
    if (static_cast<size_t>(written) != page_size) {
        return make_error(StatusCode::IO_ERROR,
                          "short write for page " + std::to_string(page_id) + ": expected " +
                              std::to_string(page_size) + " bytes, wrote " +
                              std::to_string(written));
    }

    return ok();
}

Result<PageId> DiskManager::allocate_page(FileId file_id) {
    return allocate_pages(file_id, 1);
}

Result<PageId> DiskManager::allocate_pages(FileId file_id, uint32_t count) {
    if (count == 0) {
        return make_error(StatusCode::INVALID_ARGUMENT, "page count must be greater than 0");
    }

    auto file_result = get_open_file(file_id);
    if (!file_result) {
        return tl::unexpected(file_result.error());
    }
    OpenFile* file = *file_result;

    PageId first_page_id = file->page_count;
    file->page_count += count;

    // Extend file if needed.
    auto extend_result = ensure_file_size(*file, file->page_count);
    if (!extend_result) {
        file->page_count -= count; // Roll back.
        return tl::unexpected(extend_result.error());
    }

    // Persist the updated page count in the file header (once for all pages).
    auto header_result = write_file_header(*file);
    if (!header_result) {
        file->page_count -= count;
        return tl::unexpected(header_result.error());
    }

    return ok(first_page_id);
}

Result<uint32_t> DiskManager::file_page_count(FileId file_id) const {
    auto file_result = get_open_file(file_id);
    if (!file_result) {
        return tl::unexpected(file_result.error());
    }
    return ok((*file_result)->page_count);
}

// -- Crash safety -------------------------------------------------------------

Result<void> DiskManager::sync_file(FileId file_id) {
    auto file_result = get_open_file(file_id);
    if (!file_result) {
        return tl::unexpected(file_result.error());
    }
    OpenFile* file = *file_result;

#ifdef __APPLE__
    // F_FULLFSYNC ensures data reaches persistent storage on macOS.
    // Unlike fsync, this flushes the drive's write cache to the platters.
    if (::fcntl(file->fd, F_FULLFSYNC) < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "F_FULLFSYNC failed: " + std::string(std::strerror(errno)));
    }
#else
    if (::fsync(file->fd) < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "fsync failed: " + std::string(std::strerror(errno)));
    }
#endif

    return ok();
}

Result<void> DiskManager::sync_data(FileId file_id) {
    auto file_result = get_open_file(file_id);
    if (!file_result) {
        return tl::unexpected(file_result.error());
    }
    OpenFile* file = *file_result;

#ifdef __APPLE__
    // macOS fsync is similar to Linux fdatasync: it flushes to the drive's
    // buffer but does not guarantee data reaches the platters.
    if (::fsync(file->fd) < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "fsync failed: " + std::string(std::strerror(errno)));
    }
#else
    // fdatasync skips metadata updates (e.g., access time), which is cheaper
    // than fsync for data file writes.
    if (::fdatasync(file->fd) < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "fdatasync failed: " + std::string(std::strerror(errno)));
    }
#endif

    return ok();
}

// -- Private helpers ----------------------------------------------------------

Result<DiskManager::OpenFile*> DiskManager::get_open_file(FileId file_id) {
    if (file_id >= files_.size() || files_[file_id].fd < 0) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid or closed file ID: " + std::to_string(file_id));
    }
    return ok(&files_[file_id]);
}

Result<const DiskManager::OpenFile*> DiskManager::get_open_file(FileId file_id) const {
    if (file_id >= files_.size() || files_[file_id].fd < 0) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid or closed file ID: " + std::to_string(file_id));
    }
    return ok(&files_[file_id]);
}

Result<void> DiskManager::write_file_header(OpenFile& file) {
    std::array<uint8_t, page_size> header{};

    // Write header fields.
    std::memcpy(&header[fh_magic_offset], &file_magic, sizeof(uint32_t));
    std::memcpy(&header[fh_version_offset], &file_format_version, sizeof(uint32_t));
    std::memcpy(&header[fh_page_count_offset], &file.page_count, sizeof(uint32_t));

    // Compute header checksum (excluding the checksum field itself).
    uint32_t cksum =
        crc32c_skip_region(header.data(), page_size, fh_checksum_offset, sizeof(uint32_t));
    std::memcpy(&header[fh_checksum_offset], &cksum, sizeof(uint32_t));

    // Write header page (page 0) to disk.
    ssize_t written = ::pwrite(file.fd, header.data(), page_size, 0);
    if (written < 0 || static_cast<size_t>(written) != page_size) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to write file header: " + std::string(std::strerror(errno)));
    }

    return ok();
}

Result<void> DiskManager::read_file_header(OpenFile& file) {
    std::array<uint8_t, page_size> header{};

    ssize_t bytes_read = ::pread(file.fd, header.data(), page_size, 0);
    if (bytes_read < 0 || static_cast<size_t>(bytes_read) != page_size) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to read file header: " + std::string(std::strerror(errno)));
    }

    // Validate magic number.
    uint32_t magic = 0;
    std::memcpy(&magic, &header[fh_magic_offset], sizeof(uint32_t));
    if (magic != file_magic) {
        return make_error(StatusCode::IO_ERROR,
                          "invalid file magic: expected " + std::to_string(file_magic) + ", got " +
                              std::to_string(magic));
    }

    // Validate version.
    uint32_t version = 0;
    std::memcpy(&version, &header[fh_version_offset], sizeof(uint32_t));
    if (version != file_format_version) {
        return make_error(StatusCode::IO_ERROR,
                          "unsupported file version: " + std::to_string(version));
    }

    // Verify header checksum.
    uint32_t stored_cksum = 0;
    std::memcpy(&stored_cksum, &header[fh_checksum_offset], sizeof(uint32_t));
    uint32_t computed_cksum =
        crc32c_skip_region(header.data(), page_size, fh_checksum_offset, sizeof(uint32_t));
    if (stored_cksum != computed_cksum) {
        return make_error(StatusCode::IO_ERROR, "file header checksum mismatch");
    }

    // Read page count.
    std::memcpy(&file.page_count, &header[fh_page_count_offset], sizeof(uint32_t));

    return ok();
}

Result<void> DiskManager::ensure_file_size(OpenFile& file, uint32_t needed_pages) {
    struct stat st{};
    if (::fstat(file.fd, &st) < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "fstat failed: " + std::string(std::strerror(errno)));
    }

    auto needed_size = static_cast<off_t>(needed_pages) * static_cast<off_t>(page_size);
    if (st.st_size >= needed_size) {
        return ok();
    }

    // Round up to the next 1MB (file_growth_pages * page_size) boundary.
    auto growth_chunk = static_cast<off_t>(file_growth_pages) * static_cast<off_t>(page_size);
    auto new_size = ((needed_size + growth_chunk - 1) / growth_chunk) * growth_chunk;

    if (::ftruncate(file.fd, new_size) < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to extend file: " + std::string(std::strerror(errno)));
    }

    return ok();
}

} // namespace giodb
