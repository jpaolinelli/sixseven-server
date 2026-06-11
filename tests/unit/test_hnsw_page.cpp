#include "sixseven/common/platform.h"
#include "sixseven/vector/hnsw_index.h"
#include "sixseven/vector/hnsw_page.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

using namespace sixseven;

// =============================================================================
// HNSW Node Serialization Tests
// =============================================================================

TEST(HnswNodeSerialization, EmptyNodeRoundTrip) {
    HnswNode node;
    node.node_id = 42;
    node.max_layer = 0;
    node.flags = 0;
    node.vector_page_id = 10;
    node.vector_slot_id = 3;
    node.neighbors.resize(1); // Layer 0 with no neighbors.

    auto bytes = serialize_hnsw_node(node);
    EXPECT_EQ(bytes.size(), hnsw_node_header_size + 2); // header + 1 layer count

    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());

    const auto& decoded = result.value();
    EXPECT_EQ(decoded.node_id, 42u);
    EXPECT_EQ(decoded.max_layer, 0);
    EXPECT_EQ(decoded.flags, 0);
    EXPECT_EQ(decoded.vector_page_id, 10u);
    EXPECT_EQ(decoded.vector_slot_id, 3);
    ASSERT_EQ(decoded.neighbors.size(), 1u);
    EXPECT_TRUE(decoded.neighbors[0].empty());
}

TEST(HnswNodeSerialization, NodeWithNeighborsRoundTrip) {
    HnswNode node;
    node.node_id = 100;
    node.max_layer = 2;
    node.flags = 0;
    node.vector_page_id = 5;
    node.vector_slot_id = 0;
    node.neighbors.resize(3);

    // Layer 0: 3 neighbors.
    node.neighbors[0] = {{1, 0.5F}, {2, 1.0F}, {3, 1.5F}};
    // Layer 1: 2 neighbors.
    node.neighbors[1] = {{10, 0.25F}, {20, 0.75F}};
    // Layer 2: 1 neighbor.
    node.neighbors[2] = {{50, 0.1F}};

    auto bytes = serialize_hnsw_node(node);
    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());

    const auto& decoded = result.value();
    EXPECT_EQ(decoded.node_id, 100u);
    EXPECT_EQ(decoded.max_layer, 2);
    ASSERT_EQ(decoded.neighbors.size(), 3u);

    // Verify layer 0.
    ASSERT_EQ(decoded.neighbors[0].size(), 3u);
    EXPECT_EQ(decoded.neighbors[0][0].node_id, 1u);
    EXPECT_FLOAT_EQ(decoded.neighbors[0][0].distance, 0.5F);
    EXPECT_EQ(decoded.neighbors[0][1].node_id, 2u);
    EXPECT_FLOAT_EQ(decoded.neighbors[0][1].distance, 1.0F);
    EXPECT_EQ(decoded.neighbors[0][2].node_id, 3u);
    EXPECT_FLOAT_EQ(decoded.neighbors[0][2].distance, 1.5F);

    // Verify layer 1.
    ASSERT_EQ(decoded.neighbors[1].size(), 2u);
    EXPECT_EQ(decoded.neighbors[1][0].node_id, 10u);
    EXPECT_FLOAT_EQ(decoded.neighbors[1][0].distance, 0.25F);

    // Verify layer 2.
    ASSERT_EQ(decoded.neighbors[1].size(), 2u);
    EXPECT_EQ(decoded.neighbors[2][0].node_id, 50u);
    EXPECT_FLOAT_EQ(decoded.neighbors[2][0].distance, 0.1F);
}

TEST(HnswNodeSerialization, TombstoneFlag) {
    HnswNode node;
    node.node_id = 7;
    node.max_layer = 0;
    node.neighbors.resize(1);

    EXPECT_FALSE(node.is_tombstone());
    node.set_tombstone();
    EXPECT_TRUE(node.is_tombstone());
    EXPECT_EQ(node.flags, hnsw_flag_tombstone);

    auto bytes = serialize_hnsw_node(node);
    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().is_tombstone());
}

TEST(HnswNodeSerialization, MaxNeighborsM16) {
    // Test with M=16: layer 0 gets 2*M=32 neighbors, others get M=16.
    HnswNode node;
    node.node_id = 200;
    node.max_layer = 1;
    node.vector_page_id = 1;
    node.vector_slot_id = 0;
    node.neighbors.resize(2);

    // Layer 0: 32 neighbors (2*M).
    for (uint32_t i = 0; i < 32; ++i) {
        node.neighbors[0].push_back({i, static_cast<float>(i) * 0.1F});
    }
    // Layer 1: 16 neighbors (M).
    for (uint32_t i = 0; i < 16; ++i) {
        node.neighbors[1].push_back({i + 100, static_cast<float>(i) * 0.2F});
    }

    auto bytes = serialize_hnsw_node(node);
    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());

    const auto& decoded = result.value();
    ASSERT_EQ(decoded.neighbors[0].size(), 32u);
    ASSERT_EQ(decoded.neighbors[1].size(), 16u);

    // Spot-check last entry in each layer.
    EXPECT_EQ(decoded.neighbors[0][31].node_id, 31u);
    EXPECT_FLOAT_EQ(decoded.neighbors[0][31].distance, 3.1F);
    EXPECT_EQ(decoded.neighbors[1][15].node_id, 115u);
    EXPECT_FLOAT_EQ(decoded.neighbors[1][15].distance, 3.0F);
}

TEST(HnswNodeSerialization, DeserializeTooShortFails) {
    std::vector<uint8_t> short_data(5, 0);
    auto result = deserialize_hnsw_node(short_data);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(HnswNodeSerialization, SerializedSizeCalculation) {
    HnswNode node;
    node.node_id = 1;
    node.max_layer = 1;
    node.neighbors.resize(2);
    node.neighbors[0] = {{10, 0.5F}, {20, 1.0F}};
    node.neighbors[1] = {{30, 0.25F}};

    size_t expected = hnsw_node_header_size + 2 + 2 * 8 // layer 0: count + 2 entries
                      + 2 + 1 * 8;                      // layer 1: count + 1 entry
    EXPECT_EQ(serialized_hnsw_node_size(node), expected);
    EXPECT_EQ(serialize_hnsw_node(node).size(), expected);
}

// =============================================================================
// HNSW Metadata Serialization Tests
// =============================================================================

TEST(HnswMetaSerialization, DefaultMetaRoundTrip) {
    HnswMeta meta;
    meta.entry_point_id = hnsw_invalid_node_id;
    meta.max_layer = 0;
    meta.m_param = 16;
    meta.ef_construction = 200;
    meta.ef_search = 64;
    meta.dimension = 128;
    meta.node_count = 0;
    meta.tombstone_count = 0;
    meta.next_node_id = 0;

    auto bytes = serialize_hnsw_meta(meta);
    // Fixed header (28) + node_page_count (4) + vector_page_count (4) +
    // metric byte (1, GDB-723) = 37.
    EXPECT_EQ(bytes.size(), hnsw_meta_size + 9);

    auto result = deserialize_hnsw_meta(bytes);
    ASSERT_TRUE(result.has_value());

    const auto& decoded = result.value();
    EXPECT_EQ(decoded.entry_point_id, hnsw_invalid_node_id);
    EXPECT_EQ(decoded.max_layer, 0);
    EXPECT_EQ(decoded.m_param, 16);
    EXPECT_EQ(decoded.ef_construction, 200);
    EXPECT_EQ(decoded.ef_search, 64);
    EXPECT_EQ(decoded.dimension, 128u);
    EXPECT_EQ(decoded.node_count, 0u);
    EXPECT_EQ(decoded.tombstone_count, 0u);
    EXPECT_EQ(decoded.next_node_id, 0u);
}

TEST(HnswMetaSerialization, PopulatedMetaRoundTrip) {
    HnswMeta meta;
    meta.entry_point_id = 42;
    meta.max_layer = 5;
    meta.m_param = 32;
    meta.ef_construction = 400;
    meta.ef_search = 128;
    meta.dimension = 384;
    meta.node_count = 10000;
    meta.tombstone_count = 50;
    meta.next_node_id = 10050;
    meta.node_page_ids = {10, 20, 30};
    meta.vector_page_ids = {40, 50};

    auto bytes = serialize_hnsw_meta(meta);
    // 28 + 4 + 3*4 + 4 + 2*4 + 1 (metric byte, GDB-723) = 57.
    EXPECT_EQ(bytes.size(), hnsw_meta_size + 4 + 3 * 4 + 4 + 2 * 4 + 1);

    auto result = deserialize_hnsw_meta(bytes);
    ASSERT_TRUE(result.has_value());

    const auto& decoded = result.value();
    EXPECT_EQ(decoded.entry_point_id, 42u);
    EXPECT_EQ(decoded.max_layer, 5);
    EXPECT_EQ(decoded.m_param, 32);
    EXPECT_EQ(decoded.ef_construction, 400);
    EXPECT_EQ(decoded.ef_search, 128);
    EXPECT_EQ(decoded.dimension, 384u);
    EXPECT_EQ(decoded.node_count, 10000u);
    EXPECT_EQ(decoded.tombstone_count, 50u);
    EXPECT_EQ(decoded.next_node_id, 10050u);
    ASSERT_EQ(decoded.node_page_ids.size(), 3u);
    EXPECT_EQ(decoded.node_page_ids[0], 10u);
    EXPECT_EQ(decoded.node_page_ids[1], 20u);
    EXPECT_EQ(decoded.node_page_ids[2], 30u);
    ASSERT_EQ(decoded.vector_page_ids.size(), 2u);
    EXPECT_EQ(decoded.vector_page_ids[0], 40u);
    EXPECT_EQ(decoded.vector_page_ids[1], 50u);
}

// GDB-723: the distance metric round-trips through meta serialization.
TEST(HnswMetaSerialization, MetricRoundTrip) {
    HnswMeta meta;
    meta.dimension = 8;
    meta.metric = DistanceMetric::COSINE;

    auto result = deserialize_hnsw_meta(serialize_hnsw_meta(meta));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().metric, DistanceMetric::COSINE);

    meta.metric = DistanceMetric::INNER_PRODUCT;
    auto result2 = deserialize_hnsw_meta(serialize_hnsw_meta(meta));
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2.value().metric, DistanceMetric::INNER_PRODUCT);
}

// GDB-723: legacy meta blobs (written before the metric byte existed) end at
// the page lists; they must deserialize with the L2 default, since legacy
// graphs were built with hardcoded L2 distances.
TEST(HnswMetaSerialization, LegacyMetaWithoutMetricDefaultsToL2) {
    HnswMeta meta;
    meta.dimension = 16;
    meta.node_page_ids = {7};
    meta.vector_page_ids = {9};
    meta.metric = DistanceMetric::COSINE;

    auto bytes = serialize_hnsw_meta(meta);
    bytes.pop_back(); // Strip the trailing metric byte => legacy layout.

    auto result = deserialize_hnsw_meta(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().metric, DistanceMetric::L2);
    ASSERT_EQ(result.value().node_page_ids.size(), 1u);
    EXPECT_EQ(result.value().node_page_ids[0], 7u);
}

// GDB-723: an out-of-range metric byte is rejected, not silently mapped.
TEST(HnswMetaSerialization, InvalidMetricByteFails) {
    HnswMeta meta;
    meta.dimension = 16;
    auto bytes = serialize_hnsw_meta(meta);
    bytes.back() = 0xFF;

    auto result = deserialize_hnsw_meta(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(HnswMetaSerialization, DeserializeTooShortFails) {
    std::vector<uint8_t> short_data(10, 0);
    auto result = deserialize_hnsw_meta(short_data);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// HNSW Vector Serialization Tests
// =============================================================================

TEST(HnswVectorSerialization, BasicRoundTrip) {
    std::vector<float> vec = {1.0F, 2.0F, 3.0F, 4.0F};
    auto bytes = serialize_hnsw_vector(vec);
    EXPECT_EQ(bytes.size(), 4 * sizeof(float));

    auto result = deserialize_hnsw_vector(bytes, 4);
    ASSERT_TRUE(result.has_value());

    const auto& decoded = result.value();
    ASSERT_EQ(decoded.size(), 4u);
    EXPECT_FLOAT_EQ(decoded[0], 1.0F);
    EXPECT_FLOAT_EQ(decoded[1], 2.0F);
    EXPECT_FLOAT_EQ(decoded[2], 3.0F);
    EXPECT_FLOAT_EQ(decoded[3], 4.0F);
}

TEST(HnswVectorSerialization, HighDimensionRoundTrip) {
    const uint32_t dim = 384;
    std::vector<float> vec(dim);
    for (uint32_t i = 0; i < dim; ++i) {
        vec[i] = static_cast<float>(i) * 0.01F;
    }

    auto bytes = serialize_hnsw_vector(vec);
    auto result = deserialize_hnsw_vector(bytes, dim);
    ASSERT_TRUE(result.has_value());

    const auto& decoded = result.value();
    ASSERT_EQ(decoded.size(), dim);
    for (uint32_t i = 0; i < dim; ++i) {
        EXPECT_FLOAT_EQ(decoded[i], static_cast<float>(i) * 0.01F);
    }
}

TEST(HnswVectorSerialization, DeserializeTooShortFails) {
    std::vector<uint8_t> short_data(8, 0);                // 2 floats.
    auto result = deserialize_hnsw_vector(short_data, 4); // Expects 4 floats.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// HNSW Page Integration Tests (with buffer pool)
// =============================================================================

namespace {

/// Helper to create a temporary directory for test files.
class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() / "sixseven_hnsw_test_XXXXXX";
        // Create a unique temp directory.
        std::string tmpl = path_.string();
        char* result = sixseven_platform::mkdtemp(tmpl.data());
        EXPECT_NE(result, nullptr);
        path_ = result;
    }

    ~TempDir() { std::filesystem::remove_all(path_); }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

/// Helper to set up a DiskManager + BufferPoolManager for tests.
struct TestFixture {
    TempDir tmp;
    DiskManager disk_manager;
    FileId file_id = 0;
    std::unique_ptr<BufferPoolManager> bpm;

    TestFixture() {
        auto db_path = tmp.path() / "test_hnsw.db";
        auto fid = disk_manager.create_file(db_path);
        EXPECT_TRUE(fid.has_value());
        file_id = fid.value();
        bpm = std::make_unique<BufferPoolManager>(disk_manager, file_id, 64);
    }
};

} // namespace

TEST(HnswPageIntegration, CreateAndLoadIndex) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 128;
    config.m = 16;
    config.ef_construction = 200;
    config.ef_search = 64;

    auto create_result = index.create(config);
    ASSERT_TRUE(create_result.has_value()) << create_result.error().message;

    PageId meta_pid = index.meta_page_id();
    EXPECT_GT(meta_pid, 0u);

    // Verify metadata.
    const auto& meta = index.meta();
    EXPECT_EQ(meta.dimension, 128u);
    EXPECT_EQ(meta.m_param, 16);
    EXPECT_EQ(meta.ef_construction, 200);
    EXPECT_EQ(meta.ef_search, 64);
    EXPECT_EQ(meta.entry_point_id, hnsw_invalid_node_id);
    EXPECT_EQ(meta.node_count, 0u);

    // Load from the same metadata page.
    HnswIndex loaded_index(*fix.bpm, nullptr);
    auto load_result = loaded_index.load(meta_pid);
    ASSERT_TRUE(load_result.has_value()) << load_result.error().message;

    EXPECT_EQ(loaded_index.dimension(), 128u);
    EXPECT_EQ(loaded_index.meta().m_param, 16);
    EXPECT_EQ(loaded_index.meta().ef_construction, 200);
}

TEST(HnswPageIntegration, AllocateAndReadNode) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 4;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    HnswNode node;
    node.node_id = 0;
    node.max_layer = 0;
    node.vector_page_id = 0;
    node.vector_slot_id = 0;
    node.neighbors.resize(1);
    node.neighbors[0] = {{1, 0.5F}, {2, 1.0F}};

    auto alloc_result = index.allocate_node(node);
    ASSERT_TRUE(alloc_result.has_value()) << alloc_result.error().message;
    auto loc = alloc_result.value();

    auto read_result = index.read_node(loc);
    ASSERT_TRUE(read_result.has_value()) << read_result.error().message;

    const auto& read_node = read_result.value();
    EXPECT_EQ(read_node.node_id, 0u);
    ASSERT_EQ(read_node.neighbors[0].size(), 2u);
    EXPECT_EQ(read_node.neighbors[0][0].node_id, 1u);
    EXPECT_FLOAT_EQ(read_node.neighbors[0][0].distance, 0.5F);
}

TEST(HnswPageIntegration, AllocateAndReadVector) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 4;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    std::vector<float> vec = {1.0F, 2.0F, 3.0F, 4.0F};
    auto alloc_result = index.allocate_vector(vec);
    ASSERT_TRUE(alloc_result.has_value()) << alloc_result.error().message;
    auto loc = alloc_result.value();

    auto read_result = index.read_vector(loc.page_id, loc.slot_id);
    ASSERT_TRUE(read_result.has_value()) << read_result.error().message;

    const auto& read_vec = read_result.value();
    ASSERT_EQ(read_vec.size(), 4u);
    EXPECT_FLOAT_EQ(read_vec[0], 1.0F);
    EXPECT_FLOAT_EQ(read_vec[1], 2.0F);
    EXPECT_FLOAT_EQ(read_vec[2], 3.0F);
    EXPECT_FLOAT_EQ(read_vec[3], 4.0F);
}

TEST(HnswPageIntegration, UpdateNodeNeighbors) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 4;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    // Allocate a node with 1 neighbor.
    HnswNode node;
    node.node_id = 0;
    node.max_layer = 0;
    node.neighbors.resize(1);
    node.neighbors[0] = {{1, 0.5F}};

    auto alloc_result = index.allocate_node(node);
    ASSERT_TRUE(alloc_result.has_value()) << alloc_result.error().message;
    auto loc = alloc_result.value();

    // Update to have 3 neighbors.
    node.neighbors[0] = {{1, 0.5F}, {2, 1.0F}, {3, 1.5F}};
    auto update_result = index.update_node(loc, node);
    ASSERT_TRUE(update_result.has_value()) << update_result.error().message;

    // Read back and verify.
    auto read_result = index.read_node(loc);
    ASSERT_TRUE(read_result.has_value()) << read_result.error().message;
    ASSERT_EQ(read_result.value().neighbors[0].size(), 3u);
    EXPECT_EQ(read_result.value().neighbors[0][2].node_id, 3u);
}

TEST(HnswPageIntegration, MultipleNodesPerPage) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 4;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    // Insert multiple small nodes; they should share HNSW_NODE pages.
    std::vector<HnswNodeLocation> locations;
    for (uint32_t i = 0; i < 20; ++i) {
        HnswNode node;
        node.node_id = i;
        node.max_layer = 0;
        node.neighbors.resize(1);
        node.neighbors[0] = {{(i + 1) % 20, static_cast<float>(i) * 0.1F}};

        auto result = index.allocate_node(node);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        locations.push_back(result.value());
    }

    // Verify all nodes are readable.
    for (uint32_t i = 0; i < 20; ++i) {
        auto result = index.read_node(locations[i]);
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(result.value().node_id, i);
    }

    // Verify some share the same page.
    EXPECT_EQ(locations[0].page_id, locations[1].page_id);
}

TEST(HnswPageIntegration, FlushAndReloadMeta) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 256;
    config.m = 32;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    PageId meta_pid = index.meta_page_id();

    // Reload and verify the persisted metadata.
    HnswIndex loaded(*fix.bpm, nullptr);
    auto lr = loaded.load(meta_pid);
    ASSERT_TRUE(lr.has_value()) << lr.error().message;

    EXPECT_EQ(loaded.dimension(), 256u);
    EXPECT_EQ(loaded.meta().m_param, 32);
}

TEST(HnswPageIntegration, CreateWithZeroDimensionFails) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 0;

    auto result = index.create(config);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(HnswPageIntegration, NodeLocationLookup) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 4;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    HnswNode node;
    node.node_id = 42;
    node.max_layer = 0;
    node.neighbors.resize(1);

    auto alloc_result = index.allocate_node(node);
    ASSERT_TRUE(alloc_result.has_value()) << alloc_result.error().message;

    auto loc_result = index.node_location(42);
    ASSERT_TRUE(loc_result.has_value());
    EXPECT_EQ(loc_result.value().page_id, alloc_result.value().page_id);
    EXPECT_EQ(loc_result.value().slot_id, alloc_result.value().slot_id);

    // Non-existent node.
    auto missing = index.node_location(999);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, StatusCode::NOT_FOUND);
}

// =============================================================================
// HNSW Insert and Search Tests (GDB-123)
// =============================================================================

namespace {

/// Create a TestFixture with a larger buffer pool for insert/search tests.
struct LargeTestFixture {
    TempDir tmp;
    DiskManager disk_manager;
    FileId file_id = 0;
    std::unique_ptr<BufferPoolManager> bpm;

    LargeTestFixture() {
        auto db_path = tmp.path() / "test_hnsw_large.db";
        auto fid = disk_manager.create_file(db_path);
        EXPECT_TRUE(fid.has_value());
        file_id = fid.value();
        bpm = std::make_unique<BufferPoolManager>(disk_manager, file_id, 4096);
    }
};

/// Generate a random vector of given dimension.
std::vector<float> random_vector(uint32_t dim, std::mt19937& rng) {
    std::uniform_real_distribution<float> dist(-1.0F, 1.0F);
    std::vector<float> vec(dim);
    for (auto& v : vec) {
        v = dist(rng);
    }
    return vec;
}

/// Compute brute-force k nearest neighbors.
std::vector<std::pair<uint32_t, float>> brute_force_knn(
    const std::vector<std::vector<float>>& dataset, const std::vector<float>& query, uint32_t k) {
    std::vector<std::pair<uint32_t, float>> distances;
    distances.reserve(dataset.size());
    for (uint32_t i = 0; i < dataset.size(); ++i) {
        float dist = 0.0F;
        for (size_t j = 0; j < query.size(); ++j) {
            float diff = query[j] - dataset[i][j];
            dist += diff * diff;
        }
        distances.emplace_back(i, dist);
    }
    std::sort(distances.begin(), distances.end(), [](const auto& a, const auto& b) {
        return a.second < b.second;
    });
    if (distances.size() > k) {
        distances.resize(k);
    }
    return distances;
}

} // namespace

TEST(HnswInsertSearch, InsertSingleVector) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 4;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    std::vector<float> vec = {1.0F, 2.0F, 3.0F, 4.0F};
    auto insert_result = index.insert(vec);
    ASSERT_TRUE(insert_result.has_value()) << insert_result.error().message;

    EXPECT_EQ(insert_result.value(), 0u);
    EXPECT_EQ(index.node_count(), 1u);
    EXPECT_EQ(index.meta().entry_point_id, 0u);
}

TEST(HnswInsertSearch, InsertAndSearchExactMatch) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 4;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    std::vector<float> vec1 = {1.0F, 0.0F, 0.0F, 0.0F};
    std::vector<float> vec2 = {0.0F, 1.0F, 0.0F, 0.0F};
    std::vector<float> vec3 = {0.0F, 0.0F, 1.0F, 0.0F};

    ASSERT_TRUE(index.insert(vec1).has_value());
    ASSERT_TRUE(index.insert(vec2).has_value());
    ASSERT_TRUE(index.insert(vec3).has_value());

    // Search for vec1 — should return node 0 as the closest.
    auto search_result = index.search(vec1, 1);
    ASSERT_TRUE(search_result.has_value()) << search_result.error().message;
    ASSERT_GE(search_result.value().size(), 1u);
    EXPECT_EQ(search_result.value()[0].node_id, 0u);
    EXPECT_FLOAT_EQ(search_result.value()[0].distance, 0.0F);
}

// GDB-723: a COSINE-configured index must rank by cosine distance, not L2.
// Non-unit-norm vectors make the two orderings diverge: [5,0] is the cosine
// best for query [1,0] (cos dist 0) but L2-far (16); [0.9,0.1] is L2-nearest.
TEST(HnswInsertSearch, CosineMetricRanksByCosineNotL2) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    config.metric = DistanceMetric::COSINE;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;
    EXPECT_EQ(index.metric(), DistanceMetric::COSINE);

    ASSERT_TRUE(index.insert(std::vector<float>{0.9F, 0.1F}).has_value()); // node 0
    ASSERT_TRUE(index.insert(std::vector<float>{5.0F, 0.0F}).has_value()); // node 1
    ASSERT_TRUE(index.insert(std::vector<float>{0.0F, 1.0F}).has_value()); // node 2

    std::vector<float> query = {1.0F, 0.0F};
    auto search_result = index.search(query, 3);
    ASSERT_TRUE(search_result.has_value()) << search_result.error().message;
    ASSERT_EQ(search_result.value().size(), 3u);

    // Cosine order: node 1 (dist 0) < node 0 (~0.0062) < node 2 (1.0).
    // Pre-fix L2 order was node 0, node 2, node 1.
    EXPECT_EQ(search_result.value()[0].node_id, 1u);
    EXPECT_EQ(search_result.value()[1].node_id, 0u);
    EXPECT_EQ(search_result.value()[2].node_id, 2u);
    EXPECT_NEAR(search_result.value()[0].distance, 0.0F, 1e-5F);
    EXPECT_NEAR(search_result.value()[2].distance, 1.0F, 1e-5F);
}

// GDB-723: a dot-product index (DOT_PRODUCT normalized to INNER_PRODUCT)
// ranks by negated dot — the largest-dot vector comes first.
TEST(HnswInsertSearch, InnerProductMetricRanksByDot) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    config.metric = DistanceMetric::DOT_PRODUCT;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;
    // DOT_PRODUCT is normalized to the sort-form INNER_PRODUCT.
    EXPECT_EQ(index.metric(), DistanceMetric::INNER_PRODUCT);

    ASSERT_TRUE(index.insert(std::vector<float>{0.9F, 0.0F}).has_value()); // node 0, dot 0.9
    ASSERT_TRUE(index.insert(std::vector<float>{5.0F, 0.0F}).has_value()); // node 1, dot 5.0
    ASSERT_TRUE(index.insert(std::vector<float>{0.0F, 1.0F}).has_value()); // node 2, dot 0.0

    std::vector<float> query = {1.0F, 0.0F};
    auto search_result = index.search(query, 3);
    ASSERT_TRUE(search_result.has_value()) << search_result.error().message;
    ASSERT_EQ(search_result.value().size(), 3u);

    EXPECT_EQ(search_result.value()[0].node_id, 1u);
    EXPECT_EQ(search_result.value()[1].node_id, 0u);
    EXPECT_EQ(search_result.value()[2].node_id, 2u);
    // Distances are negated dot products (lower = more similar).
    EXPECT_NEAR(search_result.value()[0].distance, -5.0F, 1e-5F);
    EXPECT_NEAR(search_result.value()[1].distance, -0.9F, 1e-5F);
    EXPECT_NEAR(search_result.value()[2].distance, 0.0F, 1e-5F);
}

// GDB-723: the metric survives a create → flush → load round trip.
TEST(HnswInsertSearch, MetricPersistsAcrossLoad) {
    TestFixture fix;

    PageId meta_page_id = 0;
    {
        HnswIndex index(*fix.bpm, nullptr);
        HnswIndexConfig config;
        config.dimension = 2;
        config.m = 4;
        config.metric = DistanceMetric::COSINE;
        ASSERT_TRUE(index.create(config).has_value());
        ASSERT_TRUE(index.insert(std::vector<float>{1.0F, 2.0F}).has_value());
        meta_page_id = index.meta_page_id();
    }

    HnswIndex reloaded(*fix.bpm, nullptr);
    auto load = reloaded.load(meta_page_id);
    ASSERT_TRUE(load.has_value()) << load.error().message;
    EXPECT_EQ(reloaded.metric(), DistanceMetric::COSINE);
    EXPECT_EQ(reloaded.node_count(), 1u);
}

TEST(HnswInsertSearch, SearchTopK) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 32;
    config.ef_search = 32;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    // Insert points along a line: (0,0), (1,0), (2,0), ..., (9,0).
    for (int i = 0; i < 10; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        auto ir = index.insert(vec);
        ASSERT_TRUE(ir.has_value()) << ir.error().message;
    }

    // Query at (0.5, 0) — closest should be node 0 (dist=0.25) and 1 (dist=0.25).
    std::vector<float> query = {0.5F, 0.0F};
    auto results = index.search(query, 3);
    ASSERT_TRUE(results.has_value()) << results.error().message;
    ASSERT_GE(results.value().size(), 3u);

    // The two closest should be nodes 0 and 1.
    std::set<uint32_t> top2{results.value()[0].node_id, results.value()[1].node_id};
    EXPECT_TRUE(top2.count(0) > 0);
    EXPECT_TRUE(top2.count(1) > 0);
}

TEST(HnswInsertSearch, SearchEmptyIndex) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 4;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    std::vector<float> query = {1.0F, 2.0F, 3.0F, 4.0F};
    auto results = index.search(query, 5);
    ASSERT_TRUE(results.has_value());
    EXPECT_TRUE(results.value().empty());
}

TEST(HnswInsertSearch, DimensionMismatchFails) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 4;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    // Insert with wrong dimension.
    std::vector<float> wrong_dim = {1.0F, 2.0F};
    auto ir = index.insert(wrong_dim);
    ASSERT_FALSE(ir.has_value());
    EXPECT_EQ(ir.error().code, StatusCode::INVALID_ARGUMENT);

    // Search with wrong dimension.
    auto sr = index.search(wrong_dim, 1);
    ASSERT_FALSE(sr.has_value());
    EXPECT_EQ(sr.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(HnswInsertSearch, FilteredSearch) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 32;
    config.ef_search = 32;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    // Insert 10 points.
    for (int i = 0; i < 10; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    // Search with filter: only even node IDs.
    std::vector<float> query = {0.5F, 0.0F};
    auto results = index.search(query, 3, [](uint32_t id) { return id % 2 == 0; });
    ASSERT_TRUE(results.has_value()) << results.error().message;

    for (const auto& r : results.value()) {
        EXPECT_EQ(r.node_id % 2, 0u) << "Non-even node returned by filter";
    }
}

TEST(HnswInsertSearch, RecallAt10Over1000Vectors) {
    LargeTestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 32;
    config.m = 16;
    config.ef_construction = 200;
    config.ef_search = 200;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    const uint32_t n = 1000;
    const uint32_t dim = 32;
    const uint32_t k = 10;
    const uint32_t num_queries = 50;

    std::mt19937 rng(42); // Fixed seed for reproducibility.

    // Generate and insert dataset.
    std::vector<std::vector<float>> dataset;
    dataset.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        auto vec = random_vector(dim, rng);
        dataset.push_back(vec);
        auto ir = index.insert(vec);
        ASSERT_TRUE(ir.has_value()) << "Insert " << i << " failed: " << ir.error().message;
    }

    EXPECT_EQ(index.node_count(), n);

    // Run queries and measure recall.
    uint32_t total_correct = 0;

    for (uint32_t q = 0; q < num_queries; ++q) {
        auto query = random_vector(dim, rng);

        // Brute-force ground truth.
        auto ground_truth = brute_force_knn(dataset, query, k);
        std::set<uint32_t> gt_set;
        for (const auto& [id, dist] : ground_truth) {
            gt_set.insert(id);
        }

        // HNSW search.
        auto results = index.search(query, k);
        ASSERT_TRUE(results.has_value()) << results.error().message;

        for (const auto& r : results.value()) {
            if (gt_set.count(r.node_id) > 0) {
                total_correct++;
            }
        }
    }

    float recall = static_cast<float>(total_correct) / static_cast<float>(num_queries * k);

    // Ticket requires >95% recall at default ef_search.
    EXPECT_GT(recall, 0.95F) << "Recall@" << k << " = " << recall << " (expected > 0.95)";
}

TEST(HnswInsertSearch, InsertManySmallDimension) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    // Insert 50 vectors in a grid pattern.
    for (int i = 0; i < 50; ++i) {
        std::vector<float> vec = {static_cast<float>(i % 10), static_cast<float>(i / 10)};
        auto ir = index.insert(vec);
        ASSERT_TRUE(ir.has_value()) << "Insert " << i << ": " << ir.error().message;
    }

    EXPECT_EQ(index.node_count(), 50u);

    // Search for (0,0) — should return node 0.
    std::vector<float> query = {0.0F, 0.0F};
    auto results = index.search(query, 1);
    ASSERT_TRUE(results.has_value()) << results.error().message;
    ASSERT_GE(results.value().size(), 1u);
    EXPECT_EQ(results.value()[0].node_id, 0u);
    EXPECT_FLOAT_EQ(results.value()[0].distance, 0.0F);
}

// =============================================================================
// HNSW Delete and Compaction Tests (GDB-124/125)
// =============================================================================

TEST(HnswDelete, DeleteSingleNode) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    // Insert 5 vectors.
    for (int i = 0; i < 5; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }
    EXPECT_EQ(index.node_count(), 5u);

    // Delete node 2.
    auto del = index.remove(2);
    ASSERT_TRUE(del.has_value()) << del.error().message;

    EXPECT_EQ(index.node_count(), 4u);
    EXPECT_EQ(index.meta().tombstone_count, 1u);

    // Search should not return node 2.
    std::vector<float> query = {2.0F, 0.0F};
    auto results = index.search(query, 5);
    ASSERT_TRUE(results.has_value()) << results.error().message;
    for (const auto& r : results.value()) {
        EXPECT_NE(r.node_id, 2u) << "Tombstoned node appeared in results";
    }
}

TEST(HnswDelete, DeleteNonExistentNodeFails) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    auto del = index.remove(999);
    ASSERT_FALSE(del.has_value());
    EXPECT_EQ(del.error().code, StatusCode::NOT_FOUND);
}

TEST(HnswDelete, DeleteEntryPoint) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    // Insert 3 vectors.
    for (int i = 0; i < 3; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    uint32_t entry_point = index.meta().entry_point_id;

    // Delete the entry point.
    auto del = index.remove(entry_point);
    ASSERT_TRUE(del.has_value()) << del.error().message;

    // Entry point should have changed.
    EXPECT_NE(index.meta().entry_point_id, entry_point);
    EXPECT_NE(index.meta().entry_point_id, hnsw_invalid_node_id);

    // Search should still work.
    std::vector<float> query = {1.0F, 0.0F};
    auto results = index.search(query, 2);
    ASSERT_TRUE(results.has_value()) << results.error().message;
    EXPECT_GE(results.value().size(), 1u);
}

TEST(HnswDelete, CompactionFreesTombstones) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    for (int i = 0; i < 10; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    // Delete nodes 0, 2, 4.
    ASSERT_TRUE(index.remove(0).has_value());
    ASSERT_TRUE(index.remove(2).has_value());
    ASSERT_TRUE(index.remove(4).has_value());

    EXPECT_EQ(index.meta().tombstone_count, 3u);
    EXPECT_EQ(index.node_count(), 7u);

    // Run compaction.
    auto compact = index.compact();
    ASSERT_TRUE(compact.has_value()) << compact.error().message;

    EXPECT_EQ(index.meta().tombstone_count, 0u);

    // Deleted nodes should not be findable.
    EXPECT_FALSE(index.node_location(0).has_value());
    EXPECT_FALSE(index.node_location(2).has_value());
    EXPECT_FALSE(index.node_location(4).has_value());

    // Remaining nodes should still be findable.
    EXPECT_TRUE(index.node_location(1).has_value());
    EXPECT_TRUE(index.node_location(3).has_value());
}

TEST(HnswDelete, SearchAfterDelete20Percent) {
    LargeTestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 16;
    config.m = 16;
    config.ef_construction = 200;
    config.ef_search = 200;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    const uint32_t n = 500;
    const uint32_t dim = 16;
    const uint32_t k = 10;
    const uint32_t num_deletes = n / 5; // 20%

    std::mt19937 rng(123);

    // Insert dataset.
    std::vector<std::vector<float>> dataset;
    dataset.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        auto vec = random_vector(dim, rng);
        dataset.push_back(vec);
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    // Delete 20% of nodes (evenly spaced).
    std::set<uint32_t> deleted;
    for (uint32_t i = 0; i < num_deletes; ++i) {
        uint32_t del_id = i * 5; // 0, 5, 10, 15, ...
        auto del = index.remove(del_id);
        ASSERT_TRUE(del.has_value()) << "Delete " << del_id << ": " << del.error().message;
        deleted.insert(del_id);
    }

    EXPECT_EQ(index.node_count(), n - num_deletes);

    // Build live dataset for ground truth.
    std::vector<std::pair<uint32_t, std::vector<float>>> live_data;
    for (uint32_t i = 0; i < n; ++i) {
        if (deleted.count(i) == 0) {
            live_data.emplace_back(i, dataset[i]);
        }
    }

    // Run 20 queries and measure recall against live data.
    uint32_t total_correct = 0;
    const uint32_t num_queries = 20;

    for (uint32_t q = 0; q < num_queries; ++q) {
        auto query = random_vector(dim, rng);

        // Brute-force over live data.
        std::vector<std::pair<uint32_t, float>> bf_results;
        for (const auto& [id, vec] : live_data) {
            float dist = 0.0F;
            for (size_t j = 0; j < dim; ++j) {
                float diff = query[j] - vec[j];
                dist += diff * diff;
            }
            bf_results.emplace_back(id, dist);
        }
        std::sort(bf_results.begin(), bf_results.end(), [](const auto& a, const auto& b) {
            return a.second < b.second;
        });
        std::set<uint32_t> gt_set;
        for (uint32_t i = 0; i < k && i < bf_results.size(); ++i) {
            gt_set.insert(bf_results[i].first);
        }

        auto results = index.search(query, k);
        ASSERT_TRUE(results.has_value()) << results.error().message;

        for (const auto& r : results.value()) {
            EXPECT_EQ(deleted.count(r.node_id), 0u)
                << "Deleted node " << r.node_id << " in search results";
            if (gt_set.count(r.node_id) > 0) {
                total_correct++;
            }
        }
    }

    float recall = static_cast<float>(total_correct) / static_cast<float>(num_queries * k);

    EXPECT_GT(recall, 0.90F) << "Recall@" << k << " after deleting 20% = " << recall
                             << " (expected > 0.90)";
}

TEST(HnswDelete, CompactionOnEmptyTombstones) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    // Compact with nothing to compact.
    auto compact = index.compact();
    ASSERT_TRUE(compact.has_value());
    EXPECT_EQ(index.meta().tombstone_count, 0u);
}

TEST(HnswDelete, DoubleDeleteFails) {
    TestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    std::vector<float> vec = {1.0F, 2.0F};
    ASSERT_TRUE(index.insert(vec).has_value());

    ASSERT_TRUE(index.remove(0).has_value());
    auto del2 = index.remove(0);
    ASSERT_FALSE(del2.has_value());
    EXPECT_EQ(del2.error().code, StatusCode::NOT_FOUND);
}

// =============================================================================
// Persistence Tests (load after insert)
// =============================================================================

TEST(HnswPersistence, LoadAndSearchAfterInsert) {
    LargeTestFixture fix;

    PageId meta_pid = 0;
    const uint32_t dim = 8;
    std::vector<std::vector<float>> dataset;

    // Phase 1: create and populate index.
    {
        HnswIndex index(*fix.bpm, nullptr);
        HnswIndexConfig config;
        config.dimension = dim;
        config.m = 8;
        config.ef_construction = 64;
        config.ef_search = 64;
        auto cr = index.create(config);
        ASSERT_TRUE(cr.has_value()) << cr.error().message;
        meta_pid = index.meta_page_id();

        std::mt19937 rng(99);
        for (int i = 0; i < 100; ++i) {
            auto vec = random_vector(dim, rng);
            dataset.push_back(vec);
            auto ir = index.insert(vec);
            ASSERT_TRUE(ir.has_value()) << ir.error().message;
        }

        EXPECT_EQ(index.node_count(), 100u);
    }

    // Phase 2: load from the same meta page and verify search.
    {
        HnswIndex loaded(*fix.bpm, nullptr);
        auto lr = loaded.load(meta_pid);
        ASSERT_TRUE(lr.has_value()) << lr.error().message;

        EXPECT_EQ(loaded.dimension(), dim);
        EXPECT_EQ(loaded.node_count(), 100u);

        // Search for the first vector — should return node 0 as closest.
        auto results = loaded.search(dataset[0], 1);
        ASSERT_TRUE(results.has_value()) << results.error().message;
        ASSERT_GE(results.value().size(), 1u);
        EXPECT_EQ(results.value()[0].node_id, 0u);
        EXPECT_NEAR(results.value()[0].distance, 0.0F, 1e-5F);

        // Search for the 50th vector.
        auto results2 = loaded.search(dataset[50], 1);
        ASSERT_TRUE(results2.has_value()) << results2.error().message;
        ASSERT_GE(results2.value().size(), 1u);
        EXPECT_EQ(results2.value()[0].node_id, 50u);
    }
}

TEST(HnswPersistence, InsertAfterLoad) {
    LargeTestFixture fix;

    PageId meta_pid = 0;
    const uint32_t dim = 4;

    // Phase 1: create index and insert some vectors.
    {
        HnswIndex index(*fix.bpm, nullptr);
        HnswIndexConfig config;
        config.dimension = dim;
        config.m = 4;
        config.ef_construction = 32;
        config.ef_search = 32;
        auto cr = index.create(config);
        ASSERT_TRUE(cr.has_value()) << cr.error().message;
        meta_pid = index.meta_page_id();

        for (int i = 0; i < 10; ++i) {
            std::vector<float> vec = {static_cast<float>(i), 0.0F, 0.0F, 0.0F};
            ASSERT_TRUE(index.insert(vec).has_value());
        }
    }

    // Phase 2: load and insert more.
    {
        HnswIndex loaded(*fix.bpm, nullptr);
        auto lr = loaded.load(meta_pid);
        ASSERT_TRUE(lr.has_value()) << lr.error().message;
        EXPECT_EQ(loaded.node_count(), 10u);

        // Insert additional vectors.
        for (int i = 10; i < 20; ++i) {
            std::vector<float> vec = {static_cast<float>(i), 0.0F, 0.0F, 0.0F};
            auto ir = loaded.insert(vec);
            ASSERT_TRUE(ir.has_value()) << ir.error().message;
        }

        EXPECT_EQ(loaded.node_count(), 20u);

        // Search for vector 15.
        std::vector<float> query = {15.0F, 0.0F, 0.0F, 0.0F};
        auto results = loaded.search(query, 1);
        ASSERT_TRUE(results.has_value()) << results.error().message;
        ASSERT_GE(results.value().size(), 1u);
        EXPECT_EQ(results.value()[0].node_id, 15u);
    }
}

// =============================================================================
// Concurrency Tests
// =============================================================================

TEST(HnswConcurrency, ConcurrentSearches) {
    LargeTestFixture fix;

    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 4;
    config.m = 8;
    config.ef_construction = 32;
    config.ef_search = 32;
    auto cr = index.create(config);
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    // Insert 50 vectors.
    std::mt19937 rng(77);
    for (int i = 0; i < 50; ++i) {
        auto vec = random_vector(4, rng);
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    // Run concurrent searches from multiple threads.
    constexpr int num_threads = 4;
    constexpr int queries_per_thread = 20;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            std::mt19937 local_rng(t * 1000);
            for (int q = 0; q < queries_per_thread; ++q) {
                auto query = random_vector(4, local_rng);
                auto results = index.search(query, 5);
                if (results.has_value() && !results.value().empty()) {
                    success_count.fetch_add(1);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(success_count.load(), num_threads * queries_per_thread);
}
