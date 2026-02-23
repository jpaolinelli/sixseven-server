#pragma once

#include "giodb/common/result.h"
#include "giodb/storage/buffer_pool.h"
#include "giodb/storage/wal.h"
#include "giodb/vector/hnsw_page.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace giodb {

/// Configuration for creating a new HNSW index.
struct HnswIndexConfig {
    uint32_t dimension = 0;
    uint16_t m = hnsw_default_m;
    uint16_t ef_construction = hnsw_default_ef_construction;
    uint16_t ef_search = hnsw_default_ef_search;
};

/// Location of an HNSW node in the buffer pool.
struct HnswNodeLocation {
    PageId page_id = 0;
    SlotId slot_id = 0;
};

/// Result of a k-nearest neighbor search.
struct HnswSearchResult {
    uint32_t node_id = hnsw_invalid_node_id;
    float distance = 0.0F;
};

/// Predicate callback for filtered search. Returns true if the node should
/// be included in results.
using HnswFilterPredicate = std::function<bool(uint32_t node_id)>;

/// Persistent HNSW index backed by the buffer pool and WAL.
///
/// All page reads/writes go through the buffer pool. Structural changes
/// (insert, delete, neighbor updates) are logged to WAL for crash recovery.
///
/// The index maintains an in-memory mapping of node_id → (page_id, slot_id)
/// for fast node lookups. This map is rebuilt on load by scanning HNSW_NODE pages.
class HnswIndex {
public:
    /// Construct an HnswIndex that uses the given buffer pool and WAL writer.
    /// @param buffer_pool Buffer pool for page I/O.
    /// @param wal WAL writer for crash recovery logging (may be nullptr for tests).
    HnswIndex(BufferPoolManager& buffer_pool, WalWriter* wal);

    ~HnswIndex() = default;

    HnswIndex(const HnswIndex&) = delete;
    HnswIndex& operator=(const HnswIndex&) = delete;
    HnswIndex(HnswIndex&&) = delete;
    HnswIndex& operator=(HnswIndex&&) = delete;

    // -- Index lifecycle -------------------------------------------------------

    /// Create a new empty HNSW index. Allocates the metadata page and
    /// initial node/vector data pages.
    [[nodiscard]] Result<void> create(const HnswIndexConfig& config);

    /// Load an existing HNSW index from a metadata page. Rebuilds the
    /// in-memory node location map by scanning all HNSW_NODE pages.
    [[nodiscard]] Result<void> load(PageId meta_page_id);

    // -- Node operations -------------------------------------------------------

    /// Insert a vector into the index. Allocates a node, stores vector data,
    /// and connects to neighbors using the HNSW algorithm.
    /// Returns the assigned node ID.
    [[nodiscard]] Result<uint32_t> insert(std::span<const float> vector);

    /// Search for the k nearest neighbors to the query vector.
    /// Returns results sorted by ascending distance.
    [[nodiscard]] Result<std::vector<HnswSearchResult>> search(std::span<const float> query,
                                                               uint32_t k) const;

    /// Search with a filter predicate. Only nodes where predicate returns
    /// true are included in results.
    [[nodiscard]] Result<std::vector<HnswSearchResult>>
    search(std::span<const float> query, uint32_t k, const HnswFilterPredicate& predicate) const;

    /// Mark a node as deleted (lazy tombstone).
    [[nodiscard]] Result<void> remove(uint32_t node_id);

    /// Run compaction: free tombstoned nodes and rebuild neighbor connections.
    [[nodiscard]] Result<void> compact();

    /// Clear all nodes and vectors, resetting node IDs to start from 0.
    /// Used by REEMBED to rebuild the index from scratch.
    [[nodiscard]] Result<void> reset();

    // -- Accessors -------------------------------------------------------------

    /// Return the metadata page ID (root of the index on disk).
    [[nodiscard]] PageId meta_page_id() const { return meta_page_id_; }

    /// Return a copy of the current index metadata.
    [[nodiscard]] const HnswMeta& meta() const { return meta_; }

    /// Return the number of live (non-tombstoned) nodes.
    [[nodiscard]] uint32_t node_count() const { return meta_.node_count; }

    /// Return the vector dimension.
    [[nodiscard]] uint32_t dimension() const { return meta_.dimension; }

    // -- Page-level operations (used by subtasks 2 & 3) -----------------------

    /// Allocate a new node page slot. Returns the (page_id, slot_id) where
    /// the serialized node data was stored.
    [[nodiscard]] Result<HnswNodeLocation> allocate_node(const HnswNode& node);

    /// Read a node from its storage location.
    [[nodiscard]] Result<HnswNode> read_node(const HnswNodeLocation& loc) const;

    /// Update a node at its storage location.
    [[nodiscard]] Result<void> update_node(const HnswNodeLocation& loc, const HnswNode& node);

    /// Allocate storage for a vector and return its (page_id, slot_id).
    [[nodiscard]] Result<HnswNodeLocation> allocate_vector(std::span<const float> vec);

    /// Read a vector from its storage location.
    [[nodiscard]] Result<std::vector<float>> read_vector(PageId page_id, SlotId slot_id) const;

    /// Persist the current metadata to the metadata page.
    [[nodiscard]] Result<void> flush_meta();

    /// Look up a node's storage location by its ID.
    [[nodiscard]] Result<HnswNodeLocation> node_location(uint32_t node_id) const;

private:
    /// Find or allocate a page of the given type that has at least min_free
    /// bytes of free space.
    [[nodiscard]] Result<PageId> find_page_with_space(PageType type, size_t min_free);

    /// Generate a random layer for a new node using exponential distribution.
    [[nodiscard]] uint8_t random_layer();

    /// Read a node's vector data.
    [[nodiscard]] Result<std::vector<float>> read_node_vector(const HnswNode& node) const;

    /// Greedy search: find the single closest node at the given layer,
    /// starting from entry_id.
    [[nodiscard]] Result<uint32_t>
    greedy_search_layer(std::span<const float> query, uint32_t entry_id, uint8_t layer) const;

    /// Beam search at a specific layer. Returns up to ef candidates sorted
    /// by distance (ascending).
    struct Candidate {
        uint32_t node_id;
        float distance;
        bool operator>(const Candidate& other) const { return distance > other.distance; }
    };

    [[nodiscard]] Result<std::vector<Candidate>>
    search_layer(std::span<const float> query,
                 uint32_t entry_id,
                 uint32_t ef,
                 uint8_t layer,
                 const HnswFilterPredicate& predicate) const;

    /// Select the best M neighbors from a candidate list for a given layer.
    [[nodiscard]] static std::vector<HnswNeighbor>
    select_neighbors(const std::vector<Candidate>& candidates, uint16_t max_neighbors);

    /// Log a WAL record for an HNSW page modification.
    [[nodiscard]] Result<void>
    log_wal(WalRecordType type, PageId page_id, SlotId slot_id, std::span<const uint8_t> data);

    BufferPoolManager& buffer_pool_;
    WalWriter* wal_;

    PageId meta_page_id_ = 0;
    HnswMeta meta_;

    /// Maps node_id → storage location for fast lookups.
    std::unordered_map<uint32_t, HnswNodeLocation> node_map_;

    /// Tracks pages of each type that may have free space.
    std::vector<PageId> node_pages_;
    std::vector<PageId> vector_pages_;

    mutable std::shared_mutex latch_;
    mutable std::mt19937 rng_{std::random_device{}()};
};

} // namespace giodb
