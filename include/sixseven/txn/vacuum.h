#pragma once

#include "sixseven/common/result.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/txn/mvcc.h"
#include "sixseven/txn/txn_manager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace sixseven {

/// Statistics from a vacuum pass.
struct VacuumStats {
    uint32_t pages_scanned = 0;   ///< Number of pages inspected.
    uint32_t tuples_examined = 0; ///< Total tuples inspected.
    uint32_t dead_tuples = 0;     ///< Dead tuples found and removed.
    uint32_t pages_compacted = 0; ///< Pages that were compacted.
    size_t bytes_reclaimed = 0;   ///< Approximate bytes freed by compaction.
};

/// Performs garbage collection of dead MVCC tuple versions on a single table.
///
/// Scans all data pages, identifies dead tuples using the xmin_horizon from the
/// TransactionManager, deletes them, and compacts pages to reclaim space.
///
/// Usage:
/// ```
///   Vacuum vac(bpm, dm, file_id, txn_mgr);
///   auto stats = vac.run().value();
///   // stats.dead_tuples removed, stats.bytes_reclaimed freed.
/// ```
class Vacuum {
public:
    /// @param bpm       Buffer pool managing the table's pages.
    /// @param dm        Disk manager for page count queries.
    /// @param file_id   The file backing the table.
    /// @param txn_mgr   Transaction manager (for xmin_horizon and status checks).
    Vacuum(BufferPoolManager& bpm,
           DiskManager& dm,
           FileId file_id,
           const TransactionManager& txn_mgr);

    /// Run a vacuum pass over all data pages.
    /// Scans every page, deletes dead tuples, and compacts pages with dead space.
    [[nodiscard]] Result<VacuumStats> run();

    /// Run a full vacuum: rewrites every page compactly.
    /// Unlike regular vacuum which only compacts pages with dead tuples,
    /// VACUUM FULL compacts every page to eliminate all fragmentation.
    [[nodiscard]] Result<VacuumStats> run_full();

private:
    BufferPoolManager& bpm_;
    DiskManager& dm_;
    FileId file_id_;
    const TransactionManager& txn_mgr_;

    /// Vacuum a single page. Returns the number of dead tuples removed.
    [[nodiscard]] Result<uint32_t> vacuum_page(PageId page_id, txn_id_t xmin_horizon);
};

/// Configuration for the auto-vacuum background worker.
struct AutoVacuumConfig {
    bool enabled = true;
    /// Percentage of dead tuples (relative to live) that triggers vacuum.
    double dead_tuple_threshold = 0.20; // 20%
    /// How often the auto-vacuum worker checks for work.
    std::chrono::seconds check_interval{60};
};

/// Background auto-vacuum worker that periodically checks tables and
/// runs vacuum when the dead tuple ratio exceeds the configured threshold.
///
/// Usage:
/// ```
///   AutoVacuumWorker worker(txn_mgr);
///   worker.add_table(bpm, dm, file_id);
///   worker.start();
///   // ... later ...
///   worker.stop();
/// ```
class AutoVacuumWorker {
public:
    explicit AutoVacuumWorker(const TransactionManager& txn_mgr);
    ~AutoVacuumWorker();

    // Non-copyable, non-movable.
    AutoVacuumWorker(const AutoVacuumWorker&) = delete;
    AutoVacuumWorker& operator=(const AutoVacuumWorker&) = delete;

    /// Register a table for auto-vacuum monitoring.
    void add_table(BufferPoolManager& bpm, DiskManager& dm, FileId file_id);

    /// Set auto-vacuum configuration.
    void set_config(const AutoVacuumConfig& config);

    /// Get the current configuration.
    [[nodiscard]] AutoVacuumConfig config() const;

    /// Start the background auto-vacuum thread.
    [[nodiscard]] Result<void> start();

    /// Stop the background auto-vacuum thread.
    void stop();

    /// Check if the worker is currently running.
    [[nodiscard]] bool running() const;

    /// Run one check cycle manually (for testing).
    /// Returns vacuum stats for each table that was vacuumed.
    [[nodiscard]] Result<std::vector<VacuumStats>> check_and_vacuum();

private:
    struct TableEntry {
        BufferPoolManager* bpm;
        DiskManager* dm;
        FileId file_id;
    };

    void worker_loop();

    const TransactionManager& txn_mgr_;
    mutable std::mutex mu_;
    AutoVacuumConfig config_;
    std::vector<TableEntry> tables_;

    std::thread worker_thread_;
    std::mutex cv_mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
};

} // namespace sixseven
