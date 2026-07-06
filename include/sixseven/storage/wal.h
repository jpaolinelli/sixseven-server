#pragma once

#include "sixseven/common/result.h"
#include "sixseven/storage/wal_record.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace sixseven {

/// Default WAL segment size: 16MB.
inline constexpr size_t wal_default_segment_size = size_t{16} * 1024 * 1024;

/// Configuration options for the WAL writer.
struct WalWriterOptions {
    /// Maximum segment file size in bytes.
    size_t segment_size = wal_default_segment_size;

    /// Group commit flush interval.
    std::chrono::milliseconds flush_interval = std::chrono::milliseconds(10);

    /// Whether to start the group commit thread automatically on open.
    bool enable_group_commit = true;
};

// -- WAL Writer ---------------------------------------------------------------

/// Appends WAL records to segmented files with group commit support.
///
/// WAL segments are named `wal_000001`, `wal_000002`, etc. When the current
/// segment reaches the configured size limit, the writer rotates to a new
/// segment file.
///
/// Group commit: the writer buffers writes to the OS page cache and a
/// background thread periodically fsyncs the current segment. Callers can
/// also call flush() explicitly for immediate durability.
///
/// Thread-safe: all public methods are protected by a mutex.
class WalWriter {
public:
    explicit WalWriter(std::filesystem::path wal_dir, WalWriterOptions options = {});
    ~WalWriter();

    // Non-copyable, non-movable.
    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;
    WalWriter(WalWriter&&) = delete;
    WalWriter& operator=(WalWriter&&) = delete;

    /// Open the WAL directory for writing. Creates the directory and first
    /// segment if they don't exist. If segments exist, opens the latest one
    /// and scans it to resume from the last valid record.
    [[nodiscard]] Result<void> open();

    /// Append a WAL record. Assigns the next LSN to the record, serializes
    /// it, and writes to the current segment. Rotates to a new segment if
    /// the current one is full.
    /// Thread-safe.
    [[nodiscard]] Result<lsn_t> append(WalRecord& record);

    /// Fsync the current segment to ensure durability of all records
    /// written so far.
    /// Thread-safe.
    [[nodiscard]] Result<void> flush();

    /// Block until all records up to and including `lsn` are durably
    /// flushed (flushed_lsn() >= lsn).
    ///
    /// Group-commit aware: the caller registers `lsn` as a pending flush
    /// watermark, wakes the background flush thread immediately, and blocks
    /// on a condition variable until the flush thread's next fsync covers
    /// the requested LSN. Many concurrent waiters share a single fsync.
    ///
    /// Fast path: returns immediately if `lsn` is already durable.
    /// If group commit is disabled (flush thread not running), falls back
    /// to a synchronous flush().
    ///
    /// Errors:
    /// - INVALID_ARGUMENT if the writer is not open, or if `lsn` has not
    ///   been appended yet (it could never become durable).
    /// - IO_ERROR if the flush thread's fsync fails while waiting, or if
    ///   the writer is shut down before `lsn` became durable.
    /// Thread-safe. No busy-waiting.
    [[nodiscard]] Result<void> flush_until(lsn_t lsn);

    /// Return the next LSN that will be assigned.
    [[nodiscard]] lsn_t current_lsn() const;

    /// Return the highest LSN that has been durably flushed to disk.
    [[nodiscard]] lsn_t flushed_lsn() const;

    /// Return the current segment ID.
    [[nodiscard]] uint64_t current_segment_id() const;

    /// Close the WAL writer. Flushes remaining data, stops the group
    /// commit thread, and closes the segment file.
    [[nodiscard]] Result<void> close();

    /// Write a CHECKPOINT record to the WAL. The checkpoint data payload
    /// encodes the list of active transaction IDs so recovery knows which
    /// transactions were in-flight at the time of the checkpoint.
    /// Returns the LSN assigned to the checkpoint record.
    ///
    /// @note The caller is responsible for providing an accurate snapshot of
    /// active transactions. Because append() acquires latch_ and the caller
    /// must gather active txn IDs outside the latch, there is an inherent
    /// TOCTOU window. Callers should quiesce new transaction starts or use
    /// external synchronization to obtain a consistent snapshot.
    [[nodiscard]] Result<lsn_t> write_checkpoint(const std::vector<txn_id_t>& active_txns);

    /// Write a TXN_ID_WATERMARK record (GDB-1247). The payload is the
    /// ceiling of a reserved batch of transaction ids. Callers (namely
    /// TransactionManager) MUST call this -- and wait for it to durably
    /// flush -- BEFORE handing out any id in the newly reserved batch, so a
    /// crash can never leave a handed-out id above the persisted watermark.
    /// Returns the LSN assigned to the watermark record.
    [[nodiscard]] Result<lsn_t> write_txn_id_watermark(txn_id_t ceiling);

    /// Remove WAL segment files with IDs strictly less than min_segment_id.
    /// Used after a checkpoint to reclaim disk space from segments that are
    /// no longer needed for recovery.
    ///
    /// @note Thread-safe. Acquires latch_ internally. Must not be called
    /// while latch_ is already held by the same thread (e.g., from inside
    /// append or flush callbacks) — the mutex is not recursive.
    [[nodiscard]] Result<void> truncate_before(uint64_t min_segment_id);

    /// Callback type invoked after a segment rotation completes.
    /// Parameters: (completed_segment_id, last_lsn_in_segment).
    using OnSegmentRotated = std::function<void(uint64_t, lsn_t)>;

    /// Register a callback to be invoked after each segment rotation.
    /// The callback runs under the WAL writer's mutex — it should be
    /// lightweight (e.g., enqueue work to a background thread).
    void set_on_segment_rotated(OnSegmentRotated callback);

private:
    /// Open or create a segment file for writing.
    [[nodiscard]] Result<void> open_segment(uint64_t seg_id);

    /// Close the current segment file (truncate to written size + close fd).
    [[nodiscard]] Result<void> close_segment();

    /// Fsync and close current segment, open next segment.
    [[nodiscard]] Result<void> rotate_segment();

    /// Scan the current open segment to find the write offset and last LSN.
    [[nodiscard]] Result<void> scan_segment();

    /// Generate a segment file path: wal_dir / "wal_NNNNNN".
    [[nodiscard]] std::filesystem::path segment_path(uint64_t seg_id) const;

    /// Find the highest segment ID in the WAL directory (0 if none exist).
    [[nodiscard]] uint64_t find_latest_segment() const;

    /// Start the group commit background thread.
    void start_group_commit();

    /// Stop the group commit background thread.
    void stop_group_commit();

    /// Background flush thread entry point.
    void flush_loop();

    std::filesystem::path wal_dir_;
    WalWriterOptions options_;

    mutable std::mutex latch_;
    int segment_fd_ = -1;
    uint64_t segment_id_ = 0;
    size_t segment_offset_ = 0;
    lsn_t next_lsn_ = 1;
    std::atomic<lsn_t> flushed_lsn_{0};
    bool is_open_ = false;

    // Group commit.
    std::thread flush_thread_;
    std::mutex flush_mutex_;
    std::condition_variable flush_cv_;
    std::atomic<bool> flush_running_{false};

    // Durability waiters (flush_until). All guarded by flush_mutex_.
    /// Highest LSN any waiter is currently waiting on. The flush loop wakes
    /// early (without waiting for the timer) while this exceeds flushed_lsn_.
    lsn_t pending_flush_lsn_ = 0;
    /// Incremented each time the flush thread's fsync fails, so waiters can
    /// detect failures that happened while they were blocked.
    uint64_t flush_error_epoch_ = 0;
    /// Details of the most recent flush thread fsync failure.
    StatusCode last_flush_error_code_ = StatusCode::OK;
    std::string last_flush_error_message_;
    /// Signaled after every flush thread fsync (success or failure) and on
    /// shutdown; waiters in flush_until() block on this.
    std::condition_variable flushed_cv_;

    // Segment rotation callback.
    OnSegmentRotated on_segment_rotated_;
};

// -- WAL Reader ---------------------------------------------------------------

/// Reads WAL records sequentially across all segments in a WAL directory.
///
/// The reader opens segment files in order (wal_000001, wal_000002, ...) and
/// returns records one at a time via next(). When the current segment is
/// exhausted, it automatically advances to the next segment.
///
/// @note Memory: the reader loads one segment at a time into an internal
/// buffer. Peak memory usage is proportional to the largest segment file
/// (up to segment_size bytes, default 16 MB).
///
/// Usage:
/// ```
///   WalReader reader(wal_dir);
///   reader.open();
///   while (auto result = reader.next()) {
///       process(result.value());
///   }
///   reader.close();
/// ```
class WalReader {
public:
    explicit WalReader(std::filesystem::path wal_dir);
    ~WalReader();

    // Non-copyable, non-movable.
    WalReader(const WalReader&) = delete;
    WalReader& operator=(const WalReader&) = delete;
    WalReader(WalReader&&) = delete;
    WalReader& operator=(WalReader&&) = delete;

    /// Open the WAL directory for reading, starting from the given segment.
    /// Segment IDs start at 1. If start_segment is 0, starts from the lowest
    /// available segment.
    [[nodiscard]] Result<void> open(uint64_t start_segment = 0);

    /// Read the next WAL record. Returns NOT_FOUND when all records have
    /// been read across all segments. Automatically advances to the next
    /// segment file when the current one is exhausted.
    [[nodiscard]] Result<WalRecord> next();

    /// Close the reader and release resources.
    [[nodiscard]] Result<void> close();

    /// Return the segment ID currently being read.
    [[nodiscard]] uint64_t current_segment_id() const;

private:
    /// Find all segment IDs in the directory, sorted ascending.
    [[nodiscard]] std::vector<uint64_t> find_segments() const;

    /// Load the next segment file into the read buffer.
    [[nodiscard]] Result<void> load_segment(uint64_t seg_id);

    /// Generate a segment file path: wal_dir / "wal_NNNNNN".
    [[nodiscard]] std::filesystem::path segment_path(uint64_t seg_id) const;

    std::filesystem::path wal_dir_;
    bool is_open_ = false;

    /// All segment IDs in order.
    std::vector<uint64_t> segments_;
    /// Index into segments_ for the current segment being read.
    size_t seg_index_ = 0;

    /// Current segment's data loaded into memory.
    std::vector<uint8_t> buf_;
    /// Read offset within buf_.
    size_t read_offset_ = 0;
};

} // namespace sixseven
