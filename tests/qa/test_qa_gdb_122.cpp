/// @file test_qa_gdb_122.cpp
/// @brief QA adversarial tests for GDB-122: HNSW page layouts and index
///        metadata serialization.
///
/// Covers boundary values, corrupted/truncated data, extreme values, and
/// round-trip edge cases for HnswNode, HnswMeta, and vector serialization.

#include "sixseven/vector/hnsw_page.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace sixseven;

// ---------------------------------------------------------------------------
// HnswNode serialization edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB_122_Node, HeaderOnlyNode) {
    // Node with max_layer=0 and 1 empty neighbor list.
    HnswNode node;
    node.node_id = 0;
    node.max_layer = 0;
    node.flags = 0;
    node.vector_page_id = 0;
    node.vector_slot_id = 0;
    node.neighbors.resize(1);

    auto bytes = serialize_hnsw_node(node);
    EXPECT_EQ(bytes.size(), hnsw_node_header_size + 2); // header + count(0)

    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->node_id, 0u);
    EXPECT_EQ(result->max_layer, 0);
    ASSERT_EQ(result->neighbors.size(), 1u);
    EXPECT_TRUE(result->neighbors[0].empty());
}

TEST(QA_GDB_122_Node, MaxNodeId) {
    HnswNode node;
    node.node_id = 0xFFFFFFFE; // One less than sentinel
    node.max_layer = 0;
    node.neighbors.resize(1);

    auto bytes = serialize_hnsw_node(node);
    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->node_id, 0xFFFFFFFE);
}

TEST(QA_GDB_122_Node, SentinelNodeId) {
    HnswNode node;
    node.node_id = hnsw_invalid_node_id;
    node.max_layer = 0;
    node.neighbors.resize(1);

    auto bytes = serialize_hnsw_node(node);
    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->node_id, hnsw_invalid_node_id);
}

TEST(QA_GDB_122_Node, MaxVectorPageId) {
    HnswNode node;
    node.node_id = 1;
    node.max_layer = 0;
    node.vector_page_id = 0xFFFFFFFF;
    node.vector_slot_id = 0xFFFF;
    node.neighbors.resize(1);

    auto bytes = serialize_hnsw_node(node);
    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->vector_page_id, 0xFFFFFFFF);
    EXPECT_EQ(result->vector_slot_id, 0xFFFF);
}

TEST(QA_GDB_122_Node, MultiLayerEmptyNeighbors) {
    // Node on layer 3 with all empty neighbor lists.
    HnswNode node;
    node.node_id = 5;
    node.max_layer = 3;
    node.neighbors.resize(4); // layers 0-3

    auto bytes = serialize_hnsw_node(node);
    // Header + 4 layers * 2 bytes (count=0 each) = 12 + 8 = 20
    EXPECT_EQ(bytes.size(), hnsw_node_header_size + 4 * 2);

    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->max_layer, 3);
    ASSERT_EQ(result->neighbors.size(), 4u);
    for (const auto& layer : result->neighbors) {
        EXPECT_TRUE(layer.empty());
    }
}

TEST(QA_GDB_122_Node, TombstonePreservesData) {
    HnswNode node;
    node.node_id = 99;
    node.max_layer = 1;
    node.vector_page_id = 42;
    node.vector_slot_id = 7;
    node.neighbors.resize(2);
    node.neighbors[0] = {{10, 0.5F}};
    node.neighbors[1] = {{20, 0.1F}};
    node.set_tombstone();

    auto bytes = serialize_hnsw_node(node);
    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_tombstone());
    EXPECT_EQ(result->node_id, 99u);
    EXPECT_EQ(result->vector_page_id, 42u);
    ASSERT_EQ(result->neighbors[0].size(), 1u);
    EXPECT_EQ(result->neighbors[0][0].node_id, 10u);
}

TEST(QA_GDB_122_Node, AllFlagsSet) {
    HnswNode node;
    node.node_id = 1;
    node.max_layer = 0;
    node.flags = 0xFF; // All flags set
    node.neighbors.resize(1);

    auto bytes = serialize_hnsw_node(node);
    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->flags, 0xFF);
    EXPECT_TRUE(result->is_tombstone()); // bit 0 set
}

TEST(QA_GDB_122_Node, NeighborWithZeroDistance) {
    HnswNode node;
    node.node_id = 1;
    node.max_layer = 0;
    node.neighbors.resize(1);
    node.neighbors[0] = {{0, 0.0F}};

    auto bytes = serialize_hnsw_node(node);
    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->neighbors[0].size(), 1u);
    EXPECT_FLOAT_EQ(result->neighbors[0][0].distance, 0.0F);
}

TEST(QA_GDB_122_Node, NeighborWithNegativeDistance) {
    // Dot product can produce negative distances.
    HnswNode node;
    node.node_id = 1;
    node.max_layer = 0;
    node.neighbors.resize(1);
    node.neighbors[0] = {{5, -1.5F}};

    auto bytes = serialize_hnsw_node(node);
    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_FLOAT_EQ(result->neighbors[0][0].distance, -1.5F);
}

TEST(QA_GDB_122_Node, NeighborWithInfDistance) {
    HnswNode node;
    node.node_id = 1;
    node.max_layer = 0;
    node.neighbors.resize(1);
    node.neighbors[0] = {{5, std::numeric_limits<float>::infinity()}};

    auto bytes = serialize_hnsw_node(node);
    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(std::isinf(result->neighbors[0][0].distance));
}

TEST(QA_GDB_122_Node, Layer0With2MNeighbors) {
    // Layer 0 supports up to 2*M=32 neighbors when M=16.
    HnswNode node;
    node.node_id = 1;
    node.max_layer = 0;
    node.neighbors.resize(1);
    for (uint32_t i = 0; i < 32; ++i) {
        node.neighbors[0].push_back({i + 100, static_cast<float>(i) * 0.1F});
    }

    auto bytes = serialize_hnsw_node(node);
    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->neighbors[0].size(), 32u);
    EXPECT_EQ(result->neighbors[0][0].node_id, 100u);
    EXPECT_EQ(result->neighbors[0][31].node_id, 131u);
}

TEST(QA_GDB_122_Node, SerializedSizeEmptyNode) {
    HnswNode node;
    node.node_id = 0;
    node.max_layer = 0;
    node.neighbors.resize(1);

    EXPECT_EQ(serialized_hnsw_node_size(node), hnsw_node_header_size + 2);
}

TEST(QA_GDB_122_Node, SerializedSizeMultiLayer) {
    HnswNode node;
    node.node_id = 0;
    node.max_layer = 2;
    node.neighbors.resize(3);
    node.neighbors[0] = {{1, 0.0F}, {2, 0.0F}, {3, 0.0F}};
    node.neighbors[1] = {{4, 0.0F}};
    node.neighbors[2] = {};

    // header + (2+3*8) + (2+1*8) + (2+0*8) = 12 + 26 + 10 + 2 = 50
    EXPECT_EQ(serialized_hnsw_node_size(node), 50u);
}

// -- Truncated/corrupted node data -------------------------------------------

TEST(QA_GDB_122_Node, DeserializeEmptyData) {
    std::vector<uint8_t> empty;
    auto result = deserialize_hnsw_node(empty);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB_122_Node, DeserializeOneByteShort) {
    std::vector<uint8_t> data(hnsw_node_header_size - 1, 0);
    auto result = deserialize_hnsw_node(data);
    ASSERT_FALSE(result.has_value());
}

TEST(QA_GDB_122_Node, DeserializeExactHeaderNoLayers) {
    // Header says max_layer=0 so it expects 1 layer. But data ends right
    // after header — no room for the neighbor count.
    std::vector<uint8_t> data(hnsw_node_header_size, 0);
    data[4] = 0; // max_layer = 0
    auto result = deserialize_hnsw_node(data);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB_122_Node, DeserializeTruncatedNeighborEntries) {
    // Create a valid node, then truncate the neighbor data.
    HnswNode node;
    node.node_id = 1;
    node.max_layer = 0;
    node.neighbors.resize(1);
    node.neighbors[0] = {{10, 0.5F}, {20, 1.0F}};

    auto bytes = serialize_hnsw_node(node);
    // Remove last 4 bytes (partial neighbor entry).
    bytes.resize(bytes.size() - 4);

    auto result = deserialize_hnsw_node(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB_122_Node, DeserializeTruncatedAtLayer2) {
    // Node with max_layer=2, but data truncated during layer 1.
    HnswNode node;
    node.node_id = 1;
    node.max_layer = 2;
    node.neighbors.resize(3);
    node.neighbors[0] = {{1, 0.5F}};
    node.neighbors[1] = {{2, 1.0F}};
    node.neighbors[2] = {{3, 1.5F}};

    auto bytes = serialize_hnsw_node(node);
    // Truncate so only layer 0 data remains.
    // Header(12) + layer0_count(2) + layer0_entry(8) = 22
    bytes.resize(22);

    auto result = deserialize_hnsw_node(bytes);
    ASSERT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// HnswMeta serialization edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB_122_Meta, DefaultValues) {
    HnswMeta meta;
    EXPECT_EQ(meta.entry_point_id, hnsw_invalid_node_id);
    EXPECT_EQ(meta.max_layer, 0);
    EXPECT_EQ(meta.m_param, hnsw_default_m);
    EXPECT_EQ(meta.ef_construction, hnsw_default_ef_construction);
    EXPECT_EQ(meta.ef_search, hnsw_default_ef_search);
    EXPECT_EQ(meta.dimension, 0u);
    EXPECT_EQ(meta.node_count, 0u);
    EXPECT_EQ(meta.tombstone_count, 0u);
    EXPECT_EQ(meta.next_node_id, 0u);
}

TEST(QA_GDB_122_Meta, RoundTripWithEmptyPageLists) {
    HnswMeta meta;
    meta.entry_point_id = 0;
    meta.dimension = 128;
    // No page IDs.

    auto bytes = serialize_hnsw_meta(meta);
    auto result = deserialize_hnsw_meta(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->node_page_ids.empty());
    EXPECT_TRUE(result->vector_page_ids.empty());
}

TEST(QA_GDB_122_Meta, RoundTripMaxValues) {
    HnswMeta meta;
    meta.entry_point_id = 0xFFFFFFFF;
    meta.max_layer = 0xFFFF;
    meta.m_param = 0xFFFF;
    meta.ef_construction = 0xFFFF;
    meta.ef_search = 0xFFFF;
    meta.dimension = 0xFFFFFFFF;
    meta.node_count = 0xFFFFFFFF;
    meta.tombstone_count = 0xFFFFFFFF;
    meta.next_node_id = 0xFFFFFFFF;

    auto bytes = serialize_hnsw_meta(meta);
    auto result = deserialize_hnsw_meta(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->entry_point_id, 0xFFFFFFFF);
    EXPECT_EQ(result->max_layer, 0xFFFF);
    EXPECT_EQ(result->m_param, 0xFFFF);
    EXPECT_EQ(result->ef_construction, 0xFFFF);
    EXPECT_EQ(result->ef_search, 0xFFFF);
    EXPECT_EQ(result->dimension, 0xFFFFFFFF);
    EXPECT_EQ(result->node_count, 0xFFFFFFFF);
    EXPECT_EQ(result->tombstone_count, 0xFFFFFFFF);
    EXPECT_EQ(result->next_node_id, 0xFFFFFFFF);
}

TEST(QA_GDB_122_Meta, LargePageLists) {
    HnswMeta meta;
    meta.dimension = 128;
    for (uint32_t i = 0; i < 100; ++i) {
        meta.node_page_ids.push_back(i * 10);
    }
    for (uint32_t i = 0; i < 50; ++i) {
        meta.vector_page_ids.push_back(i * 20 + 1);
    }

    auto bytes = serialize_hnsw_meta(meta);
    auto result = deserialize_hnsw_meta(bytes);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->node_page_ids.size(), 100u);
    ASSERT_EQ(result->vector_page_ids.size(), 50u);
    EXPECT_EQ(result->node_page_ids[99], 990u);
    EXPECT_EQ(result->vector_page_ids[49], 981u);
}

TEST(QA_GDB_122_Meta, FixedHeaderOnlyNoPageLists) {
    // Deserialize with exactly 28 bytes (no page lists).
    HnswMeta meta;
    meta.dimension = 64;
    meta.m_param = 8;

    auto bytes = serialize_hnsw_meta(meta);
    // Truncate to just the fixed header.
    bytes.resize(hnsw_meta_size);

    auto result = deserialize_hnsw_meta(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->dimension, 64u);
    EXPECT_EQ(result->m_param, 8);
    EXPECT_TRUE(result->node_page_ids.empty());
    EXPECT_TRUE(result->vector_page_ids.empty());
}

TEST(QA_GDB_122_Meta, DeserializeEmptyData) {
    std::vector<uint8_t> empty;
    auto result = deserialize_hnsw_meta(empty);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB_122_Meta, DeserializeTruncatedNodePageList) {
    HnswMeta meta;
    meta.dimension = 128;
    meta.node_page_ids = {1, 2, 3, 4, 5};

    auto bytes = serialize_hnsw_meta(meta);
    // Truncate in the middle of node page list.
    // Fixed header(28) + node_count(4) + partial entries.
    bytes.resize(hnsw_meta_size + 4 + 2 * 4); // Only 2 of 5 entries

    auto result = deserialize_hnsw_meta(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB_122_Meta, DeserializeTruncatedVectorPageList) {
    HnswMeta meta;
    meta.dimension = 128;
    meta.node_page_ids = {1};
    meta.vector_page_ids = {10, 20, 30};

    auto bytes = serialize_hnsw_meta(meta);
    // Truncate in the middle of vector page list.
    // Fixed(28) + node_count(4) + 1*4 + vec_count(4) + 1*4 = 44
    bytes.resize(hnsw_meta_size + 4 + 1 * 4 + 4 + 1 * 4);

    auto result = deserialize_hnsw_meta(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Vector serialization edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB_122_Vector, SingleDimension) {
    std::vector<float> vec = {42.0F};
    auto bytes = serialize_hnsw_vector(vec);
    EXPECT_EQ(bytes.size(), sizeof(float));

    auto result = deserialize_hnsw_vector(bytes, 1);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);
    EXPECT_FLOAT_EQ((*result)[0], 42.0F);
}

TEST(QA_GDB_122_Vector, EmptyVector) {
    std::vector<float> vec;
    auto bytes = serialize_hnsw_vector(vec);
    EXPECT_TRUE(bytes.empty());

    auto result = deserialize_hnsw_vector(bytes, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

TEST(QA_GDB_122_Vector, NegativeValues) {
    std::vector<float> vec = {-1.0F, -0.5F, -0.001F};
    auto bytes = serialize_hnsw_vector(vec);
    auto result = deserialize_hnsw_vector(bytes, 3);
    ASSERT_TRUE(result.has_value());
    EXPECT_FLOAT_EQ((*result)[0], -1.0F);
    EXPECT_FLOAT_EQ((*result)[1], -0.5F);
    EXPECT_FLOAT_EQ((*result)[2], -0.001F);
}

TEST(QA_GDB_122_Vector, SpecialFloatValues) {
    float inf = std::numeric_limits<float>::infinity();
    float neg_inf = -std::numeric_limits<float>::infinity();
    float denorm = std::numeric_limits<float>::denorm_min();
    float max_val = std::numeric_limits<float>::max();

    std::vector<float> vec = {inf, neg_inf, denorm, max_val, 0.0F, -0.0F};
    auto bytes = serialize_hnsw_vector(vec);
    auto result = deserialize_hnsw_vector(bytes, 6);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(std::isinf((*result)[0]) && (*result)[0] > 0);
    EXPECT_TRUE(std::isinf((*result)[1]) && (*result)[1] < 0);
    EXPECT_EQ((*result)[2], denorm);
    EXPECT_FLOAT_EQ((*result)[3], max_val);
    EXPECT_FLOAT_EQ((*result)[4], 0.0F);
}

TEST(QA_GDB_122_Vector, NaNRoundTrip) {
    float nan_val = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> vec = {nan_val};
    auto bytes = serialize_hnsw_vector(vec);
    auto result = deserialize_hnsw_vector(bytes, 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(std::isnan((*result)[0]));
}

TEST(QA_GDB_122_Vector, LargeDimension) {
    const uint32_t dim = 4096;
    std::vector<float> vec(dim);
    for (uint32_t i = 0; i < dim; ++i) {
        vec[i] = static_cast<float>(i) * 0.001F;
    }

    auto bytes = serialize_hnsw_vector(vec);
    EXPECT_EQ(bytes.size(), dim * sizeof(float));

    auto result = deserialize_hnsw_vector(bytes, dim);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), dim);
    EXPECT_FLOAT_EQ((*result)[0], 0.0F);
    EXPECT_FLOAT_EQ((*result)[dim - 1], 4.095F);
}

TEST(QA_GDB_122_Vector, DimensionMismatchTooShort) {
    std::vector<float> vec = {1.0F, 2.0F};
    auto bytes = serialize_hnsw_vector(vec);
    // Try to deserialize as dimension 4 — data is too short.
    auto result = deserialize_hnsw_vector(bytes, 4);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB_122_Vector, DimensionMismatchExtraData) {
    // Extra data beyond the expected dimension is ignored (not an error).
    std::vector<float> vec = {1.0F, 2.0F, 3.0F, 4.0F};
    auto bytes = serialize_hnsw_vector(vec);
    // Deserialize as dimension 2 — should succeed, reading only first 2.
    auto result = deserialize_hnsw_vector(bytes, 2);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2u);
    EXPECT_FLOAT_EQ((*result)[0], 1.0F);
    EXPECT_FLOAT_EQ((*result)[1], 2.0F);
}

// ---------------------------------------------------------------------------
// Constants and defaults verification
// ---------------------------------------------------------------------------

TEST(QA_GDB_122_Constants, DefaultParameters) {
    EXPECT_EQ(hnsw_default_m, 16);
    EXPECT_EQ(hnsw_default_ef_construction, 200);
    EXPECT_EQ(hnsw_default_ef_search, 64);
    EXPECT_EQ(hnsw_invalid_node_id, 0xFFFFFFFF);
    EXPECT_EQ(hnsw_flag_tombstone, 0x01);
    EXPECT_EQ(hnsw_node_header_size, 12u);
    EXPECT_EQ(hnsw_meta_size, 28u);
}

TEST(QA_GDB_122_Constants, DefaultMetaConstructor) {
    HnswMeta meta;
    EXPECT_EQ(meta.m_param, hnsw_default_m);
    EXPECT_EQ(meta.ef_construction, hnsw_default_ef_construction);
    EXPECT_EQ(meta.ef_search, hnsw_default_ef_search);
    EXPECT_EQ(meta.entry_point_id, hnsw_invalid_node_id);
}

// ---------------------------------------------------------------------------
// Stress tests
// ---------------------------------------------------------------------------

TEST(QA_GDB_122_Stress, ManyNodeRoundTrips) {
    // Serialize and deserialize 1000 nodes with varying layers and neighbors.
    for (uint32_t i = 0; i < 1000; ++i) {
        HnswNode node;
        node.node_id = i;
        node.max_layer = static_cast<uint8_t>(i % 5);
        node.vector_page_id = i * 10;
        node.vector_slot_id = static_cast<uint16_t>(i % 100);
        node.neighbors.resize(node.max_layer + 1);
        for (size_t layer = 0; layer <= node.max_layer; ++layer) {
            uint32_t n_neighbors = (layer == 0) ? (i % 10) : (i % 5);
            for (uint32_t j = 0; j < n_neighbors; ++j) {
                node.neighbors[layer].push_back({j + i * 100, static_cast<float>(j) * 0.01F});
            }
        }

        auto bytes = serialize_hnsw_node(node);
        EXPECT_EQ(bytes.size(), serialized_hnsw_node_size(node));

        auto result = deserialize_hnsw_node(bytes);
        ASSERT_TRUE(result.has_value()) << "Failed at node " << i;
        EXPECT_EQ(result->node_id, i);
        EXPECT_EQ(result->max_layer, node.max_layer);
        ASSERT_EQ(result->neighbors.size(), node.neighbors.size());
        for (size_t layer = 0; layer < node.neighbors.size(); ++layer) {
            EXPECT_EQ(result->neighbors[layer].size(), node.neighbors[layer].size());
        }
    }
}

TEST(QA_GDB_122_Stress, ManyMetaRoundTrips) {
    for (uint32_t i = 0; i < 100; ++i) {
        HnswMeta meta;
        meta.entry_point_id = i;
        meta.max_layer = static_cast<uint16_t>(i % 10);
        meta.m_param = static_cast<uint16_t>(8 + i % 56);
        meta.ef_construction = static_cast<uint16_t>(50 + i * 2);
        meta.ef_search = static_cast<uint16_t>(10 + i);
        meta.dimension = 32 + i * 8;
        meta.node_count = i * 100;
        meta.tombstone_count = i;
        meta.next_node_id = i * 100 + i;
        for (uint32_t j = 0; j < i % 20; ++j) {
            meta.node_page_ids.push_back(j + i * 1000);
        }
        for (uint32_t j = 0; j < (i + 1) % 15; ++j) {
            meta.vector_page_ids.push_back(j + i * 2000);
        }

        auto bytes = serialize_hnsw_meta(meta);
        auto result = deserialize_hnsw_meta(bytes);
        ASSERT_TRUE(result.has_value()) << "Failed at meta " << i;
        EXPECT_EQ(result->entry_point_id, meta.entry_point_id);
        EXPECT_EQ(result->dimension, meta.dimension);
        EXPECT_EQ(result->node_page_ids.size(), meta.node_page_ids.size());
        EXPECT_EQ(result->vector_page_ids.size(), meta.vector_page_ids.size());
    }
}
