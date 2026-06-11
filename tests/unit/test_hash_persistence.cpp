#include "sixseven/index/hash_index.h"
#include "sixseven/index/hash_persistence.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

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
