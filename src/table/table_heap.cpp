#include "giodb/table/table_heap.h"

#include "giodb/common/logging.h"

#include <cstring>

namespace giodb {

// -- TableHeap ----------------------------------------------------------------

TableHeap::TableHeap(BufferPoolManager& bpm, DiskManager& dm, FileId file_id)
    : bpm_(bpm), dm_(dm), file_id_(file_id) {}

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

    // First-fit scan: try each existing data page (pages 1 through total_pages-1).
    for (uint32_t pid = 1; pid < total_pages; ++pid) {
        if (pid == last_insert_page_) {
            continue; // Already tried.
        }

        auto page_result = bpm_.fetch_page(pid);
        if (!page_result) {
            continue; // Skip pages that can't be fetched.
        }

        Page* page = *page_result;
        auto slot = page->insert_tuple(data);
        if (slot) {
            last_insert_page_ = pid;
            RID rid{pid, *slot};
            auto unpin = bpm_.unpin_page(pid, true);
            if (!unpin) {
                return tl::unexpected(unpin.error());
            }
            return ok(rid);
        }

        // Page full, unpin and continue.
        auto unpin = bpm_.unpin_page(pid, false);
        if (!unpin) {
            return tl::unexpected(unpin.error());
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
            GIODB_LOG_WARN("unpin failed after insert_tuple error on page {}: {}",
                           new_pid,
                           unpin.error().message);
        }
        return tl::unexpected(slot.error());
    }

    last_insert_page_ = new_pid;
    RID rid{new_pid, *slot};

    auto unpin = bpm_.unpin_page(new_pid, true);
    if (!unpin) {
        return tl::unexpected(unpin.error());
    }

    return ok(rid);
}

Result<std::vector<uint8_t>> TableHeap::get_tuple(RID rid) {
    auto page_result = bpm_.fetch_page(rid.page_id);
    if (!page_result) {
        return tl::unexpected(page_result.error());
    }

    Page* page = *page_result;
    auto tuple_span = page->get_tuple(rid.slot_id);
    if (!tuple_span) {
        auto unpin = bpm_.unpin_page(rid.page_id, false);
        if (!unpin) {
            GIODB_LOG_WARN("unpin failed after get_tuple error on page {}: {}",
                           rid.page_id,
                           unpin.error().message);
        }
        return tl::unexpected(tuple_span.error());
    }

    // Copy the data before unpinning.
    std::vector<uint8_t> data(tuple_span->begin(), tuple_span->end());

    auto unpin = bpm_.unpin_page(rid.page_id, false);
    if (!unpin) {
        return tl::unexpected(unpin.error());
    }

    return ok(std::move(data));
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
                GIODB_LOG_WARN("unpin failed after update_tuple error on page {}: {}",
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
            GIODB_LOG_WARN("unpin failed after delete_tuple error on page {}: {}",
                           rid.page_id,
                           unpin.error().message);
        }
        return tl::unexpected(result.error());
    }

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
            auto tuple_span = page->get_tuple(current_slot_);
            if (tuple_span) {
                // Found a live tuple — copy it and advance.
                RID rid{current_page_, current_slot_};
                std::vector<uint8_t> data(tuple_span->begin(), tuple_span->end());
                current_slot_++;

                auto unpin = bpm_.unpin_page(current_page_, false);
                if (!unpin) {
                    GIODB_LOG_WARN("unpin failed during scan on page {}: {}",
                                   current_page_,
                                   unpin.error().message);
                }
                return std::make_pair(rid, std::move(data));
            }
            // Deleted slot — skip.
            current_slot_++;
        }

        // No more slots on this page — move to next.
        auto unpin = bpm_.unpin_page(current_page_, false);
        if (!unpin) {
            GIODB_LOG_WARN(
                "unpin failed during scan on page {}: {}", current_page_, unpin.error().message);
        }
        current_page_++;
        current_slot_ = 0;
    }

    exhausted_ = true;
    return std::nullopt;
}

} // namespace giodb
