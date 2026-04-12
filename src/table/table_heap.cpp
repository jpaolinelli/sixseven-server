#include "sixseven/table/table_heap.h"

#include "sixseven/common/logging.h"

#include <cstring>
#include <unordered_map>

namespace sixseven {

// Row count is stored in the file header extension area (page 0, bytes 16+).
// Extension offset 0 = row_count (uint64_t).
static constexpr size_t kRowCountExtOffset = 0;

// -- TableHeap ----------------------------------------------------------------

TableHeap::TableHeap(BufferPoolManager& bpm, DiskManager& dm, FileId file_id)
    : bpm_(bpm), dm_(dm), file_id_(file_id) {
    load_row_count_from_header();
}

Result<RID> TableHeap::insert_tuple(std::span<const uint8_t> data) {
    if (data.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "cannot insert empty tuple");
    }

    auto pc = dm_.file_page_count(file_id_);
    if (!pc) {
        return tl::unexpected(pc.error());
    }
    uint32_t total_pages = *pc;

    // Try the last-insert-page hint first.
    if (last_insert_page_ >= 1 && last_insert_page_ < total_pages) {
        auto page_result = bpm_.fetch_page(last_insert_page_);
        if (page_result) {
            Page* page = *page_result;
            auto slot = page->insert_tuple(data);
            if (slot) {
                ++row_count_;
                persist_row_count();
                RID rid{last_insert_page_, *slot};
                auto unpin = bpm_.unpin_page(last_insert_page_, true);
                if (!unpin) {
                    return tl::unexpected(unpin.error());
                }
                return ok(rid);
            }
            // Page full, unpin and try others.
            auto unpin = bpm_.unpin_page(last_insert_page_, false);
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
    if (last_insert_page_ >= 1) {
        PageId next_pid = last_insert_page_ + 1;
        if (next_pid < total_pages) {
            auto page_result = bpm_.fetch_page(next_pid);
            if (page_result) {
                Page* page = *page_result;
                auto slot = page->insert_tuple(data);
                if (slot) {
                    ++row_count_;
                    persist_row_count();
                    last_insert_page_ = next_pid;
                    RID rid{next_pid, *slot};
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

    // No existing page has room — allocate a new one.
    auto new_page_result = bpm_.new_page();
    if (!new_page_result) {
        return tl::unexpected(new_page_result.error());
    }

    Page* new_page = *new_page_result;
    PageId new_pid = new_page->page_id();

    auto slot = new_page->insert_tuple(data);
    if (!slot) {
        // Tuple too large for an empty page.
        auto unpin = bpm_.unpin_page(new_pid, false);
        if (!unpin) {
            SIXSEVEN_LOG_WARN("unpin failed after insert_tuple error on page {}: {}",
                              new_pid,
                              unpin.error().message);
        }
        return tl::unexpected(slot.error());
    }

    ++row_count_;
    persist_row_count();
    last_insert_page_ = new_pid;
    RID rid{new_pid, *slot};

    auto unpin = bpm_.unpin_page(new_pid, true);
    if (!unpin) {
        return tl::unexpected(unpin.error());
    }

    return ok(rid);
}

Result<std::vector<RID>>
TableHeap::insert_batch(const std::vector<std::span<const uint8_t>>& tuples) {
    if (tuples.empty()) {
        return ok(std::vector<RID>{});
    }

    // Pre-validate all tuples so we never partially insert and then fail.
    static constexpr size_t kMaxTupleSize = page_size - page_header_size - slot_entry_size;
    for (size_t i = 0; i < tuples.size(); ++i) {
        if (tuples[i].empty()) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "cannot insert empty tuple at index " + std::to_string(i));
        }
        if (tuples[i].size() > kMaxTupleSize) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "tuple at index " + std::to_string(i) +
                                  " too large for a single page");
        }
    }

    auto pc = dm_.file_page_count(file_id_);
    if (!pc) {
        return tl::unexpected(pc.error());
    }
    uint32_t total_pages = *pc;

    std::vector<RID> rids;
    rids.reserve(tuples.size());
    size_t idx = 0;

    // Helper: try to insert as many tuples as possible into a pinned page.
    auto fill_page = [&](Page* page, PageId pid) {
        while (idx < tuples.size()) {
            auto slot = page->insert_tuple(tuples[idx]);
            if (!slot) {
                break; // Page full.
            }
            rids.push_back(RID{pid, *slot});
            ++idx;
        }
    };

    // Try the hint page first.
    if (last_insert_page_ >= 1 && last_insert_page_ < total_pages) {
        auto page_result = bpm_.fetch_page(last_insert_page_);
        if (page_result) {
            Page* page = *page_result;
            fill_page(page, last_insert_page_);
            bool dirty = (idx > 0);
            auto unpin = bpm_.unpin_page(last_insert_page_, dirty);
            if (!unpin) {
                return tl::unexpected(unpin.error());
            }
        }
    }

    // Try the next page after the hint.
    if (idx < tuples.size() && last_insert_page_ >= 1) {
        PageId next_pid = last_insert_page_ + 1;
        if (next_pid < total_pages) {
            auto page_result = bpm_.fetch_page(next_pid);
            if (page_result) {
                size_t before = idx;
                Page* page = *page_result;
                fill_page(page, next_pid);
                bool dirty = (idx > before);
                if (dirty) {
                    last_insert_page_ = next_pid;
                }
                auto unpin = bpm_.unpin_page(next_pid, dirty);
                if (!unpin) {
                    return tl::unexpected(unpin.error());
                }
            }
        }
    }

    // Allocate new pages as needed for remaining tuples.
    while (idx < tuples.size()) {
        auto new_page_result = bpm_.new_page();
        if (!new_page_result) {
            return tl::unexpected(new_page_result.error());
        }

        Page* new_page = *new_page_result;
        PageId new_pid = new_page->page_id();

        fill_page(new_page, new_pid);
        last_insert_page_ = new_pid;
        auto unpin = bpm_.unpin_page(new_pid, true);
        if (!unpin) {
            return tl::unexpected(unpin.error());
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

    return ok(std::move(*tuple));
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
    auto result = page->update_tuple(rid.slot_id, data);
    if (!result) {
        // If update failed due to space, try compact and retry.
        if (result.error().code == StatusCode::INVALID_ARGUMENT) {
            page->compact();
            result = page->update_tuple(rid.slot_id, data);
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

    auto unpin = bpm_.unpin_page(rid.page_id, true);
    if (!unpin) {
        return tl::unexpected(unpin.error());
    }

    return ok();
}

Result<std::vector<RID>> TableHeap::update_tuples_batch(
    const std::vector<TupleUpdate>& updates) {
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

            auto result = page->update_tuple(upd.rid.slot_id, upd.data);
            if (!result) {
                // Compact once per page if update fails due to fragmentation.
                if (!compacted && result.error().code == StatusCode::INVALID_ARGUMENT) {
                    page->compact();
                    compacted = true;
                    result = page->update_tuple(upd.rid.slot_id, upd.data);
                }
                if (!result) {
                    failed_rids.push_back(upd.rid);
                    continue;
                }
            }
            any_dirty = true;
        }

        auto unpin = bpm_.unpin_page(page_id, any_dirty);
        if (!unpin) {
            SIXSEVEN_LOG_WARN("unpin failed after batch update on page {}: {}",
                              page_id, unpin.error().message);
        }
    }

    return ok(std::move(failed_rids));
}

Result<void> TableHeap::delete_tuple(RID rid) {
    auto page_result = bpm_.fetch_page(rid.page_id);
    if (!page_result) {
        return tl::unexpected(page_result.error());
    }

    Page* page = *page_result;
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
    return ok(TableIterator(bpm_, 1, *pc));
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

TableIterator::TableIterator(BufferPoolManager& bpm, PageId start_page, uint32_t total_pages)
    : bpm_(bpm), current_page_(start_page), total_pages_(total_pages) {}

std::optional<std::pair<RID, std::vector<uint8_t>>> TableIterator::next() {
    while (!exhausted_ && current_page_ < total_pages_) {
        auto page_result = bpm_.fetch_page(current_page_);
        if (!page_result) {
            // Skip pages that can't be fetched.
            current_page_++;
            current_slot_ = 0;
            continue;
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
                RID rid{current_page_, current_slot_};
                current_slot_++;

                auto unpin = bpm_.unpin_page(current_page_, false);
                if (!unpin) {
                    SIXSEVEN_LOG_WARN("unpin failed during scan on page {}: {}",
                                      current_page_,
                                      unpin.error().message);
                }
                return std::make_pair(rid, std::move(*tuple));
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
    return std::nullopt;
}

} // namespace sixseven
