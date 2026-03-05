#pragma once

#include "sixseven/common/result.h"
#include "sixseven/storage/page.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace sixseven {

/// Threshold in bytes above which values should be stored in overflow pages.
static constexpr size_t overflow_threshold = 2048;

/// Pointer stored in the main tuple to reference overflow data.
/// Total size: 8 bytes (fits in a fixed-size field slot).
struct OverflowPointer {
    uint32_t first_page_id = 0; ///< Page ID of the first overflow page in the chain.
    uint32_t total_length = 0;  ///< Total byte length of the overflowed value.

    bool operator==(const OverflowPointer& other) const = default;
};

/// Offset within an overflow page where the next_page_id field is stored.
/// Immediately after the standard page header.
static constexpr size_t overflow_next_page_offset = page_header_size;

/// Size of the next_page_id field in an overflow page.
static constexpr size_t overflow_next_page_size = sizeof(uint32_t);

/// Offset within an overflow page where chunk data begins.
static constexpr size_t overflow_data_offset = overflow_next_page_offset + overflow_next_page_size;

/// Maximum chunk data that fits in a single overflow page.
static constexpr size_t overflow_chunk_capacity = page_size - overflow_data_offset;

/// Sentinel value indicating no next page (end of overflow chain).
static constexpr uint32_t overflow_no_next_page = 0;

/// Abstract interface for allocating and freeing pages.
/// Implementations may manage an in-memory pool, a disk-backed buffer pool, etc.
///
/// get_page() returns a raw pointer to a Page whose lifetime is managed by the
/// allocator. The returned pointer remains valid until the page is freed via
/// free_page(), or until the allocator itself is destroyed.
class PageAllocator {
public:
    PageAllocator() = default;
    virtual ~PageAllocator() = default;
    PageAllocator(const PageAllocator&) = delete;
    PageAllocator& operator=(const PageAllocator&) = delete;
    PageAllocator(PageAllocator&&) = delete;
    PageAllocator& operator=(PageAllocator&&) = delete;

    /// Allocate a new page with the given type. Returns the page ID.
    virtual Result<uint32_t> allocate_page(PageType type) = 0;

    /// Get a mutable reference to a page by its ID.
    virtual Result<Page*> get_page(uint32_t page_id) = 0;

    /// Get a const reference to a page by its ID.
    virtual Result<const Page*> get_page(uint32_t page_id) const = 0;

    /// Free a page, returning it to the allocator's free pool.
    virtual Result<void> free_page(uint32_t page_id) = 0;
};

/// Manages storage and retrieval of large values across overflow page chains.
///
/// Overflow page layout:
/// ```
/// [ Page Header (24 bytes) ]
/// [ next_page_id (4 bytes) ] — 0 if this is the last page in the chain
/// [ chunk_data (up to 8164 bytes) ]
/// ```
class OverflowManager {
public:
    /// Construct an OverflowManager using the given page allocator.
    explicit OverflowManager(PageAllocator& allocator);

    /// Store data across one or more overflow pages. Returns an OverflowPointer
    /// that can be stored in the main tuple to reference this data.
    /// If allocation fails partway through, all previously allocated pages in
    /// the chain are freed before the error is returned.
    [[nodiscard]] Result<OverflowPointer> store_overflow(std::span<const uint8_t> data);

    /// Read overflow data by following the page chain starting at the given pointer.
    /// Reconstructs the complete data by concatenating chunks from all pages in the chain.
    [[nodiscard]] Result<std::vector<uint8_t>> read_overflow(const OverflowPointer& pointer) const;

    /// Free all overflow pages in the chain starting at the given pointer.
    [[nodiscard]] Result<void> free_overflow(const OverflowPointer& pointer);

    /// Return true if the given data size exceeds the overflow threshold.
    static bool needs_overflow(size_t data_size) { return data_size > overflow_threshold; }

private:
    PageAllocator* allocator_;
};

/// Simple in-memory page allocator for testing purposes.
/// Pages are stored via unique_ptr for pointer stability across allocations.
class InMemoryPageAllocator : public PageAllocator {
public:
    InMemoryPageAllocator() = default;

    Result<uint32_t> allocate_page(PageType type) override;
    Result<Page*> get_page(uint32_t page_id) override;
    Result<const Page*> get_page(uint32_t page_id) const override;
    Result<void> free_page(uint32_t page_id) override;

    /// Return the number of pages currently allocated (not freed).
    [[nodiscard]] size_t allocated_count() const;

    /// Return the total number of pages ever created.
    [[nodiscard]] size_t total_pages() const { return pages_.size(); }

private:
    std::vector<std::unique_ptr<Page>> pages_;
    std::vector<bool> active_; ///< Track which pages are active (not freed).
    uint32_t next_id_ = 1;     ///< Next page ID to assign (1-based, 0 is reserved).
};

} // namespace sixseven
