#pragma once

#include "sixseven/common/result.h"
#include "sixseven/storage/wal_record.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace sixseven {

/// Cleanup policy for archived WAL segments.
enum class ArchiveCleanupPolicy {
    KEEP_ALL,       ///< Never automatically remove archived segments.
    KEEP_LAST_N,    ///< Keep only the last N archived segments.
    KEEP_SINCE_LSN, ///< Keep segments containing records >= a given LSN.
};

/// Parse a cleanup policy string ("keep_all", "keep_last_n", "keep_since_lsn").
/// Returns KEEP_ALL if the string is unrecognized.
ArchiveCleanupPolicy parse_cleanup_policy(const std::string& policy_str);

/// Return the string representation of a cleanup policy.
const char* cleanup_policy_name(ArchiveCleanupPolicy policy);

/// Configuration for the WAL archive manager.
struct WalArchiveOptions {
    /// Whether archiving is enabled.
    bool enabled = false;

    /// Cleanup policy for old archived segments.
    ArchiveCleanupPolicy cleanup_policy = ArchiveCleanupPolicy::KEEP_ALL;

    /// For KEEP_LAST_N policy: how many segments to retain.
    uint64_t keep_last_n = 10;
};

/// Manages WAL segment archiving for replication.
///
/// Completed WAL segments are copied from the WAL directory to an archive
/// directory asynchronously via a background thread. This avoids blocking
/// the write path. Each archived segment is verified with a checksum to
/// ensure integrity.
///
/// Thread-safe: all public methods are protected by a mutex.
class WalArchiveManager {
public:
    /// Construct an archive manager.
    /// @param wal_dir      The WAL directory (source of completed segments).
    /// @param archive_dir  The archive directory (destination for copies).
    /// @param options       Configuration options.
    WalArchiveManager(std::filesystem::path wal_dir,
                      std::filesystem::path archive_dir,
                      WalArchiveOptions options = {});
    ~WalArchiveManager();

    // Non-copyable, non-movable.
    WalArchiveManager(const WalArchiveManager&) = delete;
    WalArchiveManager& operator=(const WalArchiveManager&) = delete;
    WalArchiveManager(WalArchiveManager&&) = delete;
    WalArchiveManager& operator=(WalArchiveManager&&) = delete;

    /// Start the archive manager. Creates the archive directory if needed
    /// and starts the background archival thread.
    [[nodiscard]] Result<void> start();

    /// Stop the archive manager. Drains the pending queue and joins the
    /// background thread.
    [[nodiscard]] Result<void> stop();

    /// Enqueue a completed WAL segment for archival.
    /// The segment is identified by its segment number; the manager resolves
    /// the path from the WAL directory. Archival happens asynchronously.
    /// @param segment_id  The segment number to archive.
    /// @param last_lsn    The last LSN contained in this segment.
    void enqueue_segment(uint64_t segment_id, lsn_t last_lsn);

    /// Archive a single segment synchronously (for testing or manual use).
    /// Copies the segment from the WAL directory to the archive directory
    /// with checksum verification.
    [[nodiscard]] Result<void> archive_segment(uint64_t segment_id, lsn_t last_lsn);

    /// List archived segment IDs in ascending order.
    [[nodiscard]] Result<std::vector<uint64_t>> list_archived_segments() const;

    /// Get the full path to an archived segment by segment number.
    [[nodiscard]] Result<std::filesystem::path> get_archived_segment(uint64_t segment_number) const;

    /// Set a callback that returns the minimum LSN that must be retained
    /// for replication slots. cleanup_before() will never remove segments
    /// needed by any active slot.
    void set_retention_lsn_provider(std::function<lsn_t()> provider);

    /// Remove archived segments older than the given LSN.
    /// If a retention LSN provider is set, the effective cleanup LSN is
    /// clamped to not exceed the provider's minimum.
    [[nodiscard]] Result<void> cleanup_before(lsn_t lsn);

    /// Remove archived segments, keeping only the last N.
    [[nodiscard]] Result<void> cleanup_keep_last_n(uint64_t n);

    /// Return the last archived LSN for replication coordination.
    [[nodiscard]] lsn_t last_archived_lsn() const;

    /// Return whether the archive manager is running.
    [[nodiscard]] bool is_running() const;

private:
    /// Background thread entry point: processes the archive queue.
    void archive_loop();

    /// Apply the configured cleanup policy after a successful archive.
    /// Called from archive_loop() after each segment is successfully archived.
    /// Logs a warning on cleanup failure but does NOT propagate the error —
    /// a cleanup failure is non-fatal (the archive copy already succeeded).
    void apply_cleanup_policy();

    /// Copy a WAL segment file to the archive directory with verification.
    [[nodiscard]] Result<void> copy_and_verify(uint64_t segment_id);

    /// Generate the WAL segment path: wal_dir / "wal_NNNNNN".
    [[nodiscard]] std::filesystem::path wal_segment_path(uint64_t seg_id) const;

    /// Generate the archive segment path: archive_dir / "wal_NNNNNN".
    [[nodiscard]] std::filesystem::path archive_segment_path(uint64_t seg_id) const;

    /// Compute a CRC32C checksum of a file's contents.
    [[nodiscard]] Result<uint32_t> file_checksum(const std::filesystem::path& path) const;

    std::filesystem::path wal_dir_;
    std::filesystem::path archive_dir_;
    WalArchiveOptions options_;

    /// Optional callback returning the minimum LSN needed by replication slots.
    std::function<lsn_t()> retention_lsn_provider_;

    /// Protects the pending queue.
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    /// Items pending archival: (segment_id, last_lsn).
    struct ArchiveItem {
        uint64_t segment_id;
        lsn_t last_lsn;
    };
    std::queue<ArchiveItem> pending_;

    /// Background archival thread.
    std::thread archive_thread_;
    std::atomic<bool> running_{false};

    /// The highest LSN that has been successfully archived.
    std::atomic<lsn_t> last_archived_lsn_{0};
};

} // namespace sixseven
