#include "sixseven/storage/wal_archive.h"

#include "sixseven/common/logging.h"
#include "sixseven/storage/disk_manager.h" // crc32c()

#include "sixseven/common/platform.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace sixseven {

// -- Cleanup policy helpers ---------------------------------------------------

ArchiveCleanupPolicy parse_cleanup_policy(const std::string& policy_str) {
    if (policy_str == "keep_last_n") {
        return ArchiveCleanupPolicy::KEEP_LAST_N;
    }
    if (policy_str == "keep_since_lsn") {
        return ArchiveCleanupPolicy::KEEP_SINCE_LSN;
    }
    return ArchiveCleanupPolicy::KEEP_ALL;
}

const char* cleanup_policy_name(ArchiveCleanupPolicy policy) {
    switch (policy) {
    case ArchiveCleanupPolicy::KEEP_ALL:
        return "keep_all";
    case ArchiveCleanupPolicy::KEEP_LAST_N:
        return "keep_last_n";
    case ArchiveCleanupPolicy::KEEP_SINCE_LSN:
        return "keep_since_lsn";
    }
    return "keep_all";
}

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
std::vector<uint64_t> list_segments_in_dir(const std::filesystem::path& dir) {
    std::vector<uint64_t> segments;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return segments;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
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

} // namespace

// -- WalArchiveManager --------------------------------------------------------

WalArchiveManager::WalArchiveManager(std::filesystem::path wal_dir,
                                     std::filesystem::path archive_dir,
                                     WalArchiveOptions options)
    : wal_dir_(std::move(wal_dir)), archive_dir_(std::move(archive_dir)), options_(options) {}

WalArchiveManager::~WalArchiveManager() {
    if (running_.load(std::memory_order_acquire)) {
        auto result = stop();
        if (!result) {
            SIXSEVEN_LOG_ERROR("WalArchiveManager destructor stop failed: {}", result.error().message);
        }
    }
}

Result<void> WalArchiveManager::start() {
    if (running_.load(std::memory_order_acquire)) {
        return make_error(StatusCode::INVALID_ARGUMENT, "archive manager already running");
    }

    // Create the archive directory if it doesn't exist.
    std::error_code ec;
    std::filesystem::create_directories(archive_dir_, ec);
    if (ec) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to create archive directory: " + archive_dir_.string() + ": " +
                              ec.message());
    }

    running_.store(true, std::memory_order_release);
    archive_thread_ = std::thread([this] { archive_loop(); });

    SIXSEVEN_LOG_INFO("WAL archive manager started: wal_dir={}, archive_dir={}",
                   wal_dir_.string(),
                   archive_dir_.string());

    return ok();
}

Result<void> WalArchiveManager::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return ok(); // Already stopped.
    }

    // Wake the background thread so it can exit.
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_cv_.notify_all();
    }

    if (archive_thread_.joinable()) {
        archive_thread_.join();
    }

    SIXSEVEN_LOG_INFO("WAL archive manager stopped");
    return ok();
}

void WalArchiveManager::enqueue_segment(uint64_t segment_id, lsn_t last_lsn) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_.push({segment_id, last_lsn});
    queue_cv_.notify_one();
}

Result<void> WalArchiveManager::archive_segment(uint64_t segment_id, lsn_t last_lsn) {
    auto result = copy_and_verify(segment_id);
    if (!result) {
        return result;
    }

    // Write metadata sidecar with the last LSN for this segment.
    // Used by cleanup_before() to determine which segments to remove.
    auto meta_path = archive_segment_path(segment_id);
    meta_path += ".meta";
    std::ofstream meta_file(meta_path);
    if (meta_file.is_open()) {
        meta_file << last_lsn;
    }

    // Update last archived LSN.
    lsn_t current = last_archived_lsn_.load(std::memory_order_acquire);
    while (last_lsn > current) {
        if (last_archived_lsn_.compare_exchange_weak(
                current, last_lsn, std::memory_order_release)) {
            break;
        }
    }

    SIXSEVEN_LOG_INFO("WAL segment {} archived, last_archived_lsn={}", segment_id, last_lsn);
    return ok();
}

Result<std::vector<uint64_t>> WalArchiveManager::list_archived_segments() const {
    return ok(list_segments_in_dir(archive_dir_));
}

Result<std::filesystem::path>
WalArchiveManager::get_archived_segment(uint64_t segment_number) const {
    auto path = archive_segment_path(segment_number);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return make_error(StatusCode::NOT_FOUND, "archived segment not found: " + path.string());
    }
    return ok(path);
}

void WalArchiveManager::set_retention_lsn_provider(std::function<lsn_t()> provider) {
    retention_lsn_provider_ = std::move(provider);
}

Result<void> WalArchiveManager::cleanup_before(lsn_t lsn) {
    // Clamp the cleanup LSN to respect replication slot retention.
    if (retention_lsn_provider_) {
        lsn_t min_slot_lsn = retention_lsn_provider_();
        if (min_slot_lsn != invalid_lsn && min_slot_lsn < lsn) {
            SIXSEVEN_LOG_INFO("WAL archive cleanup: clamping cleanup LSN from {} to {} "
                           "(replication slot retention)",
                           lsn,
                           min_slot_lsn);
            lsn = min_slot_lsn;
        }
    }

    auto segments = list_segments_in_dir(archive_dir_);
    if (segments.empty()) {
        return ok();
    }

    // We need to read each archived segment to determine its LSN range.
    // For simplicity, we use a heuristic: the segment's metadata is stored
    // in a sidecar file (<segment>.meta) containing the last LSN.
    // If no metadata exists, we skip the segment (safe default).
    for (uint64_t seg_id : segments) {
        auto meta_path = archive_segment_path(seg_id);
        meta_path += ".meta";

        std::error_code ec;
        if (!std::filesystem::exists(meta_path, ec)) {
            continue; // No metadata — cannot determine LSN, skip.
        }

        std::ifstream meta_file(meta_path);
        if (!meta_file.is_open()) {
            continue;
        }

        lsn_t seg_last_lsn = 0;
        meta_file >> seg_last_lsn;

        if (seg_last_lsn < lsn) {
            // Remove both the segment and its metadata.
            std::filesystem::remove(archive_segment_path(seg_id), ec);
            if (ec) {
                return make_error(StatusCode::IO_ERROR,
                                  "failed to remove archived segment: " + std::to_string(seg_id) +
                                      ": " + ec.message());
            }
            std::filesystem::remove(meta_path, ec);
            // Ignore error on metadata removal — non-critical.

            SIXSEVEN_LOG_INFO("WAL archive cleanup: removed segment {} (last_lsn={} < {})",
                           seg_id,
                           seg_last_lsn,
                           lsn);
        }
    }

    return ok();
}

Result<void> WalArchiveManager::cleanup_keep_last_n(uint64_t n) {
    auto segments = list_segments_in_dir(archive_dir_);
    if (segments.size() <= n) {
        return ok(); // Nothing to clean up.
    }

    // Remove oldest segments, keeping the last N.
    size_t to_remove = segments.size() - n;
    for (size_t i = 0; i < to_remove; ++i) {
        uint64_t seg_id = segments[i];
        std::error_code ec;

        std::filesystem::remove(archive_segment_path(seg_id), ec);
        if (ec) {
            return make_error(StatusCode::IO_ERROR,
                              "failed to remove archived segment: " + std::to_string(seg_id) +
                                  ": " + ec.message());
        }

        // Also remove metadata sidecar if present.
        auto meta_path = archive_segment_path(seg_id);
        meta_path += ".meta";
        std::filesystem::remove(meta_path, ec);

        SIXSEVEN_LOG_INFO("WAL archive cleanup: removed segment {} (keep_last_n={})", seg_id, n);
    }

    return ok();
}

lsn_t WalArchiveManager::last_archived_lsn() const {
    return last_archived_lsn_.load(std::memory_order_acquire);
}

bool WalArchiveManager::is_running() const {
    return running_.load(std::memory_order_acquire);
}

// -- Private ------------------------------------------------------------------

void WalArchiveManager::archive_loop() {
    while (running_.load(std::memory_order_acquire)) {
        ArchiveItem item{};
        bool has_item = false;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !pending_.empty() || !running_.load(std::memory_order_acquire);
            });

            if (!pending_.empty()) {
                item = pending_.front();
                pending_.pop();
                has_item = true;
            }
        }

        if (has_item) {
            auto result = archive_segment(item.segment_id, item.last_lsn);
            if (!result) {
                SIXSEVEN_LOG_ERROR("WAL archive failed for segment {}: {}",
                                item.segment_id,
                                result.error().message);
            }
        }
    }

    // Drain remaining items on shutdown.
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while (!pending_.empty()) {
        auto item = pending_.front();
        pending_.pop();

        auto result = archive_segment(item.segment_id, item.last_lsn);
        if (!result) {
            SIXSEVEN_LOG_ERROR("WAL archive (shutdown drain) failed for segment {}: {}",
                            item.segment_id,
                            result.error().message);
        }
    }
}

Result<void> WalArchiveManager::copy_and_verify(uint64_t segment_id) {
    auto src = wal_segment_path(segment_id);
    auto dst = archive_segment_path(segment_id);

    // Verify source exists.
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) {
        return make_error(StatusCode::NOT_FOUND,
                          "WAL segment not found for archiving: " + src.string());
    }

    // Get the source file size.
    auto src_size = std::filesystem::file_size(src, ec);
    if (ec) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to get WAL segment size: " + src.string() + ": " + ec.message());
    }

    if (src_size == 0) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "WAL segment is empty, cannot archive: " + src.string());
    }

    // Compute source checksum before copy.
    auto src_crc = file_checksum(src);
    if (!src_crc) {
        return tl::unexpected(src_crc.error());
    }

    // Copy the file.
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to copy WAL segment to archive: " + src.string() + " -> " +
                              dst.string() + ": " + ec.message());
    }

    // Verify the copy by computing checksum of the destination.
    auto dst_crc = file_checksum(dst);
    if (!dst_crc) {
        // Remove the bad copy.
        std::filesystem::remove(dst);
        return tl::unexpected(dst_crc.error());
    }

    if (*src_crc != *dst_crc) {
        // Checksum mismatch — remove the bad copy.
        std::filesystem::remove(dst);
        return make_error(StatusCode::IO_ERROR,
                          "archive checksum mismatch for segment " + std::to_string(segment_id) +
                              ": src=0x" + std::to_string(*src_crc) + " dst=0x" +
                              std::to_string(*dst_crc));
    }

    return ok();
}

std::filesystem::path WalArchiveManager::wal_segment_path(uint64_t seg_id) const {
    std::ostringstream oss;
    oss << "wal_" << std::setw(6) << std::setfill('0') << seg_id;
    return wal_dir_ / oss.str();
}

std::filesystem::path WalArchiveManager::archive_segment_path(uint64_t seg_id) const {
    std::ostringstream oss;
    oss << "wal_" << std::setw(6) << std::setfill('0') << seg_id;
    return archive_dir_ / oss.str();
}

Result<uint32_t> WalArchiveManager::file_checksum(const std::filesystem::path& path) const {
    // Read the entire file and compute CRC32C.
    std::error_code ec;
    auto file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to get file size for checksum: " + path.string() + ": " +
                              ec.message());
    }

    int fd = ::open(path.string().c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to open file for checksum: " + path.string() + ": " +
                              std::strerror(errno));
    }

    std::vector<uint8_t> buf(file_size);
    ssize_t bytes_read = sixseven_platform::pread(fd, buf.data(), file_size, 0);
    ::close(fd);

    if (bytes_read < 0 || static_cast<size_t>(bytes_read) != file_size) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to read file for checksum: " + path.string() + ": " +
                              std::strerror(errno));
    }

    return ok(crc32c(buf.data(), buf.size()));
}

} // namespace sixseven
