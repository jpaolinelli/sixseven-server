/// @file test_qa_gdb_124.cpp
/// @brief QA adversarial tests for GDB-124/125: HNSW delete with lazy
///        tombstones and compaction.
///
/// Targets edge cases not covered by existing test_hnsw_page.cpp tests:
/// delete all nodes, delete-then-insert cycles, compaction of full tombstones,
/// search with filter + tombstone interaction, persistence after delete,
/// insert after compaction, multi-round delete/compact.

#include "sixseven/storage/buffer_pool.h"
#include "sixseven/common/platform.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/common/platform.h"
#include "sixseven/vector/hnsw_index.h"
#include "sixseven/common/platform.h"
#include "sixseven/vector/hnsw_page.h"
#include "sixseven/common/platform.h"

#include <gtest/gtest.h>
#include "sixseven/common/platform.h"

#include <cmath>
#include "sixseven/common/platform.h"
#include <filesystem>
#include "sixseven/common/platform.h"
#include <numeric>
#include "sixseven/common/platform.h"
#include <random>
#include "sixseven/common/platform.h"
#include <set>
#include "sixseven/common/platform.h"
#include <vector>
#include "sixseven/common/platform.h"

using namespace sixseven;

namespace {

class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() / "sixseven_qa124_XXXXXX";
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

struct Fixture {
    TempDir tmp;
    DiskManager disk_manager;
    FileId file_id = 0;
    std::unique_ptr<BufferPoolManager> bpm;

    explicit Fixture(size_t pool_size = 128) {
        auto db_path = tmp.path() / "test_hnsw_qa124.db";
        auto fid = disk_manager.create_file(db_path);
        EXPECT_TRUE(fid.has_value());
        file_id = fid.value();
        bpm = std::make_unique<BufferPoolManager>(disk_manager, file_id, pool_size);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Delete edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB_124_Delete, DeleteOnlyNode) {
    Fixture fix;
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

    // Delete the only node.
    ASSERT_TRUE(index.remove(0).has_value());
    EXPECT_EQ(index.node_count(), 0u);
    EXPECT_EQ(index.meta().tombstone_count, 1u);

    // Entry point should be invalid.
    EXPECT_EQ(index.meta().entry_point_id, hnsw_invalid_node_id);

    // Search should return empty.
    auto results = index.search(vec, 1);
    ASSERT_TRUE(results.has_value());
    EXPECT_TRUE(results->empty());
}

TEST(QA_GDB_124_Delete, DeleteAllNodes) {
    Fixture fix;
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    const int n = 10;
    for (int i = 0; i < n; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }
    EXPECT_EQ(index.node_count(), static_cast<uint32_t>(n));

    // Delete all nodes.
    for (int i = 0; i < n; ++i) {
        auto del = index.remove(static_cast<uint32_t>(i));
        ASSERT_TRUE(del.has_value()) << "Delete " << i << ": " << del.error().message;
    }
    EXPECT_EQ(index.node_count(), 0u);
    EXPECT_EQ(index.meta().tombstone_count, static_cast<uint32_t>(n));
    EXPECT_EQ(index.meta().entry_point_id, hnsw_invalid_node_id);

    // Search should return empty.
    std::vector<float> query = {5.0F, 0.0F};
    auto results = index.search(query, 5);
    ASSERT_TRUE(results.has_value());
    EXPECT_TRUE(results->empty());
}

TEST(QA_GDB_124_Delete, DeleteFirstNode) {
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

    // Delete node 0 (the first inserted).
    ASSERT_TRUE(index.remove(0).has_value());
    EXPECT_EQ(index.node_count(), 4u);

    // Search should not return node 0.
    std::vector<float> query = {0.0F, 0.0F};
    auto results = index.search(query, 5);
    ASSERT_TRUE(results.has_value());
    for (const auto& r : results.value()) {
        EXPECT_NE(r.node_id, 0u);
    }
}

TEST(QA_GDB_124_Delete, DeleteLastNode) {
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

    // Delete node 4 (the last inserted).
    ASSERT_TRUE(index.remove(4).has_value());
    EXPECT_EQ(index.node_count(), 4u);

    // Search for deleted vector position should not return node 4.
    std::vector<float> query = {4.0F, 0.0F};
    auto results = index.search(query, 5);
    ASSERT_TRUE(results.has_value());
    for (const auto& r : results.value()) {
        EXPECT_NE(r.node_id, 4u);
    }
}

// ---------------------------------------------------------------------------
// Delete + insert interaction
// ---------------------------------------------------------------------------

TEST(QA_GDB_124_DeleteInsert, InsertAfterDelete) {
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

    // Delete node 2.
    ASSERT_TRUE(index.remove(2).has_value());
    EXPECT_EQ(index.node_count(), 4u);

    // Insert a new vector — should get node ID 5 (not reuse 2).
    std::vector<float> new_vec = {10.0F, 10.0F};
    auto new_id = index.insert(new_vec);
    ASSERT_TRUE(new_id.has_value()) << new_id.error().message;
    EXPECT_EQ(new_id.value(), 5u);
    EXPECT_EQ(index.node_count(), 5u);

    // New node should be searchable.
    auto results = index.search(new_vec, 1);
    ASSERT_TRUE(results.has_value());
    ASSERT_GE(results->size(), 1u);
    EXPECT_EQ((*results)[0].node_id, 5u);
}

TEST(QA_GDB_124_DeleteInsert, InsertAfterDeleteAll) {
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

    // Delete all.
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(index.remove(static_cast<uint32_t>(i)).has_value());
    }
    EXPECT_EQ(index.node_count(), 0u);
    EXPECT_EQ(index.meta().entry_point_id, hnsw_invalid_node_id);

    // Insert new vector into empty (tombstone-filled) index.
    std::vector<float> new_vec = {99.0F, 99.0F};
    auto new_id = index.insert(new_vec);
    ASSERT_TRUE(new_id.has_value()) << new_id.error().message;
    EXPECT_EQ(new_id.value(), 3u); // IDs don't reset after delete.
    EXPECT_EQ(index.node_count(), 1u);

    // Should be searchable.
    auto results = index.search(new_vec, 1);
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1u);
    EXPECT_EQ((*results)[0].node_id, 3u);
}

// ---------------------------------------------------------------------------
// Compaction edge cases
// ---------------------------------------------------------------------------

TEST(QA_GDB_124_Compact, CompactAllTombstoned) {
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

    // Delete all.
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(index.remove(static_cast<uint32_t>(i)).has_value());
    }
    EXPECT_EQ(index.meta().tombstone_count, 5u);

    // Compact all tombstones.
    auto compact = index.compact();
    ASSERT_TRUE(compact.has_value()) << compact.error().message;
    EXPECT_EQ(index.meta().tombstone_count, 0u);

    // All nodes gone from node map.
    for (uint32_t i = 0; i < 5; ++i) {
        EXPECT_FALSE(index.node_location(i).has_value());
    }
}

TEST(QA_GDB_124_Compact, CompactThenSearch) {
    Fixture fix;
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

    // Delete even nodes.
    for (int i = 0; i < 10; i += 2) {
        ASSERT_TRUE(index.remove(static_cast<uint32_t>(i)).has_value());
    }

    // Compact.
    ASSERT_TRUE(index.compact().has_value());
    EXPECT_EQ(index.meta().tombstone_count, 0u);
    EXPECT_EQ(index.node_count(), 5u);

    // Search for remaining odd nodes.
    for (int i = 1; i < 10; i += 2) {
        std::vector<float> query = {static_cast<float>(i), 0.0F};
        auto results = index.search(query, 1);
        ASSERT_TRUE(results.has_value()) << "Search for node " << i;
        ASSERT_GE(results->size(), 1u);
        EXPECT_EQ((*results)[0].node_id, static_cast<uint32_t>(i));
        EXPECT_NEAR((*results)[0].distance, 0.0F, 1e-5F);
    }
}

TEST(QA_GDB_124_Compact, InsertAfterCompaction) {
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

    // Delete nodes 1, 3.
    ASSERT_TRUE(index.remove(1).has_value());
    ASSERT_TRUE(index.remove(3).has_value());

    // Compact.
    ASSERT_TRUE(index.compact().has_value());
    EXPECT_EQ(index.node_count(), 3u);

    // Insert new vector — should get ID 5 (IDs keep incrementing).
    std::vector<float> new_vec = {20.0F, 0.0F};
    auto new_id = index.insert(new_vec);
    ASSERT_TRUE(new_id.has_value()) << new_id.error().message;
    EXPECT_EQ(new_id.value(), 5u);
    EXPECT_EQ(index.node_count(), 4u);

    // Search for the new vector.
    auto results = index.search(new_vec, 1);
    ASSERT_TRUE(results.has_value());
    ASSERT_GE(results->size(), 1u);
    EXPECT_EQ((*results)[0].node_id, 5u);
}

TEST(QA_GDB_124_Compact, MultiRoundDeleteCompact) {
    Fixture fix;
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    // Round 1: insert 10, delete 3, compact.
    for (int i = 0; i < 10; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }
    ASSERT_TRUE(index.remove(0).has_value());
    ASSERT_TRUE(index.remove(5).has_value());
    ASSERT_TRUE(index.remove(9).has_value());
    ASSERT_TRUE(index.compact().has_value());
    EXPECT_EQ(index.node_count(), 7u);
    EXPECT_EQ(index.meta().tombstone_count, 0u);

    // Round 2: insert 5 more (IDs 10-14), delete 2, compact.
    for (int i = 10; i < 15; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }
    EXPECT_EQ(index.node_count(), 12u);
    ASSERT_TRUE(index.remove(1).has_value());
    ASSERT_TRUE(index.remove(12).has_value());
    ASSERT_TRUE(index.compact().has_value());
    EXPECT_EQ(index.node_count(), 10u);
    EXPECT_EQ(index.meta().tombstone_count, 0u);

    // Verify remaining nodes are searchable.
    std::vector<float> query = {3.0F, 0.0F};
    auto results = index.search(query, 1);
    ASSERT_TRUE(results.has_value());
    ASSERT_GE(results->size(), 1u);
    EXPECT_EQ((*results)[0].node_id, 3u);
}

TEST(QA_GDB_124_Compact, CompactTwiceIsIdempotent) {
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

    ASSERT_TRUE(index.remove(2).has_value());
    ASSERT_TRUE(index.compact().has_value());
    EXPECT_EQ(index.meta().tombstone_count, 0u);

    // Second compact should be a no-op.
    ASSERT_TRUE(index.compact().has_value());
    EXPECT_EQ(index.meta().tombstone_count, 0u);
    EXPECT_EQ(index.node_count(), 4u);
}

// ---------------------------------------------------------------------------
// Filter + tombstone interaction
// ---------------------------------------------------------------------------

TEST(QA_GDB_124_Filter, FilteredSearchAfterDelete) {
    Fixture fix;
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

    // Delete nodes 0, 2, 4.
    ASSERT_TRUE(index.remove(0).has_value());
    ASSERT_TRUE(index.remove(2).has_value());
    ASSERT_TRUE(index.remove(4).has_value());

    // Filter accepts only even-numbered IDs — but 0, 2, 4 are tombstoned.
    // So only 6 and 8 should pass (even AND not tombstoned).
    std::vector<float> query = {5.0F, 0.0F};
    auto results = index.search(query, 10, [](uint32_t id) { return id % 2 == 0; });
    ASSERT_TRUE(results.has_value());
    for (const auto& r : results.value()) {
        EXPECT_TRUE(r.node_id == 6 || r.node_id == 8)
            << "Unexpected node " << r.node_id << " in results";
    }
}

TEST(QA_GDB_124_Filter, FilterRejectsAllAfterDelete) {
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

    // Delete node 2.
    ASSERT_TRUE(index.remove(2).has_value());

    // Filter rejects everything.
    std::vector<float> query = {2.0F, 0.0F};
    auto results = index.search(query, 5, [](uint32_t) { return false; });
    ASSERT_TRUE(results.has_value());
    EXPECT_TRUE(results->empty());
}

// ---------------------------------------------------------------------------
// Persistence after delete
// ---------------------------------------------------------------------------

TEST(QA_GDB_124_Persistence, TombstonesSurviveLoad) {
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

        for (int i = 0; i < 10; ++i) {
            std::vector<float> vec = {static_cast<float>(i), 0.0F};
            ASSERT_TRUE(index.insert(vec).has_value());
        }

        // Delete nodes 3 and 7.
        ASSERT_TRUE(index.remove(3).has_value());
        ASSERT_TRUE(index.remove(7).has_value());
        EXPECT_EQ(index.node_count(), 8u);
        EXPECT_EQ(index.meta().tombstone_count, 2u);
    }

    {
        HnswIndex loaded(*fix.bpm, nullptr);
        ASSERT_TRUE(loaded.load(meta_pid).has_value());
        EXPECT_EQ(loaded.node_count(), 8u);
        EXPECT_EQ(loaded.meta().tombstone_count, 2u);

        // Search should not return deleted nodes.
        std::vector<float> query = {3.0F, 0.0F};
        auto results = loaded.search(query, 10);
        ASSERT_TRUE(results.has_value());
        for (const auto& r : results.value()) {
            EXPECT_NE(r.node_id, 3u) << "Deleted node 3 in results after load";
            EXPECT_NE(r.node_id, 7u) << "Deleted node 7 in results after load";
        }
    }
}

TEST(QA_GDB_124_Persistence, CompactAfterLoad) {
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

        for (int i = 0; i < 8; ++i) {
            std::vector<float> vec = {static_cast<float>(i), 0.0F};
            ASSERT_TRUE(index.insert(vec).has_value());
        }

        // Delete nodes 1, 4, 6.
        ASSERT_TRUE(index.remove(1).has_value());
        ASSERT_TRUE(index.remove(4).has_value());
        ASSERT_TRUE(index.remove(6).has_value());
    }

    {
        HnswIndex loaded(*fix.bpm, nullptr);
        ASSERT_TRUE(loaded.load(meta_pid).has_value());
        EXPECT_EQ(loaded.meta().tombstone_count, 3u);

        // Compact after load.
        ASSERT_TRUE(loaded.compact().has_value());
        EXPECT_EQ(loaded.meta().tombstone_count, 0u);
        EXPECT_EQ(loaded.node_count(), 5u);

        // Verify remaining nodes searchable.
        std::vector<float> query = {0.0F, 0.0F};
        auto results = loaded.search(query, 1);
        ASSERT_TRUE(results.has_value());
        ASSERT_GE(results->size(), 1u);
        EXPECT_EQ((*results)[0].node_id, 0u);
    }
}

// ---------------------------------------------------------------------------
// Node count accuracy
// ---------------------------------------------------------------------------

TEST(QA_GDB_124_NodeCount, AccurateThroughCycles) {
    Fixture fix;
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
    EXPECT_EQ(index.node_count(), 5u);

    // Delete 2.
    ASSERT_TRUE(index.remove(1).has_value());
    ASSERT_TRUE(index.remove(3).has_value());
    EXPECT_EQ(index.node_count(), 3u);

    // Insert 3 more.
    for (int i = 5; i < 8; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }
    EXPECT_EQ(index.node_count(), 6u);

    // Compact.
    ASSERT_TRUE(index.compact().has_value());
    EXPECT_EQ(index.node_count(), 6u);

    // Delete 1 more.
    ASSERT_TRUE(index.remove(0).has_value());
    EXPECT_EQ(index.node_count(), 5u);
}

// ---------------------------------------------------------------------------
// Delete entry point when it's the highest-layer node
// ---------------------------------------------------------------------------

TEST(QA_GDB_124_Delete, DeleteEntryPointAndCompact) {
    Fixture fix;
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    // Insert enough nodes to likely have multi-layer structure.
    for (int i = 0; i < 20; ++i) {
        std::vector<float> vec = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    uint32_t entry_id = index.meta().entry_point_id;

    // Delete the entry point.
    ASSERT_TRUE(index.remove(entry_id).has_value());
    EXPECT_NE(index.meta().entry_point_id, entry_id);

    // Compact.
    ASSERT_TRUE(index.compact().has_value());
    EXPECT_EQ(index.meta().tombstone_count, 0u);

    // Search should still work after entry point change + compact.
    std::vector<float> query = {10.0F, 0.0F};
    auto results = index.search(query, 1);
    ASSERT_TRUE(results.has_value());
    ASSERT_GE(results->size(), 1u);
    // Node 10 should still exist (unless it was the entry point).
    if (entry_id != 10u) {
        EXPECT_EQ((*results)[0].node_id, 10u);
    }
}

// ---------------------------------------------------------------------------
// Stress: delete-heavy workload
// ---------------------------------------------------------------------------

TEST(QA_GDB_124_Stress, DeleteHalfAndSearch) {
    Fixture fix(256);
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 4;
    config.m = 8;
    config.ef_construction = 32;
    config.ef_search = 32;
    ASSERT_TRUE(index.create(config).has_value());

    const int n = 50;
    std::mt19937 rng(42);

    // Insert.
    std::vector<std::vector<float>> dataset;
    for (int i = 0; i < n; ++i) {
        std::vector<float> vec(4);
        for (auto& v : vec) {
            v = std::uniform_real_distribution<float>(-1.0F, 1.0F)(rng);
        }
        dataset.push_back(vec);
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    // Delete the first half.
    for (int i = 0; i < n / 2; ++i) {
        ASSERT_TRUE(index.remove(static_cast<uint32_t>(i)).has_value());
    }
    EXPECT_EQ(index.node_count(), static_cast<uint32_t>(n / 2));

    // Compact.
    ASSERT_TRUE(index.compact().has_value());
    EXPECT_EQ(index.meta().tombstone_count, 0u);

    // Search for a surviving node.
    auto results = index.search(dataset[n / 2], 1);
    ASSERT_TRUE(results.has_value());
    ASSERT_GE(results->size(), 1u);
    EXPECT_EQ((*results)[0].node_id, static_cast<uint32_t>(n / 2));
}

TEST(QA_GDB_124_Stress, AlternatingInsertDelete) {
    Fixture fix(256);
    HnswIndex index(*fix.bpm, nullptr);
    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    // Insert 5, delete 2, insert 5, delete 2, repeat.
    uint32_t next_id = 0;
    std::set<uint32_t> alive;

    for (int round = 0; round < 4; ++round) {
        // Insert 5.
        for (int i = 0; i < 5; ++i) {
            std::vector<float> vec = {static_cast<float>(next_id), 0.0F};
            auto id = index.insert(vec);
            ASSERT_TRUE(id.has_value()) << id.error().message;
            EXPECT_EQ(id.value(), next_id);
            alive.insert(next_id);
            next_id++;
        }

        // Delete 2 (oldest alive).
        int deleted = 0;
        for (auto it = alive.begin(); it != alive.end() && deleted < 2;) {
            auto del = index.remove(*it);
            ASSERT_TRUE(del.has_value()) << del.error().message;
            it = alive.erase(it);
            deleted++;
        }
    }

    EXPECT_EQ(index.node_count(), static_cast<uint32_t>(alive.size()));

    // All alive nodes should be locatable.
    for (uint32_t id : alive) {
        EXPECT_TRUE(index.node_location(id).has_value()) << "Node " << id << " not found";
    }
}
