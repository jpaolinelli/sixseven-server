#pragma once

#include "giodb/common/result.h"
#include "giodb/storage/disk_manager.h"
#include "giodb/storage/page.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace giodb {

/// Frame identifier within the buffer pool (index into the frame array).
using FrameId = uint32_t;

/// Default buffer pool size: 32K frames × 8KB = 256MB.
static constexpr uint32_t default_pool_size = 32768;

// -- LRU-K Replacement Policy ------------------------------------------------

/// LRU-K (K=2) replacement policy for buffer pool eviction.
///
/// Tracks the last K access timestamps for each frame. When eviction is
/// needed, the frame with the maximum backward K-distance is chosen:
///
///   - Frames with fewer than K accesses have infinite backward K-distance
///     and are evicted first (oldest first access wins among ties).
///   - Among frames with K or more accesses, the one whose Kth most recent
///     access is the oldest is evicted.
///
/// This approach is superior to plain LRU because it can distinguish between
/// frequently-accessed pages and pages that were only accessed once (e.g.,
/// during a sequential scan), preventing scan-resistant cache pollution.
class LRUKReplacer {
public:
    /// @param num_frames Total number of frames in the buffer pool.
    /// @param k The K parameter for LRU-K (default: 2).
    explicit LRUKReplacer(uint32_t num_frames, uint32_t k = 2);

    /// Record a new access for the given frame. Maintains the last K timestamps.
    void record_access(FrameId frame_id);

    /// Mark whether a frame is eligible for eviction.
    /// A pinned frame (pin_count > 0) should be set to non-evictable.
    void set_evictable(FrameId frame_id, bool evictable);

    /// Find and remove the frame with the maximum backward K-distance.
    /// Returns an error if no evictable frame exists.
    [[nodiscard]] Result<FrameId> evict();

    /// Remove all tracking for a frame (e.g., when the page is deleted).
    void remove(FrameId frame_id);

    /// Return the number of currently evictable frames.
    [[nodiscard]] uint32_t size() const;

private:
    struct FrameInfo {
        std::vector<uint64_t> history; ///< Last K access timestamps (oldest first).
        bool evictable = false;
    };

    uint32_t k_;
    uint64_t current_timestamp_ = 0;
    std::vector<FrameInfo> frames_;
    uint32_t evictable_count_ = 0;
};

// -- Buffer Pool Manager -----------------------------------------------------

/// Caches pages in memory and manages eviction using LRU-K replacement.
///
/// The buffer pool is the central component through which all page accesses
/// flow. It sits between the higher-level operators and the DiskManager,
/// minimizing disk I/O by keeping hot pages in memory.
///
/// Usage protocol:
/// ```
///   Page* page = bpm.fetch_page(page_id).value();
///   // ... read or modify page ...
///   bpm.unpin_page(page_id, /*is_dirty=*/true);
/// ```
///
/// Thread safety: This class is NOT thread-safe in this initial
/// implementation. Thread safety (mutex + background flusher) will be
/// added in GDB-88.
class BufferPoolManager {
public:
    /// Construct a buffer pool managing pages for a single file.
    /// @param disk_manager Reference to the disk manager for I/O.
    /// @param file_id The file whose pages this pool manages.
    /// @param pool_size Number of frames in the buffer pool.
    BufferPoolManager(DiskManager& disk_manager,
                      FileId file_id,
                      uint32_t pool_size = default_pool_size);

    ~BufferPoolManager();

    // Non-copyable, non-movable.
    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;
    BufferPoolManager(BufferPoolManager&&) = delete;
    BufferPoolManager& operator=(BufferPoolManager&&) = delete;

    /// Fetch a page from the buffer pool, pinning it.
    /// If the page is already cached, the pin count is incremented.
    /// If not cached, reads from disk and places in a free or evicted frame.
    /// Returns an error if the pool is full and no frame is evictable.
    [[nodiscard]] Result<Page*> fetch_page(PageId page_id);

    /// Allocate a new page on disk and bring it into the buffer pool (pinned).
    /// The caller can read page_id() from the returned Page.
    /// Returns an error if disk allocation fails or the pool is full.
    [[nodiscard]] Result<Page*> new_page();

    /// Unpin a page, decrementing its pin count. If is_dirty is true,
    /// the page is marked dirty (will be flushed before eviction).
    /// When pin count reaches 0, the frame becomes eligible for eviction.
    [[nodiscard]] Result<void> unpin_page(PageId page_id, bool is_dirty);

    /// Flush a specific dirty page to disk. No-op if the page is clean.
    [[nodiscard]] Result<void> flush_page(PageId page_id);

    /// Flush all dirty pages to disk.
    [[nodiscard]] Result<void> flush_all();

    /// Delete a page from the buffer pool. The page must have pin_count == 0.
    /// Note: this removes the page from the pool but does not deallocate the
    /// on-disk page (DiskManager does not support page deallocation yet).
    [[nodiscard]] Result<void> delete_page(PageId page_id);

    /// Return the current number of pages in the buffer pool.
    [[nodiscard]] uint32_t pool_page_count() const;

private:
    struct Frame {
        Page page{0, PageType::DATA};
        PageId page_id = 0;
        uint32_t pin_count = 0;
        bool is_dirty = false;
    };

    /// Find an available frame: takes from free list, or evicts a victim.
    /// On eviction, flushes the dirty page and removes it from the page table.
    [[nodiscard]] Result<FrameId> find_victim_frame();

    DiskManager& disk_manager_;
    FileId file_id_;

    std::vector<Frame> frames_;
    std::unordered_map<PageId, FrameId> page_table_;
    std::vector<FrameId> free_list_;
    LRUKReplacer replacer_;
};

} // namespace giodb
