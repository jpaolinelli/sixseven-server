#include "sixseven/storage/wal.h"

#include "sixseven/common/logging.h"
#include "sixseven/storage/wal_record.h"
#include "sixseven/storage/wal_recovery.h" // serialize_checkpoint_data()

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace sixseven {

// -- Helpers ------------------------------------------------------------------

namespace {

/// Try to parse a WAL segment filename ("wal_NNNNNN") and return the segment
/// ID, or 0 if the filename does not match the expected pattern.
uint64_t parse_segment_filename(const std::string& filename) {
    if (filename.size() != 10 || filename.substr(0, 4) != "wal_") {
        return 0;
    }
    auto num_str = filename.substr(4);
    bool all_digits =
        std::all_of(num_str.begin(), num_str.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (!all_digits) {
        return 0;
    }
    return std::stoull(num_str);
}

/// List all WAL segment IDs found in a directory, sorted ascending.
std::vector<uint64_t> list_wal_segments(const std::filesystem::path& wal_dir) {
    std::vector<uint64_t> segments;
    std::error_code ec;
    if (!std::filesystem::exists(wal_dir, ec)) {
        return segments;
    }
    for (const auto& entry : std::filesystem::directory_iterator(wal_dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        uint64_t seg_id = parse_segment_filename(entry.path().filename().string());
        if (seg_id > 0) {
            segments.push_back(seg_id);
        }
    }
    std::sort(segments.begin(), segments.end());
    return segments;
}

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
            SIXSEVEN_LOG_ERROR("WalWriter destructor close failed: {}", result.error().message);
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

    SIXSEVEN_LOG_INFO("WAL writer opened: dir={}, segment={}, next_lsn={}",
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

    // Final fsync to flush any data written after the group commit thread's
    // last flush. This may double-fsync if the thread just flushed, which is
    // harmless — fsync on already-synced data is a no-op on most kernels.
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

    SIXSEVEN_LOG_INFO("WAL writer closed: dir={}", wal_dir_.string());

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

    // Reject records that are larger than the segment size — they can never
    // fit into any single segment and would cause an infinite rotation loop.
    if (buf.size() > options_.segment_size) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "WAL record too large (" + std::to_string(buf.size()) +
                              " bytes) for segment size (" + std::to_string(options_.segment_size) +
                              " bytes)");
    }

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

    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
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

    // Remember the completed segment ID and last LSN before closing.
    uint64_t completed_segment = segment_id_;
    lsn_t completed_lsn = next_lsn_ > 0 ? next_lsn_ - 1 : 0;

    auto result = close_segment();
    if (!result) {
        return result;
    }

    uint64_t next_seg = completed_segment + 1;

    SIXSEVEN_LOG_INFO("WAL rotating to segment {}", next_seg);

    // Notify the archive callback (e.g., to enqueue async archival).
    if (on_segment_rotated_) {
        on_segment_rotated_(completed_segment, completed_lsn);
    }

    return open_segment(next_seg);
}

void WalWriter::set_on_segment_rotated(OnSegmentRotated callback) {
    std::lock_guard<std::mutex> lock(latch_);
    on_segment_rotated_ = std::move(callback);
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
            SIXSEVEN_LOG_WARN("WAL scan: incomplete record at offset {}, truncating", offset);
            break;
        }

        // Try to deserialize to validate the CRC.
        auto record_span = std::span<const uint8_t>(buf.data() + offset, total_record_size);
        auto result = deserialize_wal_record(record_span);
        if (!result) {
            // Invalid record — stop scanning here.
            SIXSEVEN_LOG_WARN(
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

    SIXSEVEN_LOG_INFO(
        "WAL scan complete: segment={}, offset={}, last_lsn={}", segment_id_, offset, last_lsn);

    return ok();
}

std::filesystem::path WalWriter::segment_path(uint64_t seg_id) const {
    std::ostringstream oss;
    oss << "wal_" << std::setw(6) << std::setfill('0') << seg_id;
    return wal_dir_ / oss.str();
}

uint64_t WalWriter::find_latest_segment() const {
    auto segments = list_wal_segments(wal_dir_);
    return segments.empty() ? 0 : segments.back();
}

// -- Checkpoint ---------------------------------------------------------------

Result<lsn_t> WalWriter::write_checkpoint(const std::vector<txn_id_t>& active_txns) {
    WalRecord record;
    record.type = WalRecordType::CHECKPOINT;
    record.txn_id = 0;
    record.data = serialize_checkpoint_data(active_txns);

    return append(record);
}

Result<void> WalWriter::truncate_before(uint64_t min_segment_id) {
    std::lock_guard<std::mutex> lock(latch_);

    auto segments = list_wal_segments(wal_dir_);
    for (uint64_t seg_id : segments) {
        if (seg_id >= min_segment_id) {
            break; // Segments are sorted — no more to remove.
        }
        std::error_code remove_ec;
        std::filesystem::remove(segment_path(seg_id), remove_ec);
        if (remove_ec) {
            return make_error(StatusCode::IO_ERROR,
                              "failed to remove WAL segment: " + segment_path(seg_id).string() +
                                  ": " + remove_ec.message());
        }
        SIXSEVEN_LOG_INFO("WAL truncated segment {}", seg_id);
    }

    return ok();
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
                SIXSEVEN_LOG_ERROR("WAL group commit flush failed: {}", result.error().message);
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

// -- WalReader ----------------------------------------------------------------

WalReader::WalReader(std::filesystem::path wal_dir) : wal_dir_(std::move(wal_dir)) {}

WalReader::~WalReader() {
    if (is_open_) {
        (void)close();
    }
}

Result<void> WalReader::open(uint64_t start_segment) {
    if (is_open_) {
        return make_error(StatusCode::INVALID_ARGUMENT, "WAL reader already open");
    }

    segments_ = find_segments();
    if (segments_.empty()) {
        // No segments — reader is open but next() will immediately return NOT_FOUND.
        is_open_ = true;
        return ok();
    }

    // If start_segment is specified, skip segments before it.
    if (start_segment > 0) {
        seg_index_ = 0;
        while (seg_index_ < segments_.size() && segments_[seg_index_] < start_segment) {
            ++seg_index_;
        }
    } else {
        seg_index_ = 0;
    }

    // Load the first segment.
    if (seg_index_ < segments_.size()) {
        auto result = load_segment(segments_[seg_index_]);
        if (!result) {
            return result;
        }
    }

    is_open_ = true;
    return ok();
}

Result<WalRecord> WalReader::next() {
    if (!is_open_) {
        return make_error(StatusCode::INVALID_ARGUMENT, "WAL reader not open");
    }

    while (true) {
        // Check if we have data remaining in the current buffer.
        if (!buf_.empty() && read_offset_ + sizeof(uint32_t) <= buf_.size()) {
            // Read the record_length prefix.
            uint32_t record_length = 0;
            std::memcpy(&record_length, buf_.data() + read_offset_, sizeof(uint32_t));

            // Zero record_length means end of valid data in this segment.
            if (record_length == 0) {
                // Fall through to advance to next segment.
            } else {
                size_t total_size = static_cast<size_t>(record_length) + sizeof(uint32_t);
                if (read_offset_ + total_size <= buf_.size()) {
                    auto span = std::span<const uint8_t>(buf_.data() + read_offset_, total_size);
                    auto result = deserialize_wal_record(span);
                    if (result) {
                        read_offset_ += total_size;
                        return result;
                    }
                    // Invalid record — treat as end of segment.
                }
                // Incomplete record — treat as end of segment.
            }
        }

        // Advance to the next segment.
        ++seg_index_;
        if (seg_index_ >= segments_.size()) {
            return make_error(StatusCode::NOT_FOUND, "no more WAL records");
        }

        auto result = load_segment(segments_[seg_index_]);
        if (!result) {
            return tl::unexpected(result.error());
        }
    }
}

Result<void> WalReader::close() {
    buf_.clear();
    segments_.clear();
    seg_index_ = 0;
    read_offset_ = 0;
    is_open_ = false;
    return ok();
}

uint64_t WalReader::current_segment_id() const {
    if (seg_index_ < segments_.size()) {
        return segments_[seg_index_];
    }
    return 0;
}

std::vector<uint64_t> WalReader::find_segments() const {
    return list_wal_segments(wal_dir_);
}

Result<void> WalReader::load_segment(uint64_t seg_id) {
    auto path = segment_path(seg_id);

    // Get file size.
    std::error_code ec;
    auto file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to get WAL segment size: " + path.string() + ": " + ec.message());
    }

    if (file_size == 0) {
        buf_.clear();
        read_offset_ = 0;
        return ok();
    }

    // Read the entire segment into memory.
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to open WAL segment for reading: " + path.string() + ": " +
                              std::strerror(errno));
    }

    buf_.resize(file_size);
    ssize_t bytes_read = ::pread(fd, buf_.data(), file_size, 0);
    ::close(fd);

    if (bytes_read < 0 || static_cast<size_t>(bytes_read) != file_size) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to read WAL segment: " + path.string() + ": " +
                              std::strerror(errno));
    }

    read_offset_ = 0;
    return ok();
}

std::filesystem::path WalReader::segment_path(uint64_t seg_id) const {
    std::ostringstream oss;
    oss << "wal_" << std::setw(6) << std::setfill('0') << seg_id;
    return wal_dir_ / oss.str();
}

} // namespace sixseven
