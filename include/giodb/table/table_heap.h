#pragma once

#include "giodb/common/result.h"
#include "giodb/index/rid.h"
#include "giodb/storage/buffer_pool.h"
#include "giodb/storage/disk_manager.h"
#include "giodb/storage/page.h"

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace giodb {

/// Forward declaration.
class TableIterator;

/// Manages tuple storage in a heap file (unordered collection of data pages).
///
/// A heap file consists of data pages (page IDs 1+) within a single database
/// file. Tuples are inserted into the first page with available space.
/// Each tuple is identified by its RID (page_id + slot_id).
///
/// Usage:
/// ```
///   TableHeap heap(bpm, dm, file_id);
///   auto rid = heap.insert_tuple(data).value();
///   auto tuple = heap.get_tuple(rid).value();
///   heap.delete_tuple(rid);
/// ```
class TableHeap {
public:
    /// Construct a TableHeap managing tuple storage for a single file.
    /// @param bpm Buffer pool manager (must be tied to the same file_id).
    /// @param dm  Disk manager for page count queries.
    /// @param file_id The file backing this heap.
    TableHeap(BufferPoolManager& bpm, DiskManager& dm, FileId file_id);

    /// Insert a tuple into the heap. Finds the first page with sufficient
    /// free space, or allocates a new page if none has room.
    /// @param data Tuple bytes (must be non-empty and fit in a single page).
    /// @return The RID (page_id, slot_id) of the inserted tuple.
    [[nodiscard]] Result<RID> insert_tuple(std::span<const uint8_t> data);

    /// Read a tuple by RID. Returns a copy of the tuple data.
    /// The page is unpinned before returning, so the data is a snapshot.
    /// @return A vector containing the tuple bytes.
    [[nodiscard]] Result<std::vector<uint8_t>> get_tuple(RID rid);

    /// Update a tuple in-place. If the new data fits in the existing slot,
    /// the update is done directly. If not, the page is compacted and the
    /// update is retried. If the tuple still doesn't fit after compaction
    /// (e.g., it exceeds available page space), the update fails.
    /// The RID remains stable on success.
    [[nodiscard]] Result<void> update_tuple(RID rid, std::span<const uint8_t> data);

    /// Delete a tuple by marking its slot as deleted.
    [[nodiscard]] Result<void> delete_tuple(RID rid);

    /// Return a sequential-scan iterator starting from the first tuple.
    /// Fails if the file page count cannot be read.
    [[nodiscard]] Result<TableIterator> begin();

    /// Return the number of data pages in the heap file.
    /// Data pages start at page ID 1 (page 0 is the file header).
    [[nodiscard]] Result<uint32_t> page_count() const;

private:
    BufferPoolManager& bpm_;
    DiskManager& dm_;
    FileId file_id_;

    /// Hint: last page we successfully inserted into.
    /// Speeds up sequential inserts by avoiding full scans.
    PageId last_insert_page_ = 0;
};

/// Sequential scan iterator over all live tuples in a TableHeap.
///
/// Usage:
/// ```
///   auto it = heap.begin().value();
///   while (auto result = it.next()) {
///       auto& [rid, data] = *result;
///       // process tuple...
///   }
/// ```
class TableIterator {
public:
    TableIterator(BufferPoolManager& bpm, PageId start_page, uint32_t total_pages);

    /// Advance to the next live tuple.
    /// @return A pair of (RID, tuple data copy), or nullopt if scan is exhausted.
    [[nodiscard]] std::optional<std::pair<RID, std::vector<uint8_t>>> next();

private:
    BufferPoolManager& bpm_;
    PageId current_page_;
    SlotId current_slot_ = 0;
    uint32_t total_pages_;
    bool exhausted_ = false;
};

} // namespace giodb
