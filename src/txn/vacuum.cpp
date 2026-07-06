#include "sixseven/txn/vacuum.h"

#include "sixseven/common/logging.h"
#include "sixseven/txn/mvcc.h"
#include "sixseven/txn/mvcc_tuple.h"

namespace sixseven {

namespace {

/// Map a transaction id genuinely UNREGISTERED with the manager to
/// frozen_txn_id so that vacuum treats rows persisted by a prior server
/// process as committed-and-live, mirroring the read-path behaviour in
/// table_heap.cpp::normalize_xid. Uses TransactionManager::is_registered
/// (GDB-1242) rather than get_transaction() != nullptr: get_transaction only
/// sees LIVE transactions, so a GC'd ABORTED transaction (remembered in
/// pruned_aborted_, tracked by is_registered) would otherwise be wrongly
/// normalized to frozen_txn_id here and have its dead tuples treated as
/// live/committed forever -- the exact resurrection bug this ticket fixes.
txn_id_t vacuum_normalize_xid(const TransactionManager& mgr, txn_id_t xid) {
    if (xid == invalid_txn_id || xid == frozen_txn_id) {
        return xid;
    }
    return mgr.is_registered(xid) ? xid : frozen_txn_id;
}

/// Vacuum-safe deadness check (GDB-969).
/// Applies the unknown-xid-as-committed mapping before calling is_dead so that:
///   - tuples from a prior process (unknown xmin) are never reclaimed, and
///   - tuples whose xmin is a real known-aborted txn ARE correctly reclaimed.
/// Option (i): wrapper local to vacuum.cpp; is_dead shared semantics unchanged.
bool vacuum_is_dead(const MvccTupleHeader& raw_header,
                    txn_id_t xmin_horizon,
                    const TransactionManager& txn_mgr) {
    MvccTupleHeader h = raw_header;
    h.xmin = vacuum_normalize_xid(txn_mgr, h.xmin);
    h.xmax = vacuum_normalize_xid(txn_mgr, h.xmax);
    return is_dead(h, xmin_horizon, txn_mgr);
}

} // namespace

// =============================================================================
// Vacuum
// =============================================================================

Vacuum::Vacuum(BufferPoolManager& bpm,
               DiskManager& dm,
               FileId file_id,
               const TransactionManager& txn_mgr)
    : bpm_(bpm), dm_(dm), file_id_(file_id), txn_mgr_(txn_mgr) {}

Result<VacuumStats> Vacuum::run() {
    auto page_count_result = dm_.file_page_count(file_id_);
    if (!page_count_result) {
        return tl::unexpected(page_count_result.error());
    }
    uint32_t total_pages = *page_count_result;

    txn_id_t horizon = txn_mgr_.xmin_horizon();
    VacuumStats stats;

    // Data pages start at page 1 (page 0 is the file header).
    for (uint32_t pid = 1; pid < total_pages; ++pid) {
        auto dead = vacuum_page(pid, horizon);
        if (!dead) {
            SIXSEVEN_LOG_WARN("vacuum: failed to process page {}: {}", pid, dead.error().message);
            continue;
        }
        stats.pages_scanned++;
        stats.dead_tuples += *dead;
    }

    SIXSEVEN_LOG_DEBUG(
        "vacuum: scanned {} pages, removed {} dead tuples", stats.pages_scanned, stats.dead_tuples);
    return ok(stats);
}

Result<VacuumStats> Vacuum::run_full() {
    auto page_count_result = dm_.file_page_count(file_id_);
    if (!page_count_result) {
        return tl::unexpected(page_count_result.error());
    }
    uint32_t total_pages = *page_count_result;

    txn_id_t horizon = txn_mgr_.xmin_horizon();
    VacuumStats stats;

    for (uint32_t pid = 1; pid < total_pages; ++pid) {
        auto dead = vacuum_page(pid, horizon);
        if (!dead) {
            SIXSEVEN_LOG_WARN(
                "vacuum full: failed to process page {}: {}", pid, dead.error().message);
            continue;
        }
        stats.pages_scanned++;
        stats.dead_tuples += *dead;

        // VACUUM FULL: compact every page (even if no dead tuples found).
        auto page_result = bpm_.fetch_page(pid);
        if (!page_result) {
            continue;
        }
        Page* page = *page_result;
        size_t free_before = page->free_space();
        page->compact();
        size_t free_after = page->free_space();
        if (free_after > free_before) {
            stats.bytes_reclaimed += (free_after - free_before);
            stats.pages_compacted++;
        }
        auto unpin = bpm_.unpin_page(pid, true);
        (void)unpin;
    }

    SIXSEVEN_LOG_DEBUG("vacuum full: scanned {} pages, removed {} dead tuples, reclaimed {} bytes",
                       stats.pages_scanned,
                       stats.dead_tuples,
                       stats.bytes_reclaimed);
    return ok(stats);
}

Result<uint32_t> Vacuum::vacuum_page(PageId page_id, txn_id_t xmin_horizon) {
    auto page_result = bpm_.fetch_page(page_id);
    if (!page_result) {
        return tl::unexpected(page_result.error());
    }
    Page* page = *page_result;

    uint32_t dead_count = 0;
    uint16_t slot_count = page->slot_count();
    bool page_modified = false;

    for (uint16_t slot = 0; slot < slot_count; ++slot) {
        auto tuple_result = page->get_tuple(slot);
        if (!tuple_result) {
            continue; // Slot is deleted or invalid — skip.
        }

        auto tuple_data = *tuple_result;
        if (tuple_data.size() < mvcc_header_size) {
            continue; // Not an MVCC tuple — skip.
        }

        auto header = read_mvcc_header(tuple_data);
        if (vacuum_is_dead(header, xmin_horizon, txn_mgr_)) {
            auto del = page->delete_tuple(slot);
            if (del) {
                dead_count++;
                page_modified = true;
            }
        }
    }

    // Compact the page if we deleted any tuples.
    if (dead_count > 0) {
        page->compact();
    }

    auto unpin = bpm_.unpin_page(page_id, page_modified);
    (void)unpin;
    return ok(dead_count);
}

// =============================================================================
// AutoVacuumWorker
// =============================================================================

AutoVacuumWorker::AutoVacuumWorker(const TransactionManager& txn_mgr) : txn_mgr_(txn_mgr) {}

AutoVacuumWorker::~AutoVacuumWorker() {
    stop();
}

void AutoVacuumWorker::add_table(BufferPoolManager& bpm, DiskManager& dm, FileId file_id) {
    std::lock_guard lock(mu_);
    tables_.push_back({&bpm, &dm, file_id});
}

void AutoVacuumWorker::set_config(const AutoVacuumConfig& config) {
    std::lock_guard lock(mu_);
    config_ = config;
}

AutoVacuumConfig AutoVacuumWorker::config() const {
    std::lock_guard lock(mu_);
    return config_;
}

Result<void> AutoVacuumWorker::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return make_error(StatusCode::ALREADY_EXISTS, "auto-vacuum worker already running");
    }
    worker_thread_ = std::thread(&AutoVacuumWorker::worker_loop, this);
    SIXSEVEN_LOG_INFO("auto-vacuum worker started");
    return ok();
}

void AutoVacuumWorker::stop() {
    if (!running_.load()) {
        return;
    }
    running_.store(false);
    {
        std::lock_guard lock(cv_mutex_);
        cv_.notify_all();
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    SIXSEVEN_LOG_INFO("auto-vacuum worker stopped");
}

bool AutoVacuumWorker::running() const {
    return running_.load();
}

Result<std::vector<VacuumStats>> AutoVacuumWorker::check_and_vacuum() {
    std::vector<TableEntry> tables_copy;
    AutoVacuumConfig config_copy;
    {
        std::lock_guard lock(mu_);
        tables_copy = tables_;
        config_copy = config_;
    }

    if (!config_copy.enabled) {
        return ok(std::vector<VacuumStats>{});
    }

    txn_id_t horizon = txn_mgr_.xmin_horizon();
    std::vector<VacuumStats> results;

    for (auto& entry : tables_copy) {
        auto page_count_result = entry.dm->file_page_count(entry.file_id);
        if (!page_count_result) {
            continue;
        }
        uint32_t total_pages = *page_count_result;

        // Quick scan to estimate dead tuple ratio.
        uint32_t total_tuples = 0;
        uint32_t dead_tuples = 0;
        for (uint32_t pid = 1; pid < total_pages; ++pid) {
            auto page_result = entry.bpm->fetch_page(pid);
            if (!page_result) {
                continue;
            }
            Page* page = *page_result;
            uint16_t slots = page->slot_count();
            for (uint16_t slot = 0; slot < slots; ++slot) {
                auto tuple_result = page->get_tuple(slot);
                if (!tuple_result) {
                    continue;
                }
                total_tuples++;
                auto data = *tuple_result;
                if (data.size() >= mvcc_header_size) {
                    auto header = read_mvcc_header(data);
                    if (vacuum_is_dead(header, horizon, txn_mgr_)) {
                        dead_tuples++;
                    }
                }
            }
            auto unpin = entry.bpm->unpin_page(pid, false);
            (void)unpin;
        }

        // Check if dead tuple ratio exceeds threshold.
        uint32_t live_tuples = total_tuples - dead_tuples;
        if (live_tuples == 0 && dead_tuples == 0) {
            continue;
        }
        double ratio = (live_tuples > 0)
                           ? static_cast<double>(dead_tuples) / static_cast<double>(live_tuples)
                           : 1.0;

        if (ratio >= config_copy.dead_tuple_threshold) {
            Vacuum vac(*entry.bpm, *entry.dm, entry.file_id, txn_mgr_);
            auto stats = vac.run();
            if (stats) {
                results.push_back(*stats);
                SIXSEVEN_LOG_INFO("auto-vacuum: table file_id={} vacuumed, {} dead tuples removed",
                                  entry.file_id,
                                  stats->dead_tuples);
            }
        }
    }

    return ok(results);
}

void AutoVacuumWorker::worker_loop() {
    while (running_.load()) {
        auto interval = config().check_interval;
        {
            std::unique_lock lock(cv_mutex_);
            cv_.wait_for(lock, interval, [this] { return !running_.load(); });
        }
        if (!running_.load()) {
            break;
        }
        auto result = check_and_vacuum();
        if (!result) {
            SIXSEVEN_LOG_WARN("auto-vacuum check failed: {}", result.error().message);
        }
    }
}

} // namespace sixseven
