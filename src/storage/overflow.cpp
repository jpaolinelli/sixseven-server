#include "sixseven/storage/overflow.h"

#include <algorithm>
#include <cstring>

namespace sixseven {

// -- OverflowManager ----------------------------------------------------------

OverflowManager::OverflowManager(PageAllocator& allocator) : allocator_(&allocator) {}

Result<OverflowPointer> OverflowManager::store_overflow(std::span<const uint8_t> data) {
    if (data.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "cannot store empty overflow data");
    }

    auto total_length = static_cast<uint32_t>(data.size());
    size_t remaining = data.size();
    size_t offset = 0;

    uint32_t first_page_id = 0;
    uint32_t prev_page_id = 0;
    std::vector<uint32_t> allocated_ids;

    while (remaining > 0) {
        // Allocate a new overflow page.
        auto page_id_result = allocator_->allocate_page(PageType::OVERFLOW_PAGE);
        if (!page_id_result) {
            // Clean up already-allocated pages on failure.
            for (auto id : allocated_ids) {
                (void)allocator_->free_page(id);
            }
            return tl::unexpected(page_id_result.error());
        }
        uint32_t page_id = *page_id_result;
        allocated_ids.push_back(page_id);

        if (first_page_id == 0) {
            first_page_id = page_id;
        }

        // Link the previous page to this one.
        if (prev_page_id != 0) {
            auto prev_page_result = allocator_->get_page(prev_page_id);
            if (!prev_page_result) {
                for (auto id : allocated_ids) {
                    (void)allocator_->free_page(id);
                }
                return tl::unexpected(prev_page_result.error());
            }
            Page* prev_page = *prev_page_result;
            // Write next_page_id into the previous page.
            std::memcpy(&prev_page->raw()[overflow_next_page_offset], &page_id, sizeof(uint32_t));
        }

        // Get the new page.
        auto page_result = allocator_->get_page(page_id);
        if (!page_result) {
            for (auto id : allocated_ids) {
                (void)allocator_->free_page(id);
            }
            return tl::unexpected(page_result.error());
        }
        Page* page = *page_result;

        // Write next_page_id = 0 (will be updated if there's a next page).
        uint32_t no_next = overflow_no_next_page;
        std::memcpy(&page->raw()[overflow_next_page_offset], &no_next, sizeof(uint32_t));

        // Write chunk data.
        size_t chunk_size = std::min(remaining, overflow_chunk_capacity);
        std::memcpy(&page->raw()[overflow_data_offset], data.data() + offset, chunk_size);

        offset += chunk_size;
        remaining -= chunk_size;
        prev_page_id = page_id;
    }

    return ok(OverflowPointer{first_page_id, total_length});
}

Result<std::vector<uint8_t>> OverflowManager::read_overflow(const OverflowPointer& pointer) const {
    if (pointer.first_page_id == 0 || pointer.total_length == 0) {
        return make_error(StatusCode::INVALID_ARGUMENT, "invalid overflow pointer");
    }

    std::vector<uint8_t> result;
    result.reserve(pointer.total_length);

    uint32_t current_page_id = pointer.first_page_id;
    size_t remaining = pointer.total_length;

    while (current_page_id != overflow_no_next_page && remaining > 0) {
        auto page_result = allocator_->get_page(current_page_id);
        if (!page_result) {
            return tl::unexpected(page_result.error());
        }
        const Page* page = *page_result;

        // Read the chunk size (min of remaining and chunk capacity).
        size_t chunk_size = std::min(remaining, overflow_chunk_capacity);

        // Copy chunk data into result.
        const uint8_t* chunk_data = &page->raw()[overflow_data_offset];
        result.insert(result.end(), chunk_data, chunk_data + chunk_size);

        remaining -= chunk_size;

        // Read next_page_id.
        std::memcpy(&current_page_id, &page->raw()[overflow_next_page_offset], sizeof(uint32_t));
    }

    if (result.size() != pointer.total_length) {
        return make_error(StatusCode::INTERNAL_ERROR,
                          "overflow chain data size mismatch: expected " +
                              std::to_string(pointer.total_length) + " bytes, got " +
                              std::to_string(result.size()));
    }

    return ok(std::move(result));
}

Result<void> OverflowManager::free_overflow(const OverflowPointer& pointer) {
    if (pointer.first_page_id == 0) {
        return make_error(StatusCode::INVALID_ARGUMENT, "invalid overflow pointer");
    }

    uint32_t current_page_id = pointer.first_page_id;

    while (current_page_id != overflow_no_next_page) {
        auto page_result = allocator_->get_page(current_page_id);
        if (!page_result) {
            return tl::unexpected(page_result.error());
        }
        const Page* page = *page_result;

        // Read next_page_id before freeing.
        uint32_t next_page_id = 0;
        std::memcpy(&next_page_id, &page->raw()[overflow_next_page_offset], sizeof(uint32_t));

        // Free the current page.
        auto free_result = allocator_->free_page(current_page_id);
        if (!free_result) {
            return tl::unexpected(free_result.error());
        }

        current_page_id = next_page_id;
    }

    return ok();
}

// -- InMemoryPageAllocator ----------------------------------------------------

Result<uint32_t> InMemoryPageAllocator::allocate_page(PageType type) {
    uint32_t id = next_id_++;
    pages_.push_back(std::make_unique<Page>(id, type));
    active_.push_back(true);
    return ok(id);
}

Result<Page*> InMemoryPageAllocator::get_page(uint32_t page_id) {
    if (page_id == 0 || page_id >= next_id_) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid page ID: " + std::to_string(page_id));
    }
    size_t index = page_id - 1; // 1-based IDs
    if (!active_[index]) {
        return make_error(StatusCode::NOT_FOUND,
                          "page " + std::to_string(page_id) + " has been freed");
    }
    return ok(pages_[index].get());
}

Result<const Page*> InMemoryPageAllocator::get_page(uint32_t page_id) const {
    if (page_id == 0 || page_id >= next_id_) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid page ID: " + std::to_string(page_id));
    }
    size_t index = page_id - 1;
    if (!active_[index]) {
        return make_error(StatusCode::NOT_FOUND,
                          "page " + std::to_string(page_id) + " has been freed");
    }
    return ok(pages_[index].get());
}

Result<void> InMemoryPageAllocator::free_page(uint32_t page_id) {
    if (page_id == 0 || page_id >= next_id_) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "invalid page ID: " + std::to_string(page_id));
    }
    size_t index = page_id - 1;
    if (!active_[index]) {
        return make_error(StatusCode::NOT_FOUND,
                          "page " + std::to_string(page_id) + " is already freed");
    }
    active_[index] = false;
    return ok();
}

size_t InMemoryPageAllocator::allocated_count() const {
    size_t count = 0;
    for (bool a : active_) {
        if (a) {
            ++count;
        }
    }
    return count;
}

} // namespace sixseven
