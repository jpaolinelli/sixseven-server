#pragma once

#include "giodb/common/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace giodb {

/// Default page size: 8KB.
static constexpr size_t page_size = 8192;

/// Page type identifier stored in the page header.
enum class PageType : uint8_t {
    DATA,
    BTREE_INTERNAL,
    BTREE_LEAF,
    OVERFLOW_PAGE,
    FREE_LIST,
    HNSW_NODE,
    HNSW_META,
    HNSW_VECTOR_DATA,
};

/// Slot identifier within a page (0-based index into the slot directory).
using SlotId = uint16_t;

/// A slot directory entry: (offset, length) pair.
/// An offset of 0 indicates a deleted slot.
struct SlotEntry {
    uint16_t offset = 0;
    uint16_t length = 0;
};

/// Page header layout (stored at the beginning of every page).
/// Total size: 24 bytes (4 + 1 + 1 padding + 2 + 2 + 2 padding + 8 + 4).
/// We pack it tightly to a known fixed size using explicit byte layout.
static constexpr size_t page_header_size = 24;

/// Slot entry size in bytes.
static constexpr size_t slot_entry_size = sizeof(uint16_t) * 2; // offset + length = 4 bytes

/// Minimum tuple size (to avoid degenerate cases).
static constexpr size_t min_tuple_size = 1;

/// An 8KB slotted page that stores variable-length tuples.
///
/// Layout:
/// ```
/// [ Page Header (24 bytes) ]
/// [ Slot Directory: slot_0, slot_1, ... slot_N ] → grows downward
///   ... free space ...
/// [ Tuple data ] → grows upward from end of page
/// ```
///
/// - The slot directory grows from the header toward the middle of the page.
/// - Tuple data grows from the end of the page toward the middle.
/// - free_space_offset tracks where the next slot entry would go (end of slot directory).
/// - data_offset_ tracks where the next tuple would be written (lowest occupied tuple byte).
class Page {
public:
    /// Byte offset of the checksum field within the page header.
    /// Exposed for external checksum computation (e.g., DiskManager CRC32C).
    static constexpr size_t checksum_offset = 20;

    /// Construct a new page with the given page_id and type.
    /// Initializes the header and empty slot directory.
    Page(uint32_t page_id, PageType page_type);

    /// Construct from raw bytes (e.g., read from disk). Header is parsed from data.
    explicit Page(const std::array<uint8_t, page_size>& raw);

    // -- Header accessors -----------------------------------------------------

    [[nodiscard]] uint32_t page_id() const;
    [[nodiscard]] PageType page_type() const;
    [[nodiscard]] uint16_t slot_count() const;
    [[nodiscard]] uint64_t lsn() const;
    [[nodiscard]] uint32_t checksum() const;

    void set_page_id(uint32_t id);
    void set_page_type(PageType type);
    void set_lsn(uint64_t new_lsn);
    void set_checksum(uint32_t new_checksum);

    // -- Tuple operations -----------------------------------------------------

    /// Insert a tuple into the page. Returns the assigned slot ID.
    /// Fails if there is not enough free space for the tuple + slot entry.
    [[nodiscard]] Result<SlotId> insert_tuple(std::span<const uint8_t> data);

    /// Read a tuple by slot ID. Returns a span pointing into the page's data.
    /// Fails if the slot ID is invalid or the slot is deleted.
    [[nodiscard]] Result<std::span<const uint8_t>> get_tuple(SlotId slot_id) const;

    /// Delete a tuple by marking its slot as deleted (offset = 0).
    /// The space is not immediately reclaimed — call compact() for that.
    /// Stale tuple data is zeroed for defense-in-depth.
    [[nodiscard]] Result<void> delete_tuple(SlotId slot_id);

    /// Update a tuple. If the new data fits in the existing slot, update in-place.
    /// Otherwise, delete the old tuple and insert the new one in the same slot.
    ///
    /// Note: this method does NOT attempt automatic compaction. If the update
    /// fails due to insufficient contiguous free space, the caller should call
    /// compact() and retry. This keeps compaction under the caller's control
    /// since it is an expensive operation.
    [[nodiscard]] Result<void> update_tuple(SlotId slot_id, std::span<const uint8_t> data);

    /// Return the number of bytes available for new tuples, accounting for a
    /// new slot entry. This is conservative: if deleted slots are available for
    /// reuse, the actual insertable tuple size may be up to slot_entry_size
    /// bytes larger, since insert_tuple() reuses deleted slots before allocating
    /// new directory entries.
    [[nodiscard]] size_t free_space() const;

    /// Defragment the page by moving all tuples to the end of the page,
    /// eliminating gaps left by deleted or updated tuples.
    /// Freed regions are zeroed for defense-in-depth.
    void compact();

    /// Access the raw page data (e.g., for writing to disk).
    [[nodiscard]] const std::array<uint8_t, page_size>& raw() const { return data_; }
    [[nodiscard]] std::array<uint8_t, page_size>& raw() { return data_; }

private:
    // -- Header read/write helpers (native endianness) ------------------------
    // Note: uses native endianness via memcpy. All platforms GioDB currently
    // targets are little-endian (x86-64, ARM LE). If big-endian support is
    // needed, add explicit byte-swap logic as done in serialization.cpp.

    void write_u16(size_t offset, uint16_t value);
    void write_u32(size_t offset, uint32_t value);
    void write_u64(size_t offset, uint64_t value);
    [[nodiscard]] uint16_t read_u16(size_t offset) const;
    [[nodiscard]] uint32_t read_u32(size_t offset) const;
    [[nodiscard]] uint64_t read_u64(size_t offset) const;

    // -- Header field offsets -------------------------------------------------

    static constexpr size_t off_page_id = 0;   // uint32_t (4 bytes)
    static constexpr size_t off_page_type = 4; // uint8_t  (1 byte)
    // 1 byte padding at offset 5
    static constexpr size_t off_slot_count = 6;  // uint16_t (2 bytes)
    static constexpr size_t off_data_offset = 8; // uint16_t (2 bytes) — lowest tuple byte
    // 2 bytes padding at offset 10
    static constexpr size_t off_lsn = 12;                   // uint64_t (8 bytes)
    static constexpr size_t off_checksum = checksum_offset; // uint32_t (4 bytes)
    // Total header: 24 bytes

    // -- Slot directory helpers -----------------------------------------------

    /// Return the byte offset of slot entry `slot_id` in the slot directory.
    [[nodiscard]] size_t slot_offset(SlotId slot_id) const;

    /// Read the slot entry at the given slot ID.
    [[nodiscard]] SlotEntry read_slot(SlotId slot_id) const;

    /// Write a slot entry at the given slot ID.
    void write_slot(SlotId slot_id, SlotEntry entry);

    /// Return the end of the slot directory (byte offset just past the last slot entry).
    [[nodiscard]] size_t slot_directory_end() const;

    /// Set the slot count in the header.
    void set_slot_count(uint16_t count);

    /// Get/set the data offset (lowest tuple byte position).
    [[nodiscard]] uint16_t data_offset() const;
    void set_data_offset(uint16_t offset);

    std::array<uint8_t, page_size> data_{};
};

} // namespace giodb
