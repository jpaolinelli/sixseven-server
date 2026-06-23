#include "sixseven/table/table_heap.h"

#include "sixseven/common/logging.h"
#include "sixseven/storage/wal.h"
#include "sixseven/table/table_wal.h"
#include "sixseven/txn/mvcc.h"
#include "sixseven/txn/read_view.h"
#include "sixseven/txn/txn_manager.h"

#include <cstring>
#include <unordered_map>

namespace sixseven {

// Row count is stored in the file header extension area (page 0, bytes 16+).
// Extension offset 0 = row_count (uint64_t).
static constexpr size_t kRowCountExtOffset = 0;

namespace {

/// Resolve a transaction id's status for visibility checks (GDB-747).
/// Frozen stamps and ids unknown to the manager (rows persisted by a previous
/// process — there is no persistent commit log yet) are treated as COMMITTED
/// so existing data stays readable across restarts.
TransactionStatus effective_status(const TransactionManager* mgr, txn_id_t xid) {
    if (xid == frozen_txn_id || mgr == nullptr) {
        return TransactionStatus::COMMITTED;
    }
    const Transaction* txn = mgr->get_transaction(xid);
    return txn != nullptr ? txn->status : TransactionStatus::COMMITTED;
}

/// Status-based tuple visibility (GDB-747): a version is visible unless its
/// creator is known-aborted, or it has been deleted by a transaction that is
/// not known-aborted. (Snapshot-based visibility arrives with full isolation
/// support; status-based filtering is sufficient for single-session
/// commit/abort semantics.)
bool tuple_visible(const MvccTupleHeader& header, const TransactionManager* mgr) {
    if (header.xmin != invalid_txn_id &&
        effective_status(mgr, header.xmin) == TransactionStatus::ABORTED) {
        return false;
    }
    if (header.xmax != invalid_txn_id &&
        effective_status(mgr, header.xmax) != TransactionStatus::ABORTED) {
        return false;
    }
    return true;
}

/// Map transaction ids unknown to the manager to frozen_txn_id so that
/// snapshot-based is_visible() treats them as always-committed — the same
/// restart-durability semantics effective_status() gives the status-based
/// path (TransactionManager::get_status treats unknown ids as ABORTED, which
/// would make rows persisted by a previous process vanish).
txn_id_t normalize_xid(const TransactionManager& mgr, txn_id_t xid) {
    if (xid == invalid_txn_id || xid == frozen_txn_id) {
        return xid;
    }
    return mgr.get_transaction(xid) != nullptr ? xid : frozen_txn_id;
}

/// Tuple version visibility (GDB-777): when the executor has installed a
/// per-statement MVCC read view, filter through the snapshot-based
/// is_visible() in src/txn/mvcc.cpp (single source of truth). Otherwise fall
/// back to the status-based GDB-747 check — recovery, vacuum, and direct heap
/// usage without a read view behave exactly as before.
bool version_visible(const MvccTupleHeader& header, const TransactionManager* mgr) {
    if (mgr != nullptr) {
        if (const MvccReadView* view = current_mvcc_read_view()) {
            MvccTupleHeader h = header;
            h.xmin = normalize_xid(*mgr, h.xmin);
            h.xmax = normalize_xid(*mgr, h.xmax);
            return is_visible(h, view->snapshot, *mgr, view->viewer_txn_id);
        }
    }
    return tuple_visible(header, mgr);
}

} // namespace

// -- TableHeap ----------------------------------------------------------------

TableHeap::TableHeap(BufferPoolManager& bpm,
                     DiskManager& dm,
                     FileId file_id,
                     TableHeapOptions options)
    : bpm_(bpm), dm_(dm), file_id_(file_id), options_(options) {
    load_row_count_from_header();
}

void TableHeap::attach_wal(WalWriter* wal, uint32_t table_id) {
    wal_ = wal;
    wal_table_id_ = table_id;
}

void TableHeap::attach_txn_manager(const TransactionManager* txn_mgr) {
    txn_mgr_ = txn_mgr;
}

std::vector<uint8_t> TableHeap::make_on_page_image(std::span<const uint8_t> user_data,
                                                   txn_id_t xmin) const {
    if (!options_.mvcc_headers) {
        return std::vector<uint8_t>(user_data.begin(), user_data.end());
    }
    MvccTupleHeader header;
    header.xmin = xmin;
    header.xmax = invalid_txn_id;
    return prepend_mvcc_header(header, user_data);
}

void TableHeap::log_wal(WalRecordType type,
                        RID rid,
                        txn_id_t txn_id,
                        std::span<const uint8_t> before_image,
                        std::span<const uint8_t> after_image) {
    if (wal_ == nullptr) {
        return;
    }
    WalRecord record;
    record.type = type;
    record.txn_id = txn_id;
    record.table_id = wal_table_id_;
    record.page_id = rid.page_id;
    record.slot_id = rid.slot_id;
    record.data = serialize_table_wal_payload(before_image, after_image);
    auto result = wal_->append(record);
    if (!result) {
        SIXSEVEN_LOG_WARN("table heap WAL append failed (table {}, page {}, slot {}): {}",
                          wal_table_id_,
                          rid.page_id,
                          rid.slot_id,
                          result.error().message);
    }
}

Result<RID> TableHeap::insert_tuple(std::span<const uint8_t> data, txn_id_t xmin) {
    if (data.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "cannot insert empty tuple");
    }

    // Build the on-page image (MVCC header prepended in MVCC mode).
    std::vector<uint8_t> image;
    std::span<const uint8_t> on_page = data;
    if (options_.mvcc_headers) {
        image = make_on_page_image(data, xmin);
        on_page = image;
    }

    auto pc = dm_.file_page_count(file_id_);
    if (!pc) {
        return tl::unexpected(pc.error());
    }
    uint32_t total_pages = *pc;

    // Snapshot the hint once; relaxed load is fine — it is just a hint and the
    // insert path validates the page has room before trusting it.
    const PageId hint = last_insert_page_.load(std::memory_order_relaxed);

    // Try the last-insert-page hint first.
    if (hint >= 1 && hint < total_pages) {
        auto page_result = bpm_.fetch_page(hint);
        if (page_result) {
            Page* page = *page_result;
            auto slot = page->insert_tuple(on_page);
            if (slot) {
                ++row_count_;
                persist_row_count();
                RID rid{hint, *slot};
                log_wal(WalRecordType::INSERT, rid, xmin, {}, on_page);
                auto unpin = bpm_.unpin_page(hint, true);
                if (!unpin) {
                    return tl::unexpected(unpin.error());
                }
                return ok(rid);
            }
            // Page full, unpin and try others.
            auto unpin = bpm_.unpin_page(hint, false);
            if (!unpin) {
                return tl::unexpected(unpin.error());
            }
        }
    }

    // Sequential fallback: try the next page after the hint before allocating.
    // This avoids an O(pages) first-fit scan that causes eviction storms on
    // large tables with small buffer pools. For append-heavy workloads, old
    // pages are always full so scanning them is pure waste.
    // TODO(GDB-626): For mixed insert/delete workloads, a free-space map (FSM)
    // would allow efficient reuse of space on fragmented pages without scanning.
    if (hint >= 1) {
        PageId next_pid = hint + 1;
        if (next_pid < total_pages) {
            auto page_result = bpm_.fetch_page(next_pid);
            if (page_result) {
                Page* page = *page_result;
                auto slot = page->insert_tuple(on_page);
                if (slot) {
                    ++row_count_;
                    persist_row_count();
                    last_insert_page_.store(next_pid, std::memory_order_relaxed);
                    RID rid{next_pid, *slot};
                    log_wal(WalRecordType::INSERT, rid, xmin, {}, on_page);
                    auto unpin = bpm_.unpin_page(next_pid, true);
                    if (!unpin) {
                        return tl::unexpected(unpin.error());
                    }
                    return ok(rid);
                }
                auto unpin = bpm_.unpin_page(next_pid, false);
                if (!unpin) {
                    return tl::unexpected(unpin.error());
                }
            }
        }
    }

    // No existing page has room — allocate a new one and retry in a bounded
    // loop.  Under concurrency, a newly allocated page may be filled by another
    // thread before we pin it (GDB-1267: the "not enough free space" race that
    // silently dropped rows).  If that happens we simply allocate another fresh
    // page and try again.  The loop is bounded by kMaxAllocRetries; genuine
    // IO/capacity errors still propagate immediately.
    static constexpr int kMaxAllocRetries = 64;
    for (int retry = 0; retry < kMaxAllocRetries; ++retry) {
        auto new_page_result = bpm_.new_page();
        if (!new_page_result) {
            return tl::unexpected(new_page_result.error());
        }

        Page* new_page = *new_page_result;
        PageId new_pid = new_page->page_id();

        auto slot = new_page->insert_tuple(on_page);
        if (!slot) {
            // Distinguish "tuple too large for an empty page" (unrecoverable)
            // from "another thread won the race and filled this page" (retry).
            // A newly allocated page should be empty, so any failure here means
            // the tuple is physically too large.  However, under concurrency
            // another thread may have fetched the same page (via hint or its
            // own new_page race) and inserted before us.  We detect this by
            // checking whether the error is INVALID_ARGUMENT (space); all other
            // errors are propagated immediately.
            bool space_error = (slot.error().code == StatusCode::INVALID_ARGUMENT);
            auto unpin = bpm_.unpin_page(new_pid, false);
            if (!unpin) {
                SIXSEVEN_LOG_WARN("unpin failed after insert_tuple error on page {}: {}",
                                  new_pid,
                                  unpin.error().message);
            }
            if (!space_error) {
                return tl::unexpected(slot.error());
            }
            // Space error on a supposedly-fresh page: another thread raced us.
            // Loop and allocate yet another page.
            SIXSEVEN_LOG_DEBUG(
                "GDB-1267: insert_tuple page {} filled by concurrent thread, retrying (attempt "
                "{})",
                new_pid,
                retry + 1);
            continue;
        }

        ++row_count_;
        persist_row_count();
        last_insert_page_.store(new_pid, std::memory_order_relaxed);
        RID rid{new_pid, *slot};
        log_wal(WalRecordType::INSERT, rid, xmin, {}, on_page);

        auto unpin = bpm_.unpin_page(new_pid, true);
        if (!unpin) {
            return tl::unexpected(unpin.error());
        }

        return ok(rid);
    }

    return make_error(StatusCode::INTERNAL_ERROR,
                      "insert_tuple: exceeded retry limit — concurrent page contention");
}

Result<std::vector<RID>>
TableHeap::insert_batch(const std::vector<std::span<const uint8_t>>& tuples, txn_id_t xmin) {
    if (tuples.empty()) {
        return ok(std::vector<RID>{});
    }

    // Pre-validate all tuples so we never partially insert and then fail.
    static constexpr size_t kMaxImageSize = page_size - page_header_size - slot_entry_size;
    const size_t header_overhead = options_.mvcc_headers ? mvcc_header_size : 0;
    for (size_t i = 0; i < tuples.size(); ++i) {
        if (tuples[i].empty()) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "cannot insert empty tuple at index " + std::to_string(i));
        }
        if (tuples[i].size() + header_overhead > kMaxImageSize) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "tuple at index " + std::to_string(i) +
                                  " too large for a single page");
        }
    }

    // Build the on-page images (MVCC headers prepended in MVCC mode).
    std::vector<std::vector<uint8_t>> images;
    std::vector<std::span<const uint8_t>> on_page;
    if (options_.mvcc_headers) {
        images.reserve(tuples.size());
        on_page.reserve(tuples.size());
        for (const auto& t : tuples) {
            images.push_back(make_on_page_image(t, xmin));
            on_page.emplace_back(images.back());
        }
    }
    const std::vector<std::span<const uint8_t>>& spans = options_.mvcc_headers ? on_page : tuples;

    auto pc = dm_.file_page_count(file_id_);
    if (!pc) {
        return tl::unexpected(pc.error());
    }
    uint32_t total_pages = *pc;

    std::vector<RID> rids;
    rids.reserve(spans.size());
    size_t idx = 0;

    // Helper: try to insert as many tuples as possible into a pinned page.
    auto fill_page = [&](Page* page, PageId pid) {
        while (idx < spans.size()) {
            auto slot = page->insert_tuple(spans[idx]);
            if (!slot) {
                break; // Page full.
            }
            rids.push_back(RID{pid, *slot});
            ++idx;
        }
    };

    // Snapshot the hint for this batch; relaxed load is fine (hint only).
    PageId hint = last_insert_page_.load(std::memory_order_relaxed);

    // Try the hint page first.
    if (hint >= 1 && hint < total_pages) {
        auto page_result = bpm_.fetch_page(hint);
        if (page_result) {
            Page* page = *page_result;
            fill_page(page, hint);
            bool dirty = (idx > 0);
            auto unpin = bpm_.unpin_page(hint, dirty);
            if (!unpin) {
                return tl::unexpected(unpin.error());
            }
        }
    }

    // Try the next page after the hint.
    if (idx < spans.size() && hint >= 1) {
        PageId next_pid = hint + 1;
        if (next_pid < total_pages) {
            auto page_result = bpm_.fetch_page(next_pid);
            if (page_result) {
                size_t before = idx;
                Page* page = *page_result;
                fill_page(page, next_pid);
                bool dirty = (idx > before);
                if (dirty) {
                    hint = next_pid;
                    last_insert_page_.store(hint, std::memory_order_relaxed);
                }
                auto unpin = bpm_.unpin_page(next_pid, dirty);
                if (!unpin) {
                    return tl::unexpected(unpin.error());
                }
            }
        }
    }

    // Allocate new pages as needed for remaining tuples.
    while (idx < spans.size()) {
        auto new_page_result = bpm_.new_page();
        if (!new_page_result) {
            return tl::unexpected(new_page_result.error());
        }

        Page* new_page = *new_page_result;
        PageId new_pid = new_page->page_id();

        fill_page(new_page, new_pid);
        last_insert_page_.store(new_pid, std::memory_order_relaxed);
        auto unpin = bpm_.unpin_page(new_pid, true);
        if (!unpin) {
            return tl::unexpected(unpin.error());
        }
    }

    // Log one INSERT record per tuple with its full on-page image.
    if (wal_ != nullptr) {
        for (size_t i = 0; i < rids.size(); ++i) {
            log_wal(WalRecordType::INSERT, rids[i], xmin, {}, spans[i]);
        }
    }

    // Update row count in bulk (seq_cst to match insert_tuple's ++row_count_).
    row_count_.fetch_add(rids.size());
    persist_row_count();

    return ok(std::move(rids));
}

Result<std::vector<uint8_t>> TableHeap::get_tuple(RID rid) {
    auto page_result = bpm_.fetch_page(rid.page_id);
    if (!page_result) {
        return tl::unexpected(page_result.error());
    }

    Page* page = *page_result;
    // Page::get_tuple now returns an owned std::vector<uint8_t>. The copy
    // happens under the page's shared latch, so the returned bytes are a
    // stable snapshot safe to use after the page is unpinned.
    auto tuple = page->get_tuple(rid.slot_id);
    if (!tuple) {
        auto unpin = bpm_.unpin_page(rid.page_id, false);
        if (!unpin) {
            SIXSEVEN_LOG_WARN("unpin failed after get_tuple error on page {}: {}",
                              rid.page_id,
                              unpin.error().message);
        }
        return tl::unexpected(tuple.error());
    }

    auto unpin = bpm_.unpin_page(rid.page_id, false);
    if (!unpin) {
        return tl::unexpected(unpin.error());
    }

    if (options_.mvcc_headers) {
        if (tuple->size() <= mvcc_header_size) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "tuple smaller than MVCC header (corrupt page or wrong heap mode)");
        }
        // Visibility filter (GDB-747): a version created by a known-aborted
        // transaction, or logically deleted (xmax stamped, deleter not
        // known-aborted), reads as NOT_FOUND — the same observable result a
        // physical delete produced before logical deletes existed.
        if (!version_visible(read_mvcc_header(*tuple), txn_mgr_)) {
            return make_error(StatusCode::NOT_FOUND,
                              "tuple version not visible (page " + std::to_string(rid.page_id) +
                                  ", slot " + std::to_string(rid.slot_id) + ")");
        }
        tuple->erase(tuple->begin(),
                     tuple->begin() + static_cast<std::ptrdiff_t>(mvcc_header_size));
    }

    return ok(std::move(*tuple));
}

Result<MvccTupleHeader> TableHeap::get_tuple_header(RID rid) {
    if (!options_.mvcc_headers) {
        return make_error(StatusCode::NOT_IMPLEMENTED, "heap does not store MVCC tuple headers");
    }

    auto page_result = bpm_.fetch_page(rid.page_id);
    if (!page_result) {
        return tl::unexpected(page_result.error());
    }

    Page* page = *page_result;
    auto tuple = page->get_tuple(rid.slot_id);
    auto unpin = bpm_.unpin_page(rid.page_id, false);
    if (!unpin) {
        SIXSEVEN_LOG_WARN("unpin failed after get_tuple_header on page {}: {}",
                          rid.page_id,
                          unpin.error().message);
    }
    if (!tuple) {
        return tl::unexpected(tuple.error());
    }
    if (tuple->size() < mvcc_header_size) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "tuple smaller than MVCC header (corrupt page or wrong heap mode)");
    }
    return ok(read_mvcc_header(*tuple));
}

Result<void> TableHeap::update_tuple(RID rid, std::span<const uint8_t> data) {
    if (data.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "cannot update with empty tuple");
    }

    auto page_result = bpm_.fetch_page(rid.page_id);
    if (!page_result) {
        return tl::unexpected(page_result.error());
    }
    Page* page = *page_result;

    // In MVCC mode (or when WAL logging) read the current on-page image: the
    // existing header is preserved across in-place updates and the old image
    // becomes the WAL before-image.
    std::vector<uint8_t> before;
    if (options_.mvcc_headers || wal_ != nullptr) {
        auto old = page->get_tuple(rid.slot_id);
        if (!old) {
            auto unpin = bpm_.unpin_page(rid.page_id, false);
            if (!unpin) {
                SIXSEVEN_LOG_WARN("unpin failed after update_tuple error on page {}: {}",
                                  rid.page_id,
                                  unpin.error().message);
            }
            return tl::unexpected(old.error());
        }
        before = std::move(*old);
    }

    std::vector<uint8_t> image;
    std::span<const uint8_t> on_page = data;
    if (options_.mvcc_headers) {
        MvccTupleHeader header;
        if (before.size() >= mvcc_header_size) {
            header = read_mvcc_header(before);
        } else {
            header.xmin = frozen_txn_id;
        }
        image = prepend_mvcc_header(header, data);
        on_page = image;
    }

    auto result = page->update_tuple(rid.slot_id, on_page);
    if (!result) {
        // If update failed due to space, try compact and retry.
        if (result.error().code == StatusCode::INVALID_ARGUMENT) {
            page->compact();
            result = page->update_tuple(rid.slot_id, on_page);
        }
        if (!result) {
            auto unpin = bpm_.unpin_page(rid.page_id, false);
            if (!unpin) {
                SIXSEVEN_LOG_WARN("unpin failed after update_tuple error on page {}: {}",
                                  rid.page_id,
                                  unpin.error().message);
            }
            return tl::unexpected(result.error());
        }
    }

    log_wal(WalRecordType::UPDATE, rid, frozen_txn_id, before, on_page);

    auto unpin = bpm_.unpin_page(rid.page_id, true);
    if (!unpin) {
        return tl::unexpected(unpin.error());
    }

    return ok();
}

Result<std::vector<RID>> TableHeap::update_tuples_batch(const std::vector<TupleUpdate>& updates) {
    if (updates.empty()) {
        return ok(std::vector<RID>{});
    }

    // Group by page_id so we fetch/unpin each page exactly once.
    std::unordered_map<PageId, std::vector<size_t>> page_groups;
    for (size_t i = 0; i < updates.size(); ++i) {
        page_groups[updates[i].rid.page_id].push_back(i);
    }

    std::vector<RID> failed_rids;

    for (auto& [page_id, indices] : page_groups) {
        auto page_result = bpm_.fetch_page(page_id);
        if (!page_result) {
            // All updates on this page fail.
            for (auto idx : indices) {
                failed_rids.push_back(updates[idx].rid);
            }
            continue;
        }

        Page* page = *page_result;
        bool any_dirty = false;
        bool compacted = false;

        for (auto idx : indices) {
            auto& upd = updates[idx];
            if (upd.data.empty()) {
                failed_rids.push_back(upd.rid);
                continue;
            }

            // Preserve the existing MVCC header / capture the WAL before-image.
            std::vector<uint8_t> before;
            if (options_.mvcc_headers || wal_ != nullptr) {
                auto old = page->get_tuple(upd.rid.slot_id);
                if (!old) {
                    failed_rids.push_back(upd.rid);
                    continue;
                }
                before = std::move(*old);
            }

            std::vector<uint8_t> image;
            std::span<const uint8_t> on_page = upd.data;
            if (options_.mvcc_headers) {
                MvccTupleHeader header;
                if (before.size() >= mvcc_header_size) {
                    header = read_mvcc_header(before);
                } else {
                    header.xmin = frozen_txn_id;
                }
                image = prepend_mvcc_header(header, upd.data);
                on_page = image;
            }

            auto result = page->update_tuple(upd.rid.slot_id, on_page);
            if (!result) {
                // Compact once per page if update fails due to fragmentation.
                if (!compacted && result.error().code == StatusCode::INVALID_ARGUMENT) {
                    page->compact();
                    compacted = true;
                    result = page->update_tuple(upd.rid.slot_id, on_page);
                }
                if (!result) {
                    failed_rids.push_back(upd.rid);
                    continue;
                }
            }
            log_wal(WalRecordType::UPDATE, upd.rid, frozen_txn_id, before, on_page);
            any_dirty = true;
        }

        auto unpin = bpm_.unpin_page(page_id, any_dirty);
        if (!unpin) {
            SIXSEVEN_LOG_WARN(
                "unpin failed after batch update on page {}: {}", page_id, unpin.error().message);
        }
    }

    return ok(std::move(failed_rids));
}

Result<void> TableHeap::delete_tuple(RID rid, txn_id_t xmax) {
    auto page_result = bpm_.fetch_page(rid.page_id);
    if (!page_result) {
        return tl::unexpected(page_result.error());
    }
    Page* page = *page_result;

    // Capture the on-page image and stamp xmax into it. The stamped image is
    // persisted via the WAL DELETE record (redo-able header mutation); the
    // slot itself is then physically deleted to preserve the executor's
    // current observable semantics (see class documentation in table_heap.h).
    std::vector<uint8_t> before;
    if (options_.mvcc_headers || wal_ != nullptr) {
        auto old = page->get_tuple(rid.slot_id);
        if (!old) {
            auto unpin = bpm_.unpin_page(rid.page_id, false);
            if (!unpin) {
                SIXSEVEN_LOG_WARN("unpin failed after delete_tuple error on page {}: {}",
                                  rid.page_id,
                                  unpin.error().message);
            }
            return tl::unexpected(old.error());
        }
        before = std::move(*old);
        if (options_.mvcc_headers && before.size() >= mvcc_header_size) {
            write_mvcc_xmax(before.data(), xmax);
        }
    }

    auto result = page->delete_tuple(rid.slot_id);
    if (!result) {
        auto unpin = bpm_.unpin_page(rid.page_id, false);
        if (!unpin) {
            SIXSEVEN_LOG_WARN("unpin failed after delete_tuple error on page {}: {}",
                              rid.page_id,
                              unpin.error().message);
        }
        return tl::unexpected(result.error());
    }

    --row_count_;
    persist_row_count();
    log_wal(WalRecordType::DELETE, rid, xmax, before, {});

    auto unpin = bpm_.unpin_page(rid.page_id, true);
    if (!unpin) {
        return tl::unexpected(unpin.error());
    }

    return ok();
}

Result<void> TableHeap::mark_deleted(RID rid, txn_id_t xmax, RID next_version) {
    if (!options_.mvcc_headers) {
        return make_error(StatusCode::NOT_IMPLEMENTED, "mark_deleted requires MVCC tuple headers");
    }

    auto page_result = bpm_.fetch_page(rid.page_id);
    if (!page_result) {
        return tl::unexpected(page_result.error());
    }
    Page* page = *page_result;

    auto fail_unpin = [&]() {
        auto unpin = bpm_.unpin_page(rid.page_id, false);
        if (!unpin) {
            SIXSEVEN_LOG_WARN("unpin failed after mark_deleted error on page {}: {}",
                              rid.page_id,
                              unpin.error().message);
        }
    };

    auto old = page->get_tuple(rid.slot_id);
    if (!old) {
        fail_unpin();
        return tl::unexpected(old.error());
    }
    if (old->size() < mvcc_header_size) {
        fail_unpin();
        return make_error(StatusCode::INTERNAL_ERROR,
                          "tuple smaller than MVCC header (corrupt page or wrong heap mode)");
    }

    std::vector<uint8_t> before = *old;
    std::vector<uint8_t> after = std::move(*old);
    write_mvcc_xmax(after.data(), xmax);
    if (next_version != RID::invalid()) {
        write_mvcc_ctid(after.data(), next_version);
    }

    // Same-size image: rewriting the slot in place always fits.
    auto result = page->update_tuple(rid.slot_id, after);
    if (!result) {
        fail_unpin();
        return tl::unexpected(result.error());
    }

    // Header mutation is logged as an UPDATE record carrying full before /
    // after images, so existing redo reproduces the stamped header (GDB-714
    // recovery path).
    log_wal(WalRecordType::UPDATE, rid, xmax, before, after);

    if (row_count_.load(std::memory_order_relaxed) > 0) {
        --row_count_;
    }
    persist_row_count();

    auto unpin = bpm_.unpin_page(rid.page_id, true);
    if (!unpin) {
        return tl::unexpected(unpin.error());
    }
    return ok();
}

void TableHeap::adjust_row_count(int64_t delta) {
    if (delta >= 0) {
        row_count_.fetch_add(static_cast<uint64_t>(delta), std::memory_order_relaxed);
    } else {
        // Negative delta: clamp at zero without going below.  A simple
        // load-compute-store would race with concurrent callers (GDB-1267).
        // Use a CAS loop so every decrement is applied atomically.
        uint64_t dec = static_cast<uint64_t>(-delta);
        uint64_t current = row_count_.load(std::memory_order_relaxed);
        uint64_t desired;
        do {
            desired = current >= dec ? current - dec : 0;
        } while (!row_count_.compare_exchange_weak(
            current, desired, std::memory_order_relaxed, std::memory_order_relaxed));
    }
    persist_row_count();
}

// -- WAL recovery primitives (GDB-714) -----------------------------------------

Result<void> TableHeap::restore_raw_tuple(RID rid, std::span<const uint8_t> raw_image) {
    if (raw_image.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "cannot restore empty tuple image");
    }
    if (rid.page_id == 0) {
        return make_error(StatusCode::INVALID_ARGUMENT, "cannot restore into header page 0");
    }

    // Allocate pages until the target page exists (redo onto a fresh or
    // truncated file). new_page() allocates sequentially ascending ids.
    auto pc = dm_.file_page_count(file_id_);
    if (!pc) {
        return tl::unexpected(pc.error());
    }
    while (*pc <= rid.page_id) {
        auto new_page_result = bpm_.new_page();
        if (!new_page_result) {
            return tl::unexpected(new_page_result.error());
        }
        PageId new_pid = (*new_page_result)->page_id();
        auto unpin = bpm_.unpin_page(new_pid, true);
        if (!unpin) {
            return tl::unexpected(unpin.error());
        }
        pc = dm_.file_page_count(file_id_);
        if (!pc) {
            return tl::unexpected(pc.error());
        }
    }

    auto page_result = bpm_.fetch_page(rid.page_id);
    if (!page_result) {
        return tl::unexpected(page_result.error());
    }
    Page* page = *page_result;

    bool was_live = page->is_slot_live(rid.slot_id);
    auto result = page->restore_tuple(rid.slot_id, raw_image);
    if (!result && result.error().code == StatusCode::INVALID_ARGUMENT) {
        page->compact();
        result = page->restore_tuple(rid.slot_id, raw_image);
    }
    if (!result) {
        auto unpin = bpm_.unpin_page(rid.page_id, false);
        if (!unpin) {
            SIXSEVEN_LOG_WARN("unpin failed after restore_raw_tuple error on page {}: {}",
                              rid.page_id,
                              unpin.error().message);
        }
        return tl::unexpected(result.error());
    }

    if (!was_live) {
        ++row_count_;
        persist_row_count();
    }

    auto unpin = bpm_.unpin_page(rid.page_id, true);
    if (!unpin) {
        return tl::unexpected(unpin.error());
    }
    return ok();
}

Result<void> TableHeap::delete_raw_tuple(RID rid) {
    auto pc = dm_.file_page_count(file_id_);
    if (!pc) {
        return tl::unexpected(pc.error());
    }
    if (rid.page_id == 0 || rid.page_id >= *pc) {
        return ok(); // Page absent — nothing to delete (idempotent).
    }

    auto page_result = bpm_.fetch_page(rid.page_id);
    if (!page_result) {
        // Page allocated but never written (e.g., redo onto a partially
        // recovered file) — nothing to delete.
        return ok();
    }
    Page* page = *page_result;

    if (!page->is_slot_live(rid.slot_id)) {
        auto unpin = bpm_.unpin_page(rid.page_id, false);
        if (!unpin) {
            return tl::unexpected(unpin.error());
        }
        return ok(); // Already deleted or out of range (idempotent).
    }

    auto result = page->delete_tuple(rid.slot_id);
    if (!result) {
        auto unpin = bpm_.unpin_page(rid.page_id, false);
        if (!unpin) {
            SIXSEVEN_LOG_WARN("unpin failed after delete_raw_tuple error on page {}: {}",
                              rid.page_id,
                              unpin.error().message);
        }
        return tl::unexpected(result.error());
    }

    if (row_count_.load(std::memory_order_relaxed) > 0) {
        --row_count_;
    }
    persist_row_count();

    auto unpin = bpm_.unpin_page(rid.page_id, true);
    if (!unpin) {
        return tl::unexpected(unpin.error());
    }
    return ok();
}

Result<TableIterator> TableHeap::begin() {
    auto pc = dm_.file_page_count(file_id_);
    if (!pc) {
        return tl::unexpected(pc.error());
    }
    return ok(TableIterator(bpm_, 1, *pc, options_.mvcc_headers, txn_mgr_));
}

Result<uint32_t> TableHeap::page_count() const {
    auto pc = dm_.file_page_count(file_id_);
    if (!pc) {
        return tl::unexpected(pc.error());
    }
    // Subtract 1 for the header page (page 0).
    return ok(*pc > 0 ? *pc - 1 : 0u);
}

uint64_t TableHeap::row_count() const {
    return row_count_.load(std::memory_order_relaxed);
}

void TableHeap::load_row_count_from_header() {
    auto result = dm_.read_header_ext_u64(file_id_, kRowCountExtOffset);
    if (result) {
        row_count_.store(*result, std::memory_order_relaxed);
    }
}

void TableHeap::persist_row_count() {
    uint64_t count = row_count_.load(std::memory_order_relaxed);
    auto result = dm_.write_header_ext_u64(file_id_, kRowCountExtOffset, count);
    if (!result) {
        SIXSEVEN_LOG_WARN("failed to persist row count: {}", result.error().message);
    }
}

// -- TableIterator ------------------------------------------------------------

TableIterator::TableIterator(BufferPoolManager& bpm,
                             PageId start_page,
                             uint32_t total_pages,
                             bool strip_mvcc_headers,
                             const TransactionManager* txn_mgr)
    : bpm_(bpm), current_page_(start_page), total_pages_(total_pages),
      strip_mvcc_headers_(strip_mvcc_headers), txn_mgr_(txn_mgr) {}

Result<std::optional<std::pair<RID, std::vector<uint8_t>>>> TableIterator::next() {
    while (!exhausted_ && current_page_ < total_pages_) {
        auto page_result = bpm_.fetch_page(current_page_);
        if (!page_result) {
            // Surface the fetch failure - silently skipping a page would
            // produce an incomplete scan with no error (GDB-915).
            exhausted_ = true;
            return tl::unexpected(page_result.error());
        }

        Page* page = *page_result;
        uint16_t slot_count = page->slot_count();

        // Scan slots on the current page.
        while (current_slot_ < slot_count) {
            // page->get_tuple returns an owned vector, copied under the page's
            // shared latch. That copy is the linearization point protecting
            // this scan from concurrent writers (e.g. the embedding store
            // callback rewriting the same slot in place).
            auto tuple = page->get_tuple(current_slot_);
            if (tuple) {
                // Visibility filter (GDB-747): skip versions created by a
                // known-aborted transaction or logically deleted by a
                // transaction that is not known-aborted.
                if (strip_mvcc_headers_ && tuple->size() >= mvcc_header_size &&
                    !version_visible(read_mvcc_header(*tuple), txn_mgr_)) {
                    current_slot_++;
                    continue;
                }

                RID rid{current_page_, current_slot_};
                current_slot_++;

                auto unpin = bpm_.unpin_page(current_page_, false);
                if (!unpin) {
                    SIXSEVEN_LOG_WARN("unpin failed during scan on page {}: {}",
                                      current_page_,
                                      unpin.error().message);
                }
                if (strip_mvcc_headers_) {
                    // Drop the MVCC header prefix; a tuple shorter than the
                    // header (corrupt) yields an empty payload, which the
                    // deserialization layer rejects per row.
                    auto strip = std::min(tuple->size(), mvcc_header_size);
                    tuple->erase(tuple->begin(),
                                 tuple->begin() + static_cast<std::ptrdiff_t>(strip));
                }
                return ok(std::make_optional(std::make_pair(rid, std::move(*tuple))));
            }
            // Deleted slot — skip.
            current_slot_++;
        }

        // No more slots on this page — move to next.
        auto unpin = bpm_.unpin_page(current_page_, false);
        if (!unpin) {
            SIXSEVEN_LOG_WARN(
                "unpin failed during scan on page {}: {}", current_page_, unpin.error().message);
        }
        current_page_++;
        current_slot_ = 0;
    }

    exhausted_ = true;
    return ok(std::optional<std::pair<RID, std::vector<uint8_t>>>(std::nullopt));
}

void TableIterator::skip(size_t count) {
    // MVCC heaps must honor version visibility (GDB-747): logically deleted
    // slots are still live in the slot directory, so the fast slot-count path
    // would over-skip. Fall back to next(), which applies the filter.
    if (strip_mvcc_headers_) {
        for (size_t i = 0; i < count; ++i) {
            auto r = next();
            // Stop on error (scan is invalid) or on end of heap.
            if (!r || !r->has_value()) {
                break;
            }
        }
        return;
    }

    size_t skipped = 0;

    while (skipped < count && !exhausted_ && current_page_ < total_pages_) {
        auto page_result = bpm_.fetch_page(current_page_);
        if (!page_result) {
            // Stop on a fetch failure - treat the scan as exhausted so the
            // caller does not read past an unreadable page (GDB-915).
            exhausted_ = true;
            break;
        }

        Page* page = *page_result;
        uint16_t slot_count = page->slot_count();

        // Count live slots on this page without copying tuple data.
        while (current_slot_ < slot_count && skipped < count) {
            if (page->is_slot_live(current_slot_)) {
                ++skipped;
            }
            ++current_slot_;
        }

        (void)bpm_.unpin_page(current_page_, false);

        if (current_slot_ >= slot_count) {
            current_page_++;
            current_slot_ = 0;
        }
    }

    if (current_page_ >= total_pages_) {
        exhausted_ = true;
    }
}

} // namespace sixseven
