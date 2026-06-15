#include "sixseven/index/btree_persistence.h"

#include "sixseven/common/logging.h"
#include "sixseven/storage/serialization.h"

#include <cstring>
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

/// Serialize a composite key (vector<Value>) to bytes.
void serialize_key(std::vector<uint8_t>& buf, const KeyType& key) {
    for (const auto& val : key) {
        auto bytes = serialize(val);
        buf.insert(buf.end(), bytes.begin(), bytes.end());
    }
}

/// Deserialize a composite key from bytes.
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

} // namespace

// ---------------------------------------------------------------------------
// persist
// ---------------------------------------------------------------------------

Result<PageId> BTreePersistence::persist(BufferPoolManager& bpm, const BTreeIndex& index) {
    // Acquire shared lock to ensure consistent snapshot while iterating.
    // The index's public methods guarantee thread safety via this latch,
    // and persist reads index internals directly as a friend class.
    std::shared_lock lock(index.tree_latch());

    // Collect all leaf and internal node page IDs from the in-memory tree.
    // We store each node as a tuple on its own page in the index file.

    // Serialize each leaf node.
    struct LeafRecord {
        PageId mem_page_id; // In-memory page ID (key in leaf_nodes_ map)
        std::vector<uint8_t> data;
    };
    std::vector<LeafRecord> leaf_records;
    leaf_records.reserve(index.leaf_nodes_.size());

    for (const auto& [pid, leaf] : index.leaf_nodes_) {
        std::vector<uint8_t> buf;
        // Header: page_id, parent, next_leaf, prev_leaf, key_count
        write_u32(buf, leaf->page_id());
        write_u32(buf, leaf->parent_page_id());
        write_u32(buf, leaf->next_leaf_id());
        write_u32(buf, leaf->prev_leaf_id());
        write_u16(buf, leaf->key_count());

        // Key-RID pairs
        for (uint16_t i = 0; i < leaf->key_count(); ++i) {
            serialize_key(buf, leaf->key_at(i));
            write_u32(buf, leaf->rid_at(i).page_id);
            write_u16(buf, leaf->rid_at(i).slot_id);
        }

        leaf_records.push_back({pid, std::move(buf)});
    }

    // Serialize each internal node.
    struct InternalRecord {
        PageId mem_page_id;
        std::vector<uint8_t> data;
    };
    std::vector<InternalRecord> internal_records;
    internal_records.reserve(index.internal_nodes_.size());

    for (const auto& [pid, node] : index.internal_nodes_) {
        std::vector<uint8_t> buf;
        // Header: page_id, parent, key_count
        write_u32(buf, node->page_id());
        write_u32(buf, node->parent_page_id());
        write_u16(buf, node->key_count());

        // Keys
        for (uint16_t i = 0; i < node->key_count(); ++i) {
            serialize_key(buf, node->key_at(i));
        }

        // Children (key_count + 1 children)
        write_u16(buf, static_cast<uint16_t>(node->children().size()));
        for (auto child_id : node->children()) {
            write_u32(buf, child_id);
        }

        internal_records.push_back({pid, std::move(buf)});
    }

    // Write leaf node pages.
    std::vector<PageId> leaf_disk_pages;
    leaf_disk_pages.reserve(leaf_records.size());
    for (auto& rec : leaf_records) {
        auto page_r = bpm.new_page();
        if (!page_r) {
            return make_error(page_r.error().code,
                              "btree persist: failed to allocate leaf page: " +
                                  page_r.error().message);
        }
        auto* page = *page_r;
        page->reset(page->page_id(), PageType::BTREE_LEAF);
        auto slot = page->insert_tuple(std::span<const uint8_t>(rec.data));
        if (!slot) {
            return make_error(slot.error().code,
                              "btree persist: leaf data too large for page: " +
                                  slot.error().message);
        }
        leaf_disk_pages.push_back(page->page_id());
        (void)bpm.unpin_page(page->page_id(), /*is_dirty=*/true);
    }

    // Write internal node pages.
    std::vector<PageId> internal_disk_pages;
    internal_disk_pages.reserve(internal_records.size());
    for (auto& rec : internal_records) {
        auto page_r = bpm.new_page();
        if (!page_r) {
            return make_error(page_r.error().code,
                              "btree persist: failed to allocate internal page: " +
                                  page_r.error().message);
        }
        auto* page = *page_r;
        page->reset(page->page_id(), PageType::BTREE_INTERNAL);
        auto slot = page->insert_tuple(std::span<const uint8_t>(rec.data));
        if (!slot) {
            return make_error(slot.error().code,
                              "btree persist: internal data too large for page: " +
                                  slot.error().message);
        }
        internal_disk_pages.push_back(page->page_id());
        (void)bpm.unpin_page(page->page_id(), /*is_dirty=*/true);
    }

    // Build the page directory bytes: leaf mappings followed by internal mappings.
    std::vector<uint8_t> dir_buf;
    dir_buf.reserve((leaf_records.size() + internal_records.size()) * 8 + 8);
    write_u32(dir_buf, static_cast<uint32_t>(leaf_records.size()));
    for (size_t i = 0; i < leaf_records.size(); ++i) {
        write_u32(dir_buf, leaf_records[i].mem_page_id);
        write_u32(dir_buf, leaf_disk_pages[i]);
    }
    write_u32(dir_buf, static_cast<uint32_t>(internal_records.size()));
    for (size_t i = 0; i < internal_records.size(); ++i) {
        write_u32(dir_buf, internal_records[i].mem_page_id);
        write_u32(dir_buf, internal_disk_pages[i]);
    }

    // Build meta page header (fixed-size portion that always fits in one page).
    // Format version 0 = legacy inline, version 2 = overflow pages.
    // We try inline first; if it doesn't fit, use overflow.
    std::vector<uint8_t> header_buf;
    auto build_header = [&](bool include_directory) {
        header_buf.clear();
        if (!include_directory) {
            // Version 2 sentinel: first byte 0 signals multi-page format.
            write_u8(header_buf, 0); // sentinel
            write_u8(header_buf, 2); // version
        }
        write_u32(header_buf, index.root_page_id_);
        write_u64(header_buf, index.size_);
        write_u32(header_buf, index.next_page_id_);
        write_u16(header_buf, index.config_.internal_max_keys);
        write_u16(header_buf, index.config_.leaf_max_keys);
        write_u8(header_buf, index.config_.is_unique ? 1 : 0);
        write_u8(header_buf, static_cast<uint8_t>(index.config_.key_types.size()));
        for (auto t : index.config_.key_types) {
            write_u8(header_buf, static_cast<uint8_t>(t));
        }
    };

    // Try inline format first (original single-page meta).
    build_header(/*include_directory=*/true);
    std::vector<uint8_t> inline_buf;
    inline_buf.reserve(header_buf.size() + dir_buf.size());
    inline_buf.insert(inline_buf.end(), header_buf.begin(), header_buf.end());
    inline_buf.insert(inline_buf.end(), dir_buf.begin(), dir_buf.end());

    // Approximate check: page_size - header(24) - slot_entry(4) ≈ 8164 usable bytes.
    constexpr size_t MAX_INLINE_SIZE = 8100;

    PageId meta_page_id = 0;

    if (inline_buf.size() <= MAX_INLINE_SIZE) {
        // Small tree — write everything inline (backward-compatible).
        auto meta_page_r = bpm.new_page();
        if (!meta_page_r) {
            return make_error(meta_page_r.error().code,
                              "btree persist: failed to allocate meta page: " +
                                  meta_page_r.error().message);
        }
        auto* meta_page = *meta_page_r;
        meta_page_id = meta_page->page_id();
        meta_page->reset(meta_page_id, PageType::BTREE_META);
        auto slot = meta_page->insert_tuple(std::span<const uint8_t>(inline_buf));
        if (!slot) {
            return make_error(slot.error().code,
                              "btree persist: meta data too large for page: " +
                                  slot.error().message);
        }
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/true);
    } else {
        // Large tree — write directory to overflow pages, meta page gets compact header.
        // Chunk directory into pages (~8100 bytes per page).
        std::vector<PageId> overflow_page_ids;
        size_t offset = 0;
        while (offset < dir_buf.size()) {
            size_t chunk_size = std::min(MAX_INLINE_SIZE, dir_buf.size() - offset);
            auto ovf_page_r = bpm.new_page();
            if (!ovf_page_r) {
                return make_error(ovf_page_r.error().code,
                                  "btree persist: failed to allocate overflow page: " +
                                      ovf_page_r.error().message);
            }
            auto* ovf_page = *ovf_page_r;
            ovf_page->reset(ovf_page->page_id(), PageType::BTREE_META);
            auto ovf_slot = ovf_page->insert_tuple(
                std::span<const uint8_t>(dir_buf.data() + offset, chunk_size));
            if (!ovf_slot) {
                return make_error(ovf_slot.error().code,
                                  "btree persist: overflow chunk too large: " +
                                      ovf_slot.error().message);
            }
            overflow_page_ids.push_back(ovf_page->page_id());
            (void)bpm.unpin_page(ovf_page->page_id(), /*is_dirty=*/true);
            offset += chunk_size;
        }

        // Build version-2 meta header + overflow page references.
        build_header(/*include_directory=*/false);
        write_u32(header_buf, static_cast<uint32_t>(overflow_page_ids.size()));
        for (auto ovf_id : overflow_page_ids) {
            write_u32(header_buf, ovf_id);
        }

        auto meta_page_r = bpm.new_page();
        if (!meta_page_r) {
            return make_error(meta_page_r.error().code,
                              "btree persist: failed to allocate meta page: " +
                                  meta_page_r.error().message);
        }
        auto* meta_page = *meta_page_r;
        meta_page_id = meta_page->page_id();
        meta_page->reset(meta_page_id, PageType::BTREE_META);
        auto slot = meta_page->insert_tuple(std::span<const uint8_t>(header_buf));
        if (!slot) {
            return make_error(slot.error().code,
                              "btree persist: v2 meta header too large: " + slot.error().message);
        }
        (void)bpm.unpin_page(meta_page_id, /*is_dirty=*/true);
    }

    // Flush all pages to disk.
    auto flush = bpm.flush_all();
    if (!flush) {
        return make_error(flush.error().code,
                          "btree persist: flush failed: " + flush.error().message);
    }

    // Store meta page ID at offset 0 of the file header extension area
    // so the loader can find it without guessing.
    // Access the DiskManager through the BPM's internal state isn't possible,
    // so we write it as a special tuple on page 1 instead.
    // Actually, we always write meta page last, so just return the ID.

    SIXSEVEN_LOG_DEBUG("btree persist: wrote {} leaf + {} internal nodes, meta page {}",
                       leaf_records.size(),
                       internal_records.size(),
                       meta_page_id);

    return ok(meta_page_id);
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------

Result<std::unique_ptr<BTreeIndex>> BTreePersistence::load(BufferPoolManager& bpm,
                                                           PageId meta_page_id) {
    // Read meta page.
    auto meta_page_r = bpm.fetch_page(meta_page_id);
    if (!meta_page_r) {
        return make_error(meta_page_r.error().code,
                          "btree load: failed to fetch meta page: " + meta_page_r.error().message);
    }
    auto* meta_page = *meta_page_r;
    auto meta_data = meta_page->get_tuple(0);
    if (!meta_data) {
        (void)bpm.unpin_page(meta_page_id, false);
        return make_error(meta_data.error().code,
                          "btree load: failed to read meta tuple: " + meta_data.error().message);
    }

    const uint8_t* p = meta_data->data();

    // Detect format version. Version 2 starts with sentinel byte 0 + version byte 2.
    // Legacy format starts with root_page_id (u32 LE). When root_page_id is 0 (empty tree),
    // the first two bytes are [0x00, 0x00], which differs from v2's [0x00, 0x02].
    bool is_v2 = (meta_data->size() >= 2 && p[0] == 0 && p[1] == 2);
    if (is_v2) {
        p += 1; // skip sentinel
        p += 1; // skip version byte
    }

    PageId root_page_id = read_u32(p);
    uint64_t tree_size = read_u64(p);
    PageId next_page_id = read_u32(p);
    uint16_t internal_max_keys = read_u16(p);
    uint16_t leaf_max_keys = read_u16(p);
    bool is_unique = read_u8(p) != 0;

    uint8_t key_type_count = read_u8(p);
    std::vector<TypeId> key_types;
    key_types.reserve(key_type_count);
    for (uint8_t i = 0; i < key_type_count; ++i) {
        key_types.push_back(static_cast<TypeId>(read_u8(p)));
    }

    // Read page directory — either inline (legacy) or from overflow pages (v2).
    struct PageMapping {
        PageId mem_page_id;
        PageId disk_page_id;
    };
    std::vector<PageMapping> leaf_mappings;
    std::vector<PageMapping> internal_mappings;

    if (!is_v2) {
        // Legacy inline directory.
        uint32_t leaf_count = read_u32(p);
        leaf_mappings.resize(leaf_count);
        for (uint32_t i = 0; i < leaf_count; ++i) {
            leaf_mappings[i].mem_page_id = read_u32(p);
            leaf_mappings[i].disk_page_id = read_u32(p);
        }
        uint32_t internal_count = read_u32(p);
        internal_mappings.resize(internal_count);
        for (uint32_t i = 0; i < internal_count; ++i) {
            internal_mappings[i].mem_page_id = read_u32(p);
            internal_mappings[i].disk_page_id = read_u32(p);
        }
    } else {
        // Version 2: read overflow page IDs, then reassemble directory from those pages.
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
                                  "btree load: failed to fetch overflow page: " +
                                      ovf_page_r.error().message);
            }
            auto ovf_tuple = (*ovf_page_r)->get_tuple(0);
            if (!ovf_tuple) {
                (void)bpm.unpin_page(ovf_id, false);
                (void)bpm.unpin_page(meta_page_id, false);
                return make_error(ovf_tuple.error().code,
                                  "btree load: failed to read overflow tuple: " +
                                      ovf_tuple.error().message);
            }
            dir_buf.insert(dir_buf.end(), ovf_tuple->begin(), ovf_tuple->end());
            (void)bpm.unpin_page(ovf_id, false);
        }

        // Parse directory from concatenated buffer.
        const uint8_t* dp = dir_buf.data();
        uint32_t leaf_count = read_u32(dp);
        leaf_mappings.resize(leaf_count);
        for (uint32_t i = 0; i < leaf_count; ++i) {
            leaf_mappings[i].mem_page_id = read_u32(dp);
            leaf_mappings[i].disk_page_id = read_u32(dp);
        }
        uint32_t internal_count = read_u32(dp);
        internal_mappings.resize(internal_count);
        for (uint32_t i = 0; i < internal_count; ++i) {
            internal_mappings[i].mem_page_id = read_u32(dp);
            internal_mappings[i].disk_page_id = read_u32(dp);
        }
    }

    (void)bpm.unpin_page(meta_page_id, false);

    // Build the BTreeIndex.
    BTreeConfig config;
    config.key_types = std::move(key_types);
    config.internal_max_keys = internal_max_keys;
    config.leaf_max_keys = leaf_max_keys;
    config.is_unique = is_unique;

    auto index = std::make_unique<BTreeIndex>(std::move(config));
    index->root_page_id_ = root_page_id;
    index->size_ = tree_size;
    index->next_page_id_ = next_page_id;

    uint16_t eff_leaf_max = index->effective_leaf_max_keys();
    uint16_t eff_internal_max = index->effective_internal_max_keys();

    // Load leaf nodes.
    for (const auto& mapping : leaf_mappings) {
        auto page_r = bpm.fetch_page(mapping.disk_page_id);
        if (!page_r) {
            return make_error(page_r.error().code,
                              "btree load: failed to fetch leaf page: " + page_r.error().message);
        }
        auto* page = *page_r;
        auto tuple_data = page->get_tuple(0);
        if (!tuple_data) {
            (void)bpm.unpin_page(mapping.disk_page_id, false);
            return make_error(tuple_data.error().code,
                              "btree load: failed to read leaf tuple: " +
                                  tuple_data.error().message);
        }

        const uint8_t* lp = tuple_data->data();
        const uint8_t* lend = lp + tuple_data->size();

        PageId page_id = read_u32(lp);
        PageId parent = read_u32(lp);
        PageId next_leaf = read_u32(lp);
        PageId prev_leaf = read_u32(lp);
        uint16_t key_count = read_u16(lp);

        auto leaf = std::make_unique<BTreeLeafNode>(page_id, eff_leaf_max);
        leaf->set_parent_page_id(parent);
        leaf->set_next_leaf_id(next_leaf);
        leaf->set_prev_leaf_id(prev_leaf);

        for (uint16_t i = 0; i < key_count; ++i) {
            auto key = deserialize_key(lp, lend, index->config_.key_types);
            if (!key) {
                (void)bpm.unpin_page(mapping.disk_page_id, false);
                return make_error(key.error().code,
                                  "btree load: failed to deserialize leaf key: " +
                                      key.error().message);
            }
            PageId rid_page = read_u32(lp);
            SlotId rid_slot = read_u16(lp);
            leaf->keys().push_back(std::move(*key));
            leaf->rids().push_back(RID{rid_page, rid_slot});
        }

        index->leaf_nodes_[page_id] = std::move(leaf);
        (void)bpm.unpin_page(mapping.disk_page_id, false);
    }

    // Load internal nodes.
    for (const auto& mapping : internal_mappings) {
        auto page_r = bpm.fetch_page(mapping.disk_page_id);
        if (!page_r) {
            return make_error(page_r.error().code,
                              "btree load: failed to fetch internal page: " +
                                  page_r.error().message);
        }
        auto* page = *page_r;
        auto tuple_data = page->get_tuple(0);
        if (!tuple_data) {
            (void)bpm.unpin_page(mapping.disk_page_id, false);
            return make_error(tuple_data.error().code,
                              "btree load: failed to read internal tuple: " +
                                  tuple_data.error().message);
        }

        const uint8_t* ip = tuple_data->data();
        const uint8_t* iend = ip + tuple_data->size();

        PageId page_id = read_u32(ip);
        PageId parent = read_u32(ip);
        uint16_t key_count = read_u16(ip);

        auto node = std::make_unique<BTreeInternalNode>(page_id, eff_internal_max);
        node->set_parent_page_id(parent);

        for (uint16_t i = 0; i < key_count; ++i) {
            auto key = deserialize_key(ip, iend, index->config_.key_types);
            if (!key) {
                (void)bpm.unpin_page(mapping.disk_page_id, false);
                return make_error(key.error().code,
                                  "btree load: failed to deserialize internal key: " +
                                      key.error().message);
            }
            node->keys().push_back(std::move(*key));
        }

        uint16_t child_count = read_u16(ip);
        for (uint16_t i = 0; i < child_count; ++i) {
            node->children().push_back(read_u32(ip));
        }

        index->internal_nodes_[page_id] = std::move(node);
        (void)bpm.unpin_page(mapping.disk_page_id, false);
    }

    SIXSEVEN_LOG_DEBUG("btree load: restored {} leaf + {} internal nodes, size {}",
                       leaf_mappings.size(),
                       internal_mappings.size(),
                       tree_size);

    return ok(std::move(index));
}

} // namespace sixseven
