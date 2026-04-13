#include "sixseven/index/hash_persistence.h"

#include "sixseven/common/logging.h"
#include "sixseven/storage/serialization.h"

#include <cstring>
#include <unordered_map>
#include <vector>

namespace sixseven {

// ---------------------------------------------------------------------------
// Binary encoding helpers (little-endian)
// ---------------------------------------------------------------------------

namespace {

void write_u8(std::vector<uint8_t>& buf, uint8_t v) { buf.push_back(v); }

void write_u16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
}

void write_u32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v >> 16));
    buf.push_back(static_cast<uint8_t>(v >> 24));
}

void write_u64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
}

uint8_t read_u8(const uint8_t*& p) {
    uint8_t v = *p;
    ++p;
    return v;
}

uint16_t read_u16(const uint8_t*& p) {
    uint16_t v = 0;
    std::memcpy(&v, p, 2);
    p += 2;
    return v;
}

uint32_t read_u32(const uint8_t*& p) {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    p += 4;
    return v;
}

uint64_t read_u64(const uint8_t*& p) {
    uint64_t v = 0;
    std::memcpy(&v, p, 8);
    p += 8;
    return v;
}

void serialize_key(std::vector<uint8_t>& buf, const KeyType& key) {
    for (const auto& val : key) {
        auto bytes = serialize(val);
        buf.insert(buf.end(), bytes.begin(), bytes.end());
    }
}

Result<KeyType> deserialize_key(const uint8_t*& p, const uint8_t* end,
                                const std::vector<TypeId>& key_types) {
    KeyType key;
    key.reserve(key_types.size());
    for (auto type_id : key_types) {
        size_t remaining = static_cast<size_t>(end - p);
        auto val = deserialize(std::span<const uint8_t>(p, remaining), type_id);
        if (!val) {
            return make_error(val.error().code, val.error().message);
        }
        size_t consumed = serialized_size(*val);
        p += consumed;
        key.push_back(std::move(*val));
    }
    return ok(std::move(key));
}

} // namespace

// ---------------------------------------------------------------------------
// persist
// ---------------------------------------------------------------------------

Result<PageId> HashPersistence::persist(BufferPoolManager& bpm, const HashIndex& index) {
    // Deduplicate buckets: multiple directory slots can point to the same bucket.
    // Map unique bucket pointer → sequential ID.
    std::unordered_map<const HashBucket*, uint32_t> bucket_ids;
    std::vector<const HashBucket*> unique_buckets;

    for (const auto& bucket_ptr : index.directory_) {
        auto* raw = bucket_ptr.get();
        if (bucket_ids.find(raw) == bucket_ids.end()) {
            bucket_ids[raw] = static_cast<uint32_t>(unique_buckets.size());
            unique_buckets.push_back(raw);
        }
    }

    // Write each unique bucket to its own page.
    std::vector<PageId> bucket_disk_pages;
    bucket_disk_pages.reserve(unique_buckets.size());

    for (const auto* bucket : unique_buckets) {
        std::vector<uint8_t> buf;
        write_u32(buf, bucket->local_depth);
        write_u32(buf, static_cast<uint32_t>(bucket->entries.size()));

        for (const auto& entry : bucket->entries) {
            write_u64(buf, entry.hash);
            serialize_key(buf, entry.key);
            write_u32(buf, entry.rid.page_id);
            write_u16(buf, entry.rid.slot_id);
        }

        auto page_r = bpm.new_page();
        if (!page_r) {
            return make_error(page_r.error().code,
                              "hash persist: failed to allocate bucket page: " +
                                  page_r.error().message);
        }
        auto* page = *page_r;
        page->reset(page->page_id(), PageType::HASH_BUCKET);
        auto slot = page->insert_tuple(std::span<const uint8_t>(buf));
        if (!slot) {
            return make_error(slot.error().code,
                              "hash persist: bucket data too large for page: " +
                                  slot.error().message);
        }
        bucket_disk_pages.push_back(page->page_id());
        (void)bpm.unpin_page(page->page_id(), /*is_dirty=*/true);
    }

    // Build meta page.
    std::vector<uint8_t> meta_buf;
    write_u32(meta_buf, index.global_depth_);
    write_u64(meta_buf, index.size_);
    write_u32(meta_buf, index.config_.bucket_capacity);
    write_u8(meta_buf, index.config_.is_unique ? 1 : 0);

    // Key types
    write_u8(meta_buf, static_cast<uint8_t>(index.config_.key_types.size()));
    for (auto t : index.config_.key_types) {
        write_u8(meta_buf, static_cast<uint8_t>(t));
    }

    // Directory: for each slot, store the bucket's disk page ID.
    write_u32(meta_buf, static_cast<uint32_t>(index.directory_.size()));
    for (const auto& bucket_ptr : index.directory_) {
        uint32_t bid = bucket_ids[bucket_ptr.get()];
        write_u32(meta_buf, bucket_disk_pages[bid]);
    }

    // Write meta page.
    auto meta_page_r = bpm.new_page();
    if (!meta_page_r) {
        return make_error(meta_page_r.error().code,
                          "hash persist: failed to allocate meta page: " +
                              meta_page_r.error().message);
    }
    auto* meta_page = *meta_page_r;
    PageId meta_page_id = meta_page->page_id();
    meta_page->reset(meta_page_id, PageType::HASH_META);
    auto slot = meta_page->insert_tuple(std::span<const uint8_t>(meta_buf));
    if (!slot) {
        return make_error(slot.error().code,
                          "hash persist: meta data too large for page: " + slot.error().message);
    }
    (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/true);

    auto flush = bpm.flush_all();
    if (!flush) {
        return make_error(flush.error().code,
                          "hash persist: flush failed: " + flush.error().message);
    }

    SIXSEVEN_LOG_DEBUG("hash persist: wrote {} unique buckets, directory size {}, meta page {}",
                       unique_buckets.size(), index.directory_.size(), meta_page_id);

    return ok(meta_page_id);
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------

Result<std::unique_ptr<HashIndex>> HashPersistence::load(BufferPoolManager& bpm,
                                                          PageId meta_page_id) {
    auto meta_page_r = bpm.fetch_page(meta_page_id);
    if (!meta_page_r) {
        return make_error(meta_page_r.error().code,
                          "hash load: failed to fetch meta page: " + meta_page_r.error().message);
    }
    auto* meta_page = *meta_page_r;
    auto meta_data = meta_page->get_tuple(0);
    if (!meta_data) {
        (void)bpm.unpin_page(meta_page_id, false);
        return make_error(meta_data.error().code,
                          "hash load: failed to read meta tuple: " + meta_data.error().message);
    }

    const uint8_t* p = meta_data->data();

    uint32_t global_depth = read_u32(p);
    uint64_t total_size = read_u64(p);
    uint32_t bucket_capacity = read_u32(p);
    bool is_unique = read_u8(p) != 0;

    uint8_t key_type_count = read_u8(p);
    std::vector<TypeId> key_types;
    key_types.reserve(key_type_count);
    for (uint8_t i = 0; i < key_type_count; ++i) {
        key_types.push_back(static_cast<TypeId>(read_u8(p)));
    }

    // Read directory: disk page IDs for each directory slot.
    uint32_t dir_size = read_u32(p);
    std::vector<PageId> dir_disk_pages(dir_size);
    for (uint32_t i = 0; i < dir_size; ++i) {
        dir_disk_pages[i] = read_u32(p);
    }

    (void)bpm.unpin_page(meta_page_id, false);

    // Load unique buckets. Multiple directory slots may share the same page.
    std::unordered_map<PageId, std::shared_ptr<HashBucket>> loaded_buckets;

    for (auto disk_page_id : dir_disk_pages) {
        if (loaded_buckets.count(disk_page_id) != 0) {
            continue;
        }

        auto page_r = bpm.fetch_page(disk_page_id);
        if (!page_r) {
            return make_error(page_r.error().code,
                              "hash load: failed to fetch bucket page: " +
                                  page_r.error().message);
        }
        auto* page = *page_r;
        auto tuple_data = page->get_tuple(0);
        if (!tuple_data) {
            (void)bpm.unpin_page(disk_page_id, false);
            return make_error(tuple_data.error().code,
                              "hash load: failed to read bucket tuple: " +
                                  tuple_data.error().message);
        }

        const uint8_t* bp = tuple_data->data();
        const uint8_t* bend = bp + tuple_data->size();

        uint32_t local_depth = read_u32(bp);
        uint32_t entry_count = read_u32(bp);

        auto bucket = std::make_shared<HashBucket>();
        bucket->local_depth = local_depth;
        bucket->entries.reserve(entry_count);

        for (uint32_t i = 0; i < entry_count; ++i) {
            HashBucketEntry entry;
            entry.hash = static_cast<size_t>(read_u64(bp));
            auto key = deserialize_key(bp, bend, key_types);
            if (!key) {
                (void)bpm.unpin_page(disk_page_id, false);
                return make_error(key.error().code,
                                  "hash load: failed to deserialize key: " +
                                      key.error().message);
            }
            entry.key = std::move(*key);
            entry.rid.page_id = read_u32(bp);
            entry.rid.slot_id = read_u16(bp);
            bucket->entries.push_back(std::move(entry));
        }

        loaded_buckets[disk_page_id] = std::move(bucket);
        (void)bpm.unpin_page(disk_page_id, false);
    }

    // Build the HashIndex.
    HashIndexConfig config;
    config.key_types = std::move(key_types);
    config.bucket_capacity = bucket_capacity;
    config.is_unique = is_unique;

    auto index = std::make_unique<HashIndex>(std::move(config));
    index->global_depth_ = global_depth;
    index->size_ = total_size;

    // Rebuild directory from disk page mappings.
    index->directory_.resize(dir_size);
    for (uint32_t i = 0; i < dir_size; ++i) {
        index->directory_[i] = loaded_buckets[dir_disk_pages[i]];
    }

    SIXSEVEN_LOG_DEBUG("hash load: restored {} unique buckets, directory size {}, size {}",
                       loaded_buckets.size(), dir_size, total_size);

    return ok(std::move(index));
}

} // namespace sixseven
