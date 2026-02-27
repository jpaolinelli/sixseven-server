#include "giodb/storage/buffer_pool.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
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
    stop_flusher();

    // Best-effort flush all dirty pages on destruction.
    // Ignoring errors since we can't propagate from a destructor.
    for (auto& [page_id, frame_id] : page_table_) {
        Frame& frame = frames_[frame_id];
        if (frame.is_dirty) {
            (void)write_page_impl(frame.page_id, frame.page);
        }
    }

    if (dwb_fd_ >= 0) {
        ::close(dwb_fd_);
        dwb_fd_ = -1;
    }
}

Result<Page*> BufferPoolManager::fetch_page(PageId page_id) {
    std::lock_guard<std::mutex> lock(latch_);

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

    // Page not in pool -- need to bring it from disk.
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
    std::lock_guard<std::mutex> lock(latch_);

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
    std::lock_guard<std::mutex> lock(latch_);

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
    std::lock_guard<std::mutex> lock(latch_);

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

    auto write_result = write_page_impl(frame.page_id, frame.page);
    if (!write_result) {
        return tl::unexpected(write_result.error());
    }

    frame.is_dirty = false;
    return ok();
}

Result<void> BufferPoolManager::flush_all() {
    std::lock_guard<std::mutex> lock(latch_);

    for (auto& [page_id, frame_id] : page_table_) {
        Frame& frame = frames_[frame_id];
        if (frame.is_dirty) {
            auto write_result = write_page_impl(frame.page_id, frame.page);
            if (!write_result) {
                return tl::unexpected(write_result.error());
            }
            frame.is_dirty = false;
        }
    }
    return ok();
}

Result<void> BufferPoolManager::delete_page(PageId page_id) {
    std::lock_guard<std::mutex> lock(latch_);

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
    std::lock_guard<std::mutex> lock(latch_);
    return static_cast<uint32_t>(page_table_.size());
}

// -- Thread safety & background flusher ---------------------------------------

Result<void> BufferPoolManager::enable_double_write(const std::filesystem::path& dwb_path) {
    std::lock_guard<std::mutex> lock(latch_);

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
    std::lock_guard<std::mutex> lock(latch_);

    if (flusher_running_.load()) {
        return make_error(StatusCode::ALREADY_EXISTS, "background flusher already running");
    }

    flusher_running_.store(true);
    flusher_thread_ = std::thread(&BufferPoolManager::flusher_loop, this, interval);
    return ok();
}

void BufferPoolManager::stop_flusher() {
    // Must NOT hold latch_ here — the flusher thread acquires it during flush.
    if (!flusher_running_.exchange(false)) {
        return; // Already stopped or never started.
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
    // Prefer a free frame.
    if (!free_list_.empty()) {
        FrameId frame_id = free_list_.back();
        free_list_.pop_back();
        return ok(frame_id);
    }

    // No free frames -- evict using LRU-K.
    auto evict_result = replacer_.evict();
    if (!evict_result) {
        return make_error(StatusCode::INTERNAL_ERROR, "buffer pool is full: all frames are pinned");
    }

    FrameId victim_id = *evict_result;
    Frame& victim = frames_[victim_id];

    // Flush dirty page before eviction.
    if (victim.is_dirty) {
        auto write_result = write_page_impl(victim.page_id, victim.page);
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

Result<void> BufferPoolManager::write_page_impl(PageId page_id, Page& page) {
    if (dwb_fd_ >= 0) {
        // Compute and store checksum so the DWB copy has a valid checksum.
        page.set_checksum(compute_page_checksum(page));

        ssize_t written = ::pwrite(dwb_fd_, page.raw().data(), page_size, 0);
        if (written < 0 || static_cast<size_t>(written) != page_size) {
            return make_error(StatusCode::IO_ERROR,
                              "DWB write failed: " + std::string(std::strerror(errno)));
        }

        // Ensure DWB is persisted before writing to the data file.
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

    // Write to the actual data file via DiskManager.
    return disk_manager_.write_page(file_id_, page_id, page);
}

void BufferPoolManager::flush_dirty_pages_locked(bool include_pinned) {
    for (auto& [page_id, frame_id] : page_table_) {
        Frame& frame = frames_[frame_id];
        if (frame.is_dirty && (include_pinned || frame.pin_count == 0)) {
            auto result = write_page_impl(frame.page_id, frame.page);
            if (result) {
                frame.is_dirty = false;
            }
            // Silently skip failures -- background flushing is best-effort.
        }
    }
}

void BufferPoolManager::flusher_loop(std::chrono::milliseconds interval) {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(flusher_mutex_);
            flusher_cv_.wait_for(lock, interval, [this] { return !flusher_running_.load(); });
        }

        bool stopping = !flusher_running_.load();
        {
            std::lock_guard<std::mutex> lock(latch_);
            // On shutdown, flush all dirty pages (including pinned).
            // During normal operation, only flush dirty unpinned pages.
            flush_dirty_pages_locked(/*include_pinned=*/stopping);
        }

        if (stopping) {
            break;
        }
    }
}

} // namespace giodb
