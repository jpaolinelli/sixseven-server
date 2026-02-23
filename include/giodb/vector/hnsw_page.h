#pragma once

#include "giodb/common/result.h"

#include <cstdint>
#include <span>
#include <vector>

namespace giodb {

/// Invalid/sentinel node ID (no entry point, no neighbor).
static constexpr uint32_t hnsw_invalid_node_id = 0xFFFFFFFF;

/// Default HNSW parameters.
static constexpr uint16_t hnsw_default_m = 16;
static constexpr uint16_t hnsw_default_ef_construction = 200;
static constexpr uint16_t hnsw_default_ef_search = 64;

/// Node flags.
static constexpr uint8_t hnsw_flag_tombstone = 0x01;

// -- HNSW neighbor entry -----------------------------------------------------

struct HnswNeighbor {
    uint32_t node_id = hnsw_invalid_node_id;
    float distance = 0.0F;
};

// -- HNSW node ---------------------------------------------------------------

/// In-memory representation of an HNSW node, serialized into HNSW_NODE pages.
///
/// Binary format:
///   [node_id:         uint32]  4 bytes
///   [max_layer:       uint8]   1 byte
///   [flags:           uint8]   1 byte (bit 0 = tombstone)
///   [vector_page_id:  uint32]  4 bytes
///   [vector_slot_id:  uint16]  2 bytes
///   Per layer (0..max_layer inclusive):
///     [neighbor_count: uint16]                         2 bytes
///     [entries: (node_id uint32, distance float32) * count]  8 bytes each
struct HnswNode {
    uint32_t node_id = hnsw_invalid_node_id;
    uint8_t max_layer = 0;
    uint8_t flags = 0;
    uint32_t vector_page_id = 0;
    uint16_t vector_slot_id = 0;
    /// neighbors[i] = neighbor list for layer i. Size = max_layer + 1.
    std::vector<std::vector<HnswNeighbor>> neighbors;

    [[nodiscard]] bool is_tombstone() const { return (flags & hnsw_flag_tombstone) != 0; }
    void set_tombstone() { flags |= hnsw_flag_tombstone; }
};

/// Fixed portion of the node binary format (before per-layer data).
static constexpr size_t hnsw_node_header_size = 12; // 4 + 1 + 1 + 4 + 2

// -- HNSW metadata -----------------------------------------------------------

/// In-memory representation of HNSW index metadata, stored in HNSW_META page.
///
/// Binary format (28 bytes fixed):
///   [entry_point_id:  uint32]  4 bytes
///   [max_layer:       uint16]  2 bytes
///   [m_param:         uint16]  2 bytes
///   [ef_construction: uint16]  2 bytes
///   [ef_search:       uint16]  2 bytes
///   [dimension:       uint32]  4 bytes
///   [node_count:      uint32]  4 bytes
///   [tombstone_count: uint32]  4 bytes
///   [next_node_id:    uint32]  4 bytes
struct HnswMeta {
    uint32_t entry_point_id = hnsw_invalid_node_id;
    uint16_t max_layer = 0;
    uint16_t m_param = hnsw_default_m;
    uint16_t ef_construction = hnsw_default_ef_construction;
    uint16_t ef_search = hnsw_default_ef_search;
    uint32_t dimension = 0;
    uint32_t node_count = 0;
    uint32_t tombstone_count = 0;
    uint32_t next_node_id = 0;
    /// Page IDs for node and vector data pages (persisted for restart).
    std::vector<uint32_t> node_page_ids;
    std::vector<uint32_t> vector_page_ids;
};

static constexpr size_t hnsw_meta_size = 28;

// -- Serialization -----------------------------------------------------------

/// Serialize an HnswNode to bytes for storage in an HNSW_NODE page tuple.
[[nodiscard]] std::vector<uint8_t> serialize_hnsw_node(const HnswNode& node);

/// Deserialize an HnswNode from bytes read from an HNSW_NODE page tuple.
[[nodiscard]] Result<HnswNode> deserialize_hnsw_node(std::span<const uint8_t> data);

/// Compute the serialized size of an HnswNode.
[[nodiscard]] size_t serialized_hnsw_node_size(const HnswNode& node);

/// Serialize HnswMeta to bytes for storage in an HNSW_META page tuple.
[[nodiscard]] std::vector<uint8_t> serialize_hnsw_meta(const HnswMeta& meta);

/// Deserialize HnswMeta from bytes read from an HNSW_META page tuple.
[[nodiscard]] Result<HnswMeta> deserialize_hnsw_meta(std::span<const uint8_t> data);

/// Serialize a vector (float array) for storage in an HNSW_VECTOR_DATA page tuple.
[[nodiscard]] std::vector<uint8_t> serialize_hnsw_vector(std::span<const float> vec);

/// Deserialize a vector from bytes read from an HNSW_VECTOR_DATA page tuple.
[[nodiscard]] Result<std::vector<float>> deserialize_hnsw_vector(std::span<const uint8_t> data,
                                                                 uint32_t dimension);

} // namespace giodb
