#include "giodb/storage/page.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace giodb {

// -- Construction -------------------------------------------------------------

Page::Page(uint32_t page_id, PageType page_type) {
    data_.fill(0);
    write_u32(OFF_PAGE_ID, page_id);
    data_[OFF_PAGE_TYPE] = static_cast<uint8_t>(page_type);
    set_slot_count(0);
    set_data_offset(static_cast<uint16_t>(PAGE_SIZE));
    write_u64(OFF_LSN, 0);
    write_u32(OFF_CHECKSUM, 0);
}

Page::Page(const std::array<uint8_t, PAGE_SIZE>& raw) : data_(raw) {}

// -- Header accessors ---------------------------------------------------------

uint32_t Page::page_id() const {
    return read_u32(OFF_PAGE_ID);
}

PageType Page::page_type() const {
    return static_cast<PageType>(data_[OFF_PAGE_TYPE]);
}

uint16_t Page::slot_count() const {
    return read_u16(OFF_SLOT_COUNT);
}

uint64_t Page::lsn() const {
    return read_u64(OFF_LSN);
}

uint32_t Page::checksum() const {
    return read_u32(OFF_CHECKSUM);
}

void Page::set_page_id(uint32_t id) {
    write_u32(OFF_PAGE_ID, id);
}

void Page::set_page_type(PageType type) {
    data_[OFF_PAGE_TYPE] = static_cast<uint8_t>(type);
}

void Page::set_lsn(uint64_t lsn_val) {
    write_u64(OFF_LSN, lsn_val);
}

void Page::set_checksum(uint32_t cksum) {
    write_u32(OFF_CHECKSUM, cksum);
}

// -- Tuple operations ---------------------------------------------------------

Result<SlotId> Page::insert_tuple(std::span<const uint8_t> data) {
    if (data.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "cannot insert empty tuple");
    }

    uint16_t tuple_len = static_cast<uint16_t>(data.size());
    size_t needed = tuple_len + SLOT_ENTRY_SIZE;

    // Check if we can reuse a deleted slot.
    SlotId reuse_slot = slot_count();
    uint16_t sc = slot_count();
    for (SlotId i = 0; i < sc; ++i) {
        SlotEntry entry = read_slot(i);
        if (entry.offset == 0 && entry.length == 0) {
            reuse_slot = i;
            needed = tuple_len; // no new slot entry needed
            break;
        }
    }

    // Compare against raw available space (data_offset - slot_directory_end).
    size_t dir_end = slot_directory_end();
    size_t d_off = data_offset();
    size_t raw_available = (d_off > dir_end) ? (d_off - dir_end) : 0;
    if (needed > raw_available) {
        return make_error(StatusCode::INVALID_ARGUMENT, "not enough free space in page");
    }

    // Allocate tuple space at the bottom of the page.
    uint16_t new_data_offset = static_cast<uint16_t>(data_offset() - tuple_len);
    std::memcpy(&data_[new_data_offset], data.data(), tuple_len);

    // Write the slot entry.
    SlotEntry new_entry{new_data_offset, tuple_len};
    if (reuse_slot < sc) {
        // Reuse an existing deleted slot.
        write_slot(reuse_slot, new_entry);
    } else {
        // Add a new slot at the end of the directory.
        write_slot(sc, new_entry);
        set_slot_count(sc + 1);
    }

    set_data_offset(new_data_offset);
    return ok(reuse_slot);
}

Result<std::span<const uint8_t>> Page::get_tuple(SlotId slot_id) const {
    if (slot_id >= slot_count()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "slot ID out of range");
    }

    SlotEntry entry = read_slot(slot_id);
    if (entry.offset == 0) {
        return make_error(StatusCode::NOT_FOUND, "slot is deleted");
    }

    return ok(std::span<const uint8_t>(&data_[entry.offset], entry.length));
}

Result<void> Page::delete_tuple(SlotId slot_id) {
    if (slot_id >= slot_count()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "slot ID out of range");
    }

    SlotEntry entry = read_slot(slot_id);
    if (entry.offset == 0) {
        return make_error(StatusCode::NOT_FOUND, "slot is already deleted");
    }

    // Mark as deleted by zeroing the slot entry.
    write_slot(slot_id, {0, 0});
    return ok();
}

Result<void> Page::update_tuple(SlotId slot_id, std::span<const uint8_t> data) {
    if (slot_id >= slot_count()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "slot ID out of range");
    }

    if (data.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "cannot update with empty tuple");
    }

    SlotEntry entry = read_slot(slot_id);
    if (entry.offset == 0) {
        return make_error(StatusCode::NOT_FOUND, "slot is deleted");
    }

    uint16_t new_len = static_cast<uint16_t>(data.size());

    if (new_len <= entry.length) {
        // Fits in-place: overwrite the existing tuple.
        std::memcpy(&data_[entry.offset], data.data(), new_len);
        // Update slot with new (shorter) length. Keep same offset.
        // The gap between entry.offset and entry.offset + (entry.length - new_len)
        // becomes dead space until compaction.
        write_slot(slot_id, {entry.offset, new_len});
        return ok();
    }

    // Doesn't fit in-place: delete the old tuple and allocate new space.
    // Mark old slot as deleted.
    write_slot(slot_id, {0, 0});

    // Check if there's enough space for the new data (no new slot entry needed).
    // We reuse the existing slot, so only need raw space for the tuple data.
    size_t raw_avail = data_offset() - slot_directory_end();
    if (new_len > raw_avail) {
        // Restore the old slot entry since we can't complete the update.
        write_slot(slot_id, entry);
        return make_error(StatusCode::INVALID_ARGUMENT, "not enough free space for updated tuple");
    }

    // Allocate new space at the bottom.
    uint16_t new_offset = static_cast<uint16_t>(data_offset() - new_len);
    std::memcpy(&data_[new_offset], data.data(), new_len);
    write_slot(slot_id, {new_offset, new_len});
    set_data_offset(new_offset);

    return ok();
}

size_t Page::free_space() const {
    size_t dir_end = slot_directory_end();
    size_t d_off = data_offset();
    if (d_off <= dir_end) {
        return 0;
    }
    size_t raw = d_off - dir_end;
    // Reserve space for one new slot entry.
    if (raw < SLOT_ENTRY_SIZE) {
        return 0;
    }
    return raw - SLOT_ENTRY_SIZE;
}

void Page::compact() {
    uint16_t sc = slot_count();
    if (sc == 0) {
        set_data_offset(static_cast<uint16_t>(PAGE_SIZE));
        return;
    }

    // Collect all live tuples with their data.
    struct LiveTuple {
        SlotId slot_id;
        std::vector<uint8_t> data;
    };
    std::vector<LiveTuple> live;
    live.reserve(sc);

    for (SlotId i = 0; i < sc; ++i) {
        SlotEntry entry = read_slot(i);
        if (entry.offset != 0) {
            live.push_back(
                {i,
                 std::vector<uint8_t>(&data_[entry.offset], &data_[entry.offset + entry.length])});
        }
    }

    // Rewrite tuples from the end of the page, tightly packed.
    uint16_t write_pos = static_cast<uint16_t>(PAGE_SIZE);
    for (auto& tuple : live) {
        uint16_t len = static_cast<uint16_t>(tuple.data.size());
        write_pos -= len;
        std::memcpy(&data_[write_pos], tuple.data.data(), len);
        write_slot(tuple.slot_id, {write_pos, len});
    }

    set_data_offset(write_pos);
}

// -- Header read/write helpers ------------------------------------------------

void Page::write_u16(size_t offset, uint16_t value) {
    std::memcpy(&data_[offset], &value, sizeof(uint16_t));
}

void Page::write_u32(size_t offset, uint32_t value) {
    std::memcpy(&data_[offset], &value, sizeof(uint32_t));
}

void Page::write_u64(size_t offset, uint64_t value) {
    std::memcpy(&data_[offset], &value, sizeof(uint64_t));
}

uint16_t Page::read_u16(size_t offset) const {
    uint16_t value = 0;
    std::memcpy(&value, &data_[offset], sizeof(uint16_t));
    return value;
}

uint32_t Page::read_u32(size_t offset) const {
    uint32_t value = 0;
    std::memcpy(&value, &data_[offset], sizeof(uint32_t));
    return value;
}

uint64_t Page::read_u64(size_t offset) const {
    uint64_t value = 0;
    std::memcpy(&value, &data_[offset], sizeof(uint64_t));
    return value;
}

// -- Slot directory helpers ---------------------------------------------------

size_t Page::slot_offset(SlotId slot_id) const {
    return PAGE_HEADER_SIZE + static_cast<size_t>(slot_id) * SLOT_ENTRY_SIZE;
}

SlotEntry Page::read_slot(SlotId slot_id) const {
    size_t off = slot_offset(slot_id);
    return {read_u16(off), read_u16(off + sizeof(uint16_t))};
}

void Page::write_slot(SlotId slot_id, SlotEntry entry) {
    size_t off = slot_offset(slot_id);
    write_u16(off, entry.offset);
    write_u16(off + sizeof(uint16_t), entry.length);
}

size_t Page::slot_directory_end() const {
    return PAGE_HEADER_SIZE + static_cast<size_t>(slot_count()) * SLOT_ENTRY_SIZE;
}

void Page::set_slot_count(uint16_t count) {
    write_u16(OFF_SLOT_COUNT, count);
}

uint16_t Page::data_offset() const {
    return read_u16(OFF_DATA_OFFSET);
}

void Page::set_data_offset(uint16_t offset) {
    write_u16(OFF_DATA_OFFSET, offset);
}

} // namespace giodb
