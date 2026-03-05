/// @file test_qa_gdb_123.cpp
/// @brief QA adversarial tests for GDB-123: Persistent HNSW insert and search.
///
/// Targets edge cases not covered by the existing test_hnsw_page.cpp tests:
/// search with k=0, k > dataset, duplicate vectors, M=0, identical vectors,
/// insert-after-reset, and filtered search edge cases.

#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/hnsw_index.h"
#include "sixseven/vector/hnsw_page.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <numeric>
#include <random>
#include <set>
#include <vector>

using namespace sixseven;

namespace {

class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() / "sixseven_qa123_XXXXXX";
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

struct Fixture {
    TempDir tmp;
    DiskManager disk_manager;
    FileId file_id = 0;
    std::unique_ptr<BufferPoolManager> bpm;

    explicit Fixture(size_t pool_size = 128) {
        auto db_path = tmp.path() / "test_hnsw_qa.db";
        auto fid = disk_manager.create_file(db_path);
        EXPECT_TRUE(fid.has_value());
        file_id = fid.value();
        bpm = std::make_unique<BufferPoolManager>(disk_manager, file_id, pool_size);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Search edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB_123_Search, SearchKZero) {
    Fixture fix;
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> vec = {1.0F, 0.0F};
    ASSERT_TRUE(index.insert(vec).has_value());

    auto results = index.search(vec, 0);
    ASSERT_TRUE(results.has_value());
    EXPECT_TRUE(results->empty());
}

TEST(QA_GDB_123_Search, SearchKLargerThanDataset) {
    Fixture fix;
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    for (int i = 0; i < 3; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    // Ask for 100 results when only 3 exist.
    std::vector<float> query = {1.0F, 0.0F};
    auto results = index.search(query, 100);
    ASSERT_TRUE(results.has_value());
    EXPECT_LE(results->size(), 3u);
    EXPECT_GE(results->size(), 1u);
}

TEST(QA_GDB_123_Search, SearchSingleElement) {
    Fixture fix;
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> vec = {5.0F, 5.0F};
    ASSERT_TRUE(index.insert(vec).has_value());

    auto results = index.search(vec, 1);
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1u);
    EXPECT_EQ((*results)[0].node_id, 0u);
    EXPECT_FLOAT_EQ((*results)[0].distance, 0.0F);
}

TEST(QA_GDB_123_Search, DuplicateVectors) {
    Fixture fix;
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    // Insert the same vector 5 times.
    std::vector<float> vec = {1.0F, 1.0F};
    for (int i = 0; i < 5; ++i) {
        auto ir = index.insert(vec);
        ASSERT_TRUE(ir.has_value()) << ir.error().message;
        EXPECT_EQ(ir.value(), static_cast<uint32_t>(i));
    }

    EXPECT_EQ(index.node_count(), 5u);

    // All results should have distance 0.
    auto results = index.search(vec, 5);
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 5u);
    for (const auto& r : results.value()) {
        EXPECT_FLOAT_EQ(r.distance, 0.0F);
    }
}

TEST(QA_GDB_123_Search, FilterRejectsAll) {
    Fixture fix;
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

    // Filter rejects everything.
    std::vector<float> query = {0.0F, 0.0F};
    auto results = index.search(query, 5, [](uint32_t) { return false; });
    ASSERT_TRUE(results.has_value());
    EXPECT_TRUE(results->empty());
}

TEST(QA_GDB_123_Search, FilterAcceptsAll) {
    Fixture fix;
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

    std::vector<float> query = {0.0F, 0.0F};
    auto results = index.search(query, 5, [](uint32_t) { return true; });
    ASSERT_TRUE(results.has_value());
    EXPECT_EQ(results->size(), 5u);
}

// ---------------------------------------------------------------------------
// Insert edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB_123_Insert, ZeroDimensionConfig) {
    Fixture fix;
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 0;
    auto result = index.create(config);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB_123_Insert, ZeroMConfig) {
    Fixture fix;
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 4;
    config.m = 0;
    auto result = index.create(config);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB_123_Insert, InsertEmptyVector) {
    Fixture fix;
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 4;
    ASSERT_TRUE(index.create(config).has_value());

    // Empty vector (dimension mismatch).
    std::vector<float> empty;
    auto result = index.insert(empty);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB_123_Insert, InsertWrongDimension) {
    Fixture fix;
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 4;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> too_short = {1.0F, 2.0F};
    EXPECT_FALSE(index.insert(too_short).has_value());

    std::vector<float> too_long = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F};
    EXPECT_FALSE(index.insert(too_long).has_value());
}

TEST(QA_GDB_123_Insert, SequentialNodeIds) {
    Fixture fix;
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    for (int i = 0; i < 20; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        auto id = index.insert(vec);
        ASSERT_TRUE(id.has_value()) << id.error().message;
        EXPECT_EQ(id.value(), static_cast<uint32_t>(i));
    }
    EXPECT_EQ(index.node_count(), 20u);
}

TEST(QA_GDB_123_Insert, LargeDimension) {
    Fixture fix(512);
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 1024;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> vec(1024, 0.5F);
    auto id = index.insert(vec);
    ASSERT_TRUE(id.has_value()) << id.error().message;
    EXPECT_EQ(id.value(), 0u);

    auto results = index.search(vec, 1);
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1u);
    EXPECT_EQ((*results)[0].node_id, 0u);
    EXPECT_NEAR((*results)[0].distance, 0.0F, 1e-5F);
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

TEST(QA_GDB_123_Reset, InsertAfterReset) {
    Fixture fix;
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    // Insert some vectors.
    for (int i = 0; i < 5; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }
    EXPECT_EQ(index.node_count(), 5u);

    // Reset.
    auto reset = index.reset();
    ASSERT_TRUE(reset.has_value()) << reset.error().message;
    EXPECT_EQ(index.node_count(), 0u);
    EXPECT_EQ(index.meta().entry_point_id, hnsw_invalid_node_id);

    // Search should return empty.
    std::vector<float> query = {0.0F, 0.0F};
    auto results = index.search(query, 5);
    ASSERT_TRUE(results.has_value());
    EXPECT_TRUE(results->empty());

    // Insert again — IDs should restart from 0.
    std::vector<float> vec2 = {10.0F, 10.0F};
    auto id = index.insert(vec2);
    ASSERT_TRUE(id.has_value()) << id.error().message;
    EXPECT_EQ(id.value(), 0u);
    EXPECT_EQ(index.node_count(), 1u);

    // Search should find the new vector.
    auto results2 = index.search(vec2, 1);
    ASSERT_TRUE(results2.has_value());
    ASSERT_EQ(results2->size(), 1u);
    EXPECT_EQ((*results2)[0].node_id, 0u);
}

// ---------------------------------------------------------------------------
// M parameter variations
// ---------------------------------------------------------------------------

TEST(QA_GDB_123_MParam, SmallM) {
    Fixture fix;
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 2; // Very small M
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    for (int i = 0; i < 10; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    // Should still find exact match.
    std::vector<float> query = {5.0F, 0.0F};
    auto results = index.search(query, 1);
    ASSERT_TRUE(results.has_value());
    ASSERT_GE(results->size(), 1u);
    EXPECT_EQ((*results)[0].node_id, 5u);
    EXPECT_NEAR((*results)[0].distance, 0.0F, 1e-5F);
}

TEST(QA_GDB_123_MParam, LargeM) {
    Fixture fix(256);
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 64; // Large M
    config.ef_construction = 128;
    config.ef_search = 128;
    ASSERT_TRUE(index.create(config).has_value());

    for (int i = 0; i < 30; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    // With large M, recall should be very high.
    std::vector<float> query = {15.0F, 0.0F};
    auto results = index.search(query, 1);
    ASSERT_TRUE(results.has_value());
    ASSERT_GE(results->size(), 1u);
    EXPECT_EQ((*results)[0].node_id, 15u);
}

// ---------------------------------------------------------------------------
// Search with wrong dimension
// ---------------------------------------------------------------------------

TEST(QA_GDB_123_Search, WrongQueryDimension) {
    Fixture fix;
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 4;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> vec = {1.0F, 2.0F, 3.0F, 4.0F};
    ASSERT_TRUE(index.insert(vec).has_value());

    // Query with wrong dimension.
    std::vector<float> query2 = {1.0F, 2.0F};
    EXPECT_FALSE(index.search(query2, 1).has_value());

    std::vector<float> query6 = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
    EXPECT_FALSE(index.search(query6, 1).has_value());
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

TEST(QA_GDB_123_Persistence, NodeCountAfterLoad) {
    Fixture fix(256);
    PageId meta_pid = 0;

    {
        HnswIndex index(*fix.bpm, nullptr);
        HnswIndexConfig config;
        config.dimension = 2;
        config.m = 4;
        config.ef_construction = 16;
        config.ef_search = 16;
        ASSERT_TRUE(index.create(config).has_value());
        meta_pid = index.meta_page_id();

        for (int i = 0; i < 25; ++i) {
            std::vector<float> vec = {static_cast<float>(i), 0.0F};
            ASSERT_TRUE(index.insert(vec).has_value());
        }
        EXPECT_EQ(index.node_count(), 25u);
    }

    {
        HnswIndex loaded(*fix.bpm, nullptr);
        ASSERT_TRUE(loaded.load(meta_pid).has_value());
        EXPECT_EQ(loaded.node_count(), 25u);
        EXPECT_EQ(loaded.dimension(), 2u);

        // All nodes should be locatable.
        for (uint32_t i = 0; i < 25; ++i) {
            auto loc = loaded.node_location(i);
            EXPECT_TRUE(loc.has_value()) << "Node " << i << " not found after load";
        }
    }
}
