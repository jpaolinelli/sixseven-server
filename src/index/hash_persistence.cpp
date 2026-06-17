#include "sixseven/index/hash_persistence.h"

#include "sixseven/common/logging.h"
#include "sixseven/index/index_encoding.h"
#include "sixseven/storage/serialization.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace sixseven {

using namespace index_encoding;

// ---------------------------------------------------------------------------
// Binary encoding helpers (little-endian)
// ---------------------------------------------------------------------------

namespace {

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

// Magic number written at the start of a V2 meta page (little-endian uint32).
//
// Value: 0xFF534858 => LE bytes [0x58, 0x48, 0x53, 0xFF].
//
// Collision-free with V1: a V1 page begins with global_depth as a uint32 LE.
// The most-significant byte of 0xFF534858 is 0xFF, so for this to match a V1
// global_depth the depth would have to be >= 0xFF000000 = 4,278,190,080.
// That would require 2^4,278,190,080 directory slots -- physically impossible.
// No realistic V1 page can ever produce these leading bytes.
constexpr uint32_t HASH_V2_MAGIC = 0xFF534858U;

/// RAII guard that unpins a buffer-pool page when it goes out of scope.
///
/// Prevents pin leaks on early-return error paths without requiring a manual
/// unpin before each return.  Call dismiss() when the pin is intentionally
/// transferred (e.g. the page pointer is handed off and the caller will unpin).
///
/// The dirty flag defaults to false; call mark_dirty() before destruction (or
/// before dismiss()) to unpin with is_dirty=true.
struct PinGuard {
    PinGuard(BufferPoolManager& bpm, PageId pid, bool dirty = false)
        : bpm_(&bpm), page_id_(pid), is_dirty_(dirty) {}

    ~PinGuard() {
        if (bpm_ != nullptr) {
            (void)bpm_->unpin_page(page_id_, is_dirty_);
        }
    }

    /// Mark the page dirty before destruction.
    void mark_dirty() { is_dirty_ = true; }

    /// Release ownership — the destructor will NOT unpin.
    void dismiss() { bpm_ = nullptr; }

    PinGuard(const PinGuard&) = delete;
    PinGuard& operator=(const PinGuard&) = delete;
    PinGuard(PinGuard&&) = delete;
    PinGuard& operator=(PinGuard&&) = delete;

private:
    BufferPoolManager* bpm_;
    PageId page_id_;
    bool is_dirty_;
};

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
//   magic        (u32 = HASH_V2_MAGIC = 0xFF534858,
//                 i.e. LE bytes [0x58, 0x48, 0x53, 0xFF])
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
// Detection on load: read the first 4 bytes as a little-endian uint32; if it
// equals HASH_V2_MAGIC (0xFF534858) then this is a V2 page.
//
// Why HASH_V2_MAGIC is collision-free with V1:
//   V1 begins with global_depth as a little-endian uint32.  In little-endian
//   byte order, 0xFF534858 has its most-significant byte equal to 0xFF (byte[3]
//   of the 4-byte field).  For that byte to equal 0xFF the global_depth would
//   have to be >= 0xFF000000 = 4,278,190,080, which requires 2^4,278,190,080
//   directory slots — physically impossible.  Therefore no V1 serialization can
//   ever start with this magic value.

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
        // PinGuard ensures the bucket page is unpinned on every exit path.
        PinGuard bucket_guard(bpm, page->page_id(), /*dirty=*/false);
        page->reset(page->page_id(), PageType::HASH_BUCKET);
        auto slot = page->insert_tuple(std::span<const uint8_t>(buf));
        if (!slot) {
            return make_error(slot.error().code,
                              "hash persist: bucket data too large for page: " +
                                  slot.error().message);
        }
        bucket_disk_pages.push_back(page->page_id());
        bucket_guard.mark_dirty();
        // bucket_guard destructs here, unpinning dirty.
    }

    // Build the fixed header (config + key types, no directory).
    auto build_header = [&](std::vector<uint8_t>& hdr, bool include_magic) {
        hdr.clear();
        if (include_magic) {
            // V2 prefix: 4-byte magic 0xFF534858.
            write_u32(hdr, HASH_V2_MAGIC);
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
    build_header(header_buf, /*include_magic=*/false);
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
        // PinGuard ensures the meta page is unpinned on every exit path.
        PinGuard meta_guard(bpm, meta_page_id, /*dirty=*/false);
        auto slot = meta_page->insert_tuple(std::span<const uint8_t>(inline_buf));
        if (!slot) {
            return make_error(slot.error().code,
                              "hash persist: meta data too large for page: " +
                                  slot.error().message);
        }
        meta_guard.mark_dirty();
        // meta_guard destructs here, unpinning dirty.
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
            // PinGuard ensures the overflow page is unpinned on every exit path.
            PinGuard ovf_guard(bpm, ovf_page->page_id(), /*dirty=*/false);
            ovf_page->reset(ovf_page->page_id(), PageType::HASH_META);
            auto ovf_slot = ovf_page->insert_tuple(
                std::span<const uint8_t>(dir_buf.data() + offset, chunk_size));
            if (!ovf_slot) {
                return make_error(ovf_slot.error().code,
                                  "hash persist: overflow chunk too large: " +
                                      ovf_slot.error().message);
            }
            overflow_page_ids.push_back(ovf_page->page_id());
            ovf_guard.mark_dirty();
            // ovf_guard destructs here at end of loop iteration, unpinning dirty.
            offset += chunk_size;
        }

        // Build V2 meta header + overflow page references.
        build_header(header_buf, /*include_magic=*/true);
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
        // PinGuard ensures the meta page is unpinned on every exit path.
        PinGuard meta_guard(bpm, meta_page_id, /*dirty=*/false);
        auto slot = meta_page->insert_tuple(std::span<const uint8_t>(header_buf));
        if (!slot) {
            return make_error(slot.error().code,
                              "hash persist: v2 meta header too large: " + slot.error().message);
        }
        meta_guard.mark_dirty();
        // meta_guard destructs here, unpinning dirty.
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

    // Detect format version by reading the first 4 bytes as a little-endian
    // uint32 and comparing against HASH_V2_MAGIC (0xFF534858).
    //
    // This is guaranteed collision-free with V1: a V1 page starts with
    // global_depth as a uint32 LE. For that to equal 0xFF534858 the index
    // would need 2^4,278,190,080 directory slots -- physically impossible.
    // V1 indexes with any reachable global_depth will never produce these
    // leading bytes, so there is no false-positive V2 detection.
    bool is_v2 = false;
    if (meta_data->size() >= 4) {
        uint32_t leading = 0;
        std::memcpy(&leading, meta_data->data(), 4);
        is_v2 = (leading == HASH_V2_MAGIC);
    }

    auto meta_span_ = std::span<const uint8_t>(*meta_data);
    Reader meta_reader{meta_span_};
    if (is_v2) {
        // Skip magic (already read via memcpy above).
        auto skip_r = meta_reader.skip(4);
        if (!skip_r) {
            (void)bpm.unpin_page(meta_page_id, false);
            return make_error(skip_r.error().code,
                              "hash load: failed to skip magic: " + skip_r.error().message);
        }
    }

    auto r_global_depth = meta_reader.read_u32();
    if (!r_global_depth) {
        (void)bpm.unpin_page(meta_page_id, false);
        return make_error(r_global_depth.error().code,
                          "hash load: failed to read global_depth: " +
                              r_global_depth.error().message);
    }
    uint32_t global_depth = *r_global_depth;

    auto r_total_size = meta_reader.read_u64();
    if (!r_total_size) {
        (void)bpm.unpin_page(meta_page_id, false);
        return make_error(r_total_size.error().code,
                          "hash load: failed to read size: " + r_total_size.error().message);
    }
    uint64_t total_size = *r_total_size;

    auto r_bucket_capacity = meta_reader.read_u32();
    if (!r_bucket_capacity) {
        (void)bpm.unpin_page(meta_page_id, false);
        return make_error(r_bucket_capacity.error().code,
                          "hash load: failed to read bucket_capacity: " +
                              r_bucket_capacity.error().message);
    }
    uint32_t bucket_capacity = *r_bucket_capacity;

    auto r_is_unique = meta_reader.read_u8();
    if (!r_is_unique) {
        (void)bpm.unpin_page(meta_page_id, false);
        return make_error(r_is_unique.error().code,
                          "hash load: failed to read is_unique: " + r_is_unique.error().message);
    }
    bool is_unique = (*r_is_unique) != 0;

    auto r_key_type_count = meta_reader.read_u8();
    if (!r_key_type_count) {
        (void)bpm.unpin_page(meta_page_id, false);
        return make_error(r_key_type_count.error().code,
                          "hash load: failed to read key_type_count: " +
                              r_key_type_count.error().message);
    }
    uint8_t key_type_count = *r_key_type_count;

    std::vector<TypeId> key_types;
    key_types.reserve(key_type_count);
    for (uint8_t i = 0; i < key_type_count; ++i) {
        auto r_kt = meta_reader.read_u8();
        if (!r_kt) {
            (void)bpm.unpin_page(meta_page_id, false);
            return make_error(r_kt.error().code,
                              "hash load: failed to read key_type: " + r_kt.error().message);
        }
        key_types.push_back(static_cast<TypeId>(*r_kt));
    }

    // Read directory: either inline (V1) or via overflow pages (V2).
    std::vector<PageId> dir_disk_pages;

    if (!is_v2) {
        // V1: directory is inlined after the header.
        auto r_dir_size = meta_reader.read_u32();
        if (!r_dir_size) {
            (void)bpm.unpin_page(meta_page_id, false);
            return make_error(r_dir_size.error().code,
                              "hash load: failed to read dir_size: " + r_dir_size.error().message);
        }
        uint32_t dir_size = *r_dir_size;
        dir_disk_pages.resize(dir_size);
        for (uint32_t i = 0; i < dir_size; ++i) {
            auto r_pid = meta_reader.read_u32();
            if (!r_pid) {
                (void)bpm.unpin_page(meta_page_id, false);
                return make_error(r_pid.error().code,
                                  "hash load: failed to read directory entry: " +
                                      r_pid.error().message);
            }
            dir_disk_pages[i] = *r_pid;
        }
    } else {
        // V2: read overflow page IDs, concatenate their tuples, then parse.
        auto r_overflow_count = meta_reader.read_u32();
        if (!r_overflow_count) {
            (void)bpm.unpin_page(meta_page_id, false);
            return make_error(r_overflow_count.error().code,
                              "hash load: failed to read overflow_count: " +
                                  r_overflow_count.error().message);
        }
        uint32_t overflow_count = *r_overflow_count;

        std::vector<PageId> overflow_ids(overflow_count);
        for (uint32_t i = 0; i < overflow_count; ++i) {
            auto r_oid = meta_reader.read_u32();
            if (!r_oid) {
                (void)bpm.unpin_page(meta_page_id, false);
                return make_error(r_oid.error().code,
                                  "hash load: failed to read overflow page id: " +
                                      r_oid.error().message);
            }
            overflow_ids[i] = *r_oid;
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
        auto dir_span_ = std::span<const uint8_t>(dir_buf);
        Reader dir_reader{dir_span_};
        auto r_dir_size = dir_reader.read_u32();
        if (!r_dir_size) {
            (void)bpm.unpin_page(meta_page_id, false);
            return make_error(r_dir_size.error().code,
                              "hash load: failed to read dir_size from overflow: " +
                                  r_dir_size.error().message);
        }
        uint32_t dir_size = *r_dir_size;
        dir_disk_pages.resize(dir_size);
        for (uint32_t i = 0; i < dir_size; ++i) {
            auto r_pid = dir_reader.read_u32();
            if (!r_pid) {
                (void)bpm.unpin_page(meta_page_id, false);
                return make_error(r_pid.error().code,
                                  "hash load: failed to read directory entry from overflow: " +
                                      r_pid.error().message);
            }
            dir_disk_pages[i] = *r_pid;
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

        auto bp_span_ = std::span<const uint8_t>(*tuple_data);
        Reader bp{bp_span_};

        auto r_local_depth = bp.read_u32();
        if (!r_local_depth) {
            (void)bpm.unpin_page(disk_page_id, false);
            return make_error(r_local_depth.error().code,
                              "hash load: failed to read local_depth: " +
                                  r_local_depth.error().message);
        }
        uint32_t local_depth = *r_local_depth;

        auto r_entry_count = bp.read_u32();
        if (!r_entry_count) {
            (void)bpm.unpin_page(disk_page_id, false);
            return make_error(r_entry_count.error().code,
                              "hash load: failed to read entry_count: " +
                                  r_entry_count.error().message);
        }
        uint32_t entry_count = *r_entry_count;

        auto bucket = std::make_shared<HashBucket>();
        bucket->local_depth = local_depth;
        bucket->entries.reserve(entry_count);

        for (uint32_t i = 0; i < entry_count; ++i) {
            HashBucketEntry entry;

            auto r_hash = bp.read_u64();
            if (!r_hash) {
                (void)bpm.unpin_page(disk_page_id, false);
                return make_error(r_hash.error().code,
                                  "hash load: failed to read entry hash: " +
                                      r_hash.error().message);
            }
            entry.hash = static_cast<size_t>(*r_hash);

            // Interop with deserialize_key which takes raw const uint8_t* pointers.
            size_t key_start = bp.pos;
            const uint8_t* kp = bp.data.data() + key_start;
            const uint8_t* kend = bp.end_ptr();
            auto key = deserialize_key(kp, kend, key_types);
            if (!key) {
                (void)bpm.unpin_page(disk_page_id, false);
                return make_error(key.error().code,
                                  "hash load: failed to deserialize key: " + key.error().message);
            }
            bp.advance(static_cast<size_t>(kp - (bp.data.data() + key_start)));
            entry.key = std::move(*key);

            auto r_page_id = bp.read_u32();
            if (!r_page_id) {
                (void)bpm.unpin_page(disk_page_id, false);
                return make_error(r_page_id.error().code,
                                  "hash load: failed to read rid.page_id: " +
                                      r_page_id.error().message);
            }
            entry.rid.page_id = *r_page_id;

            auto r_slot_id = bp.read_u16();
            if (!r_slot_id) {
                (void)bpm.unpin_page(disk_page_id, false);
                return make_error(r_slot_id.error().code,
                                  "hash load: failed to read rid.slot_id: " +
                                      r_slot_id.error().message);
            }
            entry.rid.slot_id = *r_slot_id;

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
