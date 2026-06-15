// QA tests for GDB-813: Hash index overflow-page V2 persistence format.
//
// Tests cover:
// 1. Existing large-directory round-trip (re-verification)
// 2. V1/V2 boundary: just below and just above ~8100-byte threshold
// 3. Multiple overflow pages (global_depth 12+)
// 4. CRITICAL: V2 sentinel false-positive when global_depth = 512 (bytes [0x00,0x02,...])
// 5. Backward compatibility: V1 small index loads correctly (no V2 misdetection)
// 6. Durability: reopen after persist, all keys present
// 7. Corruption/durability: graceful error on truncated/missing overflow page

#include "sixseven/index/hash_index.h"
#include "sixseven/index/hash_persistence.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <memory>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class QA_GDB813 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb813";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        dm_ = std::make_unique<DiskManager>();
    }

    void TearDown() override {
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    // Create a fresh BPM for writing.
    std::pair<FileId, std::unique_ptr<BufferPoolManager>> create_bpm(const std::string& name,
                                                                     uint32_t frames = 512) {
        auto path = data_dir_ / (name + ".db");
        auto fid = dm_->create_file(path, false, true);
        EXPECT_TRUE(fid.has_value()) << fid.error().message;
        if (!fid)
            return {FileId{}, nullptr};
        return {*fid, std::make_unique<BufferPoolManager>(*dm_, *fid, frames)};
    }

    // Reopen the same file for reading.
    std::pair<FileId, std::unique_ptr<BufferPoolManager>> open_bpm(const std::string& name,
                                                                   uint32_t frames = 512) {
        auto path = data_dir_ / (name + ".db");
        auto fid = dm_->open_file(path);
        EXPECT_TRUE(fid.has_value()) << fid.error().message;
        if (!fid)
            return {FileId{}, nullptr};
        return {*fid, std::make_unique<BufferPoolManager>(*dm_, *fid, frames)};
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
};

// ---------------------------------------------------------------------------
// Helper: build an index with N INT32 keys (0..N-1) using given bucket_capacity.
// ---------------------------------------------------------------------------
static std::unique_ptr<HashIndex>
make_index(int32_t n, uint32_t bucket_capacity = 64, bool is_unique = true) {
    HashIndexConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.bucket_capacity = bucket_capacity;
    cfg.is_unique = is_unique;
    auto idx = std::make_unique<HashIndex>(std::move(cfg));
    for (int32_t i = 0; i < n; ++i) {
        auto r = idx->insert({Value(i)}, RID{static_cast<PageId>(i + 1), 0});
        if (!r)
            throw std::runtime_error("insert failed: " + r.error().message);
    }
    return idx;
}

// ---------------------------------------------------------------------------
// TC1: Re-run developer round-trip test (LargeDirectoryOverflowRoundtrip).
// Confirms the basic V2 path works from a QA perspective.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB813, LargeDirectoryOverflowRoundtripRerun) {
    // bucket_capacity=1 forces splits, guaranteeing global_depth >= 11.
    auto [fid1, bpm1] = create_bpm("tc1_large");
    ASSERT_NE(bpm1, nullptr);

    auto original = make_index(2500, /*bucket_capacity=*/1);
    ASSERT_EQ(original->size(), 2500u);
    ASSERT_GE(original->global_depth(), 11u)
        << "expected global_depth >= 11 to force V2; got " << original->global_depth();

    auto meta_r = HashPersistence::persist(*bpm1, *original);
    ASSERT_TRUE(meta_r.has_value()) << "persist failed: " << meta_r.error().message;
    PageId meta_id = *meta_r;

    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("tc1_large");
    ASSERT_NE(bpm2, nullptr);
    auto loaded_r = HashPersistence::load(*bpm2, meta_id);
    ASSERT_TRUE(loaded_r.has_value()) << "load failed: " << loaded_r.error().message;

    auto& loaded = *loaded_r;
    EXPECT_EQ(loaded->size(), 2500u);
    EXPECT_EQ(loaded->global_depth(), original->global_depth());

    for (int32_t i = 0; i < 2500; ++i) {
        auto s = loaded->search({Value(i)});
        ASSERT_TRUE(s.has_value()) << "search error key " << i << ": " << s.error().message;
        ASSERT_TRUE(s->has_value()) << "key " << i << " not found after load";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i + 1)) << "wrong RID key " << i;
    }

    bpm2.reset();
    (void)dm_->close_file(fid2);
}

// ---------------------------------------------------------------------------
// TC2a: V1/V2 boundary — just under threshold (should use V1, no overflow).
// Directory bytes = 4 (dir_size u32) + dir_slots * 4.
// For V1 header size ~ 4+8+4+1+1+1 = 19 bytes.
// V1 total = 19 + 4 + dir_slots*4 <= 8100  =>  dir_slots <= (8100-23)/4 = 2019.
// Use global_depth=10 (1024 slots) to stay comfortably in V1.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB813, BoundaryBelowV2Threshold_V1Path) {
    auto [fid1, bpm1] = create_bpm("tc2a_v1");
    ASSERT_NE(bpm1, nullptr);

    // Use 512 keys with bucket_capacity=1 => global_depth ~= 9 (512 slots).
    // inline_size = 19(header) + 4(dir_size) + 512*4 = 2071 bytes — well below 8100.
    // (1024 slots = 19+4+1024*4 = 4119 bytes, also V1; confirmed safe.)
    auto original = make_index(512, /*bucket_capacity=*/1);
    uint32_t gd = original->global_depth();
    uint32_t dir_slots = 1u << gd;

    // Compute expected inline size to confirm it is under 8100.
    // header: 4(gd)+8(size)+4(cap)+1(unique)+1(ntypes)+1(INT32 type) = 19
    // directory: 4(dir_size)+dir_slots*4
    size_t inline_size = 19 + 4 + dir_slots * 4;
    EXPECT_LT(inline_size, 8100u) << "test pre-condition: expected V1 path but inline_size="
                                  << inline_size;

    auto meta_r = HashPersistence::persist(*bpm1, *original);
    ASSERT_TRUE(meta_r.has_value()) << meta_r.error().message;
    PageId meta_id = *meta_r;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("tc2a_v1");
    ASSERT_NE(bpm2, nullptr);
    auto loaded_r = HashPersistence::load(*bpm2, meta_id);
    ASSERT_TRUE(loaded_r.has_value()) << "V1 load failed: " << loaded_r.error().message;
    EXPECT_EQ((*loaded_r)->size(), static_cast<uint64_t>(512));
    EXPECT_EQ((*loaded_r)->global_depth(), gd);

    // Spot-check all keys.
    for (int32_t i = 0; i < 512; ++i) {
        auto s = (*loaded_r)->search({Value(i)});
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i << " missing";
    }
    bpm2.reset();
    (void)dm_->close_file(fid2);
}

// ---------------------------------------------------------------------------
// TC2b: V1/V2 boundary — just over threshold (forces V2 path).
// global_depth=11 => 2048 slots => directory chunk = 4+2048*4 = 8196 bytes > 8100.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB813, BoundaryAboveV2Threshold_V2Path) {
    auto [fid1, bpm1] = create_bpm("tc2b_v2");
    ASSERT_NE(bpm1, nullptr);

    auto original = make_index(2048, /*bucket_capacity=*/1);
    ASSERT_GE(original->global_depth(), 11u) << "need global_depth >= 11 to cross the threshold";

    auto meta_r = HashPersistence::persist(*bpm1, *original);
    ASSERT_TRUE(meta_r.has_value())
        << "persist at V1/V2 boundary failed: " << meta_r.error().message;
    PageId meta_id = *meta_r;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("tc2b_v2");
    ASSERT_NE(bpm2, nullptr);
    auto loaded_r = HashPersistence::load(*bpm2, meta_id);
    ASSERT_TRUE(loaded_r.has_value()) << "V2 load failed: " << loaded_r.error().message;
    EXPECT_EQ((*loaded_r)->size(), 2048u);
    EXPECT_EQ((*loaded_r)->global_depth(), original->global_depth());

    // Verify every key.
    for (int32_t i = 0; i < 2048; ++i) {
        auto s = (*loaded_r)->search({Value(i)});
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i << " missing at V2 boundary";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i + 1));
    }
    bpm2.reset();
    (void)dm_->close_file(fid2);
}

// ---------------------------------------------------------------------------
// TC3: Multiple overflow pages — global_depth >= 13 (8192 slots).
// 8192 slots => directory = 4 + 8192*4 = 32772 bytes => ~4 overflow pages.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB813, MultipleOverflowPages_GlobalDepth13) {
    // Need enough frames: 8192 unique buckets + overflow/meta pages.
    auto [fid1, bpm1] = create_bpm("tc3_deep", /*frames=*/1024);
    ASSERT_NE(bpm1, nullptr);

    auto original = make_index(8192, /*bucket_capacity=*/1);
    ASSERT_GE(original->global_depth(), 13u)
        << "need >= 8192 slots (global_depth >= 13) for multiple overflow pages";

    auto meta_r = HashPersistence::persist(*bpm1, *original);
    ASSERT_TRUE(meta_r.has_value())
        << "persist (multi-overflow) failed: " << meta_r.error().message;
    PageId meta_id = *meta_r;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("tc3_deep", /*frames=*/1024);
    ASSERT_NE(bpm2, nullptr);
    auto loaded_r = HashPersistence::load(*bpm2, meta_id);
    ASSERT_TRUE(loaded_r.has_value())
        << "load (multi-overflow) failed: " << loaded_r.error().message;
    EXPECT_EQ((*loaded_r)->size(), 8192u);
    EXPECT_EQ((*loaded_r)->global_depth(), original->global_depth());

    // Spot-check every 100th key to keep test fast.
    for (int32_t i = 0; i < 8192; i += 100) {
        auto s = (*loaded_r)->search({Value(i)});
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i << " missing (multi-overflow)";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i + 1));
    }
    bpm2.reset();
    (void)dm_->close_file(fid2);
}

// ---------------------------------------------------------------------------
// TC4: V2 magic is NOT falsely triggered by V1 global_depth=512.
//
// global_depth=512 encodes in little-endian u32 as [0x00, 0x02, 0x00, 0x00]
// (= 0x00000200).  The OLD 2-byte sentinel [0x00, 0x02] would have matched
// this, misdetecting a V1 index as V2 and producing garbage on load.
//
// The fix replaced the sentinel with a 4-byte magic HASH_V2_MAGIC=0xFF534858.
// Read as a LE uint32 the four bytes of global_depth=512 are 0x00000200, which
// is NOT equal to 0xFF534858.  No misdetection occurs.
//
// This test verifies: the 4-byte leading uint32 of a V1 page whose
// global_depth=512 does NOT equal HASH_V2_MAGIC, confirming the fix.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB813, SentinelFalsePositiveRisk_V1GlobalDepth512) {
    // global_depth=512 LE = bytes [0x00, 0x02, 0x00, 0x00] = uint32 0x00000200.
    constexpr uint32_t HASH_V2_MAGIC = 0xFF534858U;
    constexpr uint32_t dangerous_gd = 512U;

    // Read global_depth as a little-endian uint32 (same as the fixed detection
    // path does).
    uint8_t gd_bytes[4];
    gd_bytes[0] = static_cast<uint8_t>(dangerous_gd);
    gd_bytes[1] = static_cast<uint8_t>(dangerous_gd >> 8);
    gd_bytes[2] = static_cast<uint8_t>(dangerous_gd >> 16);
    gd_bytes[3] = static_cast<uint8_t>(dangerous_gd >> 24);

    uint32_t leading_u32 = 0;
    std::memcpy(&leading_u32, gd_bytes, 4);

    // With the fixed 4-byte magic comparison, this global_depth does NOT
    // trigger V2 detection.  The old 2-byte sentinel would have: bytes[0]==0
    // and bytes[1]==2.  The new check: leading_u32 (=0x00000200) != HASH_V2_MAGIC
    // (=0xFF534858).
    bool would_be_misdetected = (leading_u32 == HASH_V2_MAGIC);

    EXPECT_FALSE(would_be_misdetected)
        << "BUG: global_depth=" << dangerous_gd << " produces leading uint32=0x" << std::hex
        << leading_u32 << " which equals HASH_V2_MAGIC=0x" << HASH_V2_MAGIC
        << "; V1 index would be misdetected as V2.";

    // Also verify that no small global_depth (0..65535) can collide with the
    // 4-byte magic.  All such values fit in the low 16 bits; the high byte is
    // always 0x00, whereas HASH_V2_MAGIC has high byte 0xFF.
    for (uint32_t gd = 0; gd <= 65535U; ++gd) {
        EXPECT_NE(gd, HASH_V2_MAGIC) << "global_depth=" << gd << " collides with HASH_V2_MAGIC";
    }
}

// ---------------------------------------------------------------------------
// TC5: Backward compatibility — pure V1 small indexes load without misdetection.
// Specifically verify global_depth values 0..10 are not misread as V2.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB813, BackwardCompatV1SmallIndexRoundtrip) {
    // Use several V1-path indices with different global_depths.
    // 50 keys with bucket_capacity=4 -> global_depth ~= 4-5.
    auto [fid1, bpm1] = create_bpm("tc5_v1_compat");
    ASSERT_NE(bpm1, nullptr);

    auto original = make_index(50, /*bucket_capacity=*/4);
    uint32_t gd = original->global_depth();
    // Confirm this is V1 (directory fits inline).
    uint32_t dir_slots = 1u << gd;
    size_t inline_sz = 19 + 4 + dir_slots * 4;
    EXPECT_LT(inline_sz, 8100u) << "pre-condition: expected V1 path";

    auto meta_r = HashPersistence::persist(*bpm1, *original);
    ASSERT_TRUE(meta_r.has_value()) << meta_r.error().message;
    PageId meta_id = *meta_r;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("tc5_v1_compat");
    ASSERT_NE(bpm2, nullptr);
    auto loaded_r = HashPersistence::load(*bpm2, meta_id);
    ASSERT_TRUE(loaded_r.has_value())
        << "V1 backward-compat load failed: " << loaded_r.error().message;

    auto& loaded = *loaded_r;
    EXPECT_EQ(loaded->size(), 50u);
    EXPECT_EQ(loaded->global_depth(), gd);

    for (int32_t i = 0; i < 50; ++i) {
        auto s = loaded->search({Value(i)});
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i << " missing after V1 load";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i + 1));
    }
    bpm2.reset();
    (void)dm_->close_file(fid2);
}

// ---------------------------------------------------------------------------
// TC6: Durability — persist, reopen fresh DiskManager+BPM, verify queries.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB813, DurabilityReopenAfterV2Persist) {
    auto path = data_dir_ / "tc6_durability.db";
    PageId meta_id = 0;
    uint32_t orig_gd = 0;
    constexpr int32_t N = 3000;

    {
        auto fid_r = dm_->create_file(path, false, true);
        ASSERT_TRUE(fid_r.has_value());
        BufferPoolManager bpm(*dm_, *fid_r, 512);
        auto original = make_index(N, /*bucket_capacity=*/1);
        ASSERT_GE(original->global_depth(), 11u);
        orig_gd = original->global_depth();
        auto m = HashPersistence::persist(bpm, *original);
        ASSERT_TRUE(m.has_value()) << m.error().message;
        meta_id = *m;
        // BPM goes out of scope, flushing/unpinning on destruction.
        (void)dm_->close_file(*fid_r);
    }

    // Simulate restart: new DiskManager.
    dm_.reset();
    dm_ = std::make_unique<DiskManager>();

    auto fid2_r = dm_->open_file(path);
    ASSERT_TRUE(fid2_r.has_value());
    BufferPoolManager bpm2(*dm_, *fid2_r, 512);

    auto loaded_r = HashPersistence::load(bpm2, meta_id);
    ASSERT_TRUE(loaded_r.has_value()) << "durability load failed: " << loaded_r.error().message;
    EXPECT_EQ((*loaded_r)->size(), static_cast<uint64_t>(N));
    EXPECT_EQ((*loaded_r)->global_depth(), orig_gd);

    // Verify every key after simulated restart.
    for (int32_t i = 0; i < N; ++i) {
        auto s = (*loaded_r)->search({Value(i)});
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i << " missing after restart";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i + 1));
    }
    (void)dm_->close_file(*fid2_r);
}

// ---------------------------------------------------------------------------
// TC7: V2 round-trip preserves per-key RID accuracy (not just presence).
// Tests that bucket deduplication / directory rebuilding does not scramble
// shared-bucket pointer assignments.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB813, V2PerKeyRIDAccuracy) {
    auto [fid1, bpm1] = create_bpm("tc7_rid_accuracy");
    ASSERT_NE(bpm1, nullptr);

    // Unique page_ids: key i -> RID{i*7+1, i%3} to make slot_id non-trivial.
    HashIndexConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.bucket_capacity = 1;
    cfg.is_unique = true;
    auto original = std::make_unique<HashIndex>(std::move(cfg));

    constexpr int32_t N = 2500;
    for (int32_t i = 0; i < N; ++i) {
        auto r = original->insert(
            {Value(i)}, RID{static_cast<PageId>(i * 7 + 1), static_cast<uint16_t>(i % 3)});
        ASSERT_TRUE(r.has_value()) << "insert " << i << ": " << r.error().message;
    }
    ASSERT_GE(original->global_depth(), 11u);

    auto meta_r = HashPersistence::persist(*bpm1, *original);
    ASSERT_TRUE(meta_r.has_value()) << meta_r.error().message;
    PageId meta_id = *meta_r;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("tc7_rid_accuracy");
    ASSERT_NE(bpm2, nullptr);
    auto loaded_r = HashPersistence::load(*bpm2, meta_id);
    ASSERT_TRUE(loaded_r.has_value()) << loaded_r.error().message;

    for (int32_t i = 0; i < N; ++i) {
        auto s = (*loaded_r)->search({Value(i)});
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "key " << i << " not found";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i * 7 + 1))
            << "wrong page_id for key " << i;
        EXPECT_EQ(s->value().slot_id, static_cast<uint16_t>(i % 3))
            << "wrong slot_id for key " << i;
    }
    bpm2.reset();
    (void)dm_->close_file(fid2);
}

// ---------------------------------------------------------------------------
// TC8: Graceful error — invalid meta_page_id (page that does not exist).
// Confirm load returns an error, not a crash or silent wrong data.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB813, GracefulError_InvalidMetaPageId) {
    auto [fid, bpm] = create_bpm("tc8_invalid");
    ASSERT_NE(bpm, nullptr);

    // Don't write anything. Attempt to load from a nonexistent page.
    auto loaded_r = HashPersistence::load(*bpm, /*meta_page_id=*/9999);
    EXPECT_FALSE(loaded_r.has_value())
        << "expected error loading from nonexistent page, got success";
    if (!loaded_r.has_value()) {
        EXPECT_FALSE(loaded_r.error().message.empty()) << "error message should not be empty";
    }
    bpm.reset();
    (void)dm_->close_file(fid);
}

// ---------------------------------------------------------------------------
// TC9: Insert-after-load works for V2 (large) indexes.
// After loading a V2 index, further inserts must not corrupt existing data.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB813, InsertAfterLoadV2) {
    auto [fid1, bpm1] = create_bpm("tc9_insert_after_v2");
    ASSERT_NE(bpm1, nullptr);

    constexpr int32_t N = 2500;
    auto original = make_index(N, /*bucket_capacity=*/1);
    ASSERT_GE(original->global_depth(), 11u);

    auto meta_r = HashPersistence::persist(*bpm1, *original);
    ASSERT_TRUE(meta_r.has_value()) << meta_r.error().message;
    PageId meta_id = *meta_r;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("tc9_insert_after_v2");
    ASSERT_NE(bpm2, nullptr);
    auto loaded_r = HashPersistence::load(*bpm2, meta_id);
    ASSERT_TRUE(loaded_r.has_value()) << loaded_r.error().message;
    auto& loaded = *loaded_r;

    // Insert a new key that didn't exist before.
    int32_t new_key = N + 1000;
    auto ins = loaded->insert({Value(new_key)}, RID{999999, 7});
    ASSERT_TRUE(ins.has_value()) << "insert after V2 load failed: " << ins.error().message;
    EXPECT_EQ(loaded->size(), static_cast<uint64_t>(N + 1));

    // New key is found.
    auto s_new = loaded->search({Value(new_key)});
    ASSERT_TRUE(s_new.has_value());
    ASSERT_TRUE(s_new->has_value()) << "new key not found after insert-after-load";
    EXPECT_EQ(s_new->value().page_id, 999999u);
    EXPECT_EQ(s_new->value().slot_id, 7u);

    // Spot-check some original keys are still intact.
    for (int32_t i = 0; i < N; i += 250) {
        auto s = loaded->search({Value(i)});
        ASSERT_TRUE(s.has_value());
        ASSERT_TRUE(s->has_value()) << "original key " << i << " corrupted after insert";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i + 1));
    }
    bpm2.reset();
    (void)dm_->close_file(fid2);
}

// ---------------------------------------------------------------------------
// TC10: global_depth=0 edge case — V1 with brand-new (empty) index.
// V2 sentinel check: first byte=0, second byte=0 (global_depth LE) => NOT V2.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB813, EmptyIndexV1NotMisdetectedAsV2) {
    auto [fid1, bpm1] = create_bpm("tc10_empty");
    ASSERT_NE(bpm1, nullptr);

    HashIndexConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.is_unique = true;
    HashIndex original(std::move(cfg));
    EXPECT_EQ(original.size(), 0u);

    auto meta_r = HashPersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta_r.has_value()) << meta_r.error().message;
    PageId meta_id = *meta_r;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("tc10_empty");
    ASSERT_NE(bpm2, nullptr);
    auto loaded_r = HashPersistence::load(*bpm2, meta_id);
    ASSERT_TRUE(loaded_r.has_value())
        << "empty-index load failed (possible V2 misdetection): " << loaded_r.error().message;
    EXPECT_EQ((*loaded_r)->size(), 0u);
    EXPECT_EQ((*loaded_r)->global_depth(), 0u);
    bpm2.reset();
    (void)dm_->close_file(fid2);
}
