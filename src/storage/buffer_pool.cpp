#include "sixseven/storage/buffer_pool.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace sixseven {

// -- LRU-K Replacer -----------------------------------------------------------

LRUKReplacer::LRUKReplacer(uint32_t num_frames, uint32_t k) : k_(k), frames_(num_frames) {}

void LRUKReplacer::record_access(FrameId frame_id) {
    auto& info = frames_[frame_id];

    // If this frame is currently in the heap, its priority key is about to
    // change. Remove the old entry before mutating history.
    if (info.evictable && !info.history.empty()) {
        heap_.erase(make_entry(info, frame_id));
    }

    info.history.push_back(++current_timestamp_);
    // Keep only the last K access timestamps.
    if (info.history.size() > k_) {
        info.history.erase(info.history.begin());
    }

    // Re-insert under the new priority key (or insert for the first time if
    // the frame is evictable but had no history before).
    if (info.evictable) {
        heap_.insert(make_entry(info, frame_id));
    }
}

void LRUKReplacer::set_evictable(FrameId frame_id, bool evictable) {
    auto& info = frames_[frame_id];
    if (info.evictable == evictable) {
        return;
    }

    if (evictable) {
        info.evictable = true;
        ++evictable_count_;
        if (!info.history.empty()) {
            heap_.insert(make_entry(info, frame_id));
        }
    } else {
        if (!info.history.empty()) {
            heap_.erase(make_entry(info, frame_id));
        }
        info.evictable = false;
        --evictable_count_;
    }
}

Result<FrameId> LRUKReplacer::evict() {
    return evict(nullptr);
}

Result<FrameId> LRUKReplacer::evict(const std::function<bool(FrameId)>& skip) {
    if (heap_.empty()) {
        return make_error(StatusCode::NOT_FOUND, "no evictable frames");
    }

    auto pop_victim = [&](std::set<HeapEntry>::iterator it) -> FrameId {
        FrameId victim = it->frame_id;
        heap_.erase(it);
        FrameInfo& info = frames_[victim];
        info.history.clear();
        info.evictable = false;
        --evictable_count_;
        return victim;
    };

    // Fast path: no skip predicate. The smallest entry (set begin) is the
    // highest-priority victim by construction.
    if (!skip) {
        return ok(pop_victim(heap_.begin()));
    }

    // Pass 1: walk the heap in priority order and pick the first frame for
    // which `skip` is false. Iterating an std::set is O(1) per step, so this
    // is O(log n) when the head is non-skipped (the common case) and at worst
    // O(skipped + log n) when several head entries must be passed over.
    for (auto it = heap_.begin(); it != heap_.end(); ++it) {
        if (!skip(it->frame_id)) {
            return ok(pop_victim(it));
        }
    }

    // Pass 2: every evictable frame is skipped — fall back to the
    // highest-priority skipped frame.
    return ok(pop_victim(heap_.begin()));
}

void LRUKReplacer::remove(FrameId frame_id) {
    auto& info = frames_[frame_id];
    if (info.evictable) {
        if (!info.history.empty()) {
            heap_.erase(make_entry(info, frame_id));
        }
        --evictable_count_;
    }
    info.history.clear();
    info.evictable = false;
}

uint32_t LRUKReplacer::size() const {
    return evictable_count_;
}

// -- Buffer Pool Manager ------------------------------------------------------
//
// Lock hierarchy (to avoid deadlocks, always acquire in this order):
//   pool_mutex_ > shard.mutex > frame.latch
//   replacer_mutex_ is acquired independently (never nested with shard/frame)
//
// Hot path (cache hit):
//   shard → frame (pin)  →  release both  →  replacer (record_access)
//
// Miss path (under pool_mutex_):
//   pool_mutex_ → shard → frame (eviction)  →  release shard+frame
//   pool_mutex_ → frame (setup)  →  release frame
//   pool_mutex_ → shard (insert)  →  release shard
//   pool_mutex_ → replacer (update)  →  release replacer

BufferPoolManager::BufferPoolManager(DiskManager& disk_manager, FileId file_id, uint32_t pool_size)
    : disk_manager_(disk_manager), file_id_(file_id), frames_(pool_size), replacer_(pool_size) {
    // All frames start on the free list.
    free_list_.reserve(pool_size);
    for (FrameId i = 0; i < pool_size; ++i) {
        free_list_.push_back(i);
    }
}

BufferPoolManager::~BufferPoolManager() {
    stop_flusher();

    // Best-effort flush all dirty pages on destruction.
    for (uint32_t s = 0; s < kNumShards; ++s) {
        auto& shard = page_table_shards_[s];
        for (auto& [page_id, frame_id] : shard.map) {
            Frame& frame = frames_[frame_id];
            if (frame.is_dirty.load(std::memory_order_relaxed)) {
                (void)write_page_impl(frame.page_id, frame.page);
            }
        }
    }

    if (dwb_fd_ >= 0) {
        ::close(dwb_fd_);
        dwb_fd_ = -1;
    }
}

Result<Page*> BufferPoolManager::fetch_page(PageId page_id) {
    auto& shard = get_shard(page_id);

    // Fast path: check if page is already cached.
    // Nest shard → frame to prevent eviction race, then update replacer separately.
    {
        std::lock_guard<std::mutex> shard_lock(shard.mutex);
        auto it = shard.map.find(page_id);
        if (it != shard.map.end()) {
            FrameId frame_id = it->second;
            Frame& frame = frames_[frame_id];
            {
                std::lock_guard<std::mutex> frame_lock(frame.latch);
                ++frame.pin_count;
            }
            // Release shard+frame, then update replacer (never nest replacer inside shard/frame).
            {
                std::lock_guard<std::mutex> rlock(replacer_mutex_);
                replacer_.record_access(frame_id);
                replacer_.set_evictable(frame_id, false);
            }
            hits_.fetch_add(1, std::memory_order_relaxed);
            return ok(&frame.page);
        }
    }

    // Slow path: cache miss — serialize structural changes.
    std::lock_guard<std::mutex> pool_lock(pool_mutex_);

    // Double-check: another thread may have loaded the page while we waited.
    {
        std::lock_guard<std::mutex> shard_lock(shard.mutex);
        auto it = shard.map.find(page_id);
        if (it != shard.map.end()) {
            FrameId frame_id = it->second;
            Frame& frame = frames_[frame_id];
            {
                std::lock_guard<std::mutex> frame_lock(frame.latch);
                ++frame.pin_count;
            }
            {
                std::lock_guard<std::mutex> rlock(replacer_mutex_);
                replacer_.record_access(frame_id);
                replacer_.set_evictable(frame_id, false);
            }
            hits_.fetch_add(1, std::memory_order_relaxed);
            return ok(&frame.page);
        }
    }

    // Find a victim frame (free list or eviction).
    auto frame_result = find_victim_frame();
    if (!frame_result) {
        return tl::unexpected(frame_result.error());
    }
    FrameId frame_id = *frame_result;
    Frame& frame = frames_[frame_id];

    // Read the page from disk and set up frame under frame latch.
    {
        std::lock_guard<std::mutex> frame_lock(frame.latch);
        auto read_result = disk_manager_.read_page(file_id_, page_id, frame.page);
        if (!read_result) {
            free_list_.push_back(frame_id);
            return tl::unexpected(read_result.error());
        }
        frame.page_id = page_id;
        frame.pin_count = 1;
        frame.is_dirty.store(false, std::memory_order_relaxed);
    }

    // Insert into page table shard.
    {
        std::lock_guard<std::mutex> shard_lock(shard.mutex);
        shard.map[page_id] = frame_id;
    }

    // Update replacer.
    {
        std::lock_guard<std::mutex> rlock(replacer_mutex_);
        replacer_.record_access(frame_id);
        replacer_.set_evictable(frame_id, false);
    }
    misses_.fetch_add(1, std::memory_order_relaxed);

    return ok(&frame.page);
}

Result<Page*> BufferPoolManager::new_page() {
    // Serialize structural changes.
    std::lock_guard<std::mutex> pool_lock(pool_mutex_);

    // Find a frame first to avoid leaking a disk page ID on failure.
    auto frame_result = find_victim_frame();
    if (!frame_result) {
        return tl::unexpected(frame_result.error());
    }
    FrameId frame_id = *frame_result;

    // Allocate a new page on disk (only after securing a frame).
    auto alloc_result = disk_manager_.allocate_page(file_id_);
    if (!alloc_result) {
        // Return frame to free list since allocation failed.
        free_list_.push_back(frame_id);
        return tl::unexpected(alloc_result.error());
    }
    PageId page_id = *alloc_result;
    Frame& frame = frames_[frame_id];

    // Initialize the page in-memory under frame latch.
    {
        std::lock_guard<std::mutex> frame_lock(frame.latch);
        frame.page = Page(page_id, PageType::DATA);
        frame.page_id = page_id;
        frame.pin_count = 1;
        frame.is_dirty.store(false, std::memory_order_relaxed);
    }

    // Insert into page table shard.
    {
        auto& shard = get_shard(page_id);
        std::lock_guard<std::mutex> shard_lock(shard.mutex);
        shard.map[page_id] = frame_id;
    }

    // Update replacer.
    {
        std::lock_guard<std::mutex> rlock(replacer_mutex_);
        replacer_.record_access(frame_id);
        replacer_.set_evictable(frame_id, false);
    }

    return ok(&frame.page);
}

Result<void> BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    // Look up the frame. Page is pinned so it can't be evicted between
    // shard lookup and frame lock.
    FrameId frame_id;
    {
        auto& shard = get_shard(page_id);
        std::lock_guard<std::mutex> shard_lock(shard.mutex);
        auto it = shard.map.find(page_id);
        if (it == shard.map.end()) {
            return make_error(StatusCode::NOT_FOUND,
                              "page " + std::to_string(page_id) + " not in buffer pool");
        }
        frame_id = it->second;
    }

    Frame& frame = frames_[frame_id];
    bool became_evictable = false;
    bool newly_dirty = false;
    {
        std::lock_guard<std::mutex> frame_lock(frame.latch);

        if (frame.pin_count == 0) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "page " + std::to_string(page_id) + " is not pinned");
        }

        // Sticky dirty flag: once dirty, stays dirty until flushed.
        if (is_dirty && !frame.is_dirty.load(std::memory_order_relaxed)) {
            frame.is_dirty.store(true, std::memory_order_relaxed);
            dirty_count_.fetch_add(1, std::memory_order_relaxed);
            newly_dirty = true;
        }

        --frame.pin_count;
        became_evictable = (frame.pin_count == 0);
    }

    // Update replacer outside frame lock (replacer acquired independently).
    if (became_evictable) {
        std::lock_guard<std::mutex> rlock(replacer_mutex_);
        replacer_.set_evictable(frame_id, true);
    }

    // Wake the background flusher if dirty ratio crossed the threshold.
    if (newly_dirty && flusher_running_.load(std::memory_order_relaxed) &&
        dirty_ratio() >= dirty_flush_threshold_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> flock(flusher_mutex_);
        flusher_cv_.notify_one();
    }

    return ok();
}

Result<void> BufferPoolManager::flush_page(PageId page_id) {
    // Look up frame.
    FrameId frame_id;
    {
        auto& shard = get_shard(page_id);
        std::lock_guard<std::mutex> shard_lock(shard.mutex);
        auto it = shard.map.find(page_id);
        if (it == shard.map.end()) {
            return make_error(StatusCode::NOT_FOUND,
                              "page " + std::to_string(page_id) + " not in buffer pool");
        }
        frame_id = it->second;
    }

    Frame& frame = frames_[frame_id];
    std::lock_guard<std::mutex> frame_lock(frame.latch);

    // Verify the frame still holds this page (could have been evicted and reused).
    if (frame.page_id != page_id) {
        return make_error(StatusCode::NOT_FOUND,
                          "page " + std::to_string(page_id) + " not in buffer pool");
    }

    if (!frame.is_dirty.load(std::memory_order_relaxed)) {
        return ok();
    }

    auto write_result = write_page_impl(frame.page_id, frame.page);
    if (!write_result) {
        return tl::unexpected(write_result.error());
    }

    frame.is_dirty.store(false, std::memory_order_relaxed);
    dirty_count_.fetch_sub(1, std::memory_order_relaxed);
    return ok();
}

Result<void> BufferPoolManager::flush_all() {
    // Iterate all shards, collect frames, and flush dirty ones.
    for (uint32_t s = 0; s < kNumShards; ++s) {
        auto& shard = page_table_shards_[s];

        std::vector<std::pair<PageId, FrameId>> entries;
        {
            std::lock_guard<std::mutex> shard_lock(shard.mutex);
            entries.assign(shard.map.begin(), shard.map.end());
        }

        for (auto& [pid, fid] : entries) {
            Frame& frame = frames_[fid];
            std::lock_guard<std::mutex> frame_lock(frame.latch);
            if (frame.is_dirty.load(std::memory_order_relaxed) && frame.page_id == pid) {
                auto write_result = write_page_impl(frame.page_id, frame.page);
                if (!write_result) {
                    return tl::unexpected(write_result.error());
                }
                frame.is_dirty.store(false, std::memory_order_relaxed);
                dirty_count_.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    }
    return ok();
}

Result<void> BufferPoolManager::delete_page(PageId page_id) {
    auto& shard = get_shard(page_id);
    FrameId frame_id;

    // Lock shard → frame (consistent with hit path ordering).
    {
        std::lock_guard<std::mutex> shard_lock(shard.mutex);
        auto it = shard.map.find(page_id);
        if (it == shard.map.end()) {
            return make_error(StatusCode::NOT_FOUND,
                              "page " + std::to_string(page_id) + " not in buffer pool");
        }
        frame_id = it->second;
        Frame& frame = frames_[frame_id];

        std::lock_guard<std::mutex> frame_lock(frame.latch);

        if (frame.pin_count > 0) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "cannot delete pinned page " + std::to_string(page_id));
        }

        shard.map.erase(it);
        frame.page_id = 0;
        frame.pin_count = 0;
        if (frame.is_dirty.load(std::memory_order_relaxed)) {
            frame.is_dirty.store(false, std::memory_order_relaxed);
            dirty_count_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    // Update replacer and free list (replacer acquired independently).
    {
        std::lock_guard<std::mutex> rlock(replacer_mutex_);
        replacer_.remove(frame_id);
    }
    {
        std::lock_guard<std::mutex> pool_lock(pool_mutex_);
        free_list_.push_back(frame_id);
    }

    return ok();
}

uint32_t BufferPoolManager::pool_page_count() const {
    uint32_t count = 0;
    for (uint32_t s = 0; s < kNumShards; ++s) {
        auto& shard = page_table_shards_[s];
        std::lock_guard<std::mutex> shard_lock(shard.mutex);
        count += static_cast<uint32_t>(shard.map.size());
    }
    return count;
}

// -- Thread safety & background flusher ---------------------------------------

Result<void> BufferPoolManager::enable_double_write(const std::filesystem::path& dwb_path) {
    std::lock_guard<std::mutex> lock(dwb_mutex_);

    if (dwb_fd_ >= 0) {
        return make_error(StatusCode::ALREADY_EXISTS, "double-write buffer already enabled");
    }

    int fd = ::open(dwb_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to create DWB file: " + std::string(std::strerror(errno)));
    }

    // Pre-allocate one page of space.
    if (::ftruncate(fd, static_cast<off_t>(page_size)) < 0) {
        ::close(fd);
        return make_error(StatusCode::IO_ERROR,
                          "failed to extend DWB file: " + std::string(std::strerror(errno)));
    }

    dwb_fd_ = fd;
    return ok();
}

Result<void> BufferPoolManager::start_flusher(std::chrono::milliseconds interval) {
    if (flusher_running_.load()) {
        return make_error(StatusCode::ALREADY_EXISTS, "background flusher already running");
    }

    flusher_running_.store(true);
    flusher_thread_ = std::thread(&BufferPoolManager::flusher_loop, this, interval);
    return ok();
}

void BufferPoolManager::stop_flusher() {
    if (!flusher_running_.exchange(false)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(flusher_mutex_);
        flusher_cv_.notify_one();
    }
    if (flusher_thread_.joinable()) {
        flusher_thread_.join();
    }
}

// -- Private helpers ----------------------------------------------------------

Result<FrameId> BufferPoolManager::find_victim_frame() {
    // Must be called while holding pool_mutex_.

    // Prefer a free frame.
    if (!free_list_.empty()) {
        FrameId frame_id = free_list_.back();
        free_list_.pop_back();
        return ok(frame_id);
    }

    // No free frames -- evict using LRU-K, preferring clean frames.
    // May need to retry if the victim was concurrently pinned.
    while (true) {
        FrameId victim_id;
        {
            std::lock_guard<std::mutex> rlock(replacer_mutex_);
            // is_dirty is atomic, so the skip lambda needs no frame lock.
            auto evict_result = replacer_.evict([this](FrameId fid) {
                return frames_[fid].is_dirty.load(std::memory_order_relaxed);
            });
            if (!evict_result) {
                return make_error(StatusCode::INTERNAL_ERROR,
                                  "buffer pool is full: all frames are pinned");
            }
            victim_id = *evict_result;
        }

        Frame& victim = frames_[victim_id];

        // Read page_id for shard lookup. page_id is only written under pool_mutex_
        // (which we hold), so this read is safe without frame latch.
        PageId victim_page_id = victim.page_id;

        // Lock shard → frame (same order as hit path) to atomically verify and evict.
        auto& victim_shard = get_shard(victim_page_id);
        std::lock_guard<std::mutex> shard_lock(victim_shard.mutex);
        std::lock_guard<std::mutex> victim_lock(victim.latch);

        // Check if someone pinned the frame between evict() and here.
        if (victim.pin_count > 0) {
            // The hit-path thread already called record_access/set_evictable(false)
            // on the replacer for this frame, so it's correctly tracked. Retry.
            continue;
        }

        // Flush dirty page before eviction.
        if (victim.is_dirty.load(std::memory_order_relaxed)) {
            auto write_result = write_page_impl(victim.page_id, victim.page);
            if (!write_result) {
                return tl::unexpected(write_result.error());
            }
            victim.is_dirty.store(false, std::memory_order_relaxed);
            dirty_count_.fetch_sub(1, std::memory_order_relaxed);
        }

        // Remove evicted page from page table shard.
        victim_shard.map.erase(victim_page_id);
        victim.pin_count = 0;

        return ok(victim_id);
    }
}

Result<void> BufferPoolManager::write_page_impl(PageId page_id, Page& page) {
    if (dwb_fd_ >= 0) {
        std::lock_guard<std::mutex> dwb_lock(dwb_mutex_);

        // Compute and store checksum so the DWB copy has a valid checksum.
        page.set_checksum(compute_page_checksum(page));

        ssize_t written = ::pwrite(dwb_fd_, page.raw().data(), page_size, 0);
        if (written < 0 || static_cast<size_t>(written) != page_size) {
            return make_error(StatusCode::IO_ERROR,
                              "DWB write failed: " + std::string(std::strerror(errno)));
        }

#ifdef __APPLE__
        if (::fcntl(dwb_fd_, F_FULLFSYNC) < 0) {
            return make_error(StatusCode::IO_ERROR,
                              "DWB fsync failed: " + std::string(std::strerror(errno)));
        }
#else
        if (::fsync(dwb_fd_) < 0) {
            return make_error(StatusCode::IO_ERROR,
                              "DWB fsync failed: " + std::string(std::strerror(errno)));
        }
#endif
    }

    return disk_manager_.write_page(file_id_, page_id, page);
}

void BufferPoolManager::flush_dirty_pages(bool include_pinned) {
    for (uint32_t s = 0; s < kNumShards; ++s) {
        auto& shard = page_table_shards_[s];

        std::vector<std::pair<PageId, FrameId>> entries;
        {
            std::lock_guard<std::mutex> shard_lock(shard.mutex);
            entries.assign(shard.map.begin(), shard.map.end());
        }

        for (auto& [pid, fid] : entries) {
            Frame& frame = frames_[fid];
            std::lock_guard<std::mutex> frame_lock(frame.latch);
            if (frame.is_dirty.load(std::memory_order_relaxed) && frame.page_id == pid &&
                (include_pinned || frame.pin_count == 0)) {
                auto result = write_page_impl(frame.page_id, frame.page);
                if (result) {
                    frame.is_dirty.store(false, std::memory_order_relaxed);
                    dirty_count_.fetch_sub(1, std::memory_order_relaxed);
                }
            }
        }
    }
}

void BufferPoolManager::flusher_loop(std::chrono::milliseconds interval) {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(flusher_mutex_);
            flusher_cv_.wait_for(lock, interval, [this] {
                return !flusher_running_.load() ||
                       dirty_ratio() >= dirty_flush_threshold_.load(std::memory_order_relaxed);
            });
        }

        bool stopping = !flusher_running_.load();
        flush_dirty_pages(/*include_pinned=*/stopping);

        if (stopping) {
            break;
        }
    }
}

} // namespace sixseven
