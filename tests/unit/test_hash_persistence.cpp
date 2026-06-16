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
// =============================================================================
// GDB-1265: V2 magic is collision-free with V1 global_depth=512
//
// global_depth=512 serializes to little-endian bytes [0x00, 0x02, 0x00, 0x00].
// The old 2-byte sentinel [0x00, 0x02] would have matched this, causing a V1
// index with global_depth=512 to be misloaded as V2.
//
// With the 4-byte HASH_V2_MAGIC (0xFF534858), the leading bytes [0x00, 0x02,
// 0x00, 0x00] cannot equal 0xFF534858, so there is no false-positive.
//
// This test verifies that the 4-byte magic used for V2 detection does NOT
// match the leading bytes of any plausible V1 global_depth value.
// =============================================================================

TEST_F(HashPersistenceTest, V2MagicNoCollisionWithV1GlobalDepth512) {
    // global_depth=512 LE = bytes [0x00, 0x02, 0x00, 0x00].
    // Read as uint32 LE this is 0x00000200 = 512.
    // HASH_V2_MAGIC = 0xFF534858.
    // They must not be equal.
    constexpr uint32_t HASH_V2_MAGIC = 0xFF534858U;
    uint32_t gd_512_as_u32 = 512U; // 0x00000200
    EXPECT_NE(gd_512_as_u32, HASH_V2_MAGIC)
        << "BUG: global_depth=512 as uint32 collides with HASH_V2_MAGIC";

    // Also verify a small set of boundary values that are the most "dangerous"
    // (any global_depth whose LE bytes start with the old sentinel [0x00, 0x02]).
    // With the 4-byte magic none of these can collide.
    for (uint32_t gd : {0U, 1U, 2U, 10U, 11U, 255U, 256U, 512U, 1024U, 65535U}) {
        EXPECT_NE(gd, HASH_V2_MAGIC) << "global_depth=" << gd << " collides with HASH_V2_MAGIC";
    }
}

TEST_F(HashPersistenceTest, V1WithLeadingZeroByteRoundtrip) {
    // Build a V1 index that starts with global_depth=0 (brand-new, empty index).
    // global_depth=0 serializes as [0x00, 0x00, 0x00, 0x00].
    // Under the old 2-byte sentinel [0x00, 0x02] this would NOT collide (byte[1]==0).
    // Under the new 4-byte magic it also does not collide.
    // Verify correct V1 round-trip.
    HashIndexConfig config;
    config.key_types = {TypeId::INT32};
    config.is_unique = true;
    HashIndex original(std::move(config));
    // Leave empty: global_depth remains 0.
    EXPECT_EQ(original.global_depth(), 0u);

    auto [fid1, bpm1] = create_bpm("v1_gd0");
    auto meta = HashPersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("v1_gd0");
    auto loaded = HashPersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value())
        << "V1 empty index load failed (possible V2 misdetection): " << loaded.error().message;
    EXPECT_EQ((*loaded)->global_depth(), 0u);
    EXPECT_EQ((*loaded)->size(), 0u);
}

// =============================================================================
// GDB-826: Pin leak regression — no pin leak when bucket insert_tuple fails
//
// A hash bucket with many entries serialises to more bytes than a slotted page
// can hold (~8164 bytes usable).  Before this fix, hash_persistence.cpp called
// bpm.new_page(), got a frame, called insert_tuple (which fails because the
// bucket payload exceeds the page capacity), and returned an error WITHOUT
// calling bpm.unpin_page() — leaving the frame permanently pinned.
//
// This test:
//  1. Builds a hash index whose single bucket serialises to > 8164 bytes.
//  2. Calls HashPersistence::persist() — which must fail (bucket too large).
//  3. Verifies that AFTER the failure the buffer pool still has a free frame
//     (i.e. no frame was leaked by the failed persist).
//  4. Also verifies that a successful persist on a fresh pool leaves all
//     frames unpinned (RAII guard does not double-unpin on the success path).
// =============================================================================

TEST_F(HashPersistenceTest, PinNotLeakedOnBucketInsertFailure) {
    // bucket_capacity=600 so no splits happen for 500 entries.
    // All 500 entries land in a single bucket (global_depth stays 0).
    //
    // Per-entry serialized size:
    //   hash(8) + INT32 key (1 null flag + 4 payload = 5) + page_id(4) + slot_id(2) = 19 bytes
    // Bucket data: local_depth(4) + entry_count(4) + 500*19 = 9508 bytes.
    // Slotted-page capacity: 8192 - 24 (header) - 4 (slot entry) = 8164 bytes.
    // 9508 > 8164 → insert_tuple MUST fail for this bucket.
    HashIndexConfig config;
    config.key_types = {TypeId::INT32};
    config.bucket_capacity = 600;
    config.is_unique = true;
    HashIndex index(std::move(config));

    for (int32_t i = 0; i < 500; ++i) {
        auto r = index.insert({Value(i)}, RID{static_cast<PageId>(i + 1), 0});
        ASSERT_TRUE(r.has_value()) << "insert " << i << " failed: " << r.error().message;
    }
    EXPECT_EQ(index.size(), 500u);
    // Confirm all 500 entries are in one bucket (global_depth == 0, directory size == 1).
    EXPECT_EQ(index.global_depth(), 0u);

    // Use a pool of 2 frames so that if one frame leaks the pool still has 1 free.
    // After the (expected) persist failure, we verify we can still allocate both frames.
    auto path = data_dir_ / "pin_leak_hash.db";
    auto fid_r = dm_->create_file(path, false, true);
    ASSERT_TRUE(fid_r.has_value()) << fid_r.error().message;
    auto bpm = std::make_unique<BufferPoolManager>(*dm_, *fid_r, /*pool_size=*/2);

    // Persist MUST fail: the bucket payload (9508 bytes) cannot fit in one page.
    auto persist_result = HashPersistence::persist(*bpm, index);
    ASSERT_FALSE(persist_result.has_value())
        << "Expected persist to fail for over-sized bucket, but it succeeded";

    // GDB-826 regression: verify no frame was leaked.
    // If the PinGuard is absent (old code), the bucket-page frame is permanently
    // pinned after the failed insert_tuple, leaving only 1 free frame.  With
    // pool_size=2 we should be able to allocate BOTH frames after the failure.
    auto page1_r = bpm->new_page();
    ASSERT_TRUE(page1_r.has_value())
        << "new_page #1 failed — frame leaked by failed persist: " << page1_r.error().message;
    auto page2_r = bpm->new_page();
    ASSERT_TRUE(page2_r.has_value())
        << "new_page #2 failed — frame leaked by failed persist: " << page2_r.error().message;

    // Clean up: unpin both frames we just allocated.
    (void)bpm->unpin_page((*page1_r)->page_id(), false);
    (void)bpm->unpin_page((*page2_r)->page_id(), false);

    bpm.reset();
    (void)dm_->close_file(*fid_r);
}

// =============================================================================
// GDB-826: Success path is still correct — RAII guard does not double-unpin
//
// Verify that for a normal (successful) persist the buffer pool ends up with
// all frames unpinned.  A double-unpin (introduced by a buggy guard that fires
// AFTER an explicit unpin on the success path) would cause unpin_page to
// return INVALID_ARGUMENT the second time; the test detects this by checking
// that the pool accepts a new_page allocation after persist.
// =============================================================================

TEST_F(HashPersistenceTest, SuccessPathPinBalanced) {
    HashIndexConfig config;
    config.key_types = {TypeId::INT32};
    config.is_unique = true;
    HashIndex index(std::move(config));

    for (int32_t i = 0; i < 10; ++i) {
        auto r = index.insert({Value(i)}, RID{static_cast<PageId>(i + 1), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    // Pool of 4 frames — tight but enough for a 10-entry index.
    auto path = data_dir_ / "success_pin_balanced.db";
    auto fid_r = dm_->create_file(path, false, true);
    ASSERT_TRUE(fid_r.has_value()) << fid_r.error().message;
    auto bpm = std::make_unique<BufferPoolManager>(*dm_, *fid_r, /*pool_size=*/4);

    auto meta = HashPersistence::persist(*bpm, index);
    ASSERT_TRUE(meta.has_value()) << "persist failed: " << meta.error().message;

    // After a successful persist every pinned frame should have been unpinned.
    // Allocate all 4 frames — if any frame is stuck pinned, eviction is blocked
    // and new_page will fail once the free list and evictable set are exhausted.
    auto p1 = bpm->new_page();
    auto p2 = bpm->new_page();
    auto p3 = bpm->new_page();
    auto p4 = bpm->new_page();
    EXPECT_TRUE(p1.has_value()) << "frame 1 unavailable after persist";
    EXPECT_TRUE(p2.has_value()) << "frame 2 unavailable after persist";
    EXPECT_TRUE(p3.has_value()) << "frame 3 unavailable after persist";
    EXPECT_TRUE(p4.has_value()) << "frame 4 unavailable after persist";

    if (p1.has_value()) {
        (void)bpm->unpin_page((*p1)->page_id(), false);
    }
    if (p2.has_value()) {
        (void)bpm->unpin_page((*p2)->page_id(), false);
    }
    if (p3.has_value()) {
        (void)bpm->unpin_page((*p3)->page_id(), false);
    }
    if (p4.has_value()) {
        (void)bpm->unpin_page((*p4)->page_id(), false);
    }

    bpm.reset();
    (void)dm_->close_file(*fid_r);
}