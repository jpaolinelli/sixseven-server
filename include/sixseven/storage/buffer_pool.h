#pragma once

#include "sixseven/common/result.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/page.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sixseven {

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
///
/// Internally, evictable frames are tracked in an ordered set keyed by their
/// eviction priority. This makes `evict()` O(log n) in the no-skip case (and
/// in the typical skip case where few frames at the head are filtered out),
/// versus O(n) for a linear scan over all frames.
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

    /// Find and remove the frame with the maximum backward K-distance,
    /// preferring frames for which @p skip returns false (clean frames).
    /// First pass: only consider frames where skip(frame_id) is false.
    /// Second pass: fall back to frames where skip(frame_id) is true.
    /// @param skip Predicate returning true for frames to deprioritize.
    [[nodiscard]] Result<FrameId> evict(const std::function<bool(FrameId)>& skip);

    /// Remove all tracking for a frame (e.g., when the page is deleted).
    void remove(FrameId frame_id);

    /// Return the number of currently evictable frames.
    [[nodiscard]] uint32_t size() const;

private:
    struct FrameInfo {
        std::vector<uint64_t> history; ///< Last K access timestamps (oldest first).
        bool evictable = false;
    };

    /// Heap entry encoding eviction priority for a single evictable frame.
    ///
    /// Sort order (smallest = next victim):
    ///   1. `is_finite=false` (infinite K-distance) sorts before `is_finite=true`
    ///      so frames with fewer than K accesses are always evicted first.
    ///   2. Within the same class, smaller `key` (the oldest kept timestamp)
    ///      sorts first — for infinite frames this is the oldest first access;
    ///      for finite frames this is the Kth most recent access (the one that
    ///      maximizes backward K-distance).
    ///   3. `frame_id` is a final tiebreaker for strict weak ordering. In
    ///      practice, timestamps are unique so this rarely matters.
    struct HeapEntry {
        bool is_finite;
        uint64_t key;
        FrameId frame_id;

        bool operator<(const HeapEntry& other) const noexcept {
            // Infinite K-distance (is_finite=false) sorts before finite.
            if (is_finite != other.is_finite) {
                return !is_finite;
            }
            if (key != other.key) {
                return key < other.key;
            }
            return frame_id < other.frame_id;
        }
    };

    [[nodiscard]] HeapEntry make_entry(const FrameInfo& info, FrameId fid) const noexcept {
        return HeapEntry{
            /*is_finite=*/info.history.size() >= k_,
            /*key=*/info.history.front(),
            /*frame_id=*/fid,
        };
    }

    uint32_t k_;
    uint64_t current_timestamp_ = 0;
    std::vector<FrameInfo> frames_;
    uint32_t evictable_count_ = 0;
    /// Indexed priority queue of frames eligible for eviction. Contains an
    /// entry for every frame where `evictable == true && !history.empty()`.
    std::set<HeapEntry> heap_;
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
/// Thread safety: This class uses fine-grained locking — sharded page
/// table, per-frame mutexes, and separate replacer/pool locks — so that
/// concurrent access to different pages does not serialize.
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

    /// Return the number of buffer pool hits (page already in pool).
    [[nodiscard]] uint64_t hit_count() const { return hits_.load(std::memory_order_relaxed); }

    /// Return the number of buffer pool misses (page read from disk).
    [[nodiscard]] uint64_t miss_count() const { return misses_.load(std::memory_order_relaxed); }

    /// Return the current number of dirty frames in the pool.
    [[nodiscard]] uint32_t dirty_count() const {
        return dirty_count_.load(std::memory_order_relaxed);
    }

    /// Return the number of page-write failures observed by the background
    /// flusher and the destructor's best-effort final flush (GDB-778). These
    /// paths cannot propagate a Result, so failures are logged and counted.
    [[nodiscard]] uint64_t flush_error_count() const {
        return flush_errors_.load(std::memory_order_relaxed);
    }

    /// Return the fraction of frames that are dirty (0.0 to 1.0).
    [[nodiscard]] double dirty_ratio() const {
        return static_cast<double>(dirty_count_.load(std::memory_order_relaxed)) /
               static_cast<double>(frames_.size());
    }

    // -- Thread safety & background flusher ----------------------------------

    /// Enable the double-write buffer for torn page protection.
    /// Pages are written to the DWB file first, fsynced, then written to the
    /// data file. If a crash occurs during the data file write, recovery can
    /// restore the page from the DWB.
    [[nodiscard]] Result<void> enable_double_write(const std::filesystem::path& dwb_path);

    /// Set the dirty ratio threshold that triggers the background flusher.
    /// When the fraction of dirty frames exceeds this threshold, the flusher
    /// is woken immediately rather than waiting for the timer.
    /// Values outside [0.0, 1.0] are clamped: negative values become 0.0
    /// (always wake), values above 1.0 become 1.0 (never wake on threshold).
    /// @param threshold Dirty ratio threshold, clamped to [0.0, 1.0].
    void set_dirty_flush_threshold(double threshold) {
        if (threshold < 0.0) {
            threshold = 0.0;
        } else if (threshold > 1.0) {
            threshold = 1.0;
        }
        dirty_flush_threshold_.store(threshold, std::memory_order_relaxed);
    }

    /// Return the current dirty flush threshold.
    [[nodiscard]] double dirty_flush_threshold() const {
        return dirty_flush_threshold_.load(std::memory_order_relaxed);
    }

    /// Start the background flusher thread. The flusher periodically scans for
    /// dirty unpinned pages and writes them to disk. It also wakes immediately
    /// when the dirty page ratio exceeds the configured threshold.
    /// @param interval Time between flush cycles (default: 1 second).
    [[nodiscard]] Result<void>
    start_flusher(std::chrono::milliseconds interval = std::chrono::seconds(1));

    /// Stop the background flusher thread. Performs a final flush of all dirty
    /// pages (including pinned), then joins the thread.
    void stop_flusher();

private:
    struct Frame {
        Page page{0, PageType::DATA};
        PageId page_id = 0;
        uint32_t pin_count = 0;
        std::atomic<bool> is_dirty{false}; ///< Atomic for lock-free reads in evict skip predicate.
        mutable std::mutex latch;          ///< Per-frame mutex for pin_count and page data.
    };

    /// Find an available frame: takes from free list, or evicts a victim.
    /// On eviction, flushes the dirty page and removes it from the page table.
    /// Must be called while holding pool_mutex_.
    [[nodiscard]] Result<FrameId> find_victim_frame();

    /// Write a page through the double-write buffer (if enabled) then to disk.
    [[nodiscard]] Result<void> write_page_impl(PageId page_id, Page& page);

    /// Flush dirty pages. If include_pinned is true, flushes all dirty pages;
    /// otherwise only flushes dirty pages with pin_count == 0.
    void flush_dirty_pages(bool include_pinned = false);

    /// Background flusher thread entry point.
    void flusher_loop(std::chrono::milliseconds interval);

    DiskManager& disk_manager_;
    FileId file_id_;

    std::vector<Frame> frames_;
    LRUKReplacer replacer_;

    // -- Sharded page table (GDB-643) ----------------------------------------
    //
    // Lock hierarchy (acquire in this order, release in reverse):
    //   pool_mutex_ > shard.mutex > frame.latch
    //   replacer_mutex_ is acquired independently (never nested with shard/frame)
    //
    // Hot path (cache hit): shard → frame (release both) → replacer
    // Miss path: pool_mutex_ → { shard, frame } → replacer

    static constexpr uint32_t kNumShards = 64;

    struct PageTableShard {
        mutable std::mutex mutex;
        std::unordered_map<PageId, FrameId> map;
    };

    std::array<PageTableShard, kNumShards> page_table_shards_;
    std::vector<FrameId> free_list_;

    [[nodiscard]] PageTableShard& get_shard(PageId page_id) {
        return page_table_shards_[page_id % kNumShards];
    }

    [[nodiscard]] const PageTableShard& get_shard(PageId page_id) const {
        return page_table_shards_[page_id % kNumShards];
    }

    // -- Fine-grained locks (GDB-644/645/646) ---------------------------------

    mutable std::mutex
        pool_mutex_; ///< Serializes structural changes (miss path, eviction, free list).
    mutable std::mutex replacer_mutex_;               ///< Protects LRU-K replacer.
    std::mutex dwb_mutex_;                            ///< Protects double-write buffer writes.
    std::thread flusher_thread_;                      ///< Background flusher thread.
    std::mutex flusher_mutex_;                        ///< Protects flusher condition variable.
    std::condition_variable flusher_cv_;              ///< Wakes/stops the flusher.
    std::atomic<bool> flusher_running_{false};        ///< Flusher shutdown flag.
    std::atomic<double> dirty_flush_threshold_{0.75}; ///< Dirty ratio threshold for flusher wake.
    int dwb_fd_ = -1;                                 ///< Double-write buffer fd (-1 = disabled).

    // -- Observability counters --------------------------------------------------
    std::atomic<uint64_t> hits_{0};         ///< Pages found already in the pool.
    std::atomic<uint64_t> misses_{0};       ///< Pages read from disk.
    std::atomic<uint32_t> dirty_count_{0};  ///< Number of dirty frames.
    std::atomic<uint64_t> flush_errors_{0}; ///< Write failures in flusher/destructor (GDB-778).
};

} // namespace sixseven
