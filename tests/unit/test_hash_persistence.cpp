#include "sixseven/index/hash_index.h"
#include "sixseven/index/hash_persistence.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <future>
#include <memory>
#include <thread>

using namespace sixseven;

class HashPersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_hash_persist";
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
// AC: Empty hash roundtrip
// =============================================================================

TEST_F(HashPersistenceTest, EmptyHashRoundtrip) {
    HashIndexConfig config;
    config.key_types = {TypeId::INT32};
    config.is_unique = true;
    HashIndex original(std::move(config));

    EXPECT_EQ(original.size(), 0);

    auto [fid1, bpm1] = create_bpm("empty_hash");
    auto meta = HashPersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("empty_hash");
    auto loaded = HashPersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->size(), 0);
    EXPECT_TRUE((*loaded)->empty());
}

// =============================================================================
// AC: Small hash roundtrip with point lookups
// =============================================================================

TEST_F(HashPersistenceTest, SmallHashRoundtrip) {
    HashIndexConfig config;
    config.key_types = {TypeId::INT32};
    config.is_unique = true;
    HashIndex original(std::move(config));

    for (int32_t i = 1; i <= 10; ++i) {
        auto r = original.insert({Value(i)}, RID{static_cast<PageId>(i), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    EXPECT_EQ(original.size(), 10);

    auto [fid1, bpm1] = create_bpm("small_hash");
    auto meta = HashPersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("small_hash");
    auto loaded = HashPersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->size(), 10);

    for (int32_t i = 1; i <= 10; ++i) {
        auto r = (*loaded)->search({Value(i)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
        ASSERT_TRUE(r->has_value()) << "key " << i << " not found";
        EXPECT_EQ(r->value().page_id, static_cast<PageId>(i));
    }
}

// =============================================================================
// AC: After multiple splits (trigger directory growth)
// =============================================================================

TEST_F(HashPersistenceTest, MultipleSplitsRoundtrip) {
    HashIndexConfig config;
    config.key_types = {TypeId::INT32};
    config.bucket_capacity = 4; // Force splits.
    config.is_unique = true;
    HashIndex original(std::move(config));

    constexpr int32_t N = 100;
    for (int32_t i = 1; i <= N; ++i) {
        auto r = original.insert({Value(i)}, RID{static_cast<PageId>(i), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    EXPECT_EQ(original.size(), N);
    EXPECT_GT(original.global_depth(), 0u);

    auto [fid1, bpm1] = create_bpm("splits_hash");
    auto meta = HashPersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("splits_hash");
    auto loaded = HashPersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->size(), N);
    EXPECT_EQ((*loaded)->global_depth(), original.global_depth());

    for (int32_t i = 1; i <= N; ++i) {
        auto r = (*loaded)->search({Value(i)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
        ASSERT_TRUE(r->has_value()) << "key " << i << " not found";
    }
}

// =============================================================================
// AC: Non-unique index with duplicate keys
// =============================================================================

TEST_F(HashPersistenceTest, NonUniqueRoundtrip) {
    HashIndexConfig config;
    config.key_types = {TypeId::INT32};
    config.is_unique = false;
    HashIndex original(std::move(config));

    // Insert same key with different RIDs.
    for (uint16_t i = 0; i < 5; ++i) {
        auto r = original.insert({Value(int32_t(42))}, RID{1, i});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    EXPECT_EQ(original.size(), 5);

    auto [fid1, bpm1] = create_bpm("nonunique_hash");
    auto meta = HashPersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("nonunique_hash");
    auto loaded = HashPersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->size(), 5);

    auto all = (*loaded)->search_all({Value(int32_t(42))});
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->size(), 5u);
}

// =============================================================================
// AC: Insert works after load
// =============================================================================

TEST_F(HashPersistenceTest, InsertAfterLoad) {
    HashIndexConfig config;
    config.key_types = {TypeId::INT32};
    config.is_unique = true;
    HashIndex original(std::move(config));

    for (int32_t i = 1; i <= 5; ++i) {
        (void)original.insert({Value(i)}, RID{static_cast<PageId>(i), 0});
    }

    auto [fid1, bpm1] = create_bpm("insert_after");
    auto meta = HashPersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value());
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("insert_after");
    auto loaded = HashPersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value());

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

TEST_F(HashPersistenceTest, ConcurrentPersistWhileInserting) {
    HashIndexConfig config;
    config.key_types = {TypeId::INT32};
    config.is_unique = true;
    config.bucket_capacity = 64; // Larger to prevent meta page overflow.
    auto index = std::make_shared<HashIndex>(std::move(config));

    // Pre-populate with modest data.
    for (int32_t i = 1; i <= 20; ++i) {
        auto r = index->insert({Value(i)}, RID{static_cast<PageId>(i), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    auto [fid, bpm] = create_bpm("concurrent_hash");

    std::atomic<bool> stop{false};
    std::atomic<int> insert_count{0};
    std::atomic<int> persist_count{0};
    std::atomic<bool> error_flag{false};

    // Writer thread: insert new keys only (not duplicates since is_unique=true).
    auto writer = std::async(std::launch::async, [&]() {
        int32_t idx = 100;
        while (!stop.load() && idx < 200) { // Limit total insertions.
            auto r = index->insert({Value(idx)}, RID{static_cast<PageId>(idx), 0});
            if (r.has_value()) {
                ++insert_count;
            } else if (r.error().code != StatusCode::CONSTRAINT_VIOLATION) {
                error_flag = true;
            }
            ++idx;
        }
    });

    // Persist thread: periodically persists.
    auto persister = std::async(std::launch::async, [&]() {
        int count = 0;
        while (!stop.load() && count < 5) { // Limit persist calls.
            auto meta = HashPersistence::persist(*bpm, *index);
            if (meta.has_value()) {
                ++persist_count;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ++count;
        }
    });

    // Run for bounded time.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop = true;

    writer.get();
    persister.get();

    EXPECT_FALSE(error_flag) << "Insert encountered unexpected error";
    EXPECT_GT(insert_count, 0) << "Writer should have made progress";
    EXPECT_GT(persist_count, 0) << "Persister should have made progress";

    // Final persist - should succeed with smaller index.
    auto final_meta = HashPersistence::persist(*bpm, *index);
    ASSERT_TRUE(final_meta.has_value()) << final_meta.error().message;

    bpm.reset();
    (void)dm_->close_file(fid);

    // Load and verify.
    auto [fid2, bpm2] = open_bpm("concurrent_hash");
    auto loaded = HashPersistence::load(*bpm2, *final_meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    EXPECT_GE((*loaded)->size(), 20u) << "Should have at least pre-populated entries";
}

// =============================================================================
// GDB-813: Large directory overflow (global_depth >= 11, directory > one page)
//
// With bucket_capacity=1 every distinct-hash key triggers a split, driving
// global_depth upward quickly.  At global_depth=11 the directory holds 2048
// slots (2048 * 4 = 8192 bytes), which exceeds the ~8100-byte inline limit
// and forces the V2 overflow-page path.  The test inserts enough distinct keys
// to guarantee global_depth >= 11, then verifies a full persist -> load
// round-trip: all keys are still findable and the global_depth is preserved.
// =============================================================================

TEST_F(HashPersistenceTest, LargeDirectoryOverflowRoundtrip) {
    // bucket_capacity=1 forces a split on every second distinct key.
    // With 2500 distinct INT32 keys we will reach global_depth >= 11
    // (directory >= 2048 slots), which overflows the single-page meta.
    HashIndexConfig config;
    config.key_types = {TypeId::INT32};
    config.bucket_capacity = 1;
    config.is_unique = true;

    // Use a larger BPM frame pool: 2500 unique buckets + overflow/meta pages.
    auto path = data_dir_ / "large_dir_hash.db";
    auto fid_r = dm_->create_file(path, false, true);
    ASSERT_TRUE(fid_r.has_value()) << fid_r.error().message;
    auto bpm1 = std::make_unique<BufferPoolManager>(*dm_, *fid_r, 512);

    HashIndex original(std::move(config));

    constexpr int32_t N = 2500;
    for (int32_t i = 0; i < N; ++i) {
        auto r = original.insert({Value(i)}, RID{static_cast<PageId>(i + 1), 0});
        ASSERT_TRUE(r.has_value()) << "insert " << i << " failed: " << r.error().message;
    }
    ASSERT_EQ(original.size(), static_cast<uint64_t>(N));

    // Directory must be large enough to trigger V2 overflow (>= 2048 slots).
    ASSERT_GE(original.global_depth(), 11u) << "global_depth=" << original.global_depth()
                                            << " — expected >= 11 to force directory overflow";

    // Persist — must succeed (previously this would fail with "meta data too large").
    auto meta_r = HashPersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta_r.has_value()) << "persist failed: " << meta_r.error().message;
    PageId meta_page_id = *meta_r;

    bpm1.reset();
    (void)dm_->close_file(*fid_r);

    // Load back.
    auto fid2_r = dm_->open_file(path);
    ASSERT_TRUE(fid2_r.has_value()) << fid2_r.error().message;
    auto bpm2 = std::make_unique<BufferPoolManager>(*dm_, *fid2_r, 512);

    auto loaded_r = HashPersistence::load(*bpm2, meta_page_id);
    ASSERT_TRUE(loaded_r.has_value()) << "load failed: " << loaded_r.error().message;

    auto& loaded = *loaded_r;
    EXPECT_EQ(loaded->size(), static_cast<uint64_t>(N));
    EXPECT_EQ(loaded->global_depth(), original.global_depth());

    // Spot-check every key is findable.
    for (int32_t i = 0; i < N; ++i) {
        auto s = loaded->search({Value(i)});
        ASSERT_TRUE(s.has_value()) << "search error for key " << i << ": " << s.error().message;
        ASSERT_TRUE(s->has_value()) << "key " << i << " not found after load";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i + 1)) << "wrong RID for key " << i;
    }

    bpm2.reset();
    (void)dm_->close_file(*fid2_r);
}
