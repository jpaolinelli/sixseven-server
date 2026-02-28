#include "giodb/vector/hnsw_index.h"
#include "giodb/vector/hnsw_page.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <numeric>
#include <random>
#include <set>
#include <thread>
#include <vector>

using namespace giodb;

// =============================================================================
// Test Helpers
// =============================================================================

namespace {

class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() / "giodb_qa_gdb29_XXXXXX";
        std::string tmpl = path_.string();
        char* result = mkdtemp(tmpl.data());
        EXPECT_NE(result, nullptr);
        path_ = result;
    }

    ~TempDir() { std::filesystem::remove_all(path_); }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

struct SmallFixture {
    TempDir tmp;
    DiskManager disk_manager;
    FileId file_id = 0;
    std::unique_ptr<BufferPoolManager> bpm;

    SmallFixture() {
        auto db_path = tmp.path() / "qa_gdb29.db";
        auto fid = disk_manager.create_file(db_path);
        EXPECT_TRUE(fid.has_value());
        file_id = fid.value();
        bpm = std::make_unique<BufferPoolManager>(disk_manager, file_id, 64);
    }
};

struct LargeFixture {
    TempDir tmp;
    DiskManager disk_manager;
    FileId file_id = 0;
    std::unique_ptr<BufferPoolManager> bpm;

    LargeFixture() {
        auto db_path = tmp.path() / "qa_gdb29_large.db";
        auto fid = disk_manager.create_file(db_path);
        EXPECT_TRUE(fid.has_value());
        file_id = fid.value();
        bpm = std::make_unique<BufferPoolManager>(disk_manager, file_id, 8192);
    }
};

std::vector<float> random_vec(uint32_t dim, std::mt19937& rng) {
    std::uniform_real_distribution<float> dist(-1.0F, 1.0F);
    std::vector<float> vec(dim);
    for (auto& v : vec) {
        v = dist(rng);
    }
    return vec;
}

} // namespace

// =============================================================================
// Serialization Edge Cases (GDB-122)
// =============================================================================

TEST(QA_HnswSerialization, NodeWithMaxLayerZero) {
    HnswNode node;
    node.node_id = 0;
    node.max_layer = 0;
    node.flags = 0;
    node.vector_page_id = 1;
    node.vector_slot_id = 0;
    node.neighbors.resize(1);

    auto bytes = serialize_hnsw_node(node);
    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().max_layer, 0);
    EXPECT_EQ(result.value().neighbors.size(), 1u);
}

TEST(QA_HnswSerialization, NodeWithHighLayer) {
    HnswNode node;
    node.node_id = 0;
    node.max_layer = 10;
    node.flags = 0;
    node.vector_page_id = 1;
    node.vector_slot_id = 0;
    node.neighbors.resize(11); // 0..10 inclusive

    // Put one neighbor on each layer.
    for (uint8_t l = 0; l <= 10; ++l) {
        node.neighbors[l].push_back(
            {static_cast<uint32_t>(l + 100), static_cast<float>(l) * 0.1F});
    }

    auto bytes = serialize_hnsw_node(node);
    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().max_layer, 10);
    ASSERT_EQ(result.value().neighbors.size(), 11u);
    for (uint8_t l = 0; l <= 10; ++l) {
        ASSERT_EQ(result.value().neighbors[l].size(), 1u);
        EXPECT_EQ(result.value().neighbors[l][0].node_id, static_cast<uint32_t>(l + 100));
    }
}

TEST(QA_HnswSerialization, NodeWithEmptyNeighborLists) {
    // All layers have empty neighbor lists — valid for a newly inserted node.
    HnswNode node;
    node.node_id = 99;
    node.max_layer = 3;
    node.flags = 0;
    node.vector_page_id = 2;
    node.vector_slot_id = 5;
    node.neighbors.resize(4);
    // All layers empty.

    auto bytes = serialize_hnsw_node(node);
    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().max_layer, 3);
    ASSERT_EQ(result.value().neighbors.size(), 4u);
    for (int l = 0; l < 4; ++l) {
        EXPECT_TRUE(result.value().neighbors[l].empty());
    }
}

TEST(QA_HnswSerialization, DeserializeExactHeaderSizeButNoLayerData) {
    // Data is exactly hnsw_node_header_size (12 bytes), setting max_layer=0
    // means we expect 1 layer of data. But no layer data follows the header.
    std::vector<uint8_t> data(hnsw_node_header_size, 0);
    // max_layer = 0 at offset 4
    data[4] = 0;

    auto result = deserialize_hnsw_node(data);
    // Should fail because layer 0 neighbor count is missing.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_HnswSerialization, DeserializeTruncatedNeighborEntries) {
    // Create valid header + neighbor count but truncated neighbor data.
    std::vector<uint8_t> data(hnsw_node_header_size + 2, 0);
    data[4] = 0; // max_layer = 0
    // neighbor_count = 5 at offset 12
    uint16_t count = 5;
    std::memcpy(data.data() + hnsw_node_header_size, &count, sizeof(count));

    auto result = deserialize_hnsw_node(data);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_HnswSerialization, MetaWithManyPageIds) {
    HnswMeta meta;
    meta.entry_point_id = 0;
    meta.dimension = 64;
    meta.m_param = 16;
    meta.node_count = 100;

    // Add 100 node page IDs and 50 vector page IDs.
    for (uint32_t i = 0; i < 100; ++i) {
        meta.node_page_ids.push_back(i + 10);
    }
    for (uint32_t i = 0; i < 50; ++i) {
        meta.vector_page_ids.push_back(i + 500);
    }

    auto bytes = serialize_hnsw_meta(meta);
    auto result = deserialize_hnsw_meta(bytes);
    ASSERT_TRUE(result.has_value());

    ASSERT_EQ(result.value().node_page_ids.size(), 100u);
    ASSERT_EQ(result.value().vector_page_ids.size(), 50u);
    EXPECT_EQ(result.value().node_page_ids[0], 10u);
    EXPECT_EQ(result.value().node_page_ids[99], 109u);
    EXPECT_EQ(result.value().vector_page_ids[0], 500u);
    EXPECT_EQ(result.value().vector_page_ids[49], 549u);
}

TEST(QA_HnswSerialization, MetaTruncatedNodePageList) {
    // Build a meta with 3 node pages, then truncate the data.
    HnswMeta meta;
    meta.dimension = 4;
    meta.node_page_ids = {10, 20, 30};

    auto bytes = serialize_hnsw_meta(meta);
    // Truncate partway through the node page list.
    bytes.resize(hnsw_meta_size + 4 + 2 * 4); // Only 2 of 3 page IDs.

    auto result = deserialize_hnsw_meta(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_HnswSerialization, VectorDimensionZero) {
    // Empty vector serialization.
    std::vector<float> empty;
    auto bytes = serialize_hnsw_vector(empty);
    EXPECT_TRUE(bytes.empty());

    auto result = deserialize_hnsw_vector(bytes, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().empty());
}

TEST(QA_HnswSerialization, VectorWithNaN) {
    // NaN values should serialize/deserialize without data loss.
    std::vector<float> vec = {1.0F, std::numeric_limits<float>::quiet_NaN(), 3.0F};
    auto bytes = serialize_hnsw_vector(vec);
    auto result = deserialize_hnsw_vector(bytes, 3);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().size(), 3u);
    EXPECT_FLOAT_EQ(result.value()[0], 1.0F);
    EXPECT_TRUE(std::isnan(result.value()[1]));
    EXPECT_FLOAT_EQ(result.value()[2], 3.0F);
}

TEST(QA_HnswSerialization, VectorWithInfinity) {
    std::vector<float> vec = {std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity()};
    auto bytes = serialize_hnsw_vector(vec);
    auto result = deserialize_hnsw_vector(bytes, 2);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().size(), 2u);
    EXPECT_TRUE(std::isinf(result.value()[0]) && result.value()[0] > 0);
    EXPECT_TRUE(std::isinf(result.value()[1]) && result.value()[1] < 0);
}

TEST(QA_HnswSerialization, NodeMaxNodeIdBoundary) {
    // Use max valid uint32 value as node_id.
    HnswNode node;
    node.node_id = 0xFFFFFFFE; // One below sentinel.
    node.max_layer = 0;
    node.neighbors.resize(1);

    auto bytes = serialize_hnsw_node(node);
    auto result = deserialize_hnsw_node(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().node_id, 0xFFFFFFFEu);
}

// =============================================================================
// Index Creation Edge Cases (GDB-122)
// =============================================================================

TEST(QA_HnswCreate, CreateWithMParamZeroFails) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 4;
    config.m = 0;

    auto result = index.create(config);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_HnswCreate, CreateWithDimension1) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 1;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;

    auto result = index.create(config);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(index.dimension(), 1u);

    // Insert and search with 1-D vectors.
    std::vector<float> v1 = {1.0F};
    std::vector<float> v2 = {5.0F};
    std::vector<float> v3 = {10.0F};
    ASSERT_TRUE(index.insert(v1).has_value());
    ASSERT_TRUE(index.insert(v2).has_value());
    ASSERT_TRUE(index.insert(v3).has_value());

    std::vector<float> query = {4.5F};
    auto sr = index.search(query, 1);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    ASSERT_GE(sr.value().size(), 1u);
    EXPECT_EQ(sr.value()[0].node_id, 1u); // 5.0 is closest to 4.5.
}

TEST(QA_HnswCreate, CreateWithMParam1) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 1;
    config.ef_construction = 8;
    config.ef_search = 8;

    auto result = index.create(config);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Insert a few vectors with M=1 (very sparse graph).
    for (int i = 0; i < 5; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    // Search — may have degraded recall with M=1 but should not crash.
    std::vector<float> query = {2.0F, 0.0F};
    auto sr = index.search(query, 3);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    EXPECT_GE(sr.value().size(), 1u);
}

TEST(QA_HnswCreate, CreateWithLargeEfValues) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 4;
    config.m = 4;
    config.ef_construction = 65535;
    config.ef_search = 65535;

    auto result = index.create(config);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(index.meta().ef_construction, 65535);
    EXPECT_EQ(index.meta().ef_search, 65535);
}

// =============================================================================
// Insert Edge Cases (GDB-123)
// =============================================================================

TEST(QA_HnswInsert, InsertIdenticalVectors) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 3;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    // Insert 10 identical vectors.
    std::vector<float> vec = {1.0F, 1.0F, 1.0F};
    for (int i = 0; i < 10; ++i) {
        auto ir = index.insert(vec);
        ASSERT_TRUE(ir.has_value()) << "Insert " << i << ": " << ir.error().message;
        EXPECT_EQ(ir.value(), static_cast<uint32_t>(i));
    }

    EXPECT_EQ(index.node_count(), 10u);

    // Search for the same vector should return all with distance 0.
    auto sr = index.search(vec, 10);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    ASSERT_EQ(sr.value().size(), 10u);
    for (const auto& r : sr.value()) {
        EXPECT_FLOAT_EQ(r.distance, 0.0F);
    }
}

TEST(QA_HnswInsert, InsertEmptyVectorFailsIfDimIsNonZero) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 4;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> empty;
    auto ir = index.insert(empty);
    ASSERT_FALSE(ir.has_value());
    EXPECT_EQ(ir.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_HnswInsert, InsertSingleThenDeleteThenInsert) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    // Insert, delete, insert again.
    std::vector<float> v1 = {1.0F, 0.0F};
    auto ir1 = index.insert(v1);
    ASSERT_TRUE(ir1.has_value());
    EXPECT_EQ(ir1.value(), 0u);

    ASSERT_TRUE(index.remove(0).has_value());
    EXPECT_EQ(index.node_count(), 0u);

    // New insert should get node_id = 1 (not reused).
    std::vector<float> v2 = {2.0F, 0.0F};
    auto ir2 = index.insert(v2);
    ASSERT_TRUE(ir2.has_value());
    EXPECT_EQ(ir2.value(), 1u);
    EXPECT_EQ(index.node_count(), 1u);

    // Search should find only the new node.
    auto sr = index.search(v2, 5);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    ASSERT_GE(sr.value().size(), 1u);
    EXPECT_EQ(sr.value()[0].node_id, 1u);
}

// =============================================================================
// Search Edge Cases (GDB-123)
// =============================================================================

TEST(QA_HnswSearch, SearchWithKZero) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> vec = {1.0F, 0.0F};
    ASSERT_TRUE(index.insert(vec).has_value());

    // k=0 should return empty results.
    auto sr = index.search(vec, 0);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    EXPECT_TRUE(sr.value().empty());
}

TEST(QA_HnswSearch, SearchWithKGreaterThanNodeCount) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    // Insert 3 vectors.
    for (int i = 0; i < 3; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    // Ask for 100 results — should return only 3.
    std::vector<float> query = {0.0F, 0.0F};
    auto sr = index.search(query, 100);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    EXPECT_LE(sr.value().size(), 3u);
    EXPECT_GE(sr.value().size(), 1u);
}

TEST(QA_HnswSearch, SearchResultsAreSortedByDistance) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 8;
    config.ef_construction = 32;
    config.ef_search = 32;
    ASSERT_TRUE(index.create(config).has_value());

    // Insert 20 points along x axis.
    for (int i = 0; i < 20; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    std::vector<float> query = {5.5F, 0.0F};
    auto sr = index.search(query, 10);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;

    // Verify distances are non-decreasing.
    for (size_t i = 1; i < sr.value().size(); ++i) {
        EXPECT_LE(sr.value()[i - 1].distance, sr.value()[i].distance)
            << "Results not sorted at index " << i;
    }
}

TEST(QA_HnswSearch, FilteredSearchRejectsAll) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    for (int i = 0; i < 5; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    // Filter that rejects all nodes.
    std::vector<float> query = {0.0F, 0.0F};
    auto sr = index.search(query, 5, [](uint32_t /*id*/) { return false; });
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    EXPECT_TRUE(sr.value().empty());
}

TEST(QA_HnswSearch, FilteredSearchAcceptsAll) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    for (int i = 0; i < 5; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    // Filter that accepts all nodes — same as unfiltered.
    std::vector<float> query = {2.0F, 0.0F};
    auto filtered = index.search(query, 3, [](uint32_t /*id*/) { return true; });
    auto unfiltered = index.search(query, 3);
    ASSERT_TRUE(filtered.has_value());
    ASSERT_TRUE(unfiltered.has_value());

    ASSERT_EQ(filtered.value().size(), unfiltered.value().size());
    for (size_t i = 0; i < filtered.value().size(); ++i) {
        EXPECT_EQ(filtered.value()[i].node_id, unfiltered.value()[i].node_id);
    }
}

TEST(QA_HnswSearch, SearchAfterAllNodesDeleted) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    // Insert and delete all nodes.
    for (int i = 0; i < 3; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }
    for (uint32_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(index.remove(i).has_value());
    }

    EXPECT_EQ(index.node_count(), 0u);

    // Search on empty index should return empty results.
    std::vector<float> query = {0.0F, 0.0F};
    auto sr = index.search(query, 5);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    EXPECT_TRUE(sr.value().empty());
}

// =============================================================================
// Delete Edge Cases (GDB-124)
// =============================================================================

TEST(QA_HnswDelete, DeleteOnlyNode) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> vec = {1.0F, 2.0F};
    ASSERT_TRUE(index.insert(vec).has_value());
    EXPECT_EQ(index.node_count(), 1u);

    // Delete the only node (which is also the entry point).
    ASSERT_TRUE(index.remove(0).has_value());
    EXPECT_EQ(index.node_count(), 0u);
    EXPECT_EQ(index.meta().entry_point_id, hnsw_invalid_node_id);

    // Search should return empty.
    auto sr = index.search(vec, 5);
    ASSERT_TRUE(sr.has_value());
    EXPECT_TRUE(sr.value().empty());
}

TEST(QA_HnswDelete, DeleteAllButOneNode) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    for (int i = 0; i < 5; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    // Delete all but node 2.
    for (uint32_t i = 0; i < 5; ++i) {
        if (i != 2) {
            ASSERT_TRUE(index.remove(i).has_value()) << "Delete node " << i;
        }
    }

    EXPECT_EQ(index.node_count(), 1u);
    EXPECT_NE(index.meta().entry_point_id, hnsw_invalid_node_id);

    // Search should return the only remaining node.
    std::vector<float> query = {10.0F, 0.0F};
    auto sr = index.search(query, 5);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    ASSERT_EQ(sr.value().size(), 1u);
    EXPECT_EQ(sr.value()[0].node_id, 2u);
}

TEST(QA_HnswDelete, DeleteAndReinsertManyRounds) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    // 5 rounds of: insert 5 nodes, delete 3, compact.
    uint32_t next_id = 0;
    for (int round = 0; round < 5; ++round) {
        // Insert 5.
        for (int i = 0; i < 5; ++i) {
            std::vector<float> vec = {static_cast<float>(next_id), 0.0F};
            auto ir = index.insert(vec);
            ASSERT_TRUE(ir.has_value()) << "Round " << round << " insert " << i << ": "
                                        << ir.error().message;
            EXPECT_EQ(ir.value(), next_id);
            next_id++;
        }

        // Delete the first 3 of the 5 just inserted.
        for (uint32_t i = next_id - 5; i < next_id - 2; ++i) {
            auto dr = index.remove(i);
            ASSERT_TRUE(dr.has_value()) << "Round " << round << " delete " << i << ": "
                                        << dr.error().message;
        }

        // Compact.
        auto cr = index.compact();
        ASSERT_TRUE(cr.has_value()) << "Round " << round << " compact: " << cr.error().message;
    }

    // Should have 2 live nodes per round = 10 total.
    EXPECT_EQ(index.node_count(), 10u);
    EXPECT_EQ(index.meta().tombstone_count, 0u);
}

TEST(QA_HnswDelete, CompactAfterDeletingEntryPoint) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    for (int i = 0; i < 10; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    uint32_t entry = index.meta().entry_point_id;
    ASSERT_TRUE(index.remove(entry).has_value());

    // Compact should succeed without crashing.
    auto cr = index.compact();
    ASSERT_TRUE(cr.has_value()) << cr.error().message;
    EXPECT_EQ(index.meta().tombstone_count, 0u);

    // Search should still work.
    std::vector<float> query = {5.0F, 0.0F};
    auto sr = index.search(query, 3);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    EXPECT_GE(sr.value().size(), 1u);
}

TEST(QA_HnswDelete, DeleteAllThenCompactThenInsert) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    // Insert 5.
    for (int i = 0; i < 5; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    // Delete all.
    for (uint32_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(index.remove(i).has_value());
    }

    // Compact.
    auto cr = index.compact();
    ASSERT_TRUE(cr.has_value()) << cr.error().message;
    EXPECT_EQ(index.node_count(), 0u);
    EXPECT_EQ(index.meta().tombstone_count, 0u);

    // Insert new nodes — should work on an effectively empty index.
    for (int i = 0; i < 3; ++i) {
        std::vector<float> vec = {static_cast<float>(i) * 10.0F, 0.0F};
        auto ir = index.insert(vec);
        ASSERT_TRUE(ir.has_value()) << "Re-insert " << i << ": " << ir.error().message;
    }

    EXPECT_EQ(index.node_count(), 3u);

    // Search should find the new nodes.
    std::vector<float> query = {0.0F, 0.0F};
    auto sr = index.search(query, 1);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    ASSERT_GE(sr.value().size(), 1u);
}

// =============================================================================
// Persistence Edge Cases (GDB-122 + GDB-123)
// =============================================================================

TEST(QA_HnswPersistence, LoadNonMetaPageFails) {
    SmallFixture fix;

    // Create a non-HNSW page.
    auto page_result = fix.bpm->new_page();
    ASSERT_TRUE(page_result.has_value());
    Page* page = page_result.value();
    PageId pid = page->page_id();
    page->set_page_type(PageType::DATA);
    (void)fix.bpm->unpin_page(pid, true);

    HnswIndex index(*fix.bpm, nullptr);
    auto lr = index.load(pid);
    ASSERT_FALSE(lr.has_value());
    EXPECT_EQ(lr.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_HnswPersistence, PersistAfterDeleteAndCompact) {
    LargeFixture fix;
    PageId meta_pid = 0;
    const uint32_t dim = 4;

    // Phase 1: create, insert, delete, compact.
    {
        HnswIndex index(*fix.bpm, nullptr);
        HnswIndexConfig config;
        config.dimension = dim;
        config.m = 4;
        config.ef_construction = 32;
        config.ef_search = 32;
        ASSERT_TRUE(index.create(config).has_value());
        meta_pid = index.meta_page_id();

        for (int i = 0; i < 20; ++i) {
            std::vector<float> vec = {static_cast<float>(i), 0.0F, 0.0F, 0.0F};
            ASSERT_TRUE(index.insert(vec).has_value());
        }

        // Delete even nodes.
        for (uint32_t i = 0; i < 20; i += 2) {
            ASSERT_TRUE(index.remove(i).has_value());
        }

        ASSERT_TRUE(index.compact().has_value());
    }

    // Phase 2: load and verify.
    {
        HnswIndex loaded(*fix.bpm, nullptr);
        ASSERT_TRUE(loaded.load(meta_pid).has_value());

        EXPECT_EQ(loaded.node_count(), 10u);
        EXPECT_EQ(loaded.meta().tombstone_count, 0u);

        // Only odd nodes should be searchable.
        std::vector<float> query = {1.0F, 0.0F, 0.0F, 0.0F};
        auto sr = loaded.search(query, 5);
        ASSERT_TRUE(sr.has_value()) << sr.error().message;

        for (const auto& r : sr.value()) {
            EXPECT_EQ(r.node_id % 2, 1u) << "Even node " << r.node_id << " found after compact";
        }
    }
}

TEST(QA_HnswPersistence, ResetAndVerifyCleanState) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    // Insert some nodes.
    for (int i = 0; i < 10; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }
    EXPECT_EQ(index.node_count(), 10u);

    // Reset.
    auto rr = index.reset();
    ASSERT_TRUE(rr.has_value()) << rr.error().message;

    EXPECT_EQ(index.node_count(), 0u);
    EXPECT_EQ(index.meta().entry_point_id, hnsw_invalid_node_id);
    EXPECT_EQ(index.meta().tombstone_count, 0u);
    EXPECT_EQ(index.meta().next_node_id, 0u);

    // Search should be empty.
    std::vector<float> query = {0.0F, 0.0F};
    auto sr = index.search(query, 5);
    ASSERT_TRUE(sr.has_value());
    EXPECT_TRUE(sr.value().empty());

    // Insert after reset should start from ID 0.
    std::vector<float> v1 = {1.0F, 0.0F};
    auto ir = index.insert(v1);
    ASSERT_TRUE(ir.has_value()) << ir.error().message;
    EXPECT_EQ(ir.value(), 0u);
}

// =============================================================================
// Entry Point Management (GDB-123/124)
// =============================================================================

TEST(QA_HnswEntryPoint, EntryPointUpdatedOnHigherLayerInsert) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    // First insert is always entry point.
    std::vector<float> v1 = {1.0F, 0.0F};
    auto ir = index.insert(v1);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(index.meta().entry_point_id, 0u);

    // The entry point should be a valid, non-tombstoned node.
    auto loc = index.node_location(index.meta().entry_point_id);
    ASSERT_TRUE(loc.has_value());
    auto node = index.read_node(loc.value());
    ASSERT_TRUE(node.has_value());
    EXPECT_FALSE(node.value().is_tombstone());
}

TEST(QA_HnswEntryPoint, DeleteEntryPointSetsValidReplacement) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    // Insert enough nodes to have a non-trivial graph.
    for (int i = 0; i < 20; ++i) {
        std::vector<float> vec = {static_cast<float>(i), static_cast<float>(i % 3)};
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    uint32_t old_entry = index.meta().entry_point_id;
    ASSERT_TRUE(index.remove(old_entry).has_value());

    uint32_t new_entry = index.meta().entry_point_id;
    EXPECT_NE(new_entry, old_entry);
    EXPECT_NE(new_entry, hnsw_invalid_node_id);

    // New entry should be a valid, non-tombstoned node.
    auto loc = index.node_location(new_entry);
    ASSERT_TRUE(loc.has_value());
    auto node = index.read_node(loc.value());
    ASSERT_TRUE(node.has_value());
    EXPECT_FALSE(node.value().is_tombstone());
}

// =============================================================================
// Stress Tests
// =============================================================================

TEST(QA_HnswStress, InsertSearchDelete500Vectors) {
    LargeFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 8;
    config.m = 8;
    config.ef_construction = 64;
    config.ef_search = 64;
    ASSERT_TRUE(index.create(config).has_value());

    const uint32_t n = 500;
    std::mt19937 rng(42);

    // Insert.
    std::vector<std::vector<float>> dataset;
    for (uint32_t i = 0; i < n; ++i) {
        auto vec = random_vec(8, rng);
        dataset.push_back(vec);
        auto ir = index.insert(vec);
        ASSERT_TRUE(ir.has_value()) << "Insert " << i << ": " << ir.error().message;
    }
    EXPECT_EQ(index.node_count(), n);

    // Search for all inserted vectors — each should be its own nearest neighbor.
    uint32_t self_match_count = 0;
    for (uint32_t i = 0; i < n; ++i) {
        auto sr = index.search(dataset[i], 1);
        ASSERT_TRUE(sr.has_value()) << sr.error().message;
        if (!sr.value().empty() && sr.value()[0].node_id == i) {
            self_match_count++;
        }
    }
    // High self-match rate expected (should be near 100%).
    EXPECT_GT(self_match_count, n * 95 / 100) << "Self-match rate too low: " << self_match_count
                                               << "/" << n;

    // Delete 25% of nodes.
    const uint32_t num_deletes = n / 4;
    for (uint32_t i = 0; i < num_deletes; ++i) {
        ASSERT_TRUE(index.remove(i).has_value());
    }
    EXPECT_EQ(index.node_count(), n - num_deletes);

    // Compact.
    auto cr = index.compact();
    ASSERT_TRUE(cr.has_value()) << cr.error().message;
    EXPECT_EQ(index.meta().tombstone_count, 0u);

    // Search after compact — deleted nodes should not appear.
    for (uint32_t i = num_deletes; i < n; ++i) {
        auto sr = index.search(dataset[i], 1);
        ASSERT_TRUE(sr.has_value()) << sr.error().message;
        for (const auto& r : sr.value()) {
            EXPECT_GE(r.node_id, num_deletes)
                << "Deleted node " << r.node_id << " in results for query " << i;
        }
    }
}

TEST(QA_HnswStress, ConcurrentSearchesDuringHeavyLoad) {
    LargeFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 4;
    config.m = 8;
    config.ef_construction = 32;
    config.ef_search = 32;
    ASSERT_TRUE(index.create(config).has_value());

    // Insert 100 vectors.
    std::mt19937 rng(99);
    for (int i = 0; i < 100; ++i) {
        auto vec = random_vec(4, rng);
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    // Run 8 concurrent search threads.
    constexpr int num_threads = 8;
    constexpr int queries_per_thread = 50;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    std::atomic<int> error_count{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            std::mt19937 local_rng(t * 7777);
            for (int q = 0; q < queries_per_thread; ++q) {
                auto query = random_vec(4, local_rng);
                auto results = index.search(query, 5);
                if (results.has_value() && !results.value().empty()) {
                    success_count.fetch_add(1);
                } else {
                    error_count.fetch_add(1);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(success_count.load(), num_threads * queries_per_thread);
    EXPECT_EQ(error_count.load(), 0);
}

// =============================================================================
// Acceptance Criteria Verification
// =============================================================================

TEST(QA_HnswAC, PersistsAcrossRestart) {
    // AC: HNSW index persists across server restarts.
    LargeFixture fix;
    PageId meta_pid = 0;

    // Create and populate.
    {
        HnswIndex index(*fix.bpm, nullptr);
        HnswIndexConfig config;
        config.dimension = 4;
        config.m = 8;
        config.ef_construction = 32;
        config.ef_search = 32;
        ASSERT_TRUE(index.create(config).has_value());
        meta_pid = index.meta_page_id();

        for (int i = 0; i < 50; ++i) {
            std::vector<float> vec = {static_cast<float>(i), static_cast<float>(i % 5),
                                      static_cast<float>(i % 7), 0.0F};
            ASSERT_TRUE(index.insert(vec).has_value());
        }
        EXPECT_EQ(index.node_count(), 50u);
    }

    // "Restart": load from disk.
    {
        HnswIndex loaded(*fix.bpm, nullptr);
        ASSERT_TRUE(loaded.load(meta_pid).has_value());
        EXPECT_EQ(loaded.node_count(), 50u);
        EXPECT_EQ(loaded.dimension(), 4u);
        EXPECT_EQ(loaded.meta().m_param, 8);

        // Verify search still works.
        std::vector<float> query = {25.0F, 0.0F, 4.0F, 0.0F};
        auto sr = loaded.search(query, 1);
        ASSERT_TRUE(sr.has_value()) << sr.error().message;
        EXPECT_GE(sr.value().size(), 1u);
    }
}

TEST(QA_HnswAC, LazyDeleteWithCompactionReclaimsSpace) {
    // AC: Lazy delete with compaction reclaims space.
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    for (int i = 0; i < 10; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    // Delete 5 nodes.
    for (uint32_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(index.remove(i).has_value());
    }
    EXPECT_EQ(index.meta().tombstone_count, 5u);

    // Tombstoned nodes still exist in the location map pre-compact.
    for (uint32_t i = 0; i < 5; ++i) {
        EXPECT_TRUE(index.node_location(i).has_value())
            << "Node " << i << " should still be in map before compact";
    }

    // Compact.
    ASSERT_TRUE(index.compact().has_value());
    EXPECT_EQ(index.meta().tombstone_count, 0u);

    // Tombstoned nodes removed from location map post-compact.
    for (uint32_t i = 0; i < 5; ++i) {
        EXPECT_FALSE(index.node_location(i).has_value())
            << "Node " << i << " should be removed after compact";
    }

    // Live nodes still accessible.
    for (uint32_t i = 5; i < 10; ++i) {
        EXPECT_TRUE(index.node_location(i).has_value())
            << "Live node " << i << " should still be accessible";
    }
}

TEST(QA_HnswAC, UnitTestsForInsertSearchDeletePersistenceRecovery) {
    // AC: Unit tests for insert, search, delete, persistence, recovery.
    // This test verifies a complete lifecycle: create -> insert -> search ->
    // delete -> persist -> load -> search -> compact -> search.
    LargeFixture fix;
    PageId meta_pid = 0;
    const uint32_t dim = 4;

    // Phase 1: Create, insert, search, delete.
    {
        HnswIndex index(*fix.bpm, nullptr);
        HnswIndexConfig config;
        config.dimension = dim;
        config.m = 8;
        config.ef_construction = 32;
        config.ef_search = 32;
        ASSERT_TRUE(index.create(config).has_value());
        meta_pid = index.meta_page_id();

        // Insert.
        for (int i = 0; i < 30; ++i) {
            std::vector<float> vec = {static_cast<float>(i), 0.0F, 0.0F, 0.0F};
            ASSERT_TRUE(index.insert(vec).has_value());
        }

        // Search.
        std::vector<float> query = {15.0F, 0.0F, 0.0F, 0.0F};
        auto sr = index.search(query, 1);
        ASSERT_TRUE(sr.has_value());
        EXPECT_EQ(sr.value()[0].node_id, 15u);

        // Delete.
        ASSERT_TRUE(index.remove(15).has_value());
        EXPECT_EQ(index.node_count(), 29u);
    }

    // Phase 2: Load, search (deleted node absent), compact, search.
    {
        HnswIndex loaded(*fix.bpm, nullptr);
        ASSERT_TRUE(loaded.load(meta_pid).has_value());
        EXPECT_EQ(loaded.node_count(), 29u);

        // Deleted node 15 should not be returned.
        std::vector<float> query = {15.0F, 0.0F, 0.0F, 0.0F};
        auto sr = loaded.search(query, 3);
        ASSERT_TRUE(sr.has_value());
        for (const auto& r : sr.value()) {
            EXPECT_NE(r.node_id, 15u);
        }

        // Compact and re-search.
        ASSERT_TRUE(loaded.compact().has_value());
        auto sr2 = loaded.search(query, 3);
        ASSERT_TRUE(sr2.has_value());
        EXPECT_GE(sr2.value().size(), 1u);
    }
}
