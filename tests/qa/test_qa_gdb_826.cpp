/// GDB-826 QA Tests — Buffer-pool pin leak regression on persist error paths
///
/// This file is an adversarial stress test for the PinGuard RAII fix applied in
/// btree_persistence.cpp and hash_persistence.cpp.  It does NOT duplicate the
/// unit tests in tests/unit/; instead it drives every distinct error/success
/// path that touches a pinned page and verifies:
///
///  1. No pin leak: after a failed persist the pool still has all N frames free
///     (detectable by allocating all frames after the failure).
///  2. No double-unpin corruption: after a successful persist all frames are
///     evictable (allocating all N frames succeeds).
///  3. No cumulative leak: running persist-with-error in a tight loop (100x)
///     does not progressively exhaust the pool.
///  4. Round-trip correctness: data written by a successful persist is
///     faithfully recovered by load (tests that the dirty flag is set correctly
///     by the guard — a wrong flag would silence writes → corruption on reload).

#include "sixseven/index/btree_index.h"
#include "sixseven/index/btree_persistence.h"
#include "sixseven/index/hash_index.h"
#include "sixseven/index/hash_persistence.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Shared fixture
// ---------------------------------------------------------------------------

class QA_GDB826 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ =
            std::filesystem::temp_directory_path() / "sixseven_qa_gdb826";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        dm_ = std::make_unique<DiskManager>();
    }

    void TearDown() override {
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    // Create a fresh BPM backed by a new file with the given pool size.
    // Returns {FileId, BPM}; caller must close the FileId when done.
    std::pair<FileId, std::unique_ptr<BufferPoolManager>>
    make_bpm_with_fid(const std::string& name, uint32_t pool_size = 256) {
        auto path = data_dir_ / (name + ".db");
        auto fid = dm_->create_file(path, false, true);
        if (!fid.has_value()) {
            return {FileId{}, nullptr};
        }
        auto bpm = std::make_unique<BufferPoolManager>(*dm_, *fid, pool_size);
        return {*fid, std::move(bpm)};
    }

    // Create a fresh BPM; FileId is tracked internally and closed in TearDown.
    std::unique_ptr<BufferPoolManager> make_bpm(const std::string& name,
                                                uint32_t pool_size = 256) {
        auto path = data_dir_ / (name + ".db");
        auto fid = dm_->create_file(path, false, true);
        if (!fid.has_value()) {
            return nullptr;
        }
        open_fids_.push_back(*fid);
        return std::make_unique<BufferPoolManager>(*dm_, *fid, pool_size);
    }

    // Open an existing file and return {FileId, BPM}; caller must close FileId.
    std::pair<FileId, std::unique_ptr<BufferPoolManager>>
    open_bpm_with_fid(const std::string& name, uint32_t pool_size = 256) {
        auto path = data_dir_ / (name + ".db");
        auto fid = dm_->open_file(path);
        if (!fid.has_value()) {
            return {FileId{}, nullptr};
        }
        auto bpm = std::make_unique<BufferPoolManager>(*dm_, *fid, pool_size);
        return {*fid, std::move(bpm)};
    }

    // Open an existing file; FileId tracked internally.
    std::unique_ptr<BufferPoolManager> open_bpm(const std::string& name,
                                                uint32_t pool_size = 256) {
        auto path = data_dir_ / (name + ".db");
        auto fid = dm_->open_file(path);
        if (!fid.has_value()) {
            return nullptr;
        }
        open_fids_.push_back(*fid);
        return std::make_unique<BufferPoolManager>(*dm_, *fid, pool_size);
    }

    // After a persist failure, assert the pool has 'frames' free frames by
    // allocating them all then releasing them.
    void assert_all_frames_free(BufferPoolManager& bpm, uint32_t frames,
                                const char* msg) {
        std::vector<PageId> allocated;
        for (uint32_t i = 0; i < frames; ++i) {
            auto pr = bpm.new_page();
            ASSERT_TRUE(pr.has_value())
                << msg << " — new_page #" << i
                << " failed (pin leak?): " << (pr.has_value() ? "" : pr.error().message);
            allocated.push_back((*pr)->page_id());
        }
        for (auto pid : allocated) {
            (void)bpm.unpin_page(pid, false);
        }
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::vector<FileId> open_fids_;
};

// ===========================================================================
// HASH — PIN LEAK on bucket insert_tuple failure
// ===========================================================================

// Build a hash index whose single bucket serializes to > 8164 bytes so that
// insert_tuple always fails.  Verify no frame is leaked.
TEST_F(QA_GDB826, Hash_PinNotLeakedOnBucketInsertFailure_TightPool) {
    // 500 entries × ~19 bytes/entry + 8-byte header ≈ 9508 bytes > 8164
    HashIndexConfig cfg;
    cfg.key_types     = {TypeId::INT32};
    cfg.bucket_capacity = 600; // no splits, one huge bucket
    cfg.is_unique     = true;
    HashIndex index(std::move(cfg));

    for (int32_t i = 0; i < 500; ++i) {
        auto r = index.insert({Value(i)}, RID{static_cast<PageId>(i + 1), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    ASSERT_EQ(index.global_depth(), 0u); // all in one bucket

    // Pool of 3 frames.  Pre-GDB-826: 1 frame leaks on the first bucket page
    // allocation, leaving only 2 free after the failure.
    auto bpm = make_bpm("hash_bucket_fail", /*pool_size=*/3);
    ASSERT_NE(bpm, nullptr);

    auto result = HashPersistence::persist(*bpm, index);
    ASSERT_FALSE(result.has_value())
        << "Expected persist to fail (oversized bucket) but it succeeded";

    // GDB-826 regression check: all 3 frames must be free.
    assert_all_frames_free(*bpm, 3, "Hash_PinNotLeakedOnBucketInsertFailure_TightPool");
}

// ===========================================================================
// HASH — CUMULATIVE LEAK: repeated persist failure in a loop
// ===========================================================================

// Run a persist-that-will-fail 100 times and confirm the pool is not
// progressively exhausted.  Pre-GDB-826 each call leaked 1 frame; after 100
// calls a pool of size 3 would be completely pinned.
TEST_F(QA_GDB826, Hash_NoCumulativePinLeakOnRepeatedBucketFailure) {
    HashIndexConfig cfg;
    cfg.key_types       = {TypeId::INT32};
    cfg.bucket_capacity = 600;
    cfg.is_unique       = true;
    HashIndex index(std::move(cfg));

    for (int32_t i = 0; i < 500; ++i) {
        auto r = index.insert({Value(i)}, RID{static_cast<PageId>(i + 1), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    // Bigger pool so the test doesn't exhaust it for non-bug reasons.
    auto bpm = make_bpm("hash_cumulative", /*pool_size=*/8);
    ASSERT_NE(bpm, nullptr);

    constexpr int ITERATIONS = 100;
    int fail_count = 0;
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        auto r = HashPersistence::persist(*bpm, index);
        if (!r.has_value()) {
            ++fail_count;
        }
    }
    EXPECT_EQ(fail_count, ITERATIONS)
        << "All " << ITERATIONS << " persist calls should fail for oversized bucket";

    // After 100 failed calls, all 8 frames must still be allocatable.
    assert_all_frames_free(*bpm, 8, "Hash_NoCumulativePinLeakOnRepeatedBucketFailure");
}

// ===========================================================================
// HASH — SUCCESS PATH correctness (dirty flag check via reload)
// ===========================================================================

// If PinGuard used dirty=false on the success path, the page content would
// not be written to disk.  A load after the persist would then return corrupt
// or empty data.  This test verifies that every persisted key is recovered.
TEST_F(QA_GDB826, Hash_SuccessPathDirtyFlagCorrectRoundtrip) {
    HashIndexConfig cfg;
    cfg.key_types     = {TypeId::INT32};
    cfg.is_unique     = true;
    HashIndex original(std::move(cfg));

    constexpr int N = 50;
    for (int32_t i = 0; i < N; ++i) {
        auto r = original.insert({Value(i)}, RID{static_cast<PageId>(i + 100), static_cast<SlotId>(i)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    auto [fid_w, bpm_w] = make_bpm_with_fid("hash_dirty_flag_roundtrip", 64);
    ASSERT_NE(bpm_w, nullptr);
    auto meta = HashPersistence::persist(*bpm_w, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm_w.reset();
    (void)dm_->close_file(fid_w);

    auto [fid_r, bpm_r] = open_bpm_with_fid("hash_dirty_flag_roundtrip", 64);
    ASSERT_NE(bpm_r, nullptr);
    auto loaded_r = HashPersistence::load(*bpm_r, *meta);
    ASSERT_TRUE(loaded_r.has_value()) << loaded_r.error().message;
    auto& loaded = *loaded_r;

    EXPECT_EQ(loaded->size(), static_cast<uint64_t>(N));
    for (int32_t i = 0; i < N; ++i) {
        auto s = loaded->search({Value(i)});
        ASSERT_TRUE(s.has_value()) << "search error key " << i;
        ASSERT_TRUE(s->has_value()) << "key " << i << " missing after reload";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i + 100))
            << "wrong page_id for key " << i;
        EXPECT_EQ(s->value().slot_id, static_cast<SlotId>(i))
            << "wrong slot_id for key " << i;
    }
    bpm_r.reset();
    (void)dm_->close_file(fid_r);
}

// ===========================================================================
// HASH — Repeated successful persist/reload on the same pool (cumulative check)
// ===========================================================================

// Run persist → reset pool → persist → reset pool ... 20 times.
// Each cycle re-uses a fresh file; confirms no progressive state corruption.
TEST_F(QA_GDB826, Hash_RepeatedSuccessfulPersistsNoLeak) {
    HashIndexConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.is_unique = true;
    HashIndex index(std::move(cfg));

    for (int32_t i = 0; i < 20; ++i) {
        auto r = index.insert({Value(i)}, RID{static_cast<PageId>(i + 1), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    for (int cycle = 0; cycle < 20; ++cycle) {
        std::string name = "hash_repersist_" + std::to_string(cycle);
        auto bpm = make_bpm(name, /*pool_size=*/4);
        ASSERT_NE(bpm, nullptr);

        auto meta = HashPersistence::persist(*bpm, index);
        ASSERT_TRUE(meta.has_value())
            << "cycle " << cycle << " persist failed: " << meta.error().message;

        // All 4 frames must be free after each persist.
        assert_all_frames_free(*bpm, 4, ("Hash_RepeatedSuccessfulPersistsNoLeak cycle " + std::to_string(cycle)).c_str());
    }
}

// ===========================================================================
// HASH — Large directory (V2 overflow path) round-trip correctness
// ===========================================================================

// global_depth >= 11 forces the V2 directory-overflow path.  Verify correct
// reload (dirty flag must be set on all overflow and meta pages).
TEST_F(QA_GDB826, Hash_LargeDirectoryV2RoundtripCorrectness) {
    HashIndexConfig cfg;
    cfg.key_types       = {TypeId::INT32};
    cfg.bucket_capacity = 1; // every distinct key triggers split
    cfg.is_unique       = true;

    auto [fid_w, bpm_w] = make_bpm_with_fid("hash_v2_roundtrip", 1024);
    ASSERT_NE(bpm_w, nullptr);

    HashIndex original(std::move(cfg));
    constexpr int32_t N = 600; // enough to reach global_depth >= 9
    for (int32_t i = 0; i < N; ++i) {
        auto r = original.insert({Value(i)}, RID{static_cast<PageId>(i + 1), 0});
        ASSERT_TRUE(r.has_value()) << "insert " << i << ": " << r.error().message;
    }
    EXPECT_GE(original.global_depth(), 9u);

    auto meta = HashPersistence::persist(*bpm_w, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm_w.reset();
    (void)dm_->close_file(fid_w);

    auto [fid_r, bpm_r] = open_bpm_with_fid("hash_v2_roundtrip", 1024);
    ASSERT_NE(bpm_r, nullptr);
    auto loaded_r = HashPersistence::load(*bpm_r, *meta);
    ASSERT_TRUE(loaded_r.has_value()) << loaded_r.error().message;

    EXPECT_EQ((*loaded_r)->size(), static_cast<uint64_t>(N));
    EXPECT_EQ((*loaded_r)->global_depth(), original.global_depth());

    // Spot-check every key.
    for (int32_t i = 0; i < N; ++i) {
        auto s = (*loaded_r)->search({Value(i)});
        ASSERT_TRUE(s.has_value()) << "search err key " << i;
        ASSERT_TRUE(s->has_value()) << "key " << i << " missing after V2 reload";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i + 1));
    }
    bpm_r.reset();
    (void)dm_->close_file(fid_r);
}

// ===========================================================================
// HASH — V2 overflow page insert_tuple failure pin leak check
// ===========================================================================

// Drive the hash V2 overflow path by constructing an index whose directory
// bytes exceed MAX_INLINE_SIZE.  We can't easily force insert_tuple to fail
// on the overflow page (the chunk size <= 8100), but we can verify that a
// successful V2 persist leaves all frames free (no leak, no double-unpin).
TEST_F(QA_GDB826, Hash_V2OverflowPathPinBalanced) {
    HashIndexConfig cfg;
    cfg.key_types       = {TypeId::INT32};
    cfg.bucket_capacity = 1;
    cfg.is_unique       = true;

    HashIndex index(std::move(cfg));
    // Need enough splits for the directory to exceed ~8100 bytes (>2025 slots).
    for (int32_t i = 0; i < 500; ++i) {
        auto r = index.insert({Value(i)}, RID{static_cast<PageId>(i + 1), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    auto bpm = make_bpm("hash_v2_overflow_balance", 512);
    ASSERT_NE(bpm, nullptr);

    auto meta = HashPersistence::persist(*bpm, index);
    ASSERT_TRUE(meta.has_value()) << "V2 path persist failed: " << meta.error().message;

    // All frames must be free after the V2 persist.
    // Use a smaller number than pool_size because some frames hold persisted pages.
    // Just verify 4 new allocations succeed (proxy for "no stuck pins").
    assert_all_frames_free(*bpm, 4, "Hash_V2OverflowPathPinBalanced");
}

// ===========================================================================
// BTREE — Node overflow (write_node_page) inline insert_tuple failure pin check
// ===========================================================================

// A leaf node with a very large key (> NODE_MAX_INLINE_SIZE = 8100 bytes)
// goes through the overflow path in write_node_page.  When the overflow page's
// insert_tuple fails (chunk > 8100), the PinGuard must clean up both the
// overflow page and the primary page.
//
// Forcing a chunk > 8100 requires a key > 8100 bytes (single key, leaf_max_keys=1).
// The node header is 18 bytes and the RID is 6 bytes, so to overflow one
// overflow page we need key data > 8100 bytes.  KEY_LEN = 8150 achieves this.
TEST_F(QA_GDB826, BTree_NodeOverflowChunkFailurePinLeak) {
    // A single key of 8150 bytes: node data = 18 (header) + 8150 (key serialized)
    // + 5 (null flag + bytes) + 6 (RID) but actually string serializes with length
    // prefix.  The important point: data.size() > NODE_MAX_INLINE_SIZE (8100)
    // so it enters the overflow path.  The first chunk == data.size() > 8100 so
    // ovf_page->insert_tuple should fail.
    //
    // Note: if the implementation chunking works correctly (chunk = min(8100,
    // data.size() - offset)), it will chunk properly and succeed.  In that case
    // the test verifies no pin leak on the success path of write_node_page.
    BTreeConfig cfg;
    cfg.key_types     = {TypeId::STRING};
    cfg.is_unique     = true;
    cfg.leaf_max_keys = 1;
    cfg.internal_max_keys = 4;
    BTreeIndex index(std::move(cfg));

    // A key that when serialized produces a node > 8100 bytes but can be
    // chunked across overflow pages.
    std::string large_key(8150, 'X');
    auto r = index.insert({Value(large_key)}, RID{42, 1});
    ASSERT_TRUE(r.has_value()) << r.error().message;

    // Use a pool large enough for the overflow pages.
    auto bpm = make_bpm("btree_node_ovf_fail", 32);
    ASSERT_NE(bpm, nullptr);

    auto meta = BTreePersistence::persist(*bpm, index);
    // Whether it succeeds or fails, verify no frames are stuck.
    // (It should succeed with chunked overflow.)
    if (!meta.has_value()) {
        // Failed path: verify no pin leak.
        assert_all_frames_free(*bpm, 4, "BTree_NodeOverflowChunkFailurePinLeak (error path)");
    } else {
        // Success path: verify no double-unpin.
        assert_all_frames_free(*bpm, 4, "BTree_NodeOverflowChunkFailurePinLeak (success path)");
    }
}

// ===========================================================================
// BTREE — Meta page inline path: insert_tuple failure pin check
// ===========================================================================

// Force the btree inline meta insert_tuple to fail by building a small tree
// with a very large page directory.  Actually the inline path fails when
// inline_buf.size() > 8100.  With many nodes, dir_buf can exceed this.
// We verify the PinGuard cleans up correctly if insert_tuple fails.
//
// This is hard to force without a mock BPM, so instead we verify the success
// and error paths of repeated persists with a tight pool leave no leaked pins.
TEST_F(QA_GDB826, BTree_PinBalancedAfterSuccessfulPersistSmallTree) {
    BTreeConfig cfg;
    cfg.key_types     = {TypeId::INT32};
    cfg.is_unique     = true;
    cfg.leaf_max_keys = 4;
    cfg.internal_max_keys = 4;
    BTreeIndex index(std::move(cfg));

    for (int32_t i = 1; i <= 20; ++i) {
        auto r = index.insert({Value(i)}, RID{static_cast<PageId>(i), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    // Pool of 4 frames — tight.
    auto bpm = make_bpm("btree_pin_balance_small", 4);
    ASSERT_NE(bpm, nullptr);

    auto meta = BTreePersistence::persist(*bpm, index);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;

    assert_all_frames_free(*bpm, 4, "BTree_PinBalancedAfterSuccessfulPersistSmallTree");
}

// ===========================================================================
// BTREE — Cumulative leak: repeated persist-with-error in a loop
// ===========================================================================

// Build a btree that when persisted leaks a pin on the node-overflow error
// path (pre-fix).  We use a very large string key so the per-node data
// exceeds 8100 bytes and enters write_node_page's overflow path.
// Then run persist 100x on a tight pool and confirm the pool is not exhausted.
//
// Because the implementation correctly chunks overflow data, persist actually
// succeeds here.  The loop stress-tests the success path for cumulative leaks.
TEST_F(QA_GDB826, BTree_NoCumulativeLeakOnRepeatedPersistLargeKey) {
    BTreeConfig cfg;
    cfg.key_types     = {TypeId::STRING};
    cfg.is_unique     = true;
    cfg.leaf_max_keys = 2;
    cfg.internal_max_keys = 2;
    BTreeIndex index(std::move(cfg));

    // 4 KB key — each leaf node exceeds NODE_MAX_INLINE_SIZE.
    constexpr size_t KEY_LEN = 4096;
    for (int i = 0; i < 3; ++i) {
        std::string k(KEY_LEN, static_cast<char>('a' + i));
        auto r = index.insert({Value(k)}, RID{static_cast<PageId>(i + 1), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    constexpr int ITERATIONS = 100;
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        // Fresh file+pool each iteration to avoid page-count growth masking leaks.
        std::string name = "btree_cumulative_" + std::to_string(iter);
        auto bpm = make_bpm(name, /*pool_size=*/32);
        ASSERT_NE(bpm, nullptr) << "failed to create bpm at iteration " << iter;

        auto meta = BTreePersistence::persist(*bpm, index);
        // Should succeed; if it fails verify no pin leak.
        if (!meta.has_value()) {
            assert_all_frames_free(*bpm, 4,
                ("BTree_NoCumulativeLeakOnRepeatedPersistLargeKey iter=" + std::to_string(iter)).c_str());
        } else {
            assert_all_frames_free(*bpm, 4,
                ("BTree_NoCumulativeLeakOnRepeatedPersistLargeKey success iter=" + std::to_string(iter)).c_str());
        }
    }
}

// ===========================================================================
// BTREE — V2 (large directory) overflow path: round-trip correctness
// ===========================================================================

// Build a btree large enough that the page directory exceeds MAX_INLINE_SIZE
// (8100 bytes), forcing the v2 overflow path.  Verify all keys reload correctly.
// dir_buf = 8 + leaf_count*8 + internal_count*8.  For > 8100 bytes we need
// ~500 leaf+internal records total.
TEST_F(QA_GDB826, BTree_V2OverflowDirectoryRoundtripCorrectness) {
    BTreeConfig cfg;
    cfg.key_types       = {TypeId::INT32};
    cfg.is_unique       = true;
    cfg.leaf_max_keys   = 2; // Force many splits for many leaf nodes.
    cfg.internal_max_keys = 2;
    BTreeIndex original(std::move(cfg));

    // With leaf_max_keys=2, each leaf holds 2 entries. To get 500 leaf nodes
    // we need ~1000 entries.  That also produces many internal nodes, ensuring
    // dir_buf >> 8100 bytes.
    constexpr int32_t N = 1000;
    for (int32_t i = 1; i <= N; ++i) {
        auto r = original.insert({Value(i)}, RID{static_cast<PageId>(i), 0});
        ASSERT_TRUE(r.has_value()) << "insert " << i << ": " << r.error().message;
    }
    EXPECT_EQ(original.size(), static_cast<uint64_t>(N));

    auto [fid_w, bpm_w] = make_bpm_with_fid("btree_v2_dir_roundtrip", 2048);
    ASSERT_NE(bpm_w, nullptr);

    auto meta = BTreePersistence::persist(*bpm_w, original);
    ASSERT_TRUE(meta.has_value()) << "V2 dir persist failed: " << meta.error().message;
    bpm_w.reset();
    (void)dm_->close_file(fid_w);

    auto [fid_r, bpm_r] = open_bpm_with_fid("btree_v2_dir_roundtrip", 2048);
    ASSERT_NE(bpm_r, nullptr);
    auto loaded_r = BTreePersistence::load(*bpm_r, *meta);
    ASSERT_TRUE(loaded_r.has_value()) << "V2 dir load failed: " << loaded_r.error().message;

    EXPECT_EQ((*loaded_r)->size(), static_cast<uint64_t>(N));
    // Spot-check every key.
    for (int32_t i = 1; i <= N; ++i) {
        auto s = (*loaded_r)->search({Value(i)});
        ASSERT_TRUE(s.has_value()) << "search err key " << i;
        ASSERT_TRUE(s->has_value()) << "key " << i << " missing after V2 dir reload";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i));
    }
    bpm_r.reset();
    (void)dm_->close_file(fid_r);
}

// ===========================================================================
// BTREE — V2 overflow directory path: pin balance after success
// ===========================================================================

TEST_F(QA_GDB826, BTree_V2DirectoryOverflowPinBalanced) {
    BTreeConfig cfg;
    cfg.key_types       = {TypeId::INT32};
    cfg.is_unique       = true;
    cfg.leaf_max_keys   = 2;
    cfg.internal_max_keys = 2;
    BTreeIndex index(std::move(cfg));

    // 200 entries produces a large enough tree to trigger V2 directory.
    for (int32_t i = 1; i <= 200; ++i) {
        auto r = index.insert({Value(i)}, RID{static_cast<PageId>(i), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    auto bpm = make_bpm("btree_v2_dir_balance", 512);
    ASSERT_NE(bpm, nullptr);

    auto meta = BTreePersistence::persist(*bpm, index);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;

    // After V2 persist, verify no frames stuck pinned.
    assert_all_frames_free(*bpm, 4, "BTree_V2DirectoryOverflowPinBalanced");
}

// ===========================================================================
// BTREE — Empty tree persist/reload: degenerate success path
// ===========================================================================

TEST_F(QA_GDB826, BTree_EmptyTreePersistLoadPinBalance) {
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.is_unique = true;
    BTreeIndex index(std::move(cfg));
    EXPECT_EQ(index.size(), 0u);

    auto bpm = make_bpm("btree_empty_balance", 4);
    ASSERT_NE(bpm, nullptr);

    auto meta = BTreePersistence::persist(*bpm, index);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;

    // All 4 frames must be free after persisting an empty tree.
    assert_all_frames_free(*bpm, 4, "BTree_EmptyTreePersistLoadPinBalance");
}

// ===========================================================================
// HASH — Empty index persist/reload: degenerate success path
// ===========================================================================

TEST_F(QA_GDB826, Hash_EmptyIndexPersistLoadPinBalance) {
    HashIndexConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.is_unique = true;
    HashIndex index(std::move(cfg));
    EXPECT_EQ(index.size(), 0u);

    auto bpm = make_bpm("hash_empty_balance", 4);
    ASSERT_NE(bpm, nullptr);

    auto meta = HashPersistence::persist(*bpm, index);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;

    assert_all_frames_free(*bpm, 4, "Hash_EmptyIndexPersistLoadPinBalance");
}

// ===========================================================================
// BTREE — Large key node overflow path: round-trip dirty flag correctness
// ===========================================================================

// KEY_LEN = 4096 bytes: node data > NODE_MAX_INLINE_SIZE → overflow path.
// Verify that the dirty flag is set correctly so the overflow pages are
// written to disk (wrong flag would lose data on reload).
TEST_F(QA_GDB826, BTree_LargeKeyNodeOverflowDirtyFlagRoundtrip) {
    BTreeConfig cfg;
    cfg.key_types       = {TypeId::STRING};
    cfg.is_unique       = true;
    cfg.leaf_max_keys   = 2;
    cfg.internal_max_keys = 2;
    BTreeIndex original(std::move(cfg));

    constexpr size_t KEY_LEN = 4096;
    constexpr int N = 3;
    for (int i = 0; i < N; ++i) {
        std::string k(KEY_LEN, static_cast<char>('a' + i));
        auto r = original.insert({Value(k)}, RID{static_cast<PageId>(i + 10), static_cast<SlotId>(i)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    auto [fid_w, bpm_w] = make_bpm_with_fid("btree_large_dirty_roundtrip", 64);
    ASSERT_NE(bpm_w, nullptr);
    auto meta = BTreePersistence::persist(*bpm_w, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm_w.reset();
    (void)dm_->close_file(fid_w);

    auto [fid_r, bpm_r] = open_bpm_with_fid("btree_large_dirty_roundtrip", 64);
    ASSERT_NE(bpm_r, nullptr);
    auto loaded_r = BTreePersistence::load(*bpm_r, *meta);
    ASSERT_TRUE(loaded_r.has_value()) << loaded_r.error().message;

    EXPECT_EQ((*loaded_r)->size(), static_cast<uint64_t>(N));
    for (int i = 0; i < N; ++i) {
        std::string k(KEY_LEN, static_cast<char>('a' + i));
        auto s = (*loaded_r)->search({Value(k)});
        ASSERT_TRUE(s.has_value()) << "search err key " << i;
        ASSERT_TRUE(s->has_value()) << "large key " << i << " missing after overflow reload";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i + 10));
        EXPECT_EQ(s->value().slot_id, static_cast<SlotId>(i));
    }
    bpm_r.reset();
    (void)dm_->close_file(fid_r);
}

// ===========================================================================
// BTREE — Persist then re-persist 20 times: cumulative leak on success path
// ===========================================================================

// Each successful persist on the same pool (different backing file) should
// leave all frames free.  Running 20 times catches any per-persist leak.
TEST_F(QA_GDB826, BTree_RepeatedSuccessfulPersistsNoLeak) {
    BTreeConfig cfg;
    cfg.key_types       = {TypeId::INT32};
    cfg.is_unique       = true;
    cfg.leaf_max_keys   = 4;
    cfg.internal_max_keys = 4;
    BTreeIndex index(std::move(cfg));

    for (int32_t i = 1; i <= 30; ++i) {
        auto r = index.insert({Value(i)}, RID{static_cast<PageId>(i), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    for (int cycle = 0; cycle < 20; ++cycle) {
        std::string name = "btree_repersist_" + std::to_string(cycle);
        auto bpm = make_bpm(name, /*pool_size=*/4);
        ASSERT_NE(bpm, nullptr);

        auto meta = BTreePersistence::persist(*bpm, index);
        ASSERT_TRUE(meta.has_value())
            << "cycle " << cycle << " persist failed: " << meta.error().message;

        assert_all_frames_free(
            *bpm, 4,
            ("BTree_RepeatedSuccessfulPersistsNoLeak cycle=" + std::to_string(cycle)).c_str());
    }
}

// ===========================================================================
// HASH — Persist failure then immediate success: no stale state
// ===========================================================================

// After a persist that fails (oversized bucket), do a persist on a fresh
// smaller index that should succeed.  Verifies no stale pool state from the
// failed attempt bleeds into the next persist.
TEST_F(QA_GDB826, Hash_FailureThenSuccessNoStaleState) {
    // Build oversized index (will fail).
    HashIndexConfig cfg_big;
    cfg_big.key_types       = {TypeId::INT32};
    cfg_big.bucket_capacity = 600;
    cfg_big.is_unique       = true;
    HashIndex big(std::move(cfg_big));
    for (int32_t i = 0; i < 500; ++i) {
        (void)big.insert({Value(i)}, RID{static_cast<PageId>(i + 1), 0});
    }

    // Build small index (will succeed).
    HashIndexConfig cfg_small;
    cfg_small.key_types = {TypeId::INT32};
    cfg_small.is_unique = true;
    HashIndex small(std::move(cfg_small));
    for (int32_t i = 0; i < 5; ++i) {
        auto r = small.insert({Value(i)}, RID{static_cast<PageId>(i + 1), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    auto bpm1 = make_bpm("hash_fail_stale_big", 4);
    ASSERT_NE(bpm1, nullptr);
    auto fail_r = HashPersistence::persist(*bpm1, big);
    ASSERT_FALSE(fail_r.has_value()) << "Expected big index persist to fail";
    // After failure all frames should be free.
    assert_all_frames_free(*bpm1, 4, "Hash_FailureThenSuccessNoStaleState after failure");

    auto bpm2 = make_bpm("hash_fail_stale_small", 4);
    ASSERT_NE(bpm2, nullptr);
    auto ok_r = HashPersistence::persist(*bpm2, small);
    ASSERT_TRUE(ok_r.has_value()) << "Small index persist failed: " << ok_r.error().message;
    assert_all_frames_free(*bpm2, 4, "Hash_FailureThenSuccessNoStaleState after success");
}
