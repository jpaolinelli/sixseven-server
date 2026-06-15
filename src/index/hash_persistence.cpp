#include "sixseven/index/hash_persistence.h"

#include "sixseven/common/logging.h"
#include "sixseven/storage/serialization.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace sixseven {

// ---------------------------------------------------------------------------
// Binary encoding helpers (little-endian)
// ---------------------------------------------------------------------------

namespace {

void write_u8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

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

Result<KeyType>
deserialize_key(const uint8_t*& p, const uint8_t* end, const std::vector<TypeId>& key_types) {
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

/// Approximate usable bytes per 8KB slotted page
/// (8192 - page header ~24 - one slot entry ~4).
constexpr size_t MAX_INLINE_SIZE = 8100;

} // namespace

// ---------------------------------------------------------------------------
// persist
// ---------------------------------------------------------------------------
//
// Meta page format (two variants):
//
// V1 (inline, backward-compatible) — directory fits in one page:
//   global_depth (u32)
//   size         (u64)
//   bucket_cap   (u32)
//   is_unique    (u8)
//   key_type_cnt (u8)
//   key_types    (key_type_cnt × u8)
//   dir_size     (u32)
//   dir_entries  (dir_size × u32  — one disk PageId per slot)
//
// V2 (overflow) — directory spans one or more overflow pages:
//   sentinel     (u8  = 0)
//   version      (u8  = 2)
//   global_depth (u32)
//   size         (u64)
//   bucket_cap   (u32)
//   is_unique    (u8)
//   key_type_cnt (u8)
//   key_types    (key_type_cnt × u8)
//   ovf_count    (u32)
//   ovf_pages    (ovf_count × u32 — PageIds of overflow pages)
//
// Overflow pages each hold a raw chunk of the directory bytes.  Concatenating
// all chunks and parsing the result gives the same dir_size + dir_entries as
// the V1 inline layout.
//
// Detection on load: if the first byte is 0 AND the second byte is 2, it is V2.
// (V1 first-4 bytes are global_depth which can be 0 only when the index is
//  brand-new, but then the second byte would be 0 too, not 2.)

Result<PageId> HashPersistence::persist(BufferPoolManager& bpm, const HashIndex& index) {
    // Acquire shared lock to ensure consistent snapshot while iterating.
    // The index's public methods guarantee thread safety via this latch,
    // and persist reads index internals directly as a friend class.
    std::shared_lock lock(index.index_latch_);

    // Deduplicate buckets: multiple directory slots can point to the same bucket.
    // Map unique bucket pointer -> sequential ID.
    std::unordered_map<const HashBucket*, uint32_t> bucket_ids;
    std::vector<const HashBucket*> unique_buckets;

    for (const auto& bucket_ptr : index.directory_) {
        auto* raw = bucket_ptr.get();
        if (!bucket_ids.contains(raw)) {
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

    // Build the fixed header (config + key types, no directory).
    auto build_header = [&](std::vector<uint8_t>& hdr, bool include_sentinel) {
        hdr.clear();
        if (include_sentinel) {
            // V2 prefix: sentinel byte 0 + version byte 2.
            write_u8(hdr, 0);
            write_u8(hdr, 2);
        }
        write_u32(hdr, index.global_depth_);
        write_u64(hdr, index.size_);
        write_u32(hdr, index.config_.bucket_capacity);
        write_u8(hdr, index.config_.is_unique ? 1 : 0);
        write_u8(hdr, static_cast<uint8_t>(index.config_.key_types.size()));
        for (auto t : index.config_.key_types) {
            write_u8(hdr, static_cast<uint8_t>(t));
        }
    };

    // Build directory bytes: count + one uint32 disk PageId per slot.
    std::vector<uint8_t> dir_buf;
    dir_buf.reserve(4 + (index.directory_.size() * 4));
    write_u32(dir_buf, static_cast<uint32_t>(index.directory_.size()));
    for (const auto& bucket_ptr : index.directory_) {
        uint32_t bid = bucket_ids[bucket_ptr.get()];
        write_u32(dir_buf, bucket_disk_pages[bid]);
    }

    // Try inline format first (V1, backward-compatible).
    std::vector<uint8_t> header_buf;
    build_header(header_buf, /*include_sentinel=*/false);
    std::vector<uint8_t> inline_buf;
    inline_buf.reserve(header_buf.size() + dir_buf.size());
    inline_buf.insert(inline_buf.end(), header_buf.begin(), header_buf.end());
    inline_buf.insert(inline_buf.end(), dir_buf.begin(), dir_buf.end());

    PageId meta_page_id = 0;

    if (inline_buf.size() <= MAX_INLINE_SIZE) {
        // Small directory — write everything inline (V1).
        auto meta_page_r = bpm.new_page();
        if (!meta_page_r) {
            return make_error(meta_page_r.error().code,
                              "hash persist: failed to allocate meta page: " +
                                  meta_page_r.error().message);
        }
        auto* meta_page = *meta_page_r;
        meta_page_id = meta_page->page_id();
        meta_page->reset(meta_page_id, PageType::HASH_META);
        auto slot = meta_page->insert_tuple(std::span<const uint8_t>(inline_buf));
        if (!slot) {
            return make_error(slot.error().code,
                              "hash persist: meta data too large for page: " +
                                  slot.error().message);
        }
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/true);
    } else {
        // Large directory — chunk directory bytes into overflow pages, meta page
        // holds a compact V2 header + list of overflow PageIds.
        std::vector<PageId> overflow_page_ids;
        size_t offset = 0;
        while (offset < dir_buf.size()) {
            size_t chunk_size = std::min(MAX_INLINE_SIZE, dir_buf.size() - offset);
            auto ovf_page_r = bpm.new_page();
            if (!ovf_page_r) {
                return make_error(ovf_page_r.error().code,
                                  "hash persist: failed to allocate overflow page: " +
                                      ovf_page_r.error().message);
            }
            auto* ovf_page = *ovf_page_r;
            ovf_page->reset(ovf_page->page_id(), PageType::HASH_META);
            auto ovf_slot = ovf_page->insert_tuple(
                std::span<const uint8_t>(dir_buf.data() + offset, chunk_size));
            if (!ovf_slot) {
                return make_error(ovf_slot.error().code,
                                  "hash persist: overflow chunk too large: " +
                                      ovf_slot.error().message);
            }
            overflow_page_ids.push_back(ovf_page->page_id());
            (void)bpm.unpin_page(ovf_page->page_id(), /*is_dirty=*/true);
            offset += chunk_size;
        }

        // Build V2 meta header + overflow page references.
        build_header(header_buf, /*include_sentinel=*/true);
        write_u32(header_buf, static_cast<uint32_t>(overflow_page_ids.size()));
        for (auto ovf_id : overflow_page_ids) {
            write_u32(header_buf, ovf_id);
        }

        auto meta_page_r = bpm.new_page();
        if (!meta_page_r) {
            return make_error(meta_page_r.error().code,
                              "hash persist: failed to allocate meta page: " +
                                  meta_page_r.error().message);
        }
        auto* meta_page = *meta_page_r;
        meta_page_id = meta_page->page_id();
        meta_page->reset(meta_page_id, PageType::HASH_META);
        auto slot = meta_page->insert_tuple(std::span<const uint8_t>(header_buf));
        if (!slot) {
            return make_error(slot.error().code,
                              "hash persist: v2 meta header too large: " + slot.error().message);
        }
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/true);
    }

    auto flush = bpm.flush_all();
    if (!flush) {
        return make_error(flush.error().code,
                          "hash persist: flush failed: " + flush.error().message);
    }

    SIXSEVEN_LOG_DEBUG("hash persist: wrote {} unique buckets, directory size {}, meta page {}",
                       unique_buckets.size(),
                       index.directory_.size(),
                       meta_page_id);

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

    // Detect format version.
    // V2 starts with sentinel byte 0 followed by version byte 2.
    // V1 starts with global_depth (u32 LE); when global_depth > 0 the first
    // byte is non-zero, and when it is 0 the second byte is also 0 (not 2).
    bool is_v2 = (meta_data->size() >= 2 && p[0] == 0 && p[1] == 2);
    if (is_v2) {
        p += 2; // skip sentinel + version
    }

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

    // Read directory: either inline (V1) or via overflow pages (V2).
    std::vector<PageId> dir_disk_pages;

    if (!is_v2) {
        // V1: directory is inlined after the header.
        uint32_t dir_size = read_u32(p);
        dir_disk_pages.resize(dir_size);
        for (uint32_t i = 0; i < dir_size; ++i) {
            dir_disk_pages[i] = read_u32(p);
        }
    } else {
        // V2: read overflow page IDs, concatenate their tuples, then parse.
        uint32_t overflow_count = read_u32(p);
        std::vector<PageId> overflow_ids(overflow_count);
        for (uint32_t i = 0; i < overflow_count; ++i) {
            overflow_ids[i] = read_u32(p);
        }

        // Concatenate all overflow page tuples into one directory buffer.
        std::vector<uint8_t> dir_buf;
        for (auto ovf_id : overflow_ids) {
            auto ovf_page_r = bpm.fetch_page(ovf_id);
            if (!ovf_page_r) {
                (void)bpm.unpin_page(meta_page_id, false);
                return make_error(ovf_page_r.error().code,
                                  "hash load: failed to fetch overflow page: " +
                                      ovf_page_r.error().message);
            }
            auto ovf_tuple = (*ovf_page_r)->get_tuple(0);
            if (!ovf_tuple) {
                (void)bpm.unpin_page(ovf_id, false);
                (void)bpm.unpin_page(meta_page_id, false);
                return make_error(ovf_tuple.error().code,
                                  "hash load: failed to read overflow tuple: " +
                                      ovf_tuple.error().message);
            }
            dir_buf.insert(dir_buf.end(), ovf_tuple->begin(), ovf_tuple->end());
            (void)bpm.unpin_page(ovf_id, false);
        }

        // Parse directory from concatenated buffer.
        const uint8_t* dp = dir_buf.data();
        uint32_t dir_size = read_u32(dp);
        dir_disk_pages.resize(dir_size);
        for (uint32_t i = 0; i < dir_size; ++i) {
            dir_disk_pages[i] = read_u32(dp);
        }
    }

    (void)bpm.unpin_page(meta_page_id, false);

    // Load unique buckets. Multiple directory slots may share the same page.
    std::unordered_map<PageId, std::shared_ptr<HashBucket>> loaded_buckets;

    for (auto disk_page_id : dir_disk_pages) {
        if (loaded_buckets.contains(disk_page_id)) {
            continue;
        }

        auto page_r = bpm.fetch_page(disk_page_id);
        if (!page_r) {
            return make_error(page_r.error().code,
                              "hash load: failed to fetch bucket page: " + page_r.error().message);
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
                                  "hash load: failed to deserialize key: " + key.error().message);
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
    auto dir_size = static_cast<uint32_t>(dir_disk_pages.size());
    index->directory_.resize(dir_size);
    for (uint32_t i = 0; i < dir_size; ++i) {
        index->directory_[i] = loaded_buckets[dir_disk_pages[i]];
    }

    SIXSEVEN_LOG_DEBUG("hash load: restored {} unique buckets, directory size {}, size {}",
                       loaded_buckets.size(),
                       dir_size,
                       total_size);

    return ok(std::move(index));
}

} // namespace sixseven
