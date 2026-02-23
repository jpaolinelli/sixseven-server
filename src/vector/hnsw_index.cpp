#include "giodb/vector/hnsw_index.h"

#include "giodb/common/logging.h"
#include "giodb/vector/distance.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_set>

namespace giodb {

HnswIndex::HnswIndex(BufferPoolManager& buffer_pool, WalWriter* wal)
    : buffer_pool_(buffer_pool), wal_(wal) {}

// -- Index lifecycle ---------------------------------------------------------

Result<void> HnswIndex::create(const HnswIndexConfig& config) {
    std::unique_lock<std::shared_mutex> lock(latch_);

    if (config.dimension == 0) {
        return make_error(StatusCode::INVALID_ARGUMENT, "HNSW dimension must be > 0");
    }
    if (config.m == 0) {
        return make_error(StatusCode::INVALID_ARGUMENT, "HNSW M parameter must be > 0");
    }

    auto meta_page_result = buffer_pool_.new_page();
    if (!meta_page_result.has_value()) {
        return make_error(StatusCode::INTERNAL_ERROR, "Failed to allocate HNSW metadata page");
    }
    Page* meta_page = meta_page_result.value();
    meta_page_id_ = meta_page->page_id();
    meta_page->set_page_type(PageType::HNSW_META);

    meta_.entry_point_id = hnsw_invalid_node_id;
    meta_.max_layer = 0;
    meta_.m_param = config.m;
    meta_.ef_construction = config.ef_construction;
    meta_.ef_search = config.ef_search;
    meta_.dimension = config.dimension;
    meta_.node_count = 0;
    meta_.tombstone_count = 0;
    meta_.next_node_id = 0;

    auto meta_bytes = serialize_hnsw_meta(meta_);
    auto insert_result = meta_page->insert_tuple(meta_bytes);
    if (!insert_result.has_value()) {
        (void)buffer_pool_.unpin_page(meta_page_id_, false);
        return make_error(StatusCode::INTERNAL_ERROR, "Failed to write HNSW metadata to page");
    }

    auto unpin_result = buffer_pool_.unpin_page(meta_page_id_, true);
    if (!unpin_result.has_value()) {
        return make_error(StatusCode::INTERNAL_ERROR, "Failed to unpin HNSW metadata page");
    }

    GIODB_LOG_INFO("Created HNSW index: dim={}, M={}, ef_c={}, ef_s={}, meta_page={}",
                   config.dimension,
                   config.m,
                   config.ef_construction,
                   config.ef_search,
                   meta_page_id_);

    return ok();
}

Result<void> HnswIndex::load(PageId meta_page_id) {
    std::unique_lock<std::shared_mutex> lock(latch_);

    meta_page_id_ = meta_page_id;

    auto page_result = buffer_pool_.fetch_page(meta_page_id_);
    if (!page_result.has_value()) {
        return make_error(StatusCode::IO_ERROR, "Failed to fetch HNSW metadata page");
    }
    Page* meta_page = page_result.value();

    if (meta_page->page_type() != PageType::HNSW_META) {
        (void)buffer_pool_.unpin_page(meta_page_id_, false);
        return make_error(StatusCode::INVALID_ARGUMENT, "Page is not an HNSW_META page");
    }

    auto tuple_result = meta_page->get_tuple(0);
    if (!tuple_result.has_value()) {
        (void)buffer_pool_.unpin_page(meta_page_id_, false);
        return make_error(StatusCode::INTERNAL_ERROR, "Failed to read HNSW metadata tuple");
    }

    auto meta_result = deserialize_hnsw_meta(tuple_result.value());
    if (!meta_result.has_value()) {
        (void)buffer_pool_.unpin_page(meta_page_id_, false);
        return make_error(meta_result.error().code, meta_result.error().message);
    }
    meta_ = meta_result.value();

    (void)buffer_pool_.unpin_page(meta_page_id_, false);

    // Restore page lists from persisted metadata.
    node_pages_ = meta_.node_page_ids;
    vector_pages_ = meta_.vector_page_ids;

    // Rebuild node_map_ by scanning all HNSW_NODE pages.
    node_map_.clear();
    for (PageId pid : node_pages_) {
        auto fetch = buffer_pool_.fetch_page(pid);
        if (!fetch.has_value()) {
            continue;
        }
        Page* page = fetch.value();

        uint16_t slots = page->slot_count();
        for (SlotId sid = 0; sid < slots; ++sid) {
            auto tuple = page->get_tuple(sid);
            if (!tuple.has_value()) {
                continue; // deleted slot
            }
            auto node = deserialize_hnsw_node(tuple.value());
            if (!node.has_value()) {
                continue;
            }
            // Include tombstoned nodes so compact() can find them.
            node_map_[node.value().node_id] = {pid, sid};
        }

        (void)buffer_pool_.unpin_page(pid, false);
    }

    GIODB_LOG_INFO("Loaded HNSW index: dim={}, nodes={}, meta_page={}, "
                   "node_map_size={}",
                   meta_.dimension,
                   meta_.node_count,
                   meta_page_id_,
                   node_map_.size());

    return ok();
}

// -- Insert ------------------------------------------------------------------

Result<uint32_t> HnswIndex::insert(std::span<const float> vector) {
    std::unique_lock<std::shared_mutex> lock(latch_);

    if (vector.size() != meta_.dimension) {
        return make_error(StatusCode::INVALID_ARGUMENT, "Vector dimension mismatch");
    }

    // Assign node ID and determine random layer.
    uint32_t new_id = meta_.next_node_id++;
    uint8_t new_layer = random_layer();

    // Store the vector data.
    auto vec_loc_result = allocate_vector(vector);
    if (!vec_loc_result.has_value()) {
        return make_error(vec_loc_result.error().code, vec_loc_result.error().message);
    }
    auto vec_loc = vec_loc_result.value();

    // Build the node with empty neighbor lists.
    HnswNode new_node;
    new_node.node_id = new_id;
    new_node.max_layer = new_layer;
    new_node.flags = 0;
    new_node.vector_page_id = vec_loc.page_id;
    new_node.vector_slot_id = vec_loc.slot_id;
    new_node.neighbors.resize(static_cast<size_t>(new_layer) + 1);

    // First node: just store it and set as entry point.
    if (meta_.entry_point_id == hnsw_invalid_node_id) {
        auto node_loc_result = allocate_node(new_node);
        if (!node_loc_result.has_value()) {
            return make_error(node_loc_result.error().code, node_loc_result.error().message);
        }

        meta_.entry_point_id = new_id;
        meta_.max_layer = new_layer;
        meta_.node_count = 1;

        auto flush_result = flush_meta();
        if (!flush_result.has_value()) {
            return make_error(flush_result.error().code, flush_result.error().message);
        }

        // Log to WAL if available.
        if (wal_ != nullptr) {
            auto node_bytes = serialize_hnsw_node(new_node);
            (void)log_wal(WalRecordType::INSERT,
                          node_loc_result.value().page_id,
                          node_loc_result.value().slot_id,
                          node_bytes);
        }

        return ok(new_id);
    }

    // Traverse upper layers with greedy search to find entry point for lower layers.
    uint32_t current_entry = meta_.entry_point_id;
    for (int layer = static_cast<int>(meta_.max_layer); layer > static_cast<int>(new_layer);
         --layer) {
        auto greedy_result =
            greedy_search_layer(vector, current_entry, static_cast<uint8_t>(layer));
        if (greedy_result.has_value()) {
            current_entry = greedy_result.value();
        }
    }

    // For each layer from min(new_layer, max_layer) down to 0:
    // search for ef_construction neighbors and connect.
    uint8_t start_layer = std::min(new_layer, static_cast<uint8_t>(meta_.max_layer));
    for (int layer = static_cast<int>(start_layer); layer >= 0; --layer) {
        auto candidates_result = search_layer(
            vector, current_entry, meta_.ef_construction, static_cast<uint8_t>(layer), nullptr);
        if (!candidates_result.has_value()) {
            continue;
        }
        auto& candidates = candidates_result.value();

        // Select best neighbors for this layer.
        uint16_t max_neighbors =
            (layer == 0) ? static_cast<uint16_t>(meta_.m_param * 2) : meta_.m_param;
        auto selected = select_neighbors(candidates, max_neighbors);
        new_node.neighbors[static_cast<size_t>(layer)] = selected;

        // Update entry for the next lower layer.
        if (!candidates.empty()) {
            current_entry = candidates[0].node_id;
        }
    }

    // Store the new node.
    auto node_loc_result = allocate_node(new_node);
    if (!node_loc_result.has_value()) {
        return make_error(node_loc_result.error().code, node_loc_result.error().message);
    }

    // Add bidirectional connections: for each neighbor, add new_id to their
    // neighbor list (if not full, or if closer than their farthest neighbor).
    for (int layer = static_cast<int>(start_layer); layer >= 0; --layer) {
        uint16_t max_neighbors =
            (layer == 0) ? static_cast<uint16_t>(meta_.m_param * 2) : meta_.m_param;
        for (const auto& neighbor : new_node.neighbors[static_cast<size_t>(layer)]) {
            auto neighbor_loc_result = node_location(neighbor.node_id);
            if (!neighbor_loc_result.has_value()) {
                continue;
            }
            auto neighbor_node_result = read_node(neighbor_loc_result.value());
            if (!neighbor_node_result.has_value()) {
                continue;
            }
            auto neighbor_node = std::move(neighbor_node_result.value());

            if (static_cast<size_t>(layer) >= neighbor_node.neighbors.size()) {
                continue;
            }

            auto& nlist = neighbor_node.neighbors[static_cast<size_t>(layer)];

            if (static_cast<uint16_t>(nlist.size()) < max_neighbors) {
                // Room available — just add.
                nlist.push_back({new_id, neighbor.distance});
            } else {
                // Check if new node is closer than the farthest neighbor.
                auto farthest = std::max_element(
                    nlist.begin(), nlist.end(), [](const HnswNeighbor& a, const HnswNeighbor& b) {
                        return a.distance < b.distance;
                    });
                if (farthest != nlist.end() && neighbor.distance < farthest->distance) {
                    *farthest = {new_id, neighbor.distance};
                }
            }

            auto upd = update_node(neighbor_loc_result.value(), neighbor_node);
            if (upd.has_value() && wal_ != nullptr) {
                auto cur_loc = node_location(neighbor.node_id);
                if (cur_loc.has_value()) {
                    auto nbytes = serialize_hnsw_node(neighbor_node);
                    (void)log_wal(WalRecordType::UPDATE,
                                  cur_loc.value().page_id,
                                  cur_loc.value().slot_id,
                                  nbytes);
                }
            }
        }
    }

    // Update metadata if new node has higher layer.
    if (new_layer > meta_.max_layer) {
        meta_.entry_point_id = new_id;
        meta_.max_layer = new_layer;
    }
    meta_.node_count++;

    auto flush_result = flush_meta();
    if (!flush_result.has_value()) {
        return make_error(flush_result.error().code, flush_result.error().message);
    }

    // Log to WAL if available.
    if (wal_ != nullptr) {
        auto node_bytes = serialize_hnsw_node(new_node);
        (void)log_wal(WalRecordType::INSERT,
                      node_loc_result.value().page_id,
                      node_loc_result.value().slot_id,
                      node_bytes);
    }

    return ok(new_id);
}

// -- Search ------------------------------------------------------------------

Result<std::vector<HnswSearchResult>> HnswIndex::search(std::span<const float> query,
                                                        uint32_t k) const {
    return search(query, k, nullptr);
}

Result<std::vector<HnswSearchResult>> HnswIndex::search(
    std::span<const float> query, uint32_t k, const HnswFilterPredicate& predicate) const {
    std::shared_lock<std::shared_mutex> lock(latch_);

    if (query.size() != meta_.dimension) {
        return make_error(StatusCode::INVALID_ARGUMENT, "Query dimension mismatch");
    }
    if (meta_.entry_point_id == hnsw_invalid_node_id) {
        return ok(std::vector<HnswSearchResult>{});
    }

    // Greedy traverse upper layers.
    uint32_t current_entry = meta_.entry_point_id;
    for (int layer = static_cast<int>(meta_.max_layer); layer > 0; --layer) {
        auto greedy_result = greedy_search_layer(query, current_entry, static_cast<uint8_t>(layer));
        if (greedy_result.has_value()) {
            current_entry = greedy_result.value();
        }
    }

    // Beam search at layer 0 with ef_search candidates.
    uint32_t ef = std::max(k, static_cast<uint32_t>(meta_.ef_search));
    auto candidates_result = search_layer(query, current_entry, ef, 0, predicate);
    if (!candidates_result.has_value()) {
        return make_error(candidates_result.error().code, candidates_result.error().message);
    }

    auto& candidates = candidates_result.value();

    // Return top-k results (search_layer already applied the filter).
    size_t count = std::min(static_cast<size_t>(k), candidates.size());
    std::vector<HnswSearchResult> results;
    results.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        results.push_back({candidates[i].node_id, candidates[i].distance});
    }

    return ok(std::move(results));
}

// -- Delete ------------------------------------------------------------------

Result<void> HnswIndex::remove(uint32_t node_id) {
    std::unique_lock<std::shared_mutex> lock(latch_);

    auto loc_result = node_location(node_id);
    if (!loc_result.has_value()) {
        return make_error(StatusCode::NOT_FOUND, "Cannot delete: node not found");
    }
    auto loc = loc_result.value();

    auto node_result = read_node(loc);
    if (!node_result.has_value()) {
        return make_error(node_result.error().code, node_result.error().message);
    }
    auto node = std::move(node_result.value());

    if (node.is_tombstone()) {
        return make_error(StatusCode::NOT_FOUND, "Node is already tombstoned");
    }

    // Mark as tombstoned.
    node.set_tombstone();
    auto update_result = update_node(loc, node);
    if (!update_result.has_value()) {
        return make_error(update_result.error().code, update_result.error().message);
    }

    // Remove this node from all neighbors' neighbor lists.
    for (size_t layer = 0; layer < node.neighbors.size(); ++layer) {
        for (const auto& neighbor : node.neighbors[layer]) {
            if (neighbor.node_id == hnsw_invalid_node_id) {
                continue;
            }
            auto nloc = node_location(neighbor.node_id);
            if (!nloc.has_value()) {
                continue;
            }
            auto nnode_result = read_node(nloc.value());
            if (!nnode_result.has_value()) {
                continue;
            }
            auto nnode = std::move(nnode_result.value());

            if (layer >= nnode.neighbors.size()) {
                continue;
            }

            // Erase references to deleted node.
            auto& nlist = nnode.neighbors[layer];
            nlist.erase(
                std::remove_if(nlist.begin(),
                               nlist.end(),
                               [node_id](const HnswNeighbor& n) { return n.node_id == node_id; }),
                nlist.end());

            auto upd = update_node(nloc.value(), nnode);
            if (upd.has_value() && wal_ != nullptr) {
                auto cur_loc = node_location(neighbor.node_id);
                if (cur_loc.has_value()) {
                    auto nbytes = serialize_hnsw_node(nnode);
                    (void)log_wal(WalRecordType::UPDATE,
                                  cur_loc.value().page_id,
                                  cur_loc.value().slot_id,
                                  nbytes);
                }
            }
        }
    }

    // Update metadata.
    if (meta_.node_count > 0) {
        meta_.node_count--;
    }
    meta_.tombstone_count++;

    // If deleted node was the entry point, pick a new one.
    if (meta_.entry_point_id == node_id) {
        meta_.entry_point_id = hnsw_invalid_node_id;
        // Find any non-tombstoned node to be the new entry point.
        for (const auto& [nid, nloc] : node_map_) {
            if (nid == node_id) {
                continue;
            }
            auto n = read_node(nloc);
            if (n.has_value() && !n.value().is_tombstone()) {
                meta_.entry_point_id = nid;
                meta_.max_layer = n.value().max_layer;
                break;
            }
        }
    }

    auto flush_result = flush_meta();
    if (!flush_result.has_value()) {
        return make_error(flush_result.error().code, flush_result.error().message);
    }

    // Log to WAL.
    if (wal_ != nullptr) {
        auto node_bytes = serialize_hnsw_node(node);
        (void)log_wal(WalRecordType::DELETE, loc.page_id, loc.slot_id, node_bytes);
    }

    return ok();
}

// -- Compaction ---------------------------------------------------------------

Result<void> HnswIndex::compact() {
    std::unique_lock<std::shared_mutex> lock(latch_);

    if (meta_.tombstone_count == 0) {
        return ok();
    }

    // Collect tombstoned node IDs and their data (need vector location).
    std::vector<std::pair<uint32_t, HnswNode>> tombstoned;
    for (const auto& [nid, nloc] : node_map_) {
        auto node = read_node(nloc);
        if (node.has_value() && node.value().is_tombstone()) {
            tombstoned.emplace_back(nid, std::move(node.value()));
        }
    }

    // Delete tombstoned nodes' page tuples AND vector data tuples.
    for (const auto& [nid, node] : tombstoned) {
        auto loc_it = node_map_.find(nid);
        if (loc_it == node_map_.end()) {
            continue;
        }
        auto loc = loc_it->second;

        // Free the node tuple.
        auto fetch = buffer_pool_.fetch_page(loc.page_id);
        if (fetch.has_value()) {
            (void)fetch.value()->delete_tuple(loc.slot_id);
            (void)buffer_pool_.unpin_page(loc.page_id, true);
        }

        // Free the vector data tuple.
        auto vfetch = buffer_pool_.fetch_page(node.vector_page_id);
        if (vfetch.has_value()) {
            (void)vfetch.value()->delete_tuple(node.vector_slot_id);
            (void)buffer_pool_.unpin_page(node.vector_page_id, true);
        }

        node_map_.erase(loc_it);
    }

    meta_.tombstone_count = 0;

    // Rebuild neighbor connections for nodes with depleted neighbor lists.
    if (meta_.entry_point_id != hnsw_invalid_node_id) {
        uint16_t max_l0 = static_cast<uint16_t>(meta_.m_param * 2);
        uint16_t repair_threshold = max_l0 / 2;

        // Collect repair candidates (avoid modifying node_map_ during scan).
        std::vector<std::pair<uint32_t, HnswNodeLocation>> repair_list;
        for (const auto& [nid, nloc] : node_map_) {
            auto nr = read_node(nloc);
            if (!nr.has_value() || nr.value().is_tombstone()) {
                continue;
            }
            if (!nr.value().neighbors.empty() &&
                static_cast<uint16_t>(nr.value().neighbors[0].size()) < repair_threshold) {
                repair_list.emplace_back(nid, nloc);
            }
        }

        for (const auto& [nid, nloc] : repair_list) {
            auto nr = read_node(nloc);
            if (!nr.has_value()) {
                continue;
            }
            auto repair_node = std::move(nr.value());

            auto vec_r = read_node_vector(repair_node);
            if (!vec_r.has_value()) {
                continue;
            }

            auto candidates = search_layer(vec_r.value(), meta_.entry_point_id, max_l0, 0, nullptr);
            if (!candidates.has_value()) {
                continue;
            }

            std::unordered_set<uint32_t> existing;
            for (const auto& n : repair_node.neighbors[0]) {
                existing.insert(n.node_id);
            }
            existing.insert(nid);

            bool updated = false;
            for (const auto& c : candidates.value()) {
                if (existing.count(c.node_id) != 0) {
                    continue;
                }
                if (static_cast<uint16_t>(repair_node.neighbors[0].size()) >= max_l0) {
                    break;
                }
                repair_node.neighbors[0].push_back({c.node_id, c.distance});

                // Add reverse (bidirectional) connection.
                auto cloc = node_location(c.node_id);
                if (cloc.has_value()) {
                    auto cn = read_node(cloc.value());
                    if (cn.has_value() && !cn.value().neighbors.empty() &&
                        static_cast<uint16_t>(cn.value().neighbors[0].size()) < max_l0) {
                        auto cnode = std::move(cn.value());
                        cnode.neighbors[0].push_back({nid, c.distance});
                        (void)update_node(cloc.value(), cnode);
                    }
                }

                updated = true;
            }

            if (updated) {
                (void)update_node(nloc, repair_node);
            }
        }
    }

    auto flush_result = flush_meta();
    if (!flush_result.has_value()) {
        return make_error(flush_result.error().code, flush_result.error().message);
    }

    GIODB_LOG_INFO("HNSW compaction: freed {} tombstoned nodes, "
                   "repaired neighbor connections",
                   tombstoned.size());

    return ok();
}

// -- Page-level operations ---------------------------------------------------

Result<HnswNodeLocation> HnswIndex::allocate_node(const HnswNode& node) {
    auto data = serialize_hnsw_node(node);
    auto page_result = find_page_with_space(PageType::HNSW_NODE, data.size());
    if (!page_result.has_value()) {
        return make_error(page_result.error().code, page_result.error().message);
    }
    PageId page_id = page_result.value();

    auto fetch_result = buffer_pool_.fetch_page(page_id);
    if (!fetch_result.has_value()) {
        return make_error(StatusCode::IO_ERROR, "Failed to fetch HNSW node page");
    }
    Page* page = fetch_result.value();

    auto slot_result = page->insert_tuple(data);
    if (!slot_result.has_value()) {
        (void)buffer_pool_.unpin_page(page_id, false);
        return make_error(StatusCode::INTERNAL_ERROR, "Failed to insert node into HNSW page");
    }
    SlotId slot_id = slot_result.value();

    (void)buffer_pool_.unpin_page(page_id, true);

    HnswNodeLocation loc{page_id, slot_id};
    node_map_[node.node_id] = loc;

    return ok(loc);
}

Result<HnswNode> HnswIndex::read_node(const HnswNodeLocation& loc) const {
    auto fetch_result = buffer_pool_.fetch_page(loc.page_id);
    if (!fetch_result.has_value()) {
        return make_error(StatusCode::IO_ERROR, "Failed to fetch HNSW node page");
    }
    Page* page = fetch_result.value();

    auto tuple_result = page->get_tuple(loc.slot_id);
    if (!tuple_result.has_value()) {
        (void)buffer_pool_.unpin_page(loc.page_id, false);
        return make_error(StatusCode::NOT_FOUND, "HNSW node slot not found");
    }

    auto node_result = deserialize_hnsw_node(tuple_result.value());
    (void)buffer_pool_.unpin_page(loc.page_id, false);

    if (!node_result.has_value()) {
        return make_error(node_result.error().code, node_result.error().message);
    }
    return ok(std::move(node_result.value()));
}

Result<void> HnswIndex::update_node(const HnswNodeLocation& loc, const HnswNode& node) {
    auto data = serialize_hnsw_node(node);
    auto fetch_result = buffer_pool_.fetch_page(loc.page_id);
    if (!fetch_result.has_value()) {
        return make_error(StatusCode::IO_ERROR, "Failed to fetch HNSW node page");
    }
    Page* page = fetch_result.value();

    auto update_result = page->update_tuple(loc.slot_id, data);
    if (!update_result.has_value()) {
        // Tuple grew — need to delete and re-insert on a page with space.
        (void)buffer_pool_.unpin_page(loc.page_id, false);

        // Delete old tuple.
        auto fetch2 = buffer_pool_.fetch_page(loc.page_id);
        if (fetch2.has_value()) {
            (void)fetch2.value()->delete_tuple(loc.slot_id);
            (void)buffer_pool_.unpin_page(loc.page_id, true);
        }

        // Re-allocate on a fresh page.
        auto new_loc_result = allocate_node(node);
        if (!new_loc_result.has_value()) {
            return make_error(StatusCode::INTERNAL_ERROR, "Failed to re-allocate grown HNSW node");
        }
        // node_map_ is updated inside allocate_node.
        return ok();
    }

    (void)buffer_pool_.unpin_page(loc.page_id, true);
    return ok();
}

Result<HnswNodeLocation> HnswIndex::allocate_vector(std::span<const float> vec) {
    auto data = serialize_hnsw_vector(vec);
    auto page_result = find_page_with_space(PageType::HNSW_VECTOR_DATA, data.size());
    if (!page_result.has_value()) {
        return make_error(page_result.error().code, page_result.error().message);
    }
    PageId page_id = page_result.value();

    auto fetch_result = buffer_pool_.fetch_page(page_id);
    if (!fetch_result.has_value()) {
        return make_error(StatusCode::IO_ERROR, "Failed to fetch HNSW vector data page");
    }
    Page* page = fetch_result.value();

    auto slot_result = page->insert_tuple(data);
    if (!slot_result.has_value()) {
        (void)buffer_pool_.unpin_page(page_id, false);
        return make_error(StatusCode::INTERNAL_ERROR, "Failed to insert vector into HNSW page");
    }
    SlotId slot_id = slot_result.value();

    (void)buffer_pool_.unpin_page(page_id, true);

    return ok(HnswNodeLocation{page_id, slot_id});
}

Result<std::vector<float>> HnswIndex::read_vector(PageId page_id, SlotId slot_id) const {
    auto fetch_result = buffer_pool_.fetch_page(page_id);
    if (!fetch_result.has_value()) {
        return make_error(StatusCode::IO_ERROR, "Failed to fetch HNSW vector data page");
    }
    Page* page = fetch_result.value();

    auto tuple_result = page->get_tuple(slot_id);
    if (!tuple_result.has_value()) {
        (void)buffer_pool_.unpin_page(page_id, false);
        return make_error(StatusCode::NOT_FOUND, "HNSW vector slot not found");
    }

    auto vec_result = deserialize_hnsw_vector(tuple_result.value(), meta_.dimension);
    (void)buffer_pool_.unpin_page(page_id, false);

    if (!vec_result.has_value()) {
        return make_error(vec_result.error().code, vec_result.error().message);
    }
    return ok(std::move(vec_result.value()));
}

Result<void> HnswIndex::flush_meta() {
    // Sync page lists into metadata for persistence.
    meta_.node_page_ids = node_pages_;
    meta_.vector_page_ids = vector_pages_;

    auto fetch_result = buffer_pool_.fetch_page(meta_page_id_);
    if (!fetch_result.has_value()) {
        return make_error(StatusCode::IO_ERROR, "Failed to fetch HNSW metadata page for flush");
    }
    Page* meta_page = fetch_result.value();

    auto meta_bytes = serialize_hnsw_meta(meta_);
    auto update_result = meta_page->update_tuple(0, meta_bytes);
    if (!update_result.has_value()) {
        // Meta tuple grew and page is fragmented — compact and retry.
        meta_page->compact();
        update_result = meta_page->update_tuple(0, meta_bytes);
        if (!update_result.has_value()) {
            (void)buffer_pool_.unpin_page(meta_page_id_, false);
            return make_error(StatusCode::INTERNAL_ERROR, "Failed to update HNSW metadata on page");
        }
    }

    (void)buffer_pool_.unpin_page(meta_page_id_, true);

    // WAL log the metadata update.
    if (wal_ != nullptr) {
        (void)log_wal(WalRecordType::UPDATE, meta_page_id_, 0, meta_bytes);
    }

    return ok();
}

Result<HnswNodeLocation> HnswIndex::node_location(uint32_t node_id) const {
    auto it = node_map_.find(node_id);
    if (it == node_map_.end()) {
        return make_error(StatusCode::NOT_FOUND, "HNSW node not found in location map");
    }
    return ok(it->second);
}

// -- Private helpers ---------------------------------------------------------

Result<PageId> HnswIndex::find_page_with_space(PageType type, size_t min_free) {
    const size_t needed = min_free + slot_entry_size;

    auto& page_list = (type == PageType::HNSW_NODE) ? node_pages_ : vector_pages_;

    for (auto it = page_list.rbegin(); it != page_list.rend(); ++it) {
        auto fetch_result = buffer_pool_.fetch_page(*it);
        if (!fetch_result.has_value()) {
            continue;
        }
        Page* page = fetch_result.value();
        if (page->free_space() >= needed) {
            (void)buffer_pool_.unpin_page(*it, false);
            return ok(*it);
        }
        (void)buffer_pool_.unpin_page(*it, false);
    }

    auto new_page_result = buffer_pool_.new_page();
    if (!new_page_result.has_value()) {
        return make_error(StatusCode::INTERNAL_ERROR, "Failed to allocate new HNSW page");
    }
    Page* new_page = new_page_result.value();
    new_page->set_page_type(type);
    PageId new_page_id = new_page->page_id();

    (void)buffer_pool_.unpin_page(new_page_id, true);
    page_list.push_back(new_page_id);

    return ok(new_page_id);
}

uint8_t HnswIndex::random_layer() {
    // HNSW layer assignment: exponential distribution with normalization factor
    // m_L = 1 / ln(M). Higher M = lower probability of higher layers.
    double m_l = 1.0 / std::log(static_cast<double>(meta_.m_param));
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    double r = dist(rng_);
    if (r == 0.0) {
        r = 1e-10; // Avoid log(0) = -inf.
    }
    auto level = static_cast<uint8_t>(std::min(255.0, std::floor(-std::log(r) * m_l)));
    return level;
}

Result<std::vector<float>> HnswIndex::read_node_vector(const HnswNode& node) const {
    return read_vector(node.vector_page_id, node.vector_slot_id);
}

Result<uint32_t> HnswIndex::greedy_search_layer(std::span<const float> query,
                                                uint32_t entry_id,
                                                uint8_t layer) const {
    auto entry_loc_result = node_location(entry_id);
    if (!entry_loc_result.has_value()) {
        return make_error(StatusCode::NOT_FOUND, "Entry point not found");
    }
    auto entry_node_result = read_node(entry_loc_result.value());
    if (!entry_node_result.has_value()) {
        return make_error(entry_node_result.error().code, entry_node_result.error().message);
    }
    auto entry_vec_result = read_node_vector(entry_node_result.value());
    if (!entry_vec_result.has_value()) {
        return make_error(entry_vec_result.error().code, entry_vec_result.error().message);
    }

    uint32_t current_id = entry_id;
    float current_dist = compute_distance(DistanceMetric::L2, query, entry_vec_result.value());

    bool changed = true;
    while (changed) {
        changed = false;
        auto loc_result = node_location(current_id);
        if (!loc_result.has_value()) {
            break;
        }
        auto node_result = read_node(loc_result.value());
        if (!node_result.has_value()) {
            break;
        }
        const auto& node = node_result.value();

        if (node.is_tombstone() || static_cast<size_t>(layer) >= node.neighbors.size()) {
            break;
        }

        for (const auto& neighbor : node.neighbors[layer]) {
            if (neighbor.node_id == hnsw_invalid_node_id) {
                continue;
            }
            auto nloc = node_location(neighbor.node_id);
            if (!nloc.has_value()) {
                continue;
            }
            auto nnode = read_node(nloc.value());
            if (!nnode.has_value() || nnode.value().is_tombstone()) {
                continue;
            }
            auto nvec = read_node_vector(nnode.value());
            if (!nvec.has_value()) {
                continue;
            }
            float dist = compute_distance(DistanceMetric::L2, query, nvec.value());
            if (dist < current_dist) {
                current_id = neighbor.node_id;
                current_dist = dist;
                changed = true;
            }
        }
    }

    return ok(current_id);
}

Result<std::vector<HnswIndex::Candidate>>
HnswIndex::search_layer(std::span<const float> query,
                        uint32_t entry_id,
                        uint32_t ef,
                        uint8_t layer,
                        const HnswFilterPredicate& predicate) const {
    // Min-heap for candidates (closest first).
    auto cmp_min = [](const Candidate& a, const Candidate& b) { return a.distance > b.distance; };
    // Max-heap for result set (farthest first, for pruning).
    auto cmp_max = [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; };

    std::priority_queue<Candidate, std::vector<Candidate>, decltype(cmp_min)> candidates(cmp_min);
    std::priority_queue<Candidate, std::vector<Candidate>, decltype(cmp_max)> result(cmp_max);
    std::unordered_set<uint32_t> visited;

    // Initialize with entry point.
    auto entry_loc = node_location(entry_id);
    if (!entry_loc.has_value()) {
        return ok(std::vector<Candidate>{});
    }
    auto entry_node = read_node(entry_loc.value());
    if (!entry_node.has_value()) {
        return ok(std::vector<Candidate>{});
    }
    auto entry_vec = read_node_vector(entry_node.value());
    if (!entry_vec.has_value()) {
        return ok(std::vector<Candidate>{});
    }

    float entry_dist = compute_distance(DistanceMetric::L2, query, entry_vec.value());
    candidates.push({entry_id, entry_dist});
    result.push({entry_id, entry_dist});
    visited.insert(entry_id);

    while (!candidates.empty()) {
        auto closest = candidates.top();
        candidates.pop();

        // If closest candidate is farther than the farthest in result, stop.
        if (closest.distance > result.top().distance) {
            break;
        }

        // Expand neighbors of closest.
        auto loc = node_location(closest.node_id);
        if (!loc.has_value()) {
            continue;
        }
        auto node = read_node(loc.value());
        if (!node.has_value()) {
            continue;
        }
        const auto& n = node.value();
        if (n.is_tombstone() || static_cast<size_t>(layer) >= n.neighbors.size()) {
            continue;
        }

        for (const auto& neighbor : n.neighbors[layer]) {
            if (neighbor.node_id == hnsw_invalid_node_id) {
                continue;
            }
            if (visited.count(neighbor.node_id) != 0) {
                continue;
            }
            visited.insert(neighbor.node_id);

            auto nloc = node_location(neighbor.node_id);
            if (!nloc.has_value()) {
                continue;
            }
            auto nnode = read_node(nloc.value());
            if (!nnode.has_value() || nnode.value().is_tombstone()) {
                continue;
            }
            auto nvec = read_node_vector(nnode.value());
            if (!nvec.has_value()) {
                continue;
            }

            float dist = compute_distance(DistanceMetric::L2, query, nvec.value());

            if (result.size() < ef || dist < result.top().distance) {
                candidates.push({neighbor.node_id, dist});
                result.push({neighbor.node_id, dist});

                if (result.size() > ef) {
                    result.pop();
                }
            }
        }
    }

    // Extract results sorted by distance (ascending).
    std::vector<Candidate> results;
    results.reserve(result.size());
    while (!result.empty()) {
        results.push_back(result.top());
        result.pop();
    }
    std::sort(results.begin(), results.end(), [](const Candidate& a, const Candidate& b) {
        return a.distance < b.distance;
    });

    // Apply filter if provided.
    if (predicate) {
        std::vector<Candidate> filtered;
        filtered.reserve(results.size());
        for (const auto& c : results) {
            if (predicate(c.node_id)) {
                filtered.push_back(c);
            }
        }
        return ok(std::move(filtered));
    }

    return ok(std::move(results));
}

std::vector<HnswNeighbor> HnswIndex::select_neighbors(const std::vector<Candidate>& candidates,
                                                      uint16_t max_neighbors) {
    std::vector<HnswNeighbor> selected;
    selected.reserve(std::min(static_cast<size_t>(max_neighbors), candidates.size()));
    for (size_t i = 0; i < candidates.size() && selected.size() < max_neighbors; ++i) {
        selected.push_back({candidates[i].node_id, candidates[i].distance});
    }
    return selected;
}

Result<void> HnswIndex::log_wal(WalRecordType type,
                                PageId page_id,
                                SlotId slot_id,
                                std::span<const uint8_t> data) {
    if (wal_ == nullptr) {
        return ok();
    }

    WalRecord record;
    record.type = type;
    record.page_id = page_id;
    record.slot_id = slot_id;
    record.data.assign(data.begin(), data.end());

    auto result = wal_->append(record);
    if (!result.has_value()) {
        GIODB_LOG_WARN("Failed to log HNSW WAL record");
    }

    return ok();
}

} // namespace giodb
