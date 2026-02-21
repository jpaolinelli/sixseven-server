#pragma once

#include "giodb/common/result.h"
#include "giodb/storage/wal_record.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <thread>

namespace giodb {

/// Default WAL segment size: 16MB.
static constexpr size_t wal_default_segment_size = 16 * 1024 * 1024;

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

    /// Return the next LSN that will be assigned.
    [[nodiscard]] lsn_t current_lsn() const;

    /// Return the highest LSN that has been durably flushed to disk.
    [[nodiscard]] lsn_t flushed_lsn() const;

    /// Return the current segment ID.
    [[nodiscard]] uint64_t current_segment_id() const;

    /// Close the WAL writer. Flushes remaining data, stops the group
    /// commit thread, and closes the segment file.
    [[nodiscard]] Result<void> close();

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
};

} // namespace giodb
