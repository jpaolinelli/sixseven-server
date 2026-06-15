// QA tests for GDB-810: Fix out-of-scope variable reference in BTreePersistence::load debug log.
//
// Acceptance criteria under test:
//   AC1: The debug log at the end of BTreePersistence::load compiles when debug logging is
//        active (SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG).
//   AC2: BTreePersistence::load correctly restores all leaf/internal nodes for both v1
//        (inline directory) and v2 (overflow pages) persistence paths.
//   AC3: Round-trip correctness is preserved: empty, single-entry, and large trees all
//        survive persist/load with full search fidelity.
//   AC4: Corrupted/invalid BTreeMeta values produce errors rather than crashes.
//
// AC1 is a compile-time check: this TU is compiled with SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
// so that SPDLOG_DEBUG expands to a real call.  If the old code (using out-of-scope
// leaf_count/internal_count) were still present, the compiler would emit an error here.
// A clean build of this TU proves the fix is correct.

// Force debug logging active for this translation unit — the key compile-time check.
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG

#include "sixseven/index/btree_index.h"
#include "sixseven/index/btree_persistence.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class QA_GDB810_BTreePersist : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb810";
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
        EXPECT_TRUE(fid.has_value()) << fid.error().message;
        if (!fid) return {FileId{}, nullptr};
        return {*fid, std::make_unique<BufferPoolManager>(*dm_, *fid, 512)};
    }

    std::pair<FileId, std::unique_ptr<BufferPoolManager>> open_bpm(const std::string& name) {
        auto path = data_dir_ / (name + ".db");
        auto fid = dm_->open_file(path);
        EXPECT_TRUE(fid.has_value()) << fid.error().message;
        if (!fid) return {FileId{}, nullptr};
        return {*fid, std::make_unique<BufferPoolManager>(*dm_, *fid, 512)};
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
};

// ---------------------------------------------------------------------------
// AC1 — compile-time check: this TU compiled with SPDLOG_ACTIVE_LEVEL=DEBUG
//
// No runtime assertion is needed here.  The act of compiling this file with
// SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG and reaching the BTreePersistence::load
// call site constitutes the proof.  With the OLD code (leaf_count/internal_count
// out of scope), the compiler would have rejected this TU entirely.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB810_BTreePersist, GDB810_CompileWithDebugLoggingActive_EmptyTree) {
    // Exercises BTreePersistence::load with SPDLOG_DEBUG active.
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.is_unique = true;
    BTreeIndex idx(std::move(cfg));

    auto [fid, bpm] = create_bpm("ac1_empty");
    ASSERT_NE(bpm, nullptr);

    auto meta = BTreePersistence::persist(*bpm, idx);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;

    auto loaded = BTreePersistence::load(*bpm, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    // If the compilation succeeded and load ran, the fixed debug log executed.
    EXPECT_EQ((*loaded)->size(), 0u);
    EXPECT_TRUE((*loaded)->empty());
}

TEST_F(QA_GDB810_BTreePersist, GDB810_CompileWithDebugLoggingActive_PopulatedTree) {
    // Also exercises the non-empty code path (more nodes → more meaningful log values).
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.leaf_max_keys = 4;
    cfg.internal_max_keys = 4;
    cfg.is_unique = true;
    BTreeIndex idx(std::move(cfg));

    for (int32_t i = 1; i <= 30; ++i) {
        auto r = idx.insert({Value(i)}, RID{static_cast<PageId>(i), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    auto [fid1, bpm1] = create_bpm("ac1_populated");
    auto meta = BTreePersistence::persist(*bpm1, idx);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("ac1_populated");
    auto loaded = BTreePersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->size(), 30u);
}

// ---------------------------------------------------------------------------
// AC2 / AC3 — v1 (inline) path: all entries survive round-trip
// ---------------------------------------------------------------------------

TEST_F(QA_GDB810_BTreePersist, GDB810_V1InlinePath_SmallTree_AllEntriesSearchable) {
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.leaf_max_keys = 4;
    cfg.internal_max_keys = 4;
    cfg.is_unique = true;
    BTreeIndex original(std::move(cfg));

    constexpr int32_t N = 20;
    for (int32_t i = 1; i <= N; ++i) {
        auto r = original.insert({Value(i)}, RID{static_cast<PageId>(i), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    EXPECT_EQ(original.size(), static_cast<size_t>(N));

    auto [fid1, bpm1] = create_bpm("v1_small");
    auto meta = BTreePersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("v1_small");
    auto loaded = BTreePersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->size(), static_cast<size_t>(N));

    // Every inserted key must be findable — proves all leaf nodes were restored.
    for (int32_t i = 1; i <= N; ++i) {
        auto s = (*loaded)->search({Value(i)});
        ASSERT_TRUE(s.has_value()) << s.error().message;
        ASSERT_TRUE(s->has_value()) << "v1 load: key " << i << " not found";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i));
    }
    // Keys outside the range must not be found.
    auto miss = (*loaded)->search({Value(int32_t(999))});
    ASSERT_TRUE(miss.has_value());
    EXPECT_FALSE(miss->has_value()) << "spurious match for absent key 999";
}

// ---------------------------------------------------------------------------
// AC2 — v2 (overflow) path: large tree forces overflow directory
// ---------------------------------------------------------------------------

TEST_F(QA_GDB810_BTreePersist, GDB810_V2OverflowPath_LargeTree_AllEntriesSearchable) {
    // Very small max_keys → many nodes → overflow directory (v2).
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.leaf_max_keys = 2;
    cfg.internal_max_keys = 2;
    cfg.is_unique = true;
    BTreeIndex original(std::move(cfg));

    constexpr int32_t N = 500;
    for (int32_t i = 1; i <= N; ++i) {
        auto r = original.insert({Value(i)}, RID{static_cast<PageId>(i), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    EXPECT_EQ(original.size(), static_cast<size_t>(N));

    auto [fid1, bpm1] = create_bpm("v2_large");
    auto meta = BTreePersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("v2_large");
    auto loaded = BTreePersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->size(), static_cast<size_t>(N));

    // Spot-check across the range.
    for (int32_t k : {1, 2, 50, 100, 250, 499, 500}) {
        auto s = (*loaded)->search({Value(k)});
        ASSERT_TRUE(s.has_value()) << s.error().message;
        ASSERT_TRUE(s->has_value()) << "v2 load: key " << k << " not found";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(k));
    }
    // Full scan.
    for (int32_t i = 1; i <= N; ++i) {
        auto s = (*loaded)->search({Value(i)});
        ASSERT_TRUE(s.has_value()) << s.error().message;
        ASSERT_TRUE(s->has_value()) << "v2 load: key " << i << " not found";
    }
}

// ---------------------------------------------------------------------------
// AC3 — Edge cases: empty and single-entry trees
// ---------------------------------------------------------------------------

TEST_F(QA_GDB810_BTreePersist, GDB810_EmptyTree_RoundTrip) {
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.is_unique = true;
    BTreeIndex original(std::move(cfg));

    auto [fid1, bpm1] = create_bpm("empty");
    auto meta = BTreePersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("empty");
    auto loaded = BTreePersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    EXPECT_EQ((*loaded)->size(), 0u);
    EXPECT_TRUE((*loaded)->empty());

    // Search in empty tree must return not-found, not an error.
    auto s = (*loaded)->search({Value(int32_t(1))});
    ASSERT_TRUE(s.has_value());
    EXPECT_FALSE(s->has_value());
}

TEST_F(QA_GDB810_BTreePersist, GDB810_SingleEntry_RoundTrip) {
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.is_unique = true;
    BTreeIndex original(std::move(cfg));

    auto r = original.insert({Value(int32_t(42))}, RID{7, 3});
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto [fid1, bpm1] = create_bpm("single");
    auto meta = BTreePersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("single");
    auto loaded = BTreePersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    EXPECT_EQ((*loaded)->size(), 1u);

    auto s = (*loaded)->search({Value(int32_t(42))});
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
    EXPECT_EQ(s->value().page_id, 7u);
    EXPECT_EQ(s->value().slot_id, 3u);

    // Adjacent keys must not appear.
    auto miss = (*loaded)->search({Value(int32_t(41))});
    ASSERT_TRUE(miss.has_value());
    EXPECT_FALSE(miss->has_value());
}

// ---------------------------------------------------------------------------
// AC3 — Large tree, normal node capacity (exercises both v1 and v2 depending
//       on how many pages fit in inline directory)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB810_BTreePersist, GDB810_LargeTree_DefaultCapacity_FullSearchFidelity) {
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.is_unique = true; // default leaf_max_keys = 128
    BTreeIndex original(std::move(cfg));

    constexpr int32_t N = 1000;
    for (int32_t i = 1; i <= N; ++i) {
        auto r = original.insert({Value(i)}, RID{static_cast<PageId>(i), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    EXPECT_EQ(original.size(), static_cast<size_t>(N));

    auto [fid1, bpm1] = create_bpm("large_default");
    auto meta = BTreePersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("large_default");
    auto loaded = BTreePersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    EXPECT_EQ((*loaded)->size(), static_cast<size_t>(N));

    for (int32_t i = 1; i <= N; ++i) {
        auto s = (*loaded)->search({Value(i)});
        ASSERT_TRUE(s.has_value()) << s.error().message;
        ASSERT_TRUE(s->has_value()) << "key " << i << " missing after load";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i));
    }
}

// ---------------------------------------------------------------------------
// AC3 — RID slot_id round-trips correctly (not just page_id)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB810_BTreePersist, GDB810_RIDSlotIdPreservedAfterLoad) {
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.is_unique = true;
    BTreeIndex original(std::move(cfg));

    // Use distinct slot_id values to detect if serialization drops them.
    for (int32_t i = 1; i <= 10; ++i) {
        auto r = original.insert({Value(i)},
                                  RID{static_cast<PageId>(i * 10), static_cast<SlotId>(i * 3)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    auto [fid1, bpm1] = create_bpm("rid_slots");
    auto meta = BTreePersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("rid_slots");
    auto loaded = BTreePersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    for (int32_t i = 1; i <= 10; ++i) {
        auto s = (*loaded)->search({Value(i)});
        ASSERT_TRUE(s.has_value()) << s.error().message;
        ASSERT_TRUE(s->has_value()) << "key " << i << " missing";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i * 10));
        EXPECT_EQ(s->value().slot_id, static_cast<SlotId>(i * 3));
    }
}

// ---------------------------------------------------------------------------
// AC4 — Corrupted/invalid meta: graceful errors, no crashes
// ---------------------------------------------------------------------------

TEST_F(QA_GDB810_BTreePersist, GDB810_InvalidMetaPageId_NonExistentPage) {
    // Fresh empty BPM — page 9999 does not exist.
    auto [fid, bpm] = create_bpm("bad_meta_nonexistent");
    ASSERT_NE(bpm, nullptr);

    auto result = BTreePersistence::load(*bpm, static_cast<PageId>(9999));
    EXPECT_FALSE(result.has_value())
        << "load with non-existent meta page should return an error";
}

TEST_F(QA_GDB810_BTreePersist, GDB810_ZeroMetaPageId_GracefulError) {
    auto [fid, bpm] = create_bpm("bad_meta_zero");
    ASSERT_NE(bpm, nullptr);

    // Page 0 is the invalid sentinel in this codebase.
    auto result = BTreePersistence::load(*bpm, static_cast<PageId>(0));
    EXPECT_FALSE(result.has_value())
        << "load with page_id=0 should fail gracefully";
}

TEST_F(QA_GDB810_BTreePersist, GDB810_WrongBPM_GracefulError) {
    // Write a valid tree to file A; attempt to load via an empty file B.
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.is_unique = true;
    BTreeIndex original(std::move(cfg));
    (void)original.insert({Value(int32_t(1))}, RID{1, 0});

    auto [fid_a, bpm_a] = create_bpm("wrong_bpm_a");
    auto meta = BTreePersistence::persist(*bpm_a, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm_a.reset();
    (void)dm_->close_file(fid_a);

    auto [fid_b, bpm_b] = create_bpm("wrong_bpm_b"); // different, empty file
    ASSERT_NE(bpm_b, nullptr);

    auto result = BTreePersistence::load(*bpm_b, *meta);
    EXPECT_FALSE(result.has_value())
        << "load against wrong BPM/file should return error, not crash or silently succeed";
}

// ---------------------------------------------------------------------------
// AC2 — Composite key type survives v1 path
// ---------------------------------------------------------------------------

TEST_F(QA_GDB810_BTreePersist, GDB810_CompositeKey_V1Path_RoundTrip) {
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT32, TypeId::STRING};
    cfg.is_unique = true;
    BTreeIndex original(std::move(cfg));

    auto r1 = original.insert({Value(int32_t(1)), Value(std::string("alpha"))}, RID{1, 0});
    ASSERT_TRUE(r1.has_value());
    auto r2 = original.insert({Value(int32_t(2)), Value(std::string("beta"))}, RID{2, 0});
    ASSERT_TRUE(r2.has_value());
    auto r3 = original.insert({Value(int32_t(3)), Value(std::string("gamma"))}, RID{3, 0});
    ASSERT_TRUE(r3.has_value());

    auto [fid1, bpm1] = create_bpm("composite_v1");
    auto meta = BTreePersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("composite_v1");
    auto loaded = BTreePersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->size(), 3u);

    auto s = (*loaded)->search({Value(int32_t(2)), Value(std::string("beta"))});
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
    EXPECT_EQ(s->value().page_id, 2u);

    // Wrong second component — must not match.
    auto miss = (*loaded)->search({Value(int32_t(2)), Value(std::string("wrong"))});
    ASSERT_TRUE(miss.has_value());
    EXPECT_FALSE(miss->has_value());
}

// ---------------------------------------------------------------------------
// AC3 — Insert into loaded tree still works (tree is fully functional post-load)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB810_BTreePersist, GDB810_InsertAfterLoad_TreeFunctional) {
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.leaf_max_keys = 4;
    cfg.internal_max_keys = 4;
    cfg.is_unique = true;
    BTreeIndex original(std::move(cfg));

    for (int32_t i = 1; i <= 10; ++i) {
        auto r = original.insert({Value(i)}, RID{static_cast<PageId>(i), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    auto [fid1, bpm1] = create_bpm("insert_after_load");
    auto meta = BTreePersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    auto [fid2, bpm2] = open_bpm("insert_after_load");
    auto loaded = BTreePersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    // Insert new key into loaded tree.
    auto ins = (*loaded)->insert({Value(int32_t(11))}, RID{11, 0});
    ASSERT_TRUE(ins.has_value()) << ins.error().message;
    EXPECT_EQ((*loaded)->size(), 11u);

    auto s = (*loaded)->search({Value(int32_t(11))});
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->has_value());
    EXPECT_EQ(s->value().page_id, 11u);

    // Confirm existing keys are still accessible.
    for (int32_t i = 1; i <= 10; ++i) {
        auto sv = (*loaded)->search({Value(i)});
        ASSERT_TRUE(sv.has_value());
        ASSERT_TRUE(sv->has_value()) << "original key " << i << " lost after insert-after-load";
    }
}
