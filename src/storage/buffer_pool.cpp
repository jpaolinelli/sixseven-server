#include "giodb/storage/buffer_pool.h"

#include <algorithm>
#include <limits>

namespace giodb {

// -- LRU-K Replacer -----------------------------------------------------------

LRUKReplacer::LRUKReplacer(uint32_t num_frames, uint32_t k) : k_(k), frames_(num_frames) {}

void LRUKReplacer::record_access(FrameId frame_id) {
    auto& info = frames_[frame_id];
    info.history.push_back(++current_timestamp_);
    // Keep only the last K access timestamps.
    if (info.history.size() > k_) {
        info.history.erase(info.history.begin());
    }
}

void LRUKReplacer::set_evictable(FrameId frame_id, bool evictable) {
    auto& info = frames_[frame_id];
    if (info.evictable && !evictable) {
        --evictable_count_;
    } else if (!info.evictable && evictable) {
        ++evictable_count_;
    }
    info.evictable = evictable;
}

Result<FrameId> LRUKReplacer::evict() {
    if (evictable_count_ == 0) {
        return make_error(StatusCode::NOT_FOUND, "no evictable frames");
    }

    // Find the frame with the maximum backward K-distance.
    //
    // Phase 1: Prefer frames with < K accesses (infinite backward K-distance).
    //          Among these, pick the one with the oldest first access (LRU/FIFO).
    // Phase 2: If all evictable frames have >= K accesses, pick the one whose
    //          Kth most recent access is the oldest (largest backward K-distance).

    FrameId victim = 0;
    bool found = false;
    bool victim_is_inf = false;
    uint64_t victim_oldest_access = std::numeric_limits<uint64_t>::max();
    uint64_t victim_k_distance = 0;

    for (FrameId i = 0; i < static_cast<FrameId>(frames_.size()); ++i) {
        const auto& info = frames_[i];
        if (!info.evictable || info.history.empty()) {
            continue;
        }

        bool is_inf = info.history.size() < k_;

        if (is_inf) {
            // Infinite backward K-distance: candidate for eviction.
            // Among these, prefer the one with the oldest first access.
            if (!found || !victim_is_inf || info.history.front() < victim_oldest_access) {
                victim = i;
                found = true;
                victim_is_inf = true;
                victim_oldest_access = info.history.front();
            }
        } else if (!victim_is_inf) {
            // Finite backward K-distance: current_timestamp - Kth oldest access.
            // The Kth oldest is history.front() since we keep only K entries.
            uint64_t k_distance = current_timestamp_ - info.history.front();
            if (!found || k_distance > victim_k_distance) {
                victim = i;
                found = true;
                victim_k_distance = k_distance;
            }
        }
        // If victim_is_inf is true and current frame has finite distance, skip it.
    }

    if (!found) {
        return make_error(StatusCode::NOT_FOUND, "no evictable frames");
    }

    // Remove the victim from the replacer.
    frames_[victim].history.clear();
    frames_[victim].evictable = false;
    --evictable_count_;

    return ok(victim);
}

void LRUKReplacer::remove(FrameId frame_id) {
    auto& info = frames_[frame_id];
    if (info.evictable) {
        --evictable_count_;
    }
    info.history.clear();
    info.evictable = false;
}

uint32_t LRUKReplacer::size() const {
    return evictable_count_;
}

// -- Buffer Pool Manager ------------------------------------------------------

BufferPoolManager::BufferPoolManager(DiskManager& disk_manager, FileId file_id, uint32_t pool_size)
    : disk_manager_(disk_manager), file_id_(file_id), frames_(pool_size), replacer_(pool_size) {
    // All frames start on the free list.
    free_list_.reserve(pool_size);
    for (FrameId i = 0; i < pool_size; ++i) {
        free_list_.push_back(i);
    }
}

BufferPoolManager::~BufferPoolManager() {
    // Best-effort flush all dirty pages on destruction.
    // Ignoring errors since we can't propagate from a destructor.
    for (auto& [page_id, frame_id] : page_table_) {
        Frame& frame = frames_[frame_id];
        if (frame.is_dirty) {
            (void)disk_manager_.write_page(file_id_, frame.page_id, frame.page);
        }
    }
}

Result<Page*> BufferPoolManager::fetch_page(PageId page_id) {
    // Check if the page is already in the buffer pool.
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        FrameId frame_id = it->second;
        Frame& frame = frames_[frame_id];
        ++frame.pin_count;
        replacer_.record_access(frame_id);
        replacer_.set_evictable(frame_id, false);
        return ok(&frame.page);
    }

    // Page not in pool — need to bring it from disk.
    auto frame_result = find_victim_frame();
    if (!frame_result) {
        return tl::unexpected(frame_result.error());
    }
    FrameId frame_id = *frame_result;
    Frame& frame = frames_[frame_id];

    // Read the page from disk.
    auto read_result = disk_manager_.read_page(file_id_, page_id, frame.page);
    if (!read_result) {
        // Put the frame back on the free list.
        free_list_.push_back(frame_id);
        return tl::unexpected(read_result.error());
    }

    // Set up frame metadata.
    frame.page_id = page_id;
    frame.pin_count = 1;
    frame.is_dirty = false;

    // Update page table and replacer.
    page_table_[page_id] = frame_id;
    replacer_.record_access(frame_id);
    replacer_.set_evictable(frame_id, false);

    return ok(&frame.page);
}

Result<Page*> BufferPoolManager::new_page() {
    // Allocate a new page on disk.
    auto alloc_result = disk_manager_.allocate_page(file_id_);
    if (!alloc_result) {
        return tl::unexpected(alloc_result.error());
    }
    PageId page_id = *alloc_result;

    // Find a frame to hold the new page.
    auto frame_result = find_victim_frame();
    if (!frame_result) {
        return tl::unexpected(frame_result.error());
    }
    FrameId frame_id = *frame_result;
    Frame& frame = frames_[frame_id];

    // Initialize the page in-memory.
    frame.page = Page(page_id, PageType::DATA);
    frame.page_id = page_id;
    frame.pin_count = 1;
    frame.is_dirty = false;

    // Update page table and replacer.
    page_table_[page_id] = frame_id;
    replacer_.record_access(frame_id);
    replacer_.set_evictable(frame_id, false);

    return ok(&frame.page);
}

Result<void> BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "page " + std::to_string(page_id) + " not in buffer pool");
    }

    FrameId frame_id = it->second;
    Frame& frame = frames_[frame_id];

    if (frame.pin_count == 0) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "page " + std::to_string(page_id) + " is not pinned");
    }

    // Sticky dirty flag: once dirty, stays dirty until flushed.
    if (is_dirty) {
        frame.is_dirty = true;
    }

    --frame.pin_count;
    if (frame.pin_count == 0) {
        replacer_.set_evictable(frame_id, true);
    }

    return ok();
}

Result<void> BufferPoolManager::flush_page(PageId page_id) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "page " + std::to_string(page_id) + " not in buffer pool");
    }

    FrameId frame_id = it->second;
    Frame& frame = frames_[frame_id];

    if (!frame.is_dirty) {
        return ok(); // Nothing to flush.
    }

    auto write_result = disk_manager_.write_page(file_id_, frame.page_id, frame.page);
    if (!write_result) {
        return tl::unexpected(write_result.error());
    }

    frame.is_dirty = false;
    return ok();
}

Result<void> BufferPoolManager::flush_all() {
    for (auto& [page_id, frame_id] : page_table_) {
        Frame& frame = frames_[frame_id];
        if (frame.is_dirty) {
            auto write_result = disk_manager_.write_page(file_id_, frame.page_id, frame.page);
            if (!write_result) {
                return tl::unexpected(write_result.error());
            }
            frame.is_dirty = false;
        }
    }
    return ok();
}

Result<void> BufferPoolManager::delete_page(PageId page_id) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return make_error(StatusCode::NOT_FOUND,
                          "page " + std::to_string(page_id) + " not in buffer pool");
    }

    FrameId frame_id = it->second;
    Frame& frame = frames_[frame_id];

    if (frame.pin_count > 0) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "cannot delete pinned page " + std::to_string(page_id));
    }

    // Remove from page table and replacer.
    page_table_.erase(it);
    replacer_.remove(frame_id);

    // Reset frame and return to free list.
    frame.page_id = 0;
    frame.pin_count = 0;
    frame.is_dirty = false;
    free_list_.push_back(frame_id);

    return ok();
}

uint32_t BufferPoolManager::pool_page_count() const {
    return static_cast<uint32_t>(page_table_.size());
}

// -- Private helpers ----------------------------------------------------------

Result<FrameId> BufferPoolManager::find_victim_frame() {
    // Prefer a free frame.
    if (!free_list_.empty()) {
        FrameId frame_id = free_list_.back();
        free_list_.pop_back();
        return ok(frame_id);
    }

    // No free frames — evict using LRU-K.
    auto evict_result = replacer_.evict();
    if (!evict_result) {
        return make_error(StatusCode::INTERNAL_ERROR, "buffer pool is full: all frames are pinned");
    }

    FrameId victim_id = *evict_result;
    Frame& victim = frames_[victim_id];

    // Flush dirty page before eviction.
    if (victim.is_dirty) {
        auto write_result = disk_manager_.write_page(file_id_, victim.page_id, victim.page);
        if (!write_result) {
            return tl::unexpected(write_result.error());
        }
        victim.is_dirty = false;
    }

    // Remove evicted page from page table.
    page_table_.erase(victim.page_id);
    victim.pin_count = 0;

    return ok(victim_id);
}

} // namespace giodb
