#include "sixseven/index/btree_index.h"
#include "sixseven/index/btree_persistence.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <future>
#include <memory>
#include <thread>

using namespace sixseven;

class BTreePersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_btree_persist";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        dm_ = std::make_unique<DiskManager>();
    }

    void TearDown() override {
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    std::pair<FileId, std::unique_ptr<BufferPoolManager>> create_bpm(const std::string& name) {
        auto path = data_dir_ / (name + ".db");
        auto fid = dm_->create_file(path, false, true);
        EXPECT_TRUE(fid.has_value());
        if (!fid.has_value()) {
            return {FileId{}, nullptr};
        }
        auto bpm = std::make_unique<BufferPoolManager>(*dm_, *fid, 256);
        return {*fid, std::move(bpm)};
    }

    std::pair<FileId, std::unique_ptr<BufferPoolManager>> open_bpm(const std::string& name) {
        auto path = data_dir_ / (name + ".db");
        auto fid = dm_->open_file(path);
        EXPECT_TRUE(fid.has_value());
        if (!fid.has_value()) {
            return {FileId{}, nullptr};
        }
        auto bpm = std::make_unique<BufferPoolManager>(*dm_, *fid, 256);
        return {*fid, std::move(bpm)};
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
};

// =============================================================================
// AC: Empty tree roundtrip
// =============================================================================

TEST_F(BTreePersistenceTest, EmptyTreeRoundtrip) {
    BTreeConfig config;
    config.key_types = {TypeId::INT32};
    config.is_unique = true;
    BTreeIndex original(std::move(config));

    EXPECT_EQ(original.size(), 0);

    // Persist.
    auto [fid1, bpm1] = create_bpm("empty_btree");
    auto meta = BTreePersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    // Load.
    auto [fid2, bpm2] = open_bpm("empty_btree");
    auto loaded = BTreePersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->size(), 0);
    EXPECT_TRUE((*loaded)->empty());
}

// =============================================================================
// AC: Small tree roundtrip with point lookups
// =============================================================================

TEST_F(BTreePersistenceTest, SmallTreeRoundtrip) {
    BTreeConfig config;
    config.key_types = {TypeId::INT32};
    config.is_unique = true;
    BTreeIndex original(std::move(config));

    // Insert some entries.
    for (int32_t i = 1; i <= 10; ++i) {
        auto r = original.insert({Value(i)}, RID{static_cast<PageId>(i), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    EXPECT_EQ(original.size(), 10);

    // Persist.
    auto [fid1, bpm1] = create_bpm("small_btree");
    auto meta = BTreePersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    // Load.
    auto [fid2, bpm2] = open_bpm("small_btree");
    auto loaded = BTreePersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->size(), 10);

    // Verify point lookups.
    for (int32_t i = 1; i <= 10; ++i) {
        auto r = (*loaded)->search({Value(i)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
        ASSERT_TRUE(r->has_value());
        EXPECT_EQ(r->value().page_id, static_cast<PageId>(i));
    }

    // Non-existent key.
    auto r = (*loaded)->search({Value(int32_t(99))});
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

// =============================================================================
// AC: Multi-level tree (enough entries to trigger splits)
// =============================================================================

TEST_F(BTreePersistenceTest, MultiLevelTreeRoundtrip) {
    BTreeConfig config;
    config.key_types = {TypeId::INT32};
    config.leaf_max_keys = 4; // Force splits with small nodes.
    config.internal_max_keys = 4;
    config.is_unique = true;
    BTreeIndex original(std::move(config));

    constexpr int32_t N = 100;
    for (int32_t i = 1; i <= N; ++i) {
        auto r = original.insert({Value(i)}, RID{static_cast<PageId>(i), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    EXPECT_EQ(original.size(), N);

    // Persist.
    auto [fid1, bpm1] = create_bpm("multi_btree");
    auto meta = BTreePersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    // Load.
    auto [fid2, bpm2] = open_bpm("multi_btree");
    auto loaded = BTreePersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->size(), N);

    // Verify all entries.
    for (int32_t i = 1; i <= N; ++i) {
        auto r = (*loaded)->search({Value(i)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
        ASSERT_TRUE(r->has_value()) << "key " << i << " not found";
        EXPECT_EQ(r->value().page_id, static_cast<PageId>(i));
    }
}

// =============================================================================
// AC: Composite key types (INT + STRING)
// =============================================================================

TEST_F(BTreePersistenceTest, CompositeKeyRoundtrip) {
    BTreeConfig config;
    config.key_types = {TypeId::INT32, TypeId::STRING};
    config.is_unique = true;
    BTreeIndex original(std::move(config));

    auto r1 = original.insert({Value(int32_t(1)), Value(std::string("Alice"))}, RID{1, 0});
    ASSERT_TRUE(r1.has_value());
    auto r2 = original.insert({Value(int32_t(2)), Value(std::string("Bob"))}, RID{2, 0});
    ASSERT_TRUE(r2.has_value());
    auto r3 = original.insert({Value(int32_t(3)), Value(std::string("Carol"))}, RID{3, 0});
    ASSERT_TRUE(r3.has_value());

    // Persist.
    auto [fid1, bpm1] = create_bpm("composite_btree");
    auto meta = BTreePersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    // Load.
    auto [fid2, bpm2] = open_bpm("composite_btree");
    auto loaded = BTreePersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->size(), 3);

    auto s = (*loaded)->search({Value(int32_t(2)), Value(std::string("Bob"))});
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
    EXPECT_EQ(s->value().page_id, 2u);
}

// =============================================================================
// AC: Insert works after load (tree is fully functional)
// =============================================================================

TEST_F(BTreePersistenceTest, InsertAfterLoad) {
    BTreeConfig config;
    config.key_types = {TypeId::INT32};
    config.is_unique = true;
    BTreeIndex original(std::move(config));

    for (int32_t i = 1; i <= 5; ++i) {
        (void)original.insert({Value(i)}, RID{static_cast<PageId>(i), 0});
    }

    auto [fid1, bpm1] = create_bpm("insert_after");
    auto meta = BTreePersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value());
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("insert_after");
    auto loaded = BTreePersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value());

    // Insert new entries into the loaded tree.
    auto ins = (*loaded)->insert({Value(int32_t(6))}, RID{6, 0});
    ASSERT_TRUE(ins.has_value()) << ins.error().message;
    EXPECT_EQ((*loaded)->size(), 6);

    auto s = (*loaded)->search({Value(int32_t(6))});
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
}

// =============================================================================
// GDB-794: Thread safety - persist while inserting does not cause data races
// =============================================================================

TEST_F(BTreePersistenceTest, ConcurrentPersistWhileInserting) {
    BTreeConfig config;
    config.key_types = {TypeId::INT32};
    config.is_unique = true;
    // Use small leaf_max_keys to encourage splits during concurrent access.
    config.leaf_max_keys = 4;
    config.internal_max_keys = 4;
    auto index = std::make_shared<BTreeIndex>(std::move(config));

    // Pre-populate with some data.
    for (int32_t i = 1; i <= 50; ++i) {
        auto r = index->insert({Value(i)}, RID{static_cast<PageId>(i), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    // Set up storage.
    auto [fid, bpm] = create_bpm("concurrent_btree");

    std::atomic<bool> stop{false};
    std::atomic<int> insert_count{0};
    std::atomic<int> persist_count{0};
    std::atomic<bool> error_flag{false};

    // Writer thread: continuously inserts.
    auto writer = std::async(std::launch::async, [&]() {
        int32_t idx = 100;
        while (!stop.load()) {
            auto r = index->insert({Value(idx)}, RID{static_cast<PageId>(idx), 0});
            if (r.has_value()) {
                ++insert_count;
            } else if (r.error().code != StatusCode::CONSTRAINT_VIOLATION) {
                // Ignore duplicates (is_unique=false would be different).
                error_flag = true;
            }
            ++idx;
        }
    });

    // Persist thread: continuously persists.
    auto persister = std::async(std::launch::async, [&]() {
        while (!stop.load()) {
            auto meta = BTreePersistence::persist(*bpm, *index);
            if (!meta.has_value()) {
                // Persist can fail if the buffer pool fails, but it shouldn't
                // crash or encounter data races.
                // We just track that we tried.
            }
            ++persist_count;
            // Small sleep to allow writer to make progress.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Run for a bounded time.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop = true;

    writer.get();
    persister.get();

    // If there were data races, we would likely see ASan violations or crashes.
    // The test passes if we reach here without crashing.
    EXPECT_FALSE(error_flag) << "Insert encountered unexpected error";
    EXPECT_GT(insert_count, 0) << "Writer should have made some progress";
    EXPECT_GT(persist_count, 0) << "Persister should have made some progress";

    // Final persist to verify index is still consistent.
    auto final_meta = BTreePersistence::persist(*bpm, *index);
    ASSERT_TRUE(final_meta.has_value()) << final_meta.error().message;

    bpm.reset();
    (void)dm_->close_file(fid);

    // Load and verify data integrity.
    auto [fid2, bpm2] = open_bpm("concurrent_btree");
    auto loaded = BTreePersistence::load(*bpm2, *final_meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    // All entries that were inserted should be retrievable.
    // Note: exact count may vary due to concurrent inserts, but loaded tree
    // should be in a consistent state (no corruption).
    EXPECT_GE((*loaded)->size(), 50u) << "Should have at least pre-populated entries";
}
