#include "giodb/storage/wal.h"

#include "giodb/common/logging.h"
#include "giodb/storage/wal_record.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <regex>
#include <sstream>

namespace giodb {

// -- Helpers ------------------------------------------------------------------

namespace {

/// Fsync a file descriptor using the best available mechanism.
/// On macOS, F_FULLFSYNC flushes the drive's write cache; on Linux, fsync.
Result<void> fsync_fd(int fd) {
#ifdef __APPLE__
    if (::fcntl(fd, F_FULLFSYNC) < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "F_FULLFSYNC failed: " + std::string(std::strerror(errno)));
    }
#else
    if (::fsync(fd) < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "fsync failed: " + std::string(std::strerror(errno)));
    }
#endif
    return ok();
}

} // namespace

// -- WalWriter ----------------------------------------------------------------

WalWriter::WalWriter(std::filesystem::path wal_dir, WalWriterOptions options)
    : wal_dir_(std::move(wal_dir)), options_(options) {}

WalWriter::~WalWriter() {
    if (is_open_) {
        // Best-effort close: ignore errors in the destructor.
        auto result = close();
        if (!result) {
            GIODB_LOG_ERROR("WalWriter destructor close failed: {}", result.error().message);
        }
    }
}

// -- Open / Close -------------------------------------------------------------

Result<void> WalWriter::open() {
    std::lock_guard<std::mutex> lock(latch_);

    if (is_open_) {
        return make_error(StatusCode::INVALID_ARGUMENT, "WAL writer already open");
    }

    // Create the WAL directory if it doesn't exist.
    std::error_code ec;
    std::filesystem::create_directories(wal_dir_, ec);
    if (ec) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to create WAL directory: " + wal_dir_.string() + ": " +
                              ec.message());
    }

    // Find the latest segment (0 if none exist).
    uint64_t latest = find_latest_segment();

    if (latest == 0) {
        // No segments exist — start from segment 1.
        auto result = open_segment(1);
        if (!result) {
            return result;
        }
    } else {
        // Open the latest segment and scan for the last valid record.
        auto result = open_segment(latest);
        if (!result) {
            return result;
        }
        auto scan_result = scan_segment();
        if (!scan_result) {
            return scan_result;
        }
    }

    is_open_ = true;

    GIODB_LOG_INFO("WAL writer opened: dir={}, segment={}, next_lsn={}",
                   wal_dir_.string(),
                   segment_id_,
                   next_lsn_);

    // Start group commit thread if configured.
    if (options_.enable_group_commit) {
        start_group_commit();
    }

    return ok();
}

Result<void> WalWriter::close() {
    // Stop group commit thread first (must not hold latch_ during join).
    stop_group_commit();

    std::lock_guard<std::mutex> lock(latch_);

    if (!is_open_) {
        return ok(); // Already closed.
    }

    // Flush remaining data.
    if (segment_fd_ >= 0) {
        auto result = fsync_fd(segment_fd_);
        if (!result) {
            return result;
        }
        flushed_lsn_.store(next_lsn_ > 0 ? next_lsn_ - 1 : 0, std::memory_order_release);
    }

    // Close the segment file.
    auto result = close_segment();
    if (!result) {
        return result;
    }

    is_open_ = false;

    GIODB_LOG_INFO("WAL writer closed: dir={}", wal_dir_.string());

    return ok();
}

// -- Append -------------------------------------------------------------------

Result<lsn_t> WalWriter::append(WalRecord& record) {
    std::lock_guard<std::mutex> lock(latch_);

    if (!is_open_) {
        return make_error(StatusCode::INVALID_ARGUMENT, "WAL writer not open");
    }

    // Assign the next LSN.
    record.lsn = next_lsn_;

    // Serialize the record.
    auto buf = serialize_wal_record(record);

    // Check if we need to rotate to a new segment.
    if (segment_offset_ + buf.size() > options_.segment_size) {
        auto result = rotate_segment();
        if (!result) {
            return tl::unexpected(result.error());
        }
    }

    // Write to the current segment using pwrite.
    auto offset = static_cast<off_t>(segment_offset_);
    ssize_t written = ::pwrite(segment_fd_, buf.data(), buf.size(), offset);
    if (written < 0 || static_cast<size_t>(written) != buf.size()) {
        return make_error(StatusCode::IO_ERROR,
                          "WAL pwrite failed: " + std::string(std::strerror(errno)));
    }

    segment_offset_ += buf.size();
    lsn_t assigned_lsn = next_lsn_;
    ++next_lsn_;

    return ok(assigned_lsn);
}

// -- Flush --------------------------------------------------------------------

Result<void> WalWriter::flush() {
    std::lock_guard<std::mutex> lock(latch_);

    if (!is_open_ || segment_fd_ < 0) {
        return ok(); // Nothing to flush.
    }

    auto result = fsync_fd(segment_fd_);
    if (!result) {
        return result;
    }

    // Update the flushed LSN to the latest written LSN.
    lsn_t latest = next_lsn_ > 0 ? next_lsn_ - 1 : 0;
    flushed_lsn_.store(latest, std::memory_order_release);

    return ok();
}

// -- Accessors ----------------------------------------------------------------

lsn_t WalWriter::current_lsn() const {
    std::lock_guard<std::mutex> lock(latch_);
    return next_lsn_;
}

lsn_t WalWriter::flushed_lsn() const {
    return flushed_lsn_.load(std::memory_order_acquire);
}

uint64_t WalWriter::current_segment_id() const {
    std::lock_guard<std::mutex> lock(latch_);
    return segment_id_;
}

// -- Segment management -------------------------------------------------------

Result<void> WalWriter::open_segment(uint64_t seg_id) {
    auto path = segment_path(seg_id);

    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to open WAL segment: " + path.string() + ": " +
                              std::strerror(errno));
    }

    segment_fd_ = fd;
    segment_id_ = seg_id;
    segment_offset_ = 0;

    return ok();
}

Result<void> WalWriter::close_segment() {
    if (segment_fd_ < 0) {
        return ok(); // No segment open.
    }

    // Truncate to the actual written size to avoid trailing garbage.
    if (segment_offset_ > 0) {
        if (::ftruncate(segment_fd_, static_cast<off_t>(segment_offset_)) < 0) {
            return make_error(StatusCode::IO_ERROR,
                              "WAL segment truncate failed: " + std::string(std::strerror(errno)));
        }
    }

    if (::close(segment_fd_) < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "WAL segment close failed: " + std::string(std::strerror(errno)));
    }

    segment_fd_ = -1;
    return ok();
}

Result<void> WalWriter::rotate_segment() {
    // Fsync the current segment before rotating.
    if (segment_fd_ >= 0) {
        auto result = fsync_fd(segment_fd_);
        if (!result) {
            return result;
        }
        flushed_lsn_.store(next_lsn_ > 0 ? next_lsn_ - 1 : 0, std::memory_order_release);
    }

    auto result = close_segment();
    if (!result) {
        return result;
    }

    uint64_t next_seg = segment_id_ + 1;

    GIODB_LOG_INFO("WAL rotating to segment {}", next_seg);

    return open_segment(next_seg);
}

Result<void> WalWriter::scan_segment() {
    if (segment_fd_ < 0) {
        return make_error(StatusCode::INTERNAL_ERROR, "no segment open for scanning");
    }

    // Get the file size.
    struct stat st = {};
    if (::fstat(segment_fd_, &st) < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "fstat failed on WAL segment: " + std::string(std::strerror(errno)));
    }

    auto file_size = static_cast<size_t>(st.st_size);
    if (file_size == 0) {
        // Empty segment — start from offset 0, keep current next_lsn_.
        segment_offset_ = 0;
        return ok();
    }

    // Read the entire segment into memory for scanning.
    std::vector<uint8_t> buf(file_size);
    ssize_t bytes_read = ::pread(segment_fd_, buf.data(), file_size, 0);
    if (bytes_read < 0 || static_cast<size_t>(bytes_read) != file_size) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to read WAL segment for scanning: " +
                              std::string(std::strerror(errno)));
    }

    // Iterate through records, validating each one.
    size_t offset = 0;
    lsn_t last_lsn = 0;

    while (offset + sizeof(uint32_t) <= file_size) {
        // Read the record_length prefix.
        uint32_t record_length = 0;
        std::memcpy(&record_length, buf.data() + offset, sizeof(uint32_t));

        // A zero record_length indicates end of written data (uninitialized region).
        if (record_length == 0) {
            break;
        }

        size_t total_record_size = static_cast<size_t>(record_length) + sizeof(uint32_t);
        if (offset + total_record_size > file_size) {
            // Incomplete record at end of file — treat as end of valid data.
            GIODB_LOG_WARN("WAL scan: incomplete record at offset {}, truncating", offset);
            break;
        }

        // Try to deserialize to validate the CRC.
        auto record_span = std::span<const uint8_t>(buf.data() + offset, total_record_size);
        auto result = deserialize_wal_record(record_span);
        if (!result) {
            // Invalid record — stop scanning here.
            GIODB_LOG_WARN(
                "WAL scan: invalid record at offset {}: {}", offset, result.error().message);
            break;
        }

        last_lsn = result->lsn;
        offset += total_record_size;
    }

    segment_offset_ = offset;

    if (last_lsn > 0) {
        next_lsn_ = last_lsn + 1;
        flushed_lsn_.store(last_lsn, std::memory_order_release);
    }

    GIODB_LOG_INFO(
        "WAL scan complete: segment={}, offset={}, last_lsn={}", segment_id_, offset, last_lsn);

    return ok();
}

std::filesystem::path WalWriter::segment_path(uint64_t seg_id) const {
    std::ostringstream oss;
    oss << "wal_" << std::setw(6) << std::setfill('0') << seg_id;
    return wal_dir_ / oss.str();
}

uint64_t WalWriter::find_latest_segment() const {
    uint64_t latest = 0;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(wal_dir_, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        auto filename = entry.path().filename().string();

        // Match "wal_NNNNNN" pattern.
        if (filename.size() != 10 || filename.substr(0, 4) != "wal_") {
            continue;
        }

        // Parse the segment number.
        auto num_str = filename.substr(4);
        bool all_digits = std::all_of(
            num_str.begin(), num_str.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (!all_digits) {
            continue;
        }

        uint64_t seg_id = std::stoull(num_str);
        if (seg_id > latest) {
            latest = seg_id;
        }
    }

    return latest;
}

// -- Group commit -------------------------------------------------------------

void WalWriter::start_group_commit() {
    bool expected = false;
    if (!flush_running_.compare_exchange_strong(expected, true)) {
        return; // Already running.
    }

    flush_thread_ = std::thread([this] { flush_loop(); });
}

void WalWriter::stop_group_commit() {
    bool expected = true;
    if (!flush_running_.compare_exchange_strong(expected, false)) {
        return; // Not running.
    }

    {
        std::lock_guard<std::mutex> lock(flush_mutex_);
        flush_cv_.notify_all();
    }

    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }
}

void WalWriter::flush_loop() {
    while (flush_running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(flush_mutex_);
            flush_cv_.wait_for(lock, options_.flush_interval, [this] {
                return !flush_running_.load(std::memory_order_acquire);
            });
        }

        // Flush the current segment under the main latch.
        std::lock_guard<std::mutex> lock(latch_);
        if (segment_fd_ >= 0 && is_open_) {
            auto result = fsync_fd(segment_fd_);
            if (result) {
                lsn_t latest = next_lsn_ > 0 ? next_lsn_ - 1 : 0;
                flushed_lsn_.store(latest, std::memory_order_release);
            } else {
                GIODB_LOG_ERROR("WAL group commit flush failed: {}", result.error().message);
            }
        }
    }

    // Final flush on shutdown.
    std::lock_guard<std::mutex> lock(latch_);
    if (segment_fd_ >= 0 && is_open_) {
        auto result = fsync_fd(segment_fd_);
        if (result) {
            lsn_t latest = next_lsn_ > 0 ? next_lsn_ - 1 : 0;
            flushed_lsn_.store(latest, std::memory_order_release);
        }
    }
}

} // namespace giodb
