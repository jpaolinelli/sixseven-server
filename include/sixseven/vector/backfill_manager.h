#pragma once

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/vector/embedding_worker.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sixseven {

struct BackfillStmt;

/// Manages background embedding backfill jobs.
///
/// When a BACKFILL EMBEDDINGS ON <table> command is issued, the manager spawns
/// a background thread that scans the table sequentially and enqueues embedding
/// jobs for rows with NULL embedding columns. The existing EmbeddingWorkerPool
/// store callback handles writing embeddings back to tuples and inserting them
/// into HNSW indexes incrementally.
class BackfillManager {
public:
    /// Status of a backfill job for a table.
    struct Status {
        std::string table_name;
        int64_t processed = 0;  ///< Rows scanned (including already-embedded).
        int64_t generated = 0;  ///< New embedding jobs enqueued.
        int64_t skipped = 0;    ///< Rows that already had embeddings.
        bool running = false;
        double rows_per_sec = 0.0;
    };

    /// @param catalog       System catalog for schema lookups.
    /// @param storage       StorageManager for heap access.
    /// @param pool          EmbeddingWorkerPool for async job submission.
    BackfillManager(Catalog& catalog, StorageManager& storage, EmbeddingWorkerPool& pool);

    ~BackfillManager();

    /// Start a backfill job for the specified table.
    /// Returns immediately; the scan runs in a background thread.
    [[nodiscard]] Result<void> start(const BackfillStmt& stmt, database_id_t db_id);

    /// Cancel a running backfill for the specified table.
    [[nodiscard]] Result<void> cancel(const std::string& table_name);

    /// Get the status of a specific table's backfill.
    [[nodiscard]] Status status(const std::string& table_name) const;

    /// Get statuses for all known backfill jobs.
    [[nodiscard]] std::vector<Status> all_statuses() const;

private:
    /// Per-table backfill state.
    struct BackfillJob {
        std::string table_name;
        database_id_t db_id = 0;
        uint32_t batch_size = 100;
        uint32_t rate_limit = 0;
        std::atomic<int64_t> processed{0};
        std::atomic<int64_t> generated{0};
        std::atomic<int64_t> skipped{0};
        std::atomic<bool> running{false};
        std::atomic<bool> cancel_requested{false};
        std::atomic<double> rows_per_sec{0.0};
        std::jthread thread;
    };

    /// The background scan loop for a single table.
    void backfill_loop(BackfillJob& job);

    Catalog& catalog_;
    StorageManager& storage_;
    EmbeddingWorkerPool& pool_;

    mutable std::mutex mu_;
    std::vector<std::unique_ptr<BackfillJob>> jobs_;
};

} // namespace sixseven
