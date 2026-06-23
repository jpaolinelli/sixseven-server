#include "sixseven/vector/backfill_manager.h"

#include "sixseven/common/logging.h"
#include "sixseven/parser/ast.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"
#include "sixseven/vector/embedding_column.h"

#include <chrono>
#include <cstddef>

namespace sixseven {

BackfillManager::BackfillManager(Catalog& catalog,
                                 StorageManager& storage,
                                 EmbeddingWorkerPool& pool)
    : catalog_(catalog), storage_(storage), pool_(pool) {}

BackfillManager::~BackfillManager() {
    // Signal all jobs to stop and join threads.
    std::lock_guard lock(mu_);
    for (auto& job : jobs_) {
        job->cancel_requested.store(true);
        if (job->thread.joinable()) {
            job->thread.join();
        }
    }
}

Result<void> BackfillManager::start(const BackfillStmt& stmt, database_id_t db_id) {
    std::lock_guard lock(mu_);

    // Reject if a backfill is already running for this table.
    for (const auto& job : jobs_) {
        if (job->table_name == stmt.table_name && job->running.load()) {
            return make_error(StatusCode::ALREADY_EXISTS,
                              "backfill already running for table " + stmt.table_name);
        }
    }

    // Validate the table exists and has embedding columns.
    auto table_schema = catalog_.get_table(db_id, stmt.table_name);
    if (!table_schema) {
        return make_error(table_schema.error().code, table_schema.error().message);
    }

    auto emb_cols = catalog_.list_embedding_columns(table_schema->table_id);
    if (emb_cols.empty()) {
        return make_error(StatusCode::NOT_FOUND,
                          "table " + stmt.table_name + " has no EMBEDDING columns");
    }

    auto job = std::make_unique<BackfillJob>();
    job->table_name = stmt.table_name;
    job->db_id = db_id;
    job->batch_size = stmt.batch_size > 0 ? stmt.batch_size : 100;
    job->rate_limit = stmt.rate_limit;
    job->running.store(true);

    auto* job_ptr = job.get();
    job->thread = std::jthread([this, job_ptr]() { backfill_loop(*job_ptr); });

    jobs_.push_back(std::move(job));

    SIXSEVEN_LOG_INFO("backfill started for table '{}' (batch={}, rate_limit={})",
                      stmt.table_name,
                      job_ptr->batch_size,
                      job_ptr->rate_limit);

    return ok();
}

Result<void> BackfillManager::cancel(const std::string& table_name) {
    std::lock_guard lock(mu_);
    for (auto& job : jobs_) {
        if (job->table_name == table_name && job->running.load()) {
            job->cancel_requested.store(true);
            return ok();
        }
    }
    return make_error(StatusCode::NOT_FOUND, "no running backfill for table " + table_name);
}

BackfillManager::Status BackfillManager::status(const std::string& table_name) const {
    std::lock_guard lock(mu_);
    for (const auto& job : jobs_) {
        if (job->table_name == table_name) {
            Status s;
            s.table_name = job->table_name;
            s.processed = job->processed.load();
            s.generated = job->generated.load();
            s.skipped = job->skipped.load();
            s.running = job->running.load();
            s.rows_per_sec = job->rows_per_sec.load();
            return s;
        }
    }
    return Status{table_name};
}

std::vector<BackfillManager::Status> BackfillManager::all_statuses() const {
    std::lock_guard lock(mu_);
    std::vector<Status> result;
    result.reserve(jobs_.size());
    for (const auto& job : jobs_) {
        Status s;
        s.table_name = job->table_name;
        s.processed = job->processed.load();
        s.generated = job->generated.load();
        s.skipped = job->skipped.load();
        s.running = job->running.load();
        s.rows_per_sec = job->rows_per_sec.load();
        result.push_back(std::move(s));
    }
    return result;
}

void BackfillManager::backfill_loop(BackfillJob& job) {
    auto start_time = std::chrono::steady_clock::now();

    // Resolve the table schema and storage.
    auto table_schema = catalog_.get_table(job.db_id, job.table_name);
    if (!table_schema) {
        SIXSEVEN_LOG_ERROR(
            "backfill: table '{}' not found: {}", job.table_name, table_schema.error().message);
        job.running.store(false);
        return;
    }

    auto ts = storage_.get_table_storage(table_schema->table_id);
    if (!ts) {
        SIXSEVEN_LOG_ERROR(
            "backfill: storage not found for table '{}': {}", job.table_name, ts.error().message);
        job.running.store(false);
        return;
    }
    auto* table_storage = *ts;

    auto emb_cols = catalog_.list_embedding_columns(table_schema->table_id);
    if (emb_cols.empty()) {
        SIXSEVEN_LOG_ERROR("backfill: table '{}' has no EMBEDDING columns", job.table_name);
        job.running.store(false);
        return;
    }

    // Map embedding column ordinals to their schema index.
    // source_idx is intentionally NOT stored here: multi-column source_expr support
    // is handled at row-processing time via EmbeddingColumnManager::build_source_text,
    // which parses the comma-separated source_expr and concatenates matching values.
    struct EmbColInfo {
        int32_t column_id = 0; // Ordinal in catalog.
        size_t schema_idx = 0; // Index of the EMBEDDING column in table_schema.columns.
        std::string provider;
        int32_t dimension = 0;
        std::string source_expr;
    };
    std::vector<EmbColInfo> emb_infos;

    for (const auto& ec : emb_cols) {
        EmbColInfo info;
        info.column_id = ec.column_id;
        info.provider = ec.provider;
        info.dimension = ec.dimension;
        info.source_expr = ec.source_expr;

        // Find schema index for the embedding column itself.
        for (size_t i = 0; i < table_schema->columns.size(); ++i) {
            if (table_schema->columns[i].ordinal == ec.column_id) {
                info.schema_idx = i;
                break;
            }
        }

        emb_infos.push_back(info);
    }

    // Open heap iterator and scan sequentially.
    auto it = table_storage->heap->begin();
    if (!it) {
        SIXSEVEN_LOG_ERROR(
            "backfill: failed to open heap for '{}': {}", job.table_name, it.error().message);
        job.running.store(false);
        return;
    }

    std::vector<EmbeddingJob> batch;
    batch.reserve(job.batch_size);

    while (!job.cancel_requested.load()) {
        auto row_result = it->next();
        if (!row_result || !row_result->has_value()) {
            break;
        }

        auto& [rid, data] = **row_result;

        auto values = TupleSerializer::deserialize(data, table_storage->storage_schema);
        if (!values) {
            SIXSEVEN_LOG_WARN("backfill: deserialize failed at rid ({},{}): {}",
                              rid.page_id,
                              rid.slot_id,
                              values.error().message);
            job.processed.fetch_add(1);
            continue;
        }

        bool row_had_all_embeddings = true;

        for (const auto& info : emb_infos) {
            // Check if this embedding column is already populated.
            if (info.schema_idx < values->size() && !(*values)[info.schema_idx].is_null()) {
                continue; // Already has embedding.
            }

            row_had_all_embeddings = false;

            // Build source text via the shared multi-column helper.
            auto src = EmbeddingColumnManager::build_source_text(
                info.source_expr, table_schema->columns, *values);

            // Zero resolved_count means source_expr names no valid schema column —
            // this is a misconfiguration.  Log an error and skip the row to avoid
            // embedding wrong data (old behaviour was to silently embed column 0).
            if (src.resolved_count == 0) {
                SIXSEVEN_LOG_ERROR(
                    "backfill: source_expr '{}' for table '{}' column_id={} resolved no schema "
                    "columns — skipping row (check EMBEDDING column definition)",
                    info.source_expr,
                    job.table_name,
                    info.column_id);
                continue;
            }
            if (src.text.empty()) {
                continue; // All source columns were NULL or non-STRING — skip.
            }
            std::string source_text = std::move(src.text);

            // Pack RID into int64_t for the worker pool.
            int64_t packed_rid = (static_cast<int64_t>(static_cast<uint64_t>(rid.page_id)) << 32) |
                                 static_cast<int64_t>(rid.slot_id);

            EmbeddingJob emb_job;
            emb_job.table_id = table_schema->table_id;
            emb_job.row_id = packed_rid;
            emb_job.column_id = info.column_id;
            emb_job.source_text = std::move(source_text);
            emb_job.provider = info.provider;
            emb_job.dimension = info.dimension;
            emb_job.type = EmbeddingJob::Type::UPDATE;

            batch.push_back(std::move(emb_job));
            job.generated.fetch_add(1);
        }

        if (row_had_all_embeddings) {
            job.skipped.fetch_add(1);
        }
        job.processed.fetch_add(1);

        // Flush batch when full.
        if (batch.size() >= job.batch_size) {
            auto enqueue_result =
                pool_.try_enqueue_batch(std::move(batch), std::chrono::milliseconds(5000));
            if (!enqueue_result) {
                SIXSEVEN_LOG_WARN("backfill: enqueue failed: {}", enqueue_result.error().message);
            }
            batch.clear();
            batch.reserve(job.batch_size);

            // Rate limiting: if configured, sleep to throttle throughput.
            if (job.rate_limit > 0) {
                auto sleep_ms = static_cast<int64_t>(job.batch_size) * 1000 / job.rate_limit;
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            }

            // Update throughput stats.
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            auto secs =
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 1000.0;
            if (secs > 0) {
                job.rows_per_sec.store(static_cast<double>(job.processed.load()) / secs);
            }
        }
    }

    // Flush remaining batch.
    if (!batch.empty() && !job.cancel_requested.load()) {
        auto enqueue_result =
            pool_.try_enqueue_batch(std::move(batch), std::chrono::milliseconds(5000));
        if (!enqueue_result) {
            SIXSEVEN_LOG_WARN("backfill: final enqueue failed: {}", enqueue_result.error().message);
        }
    }

    // Final stats.
    auto elapsed = std::chrono::steady_clock::now() - start_time;
    auto secs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 1000.0;
    if (secs > 0) {
        job.rows_per_sec.store(static_cast<double>(job.processed.load()) / secs);
    }

    job.running.store(false);

    SIXSEVEN_LOG_INFO("backfill completed for '{}': processed={}, generated={}, skipped={}, "
                      "{:.1f} rows/sec",
                      job.table_name,
                      job.processed.load(),
                      job.generated.load(),
                      job.skipped.load(),
                      job.rows_per_sec.load());
}

} // namespace sixseven
