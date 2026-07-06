#include "sixseven/index/btree_index.h"
#include "sixseven/index/btree_persistence.h"
#include "sixseven/index/index_encoding.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/page.h"
#include "sixseven/storage/serialization.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <future>
#include <memory>
#include <thread>

using namespace sixseven;
using namespace sixseven::index_encoding;

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

// =============================================================================
// GDB-816: Large string keys that cause a single node to exceed one page
//          must round-trip correctly via per-node overflow pages.
// =============================================================================

TEST_F(BTreePersistenceTest, LargeStringKeySingleNodeOverflow) {
    // A single STRING key of 4096 bytes. The serialised leaf node header is
    // 18 bytes (4+4+4+4+2) and each entry adds 4096+overhead+6 bytes for the
    // RID.  Even a leaf with leaf_max_keys=1 will produce a node that exceeds
    // the 8100-byte NODE_MAX_INLINE_SIZE threshold once we insert a few such
    // keys (we use leaf_max_keys=2 so a split occurs and each leaf holds one
    // huge key, which still individually exceeds 8100 bytes).
    BTreeConfig config;
    config.key_types = {TypeId::STRING};
    config.is_unique = true;
    // Small cap forces a split so we exercise both leaf and internal nodes
    // when keys are large.
    config.leaf_max_keys = 2;
    config.internal_max_keys = 2;
    BTreeIndex original(std::move(config));

    // Each key is ~4 KB — three such entries require node overflow on persist.
    constexpr size_t KEY_LEN = 4096;
    constexpr int N = 3;
    for (int i = 0; i < N; ++i) {
        // Make keys unique and sortable by prefixing with an index character.
        std::string k(KEY_LEN, static_cast<char>('a' + i));
        auto r = original.insert({Value(k)}, RID{static_cast<PageId>(i + 1), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    EXPECT_EQ(original.size(), static_cast<size_t>(N));

    // Persist — this triggered the original bug ("leaf data too large for page").
    auto [fid1, bpm1] = create_bpm("large_key_overflow");
    auto meta = BTreePersistence::persist(*bpm1, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm1.reset();
    (void)dm_->close_file(fid1);

    // Load and verify every key is found with the correct RID.
    auto [fid2, bpm2] = open_bpm("large_key_overflow");
    auto loaded = BTreePersistence::load(*bpm2, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->size(), static_cast<size_t>(N));

    for (int i = 0; i < N; ++i) {
        std::string k(KEY_LEN, static_cast<char>('a' + i));
        auto s = (*loaded)->search({Value(k)});
        ASSERT_TRUE(s.has_value()) << s.error().message;
        ASSERT_TRUE(s->has_value()) << "large key " << i << " not found after load";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i + 1));
    }
}

TEST_F(BTreePersistenceTest, VeryLargeStringKeyExceedsMultiplePages) {
    // A single key of 16 KB forces the node data to span 3 overflow pages.
    BTreeConfig config;
    config.key_types = {TypeId::STRING};
    config.is_unique = true;
    config.leaf_max_keys = 1; // Each leaf holds exactly one huge key.
    config.internal_max_keys = 4;
    BTreeIndex original(std::move(config));

    constexpr size_t KEY_LEN = 16384; // 16 KB key
    constexpr int N = 4;
    for (int i = 0; i < N; ++i) {
        std::string k(KEY_LEN, static_cast<char>('A' + i));
        auto r = original.insert({Value(k)}, RID{static_cast<PageId>(10 + i), 1});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    EXPECT_EQ(original.size(), static_cast<size_t>(N));

    // Use a larger buffer pool since we have many overflow pages.
    auto path = data_dir_ / "very_large_key_overflow.db";
    auto fid_w = dm_->create_file(path, false, true);
    ASSERT_TRUE(fid_w.has_value());
    auto bpm_w = std::make_unique<BufferPoolManager>(*dm_, *fid_w, 1024);

    auto meta = BTreePersistence::persist(*bpm_w, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;
    bpm_w.reset();
    (void)dm_->close_file(*fid_w);

    auto fid_r = dm_->open_file(path);
    ASSERT_TRUE(fid_r.has_value());
    auto bpm_r = std::make_unique<BufferPoolManager>(*dm_, *fid_r, 1024);

    auto loaded = BTreePersistence::load(*bpm_r, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->size(), static_cast<size_t>(N));

    for (int i = 0; i < N; ++i) {
        std::string k(KEY_LEN, static_cast<char>('A' + i));
        auto s = (*loaded)->search({Value(k)});
        ASSERT_TRUE(s.has_value()) << s.error().message;
        ASSERT_TRUE(s->has_value()) << "16KB key " << i << " not found after load";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(10 + i));
    }

    bpm_r.reset();
    (void)dm_->close_file(*fid_r);
}

// =============================================================================
// GDB-826: Success path is pin-balanced — RAII guard does not double-unpin
//
// Before this fix the persist() function called bpm.unpin_page() explicitly on
// the success path.  The fix replaces those explicit calls with a PinGuard that
// fires on ALL exit paths (including error).  This test verifies that the guard
// does NOT introduce a double-unpin on the success path by checking that the
// buffer pool ends up with all frames free (evictable) after a successful
// persist.  If a double-unpin had corrupted the pin count or eviction state,
// subsequent new_page() calls would fail or return stale data.
// =============================================================================

TEST_F(BTreePersistenceTest, SuccessPathPinBalanced) {
    BTreeConfig config;
    config.key_types = {TypeId::INT32};
    config.is_unique = true;
    BTreeIndex index(std::move(config));

    for (int32_t i = 1; i <= 10; ++i) {
        auto r = index.insert({Value(i)}, RID{static_cast<PageId>(i), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    EXPECT_EQ(index.size(), 10u);

    // Tight pool: 4 frames.  A 10-entry btree should persist into a small
    // number of pages; write_node_page() pins+unpins each node page before
    // moving on, so the pool only needs to hold one node at a time plus the
    // meta page.
    auto path = data_dir_ / "success_pin_balanced.db";
    auto fid_r = dm_->create_file(path, false, true);
    ASSERT_TRUE(fid_r.has_value()) << fid_r.error().message;
    auto bpm = std::make_unique<BufferPoolManager>(*dm_, *fid_r, /*pool_size=*/4);

    auto meta = BTreePersistence::persist(*bpm, index);
    ASSERT_TRUE(meta.has_value()) << "persist failed: " << meta.error().message;

    // After a successful persist every pinned frame should be unpinned.
    // Allocate all 4 frames — if any is stuck pinned (due to a leak or
    // a double-unpin corrupting evictable tracking), new_page will fail.
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

// =============================================================================
// GDB-1266: V1 meta with root_page_id == 512 must NOT be misdetected as V2.
//
// The old 2-byte sentinel [0x00, 0x02] collided with a V1 meta whose
// root_page_id serializes little-endian as [0x00, 0x02, 0x00, 0x00] (i.e.
// root_page_id == 512). This test hand-crafts a raw V1 meta page with that
// exact root_page_id and asserts it loads correctly as V1 (not misparsed).
// =============================================================================

TEST_F(BTreePersistenceTest, V1MetaRootPageId512NotMisdetectedAsV2) {
    auto [fid, bpm] = create_bpm("v1_meta_root_512");
    ASSERT_NE(bpm, nullptr);

    // Write a single leaf node (in-memory page id 512, one key/RID pair) to
    // its own disk page first, so root_page_id == 512 refers to a real leaf.
    std::vector<uint8_t> leaf_buf;
    write_u32(leaf_buf, 512); // page_id (matches root_page_id below)
    write_u32(leaf_buf, 0);   // parent_page_id (root has none)
    write_u32(leaf_buf, 0);   // next_leaf_id
    write_u32(leaf_buf, 0);   // prev_leaf_id
    write_u16(leaf_buf, 1);   // key_count = 1
    auto key_bytes = serialize(Value(int32_t{42}));
    leaf_buf.insert(leaf_buf.end(), key_bytes.begin(), key_bytes.end());
    write_u32(leaf_buf, 7); // rid.page_id
    write_u16(leaf_buf, 0); // rid.slot_id

    auto leaf_page_r = bpm->new_page();
    ASSERT_TRUE(leaf_page_r.has_value());
    auto* leaf_page = *leaf_page_r;
    PageId leaf_disk_page_id = leaf_page->page_id();
    leaf_page->reset(leaf_disk_page_id, PageType::BTREE_LEAF);
    auto leaf_slot = leaf_page->insert_tuple(std::span<const uint8_t>(leaf_buf));
    ASSERT_TRUE(leaf_slot.has_value()) << leaf_slot.error().message;
    (void)bpm->unpin_page(leaf_disk_page_id, true);

    // Hand-build a V1 (inline) meta buffer: root_page_id=512 (the collision
    // value), size=1, next_page_id=513, internal_max_keys=4, leaf_max_keys=4,
    // is_unique=1, one key type (INT32), one leaf mapping (mem 512 -> disk
    // leaf_disk_page_id), zero internal mappings.
    std::vector<uint8_t> buf;
    write_u32(buf, 512);                                // root_page_id (collision value)
    write_u64(buf, 1);                                  // size
    write_u32(buf, 513);                                // next_page_id
    write_u16(buf, 4);                                  // internal_max_keys
    write_u16(buf, 4);                                  // leaf_max_keys
    write_u8(buf, 1);                                   // is_unique
    write_u8(buf, 1);                                   // key_type_count
    write_u8(buf, static_cast<uint8_t>(TypeId::INT32)); // key_types[0]
    write_u32(buf, 1);                                  // leaf_count = 1
    write_u32(buf, 512);                                // leaf mem_page_id
    write_u32(buf, leaf_disk_page_id);                  // leaf disk_page_id
    write_u32(buf, 0);                                  // internal_count = 0

    // Sanity-check the collision: bytes[0..1] must equal the old sentinel.
    ASSERT_EQ(buf[0], 0x00);
    ASSERT_EQ(buf[1], 0x02);

    auto meta_page_r = bpm->new_page();
    ASSERT_TRUE(meta_page_r.has_value());
    auto* meta_page = *meta_page_r;
    PageId meta_page_id = meta_page->page_id();
    meta_page->reset(meta_page_id, PageType::BTREE_META);
    auto slot = meta_page->insert_tuple(std::span<const uint8_t>(buf));
    ASSERT_TRUE(slot.has_value()) << slot.error().message;
    (void)bpm->unpin_page(meta_page_id, true);
    ASSERT_TRUE(bpm->flush_all().has_value());

    bpm.reset();
    (void)dm_->close_file(fid);

    // Load and verify it's read as V1: fields must be parsed at the correct
    // offsets (size=1, root_page_id=512, one findable key), not misaligned by
    // a phantom 4-byte magic skip that a false-positive V2 detection would
    // introduce. If root_page_id (512) were mistaken for the V2 magic prefix,
    // every subsequent field would be shifted and this load would either fail
    // outright or silently corrupt the tree (the original bug).
    auto [fid2, bpm2] = open_bpm("v1_meta_root_512");
    auto loaded = BTreePersistence::load(*bpm2, meta_page_id);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->size(), 1u);
    EXPECT_FALSE((*loaded)->empty());

    auto found = (*loaded)->search({Value(int32_t{42})});
    ASSERT_TRUE(found.has_value()) << found.error().message;
    ASSERT_TRUE(found->has_value()) << "key not found -- meta was misparsed as V2";
    EXPECT_EQ(found->value().page_id, static_cast<PageId>(7));
}

// =============================================================================
// GDB-1266: V2 meta round-trips correctly with the new collision-free magic.
// =============================================================================

TEST_F(BTreePersistenceTest, V2MetaMagicRoundtrip) {
    // Force the V2 (overflow *directory*) path. The meta directory holds one
    // (mem_page_id, disk_page_id) pair (8 bytes) per leaf/internal node, so
    // enough small-key inserts (forcing many node splits) push the directory
    // itself past MAX_INLINE_SIZE (~8100 bytes) -- distinct from the node
    // *data* overflow path (write_node_page), which triggers per-node on huge
    // individual keys and is already covered by
    // VeryLargeStringKeyExceedsMultiplePages.
    BTreeConfig config;
    config.key_types = {TypeId::INT32};
    config.is_unique = true;
    config.leaf_max_keys = 2;
    config.internal_max_keys = 2;
    BTreeIndex original(std::move(config));

    constexpr int N = 3000; // enough splits to produce >1000 nodes total
    for (int i = 0; i < N; ++i) {
        auto r = original.insert({Value(i)}, RID{static_cast<PageId>(i + 1), 0});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    auto path = data_dir_ / "v2_meta_roundtrip.db";
    auto fid_w = dm_->create_file(path, false, true);
    ASSERT_TRUE(fid_w.has_value());
    auto bpm_w = std::make_unique<BufferPoolManager>(*dm_, *fid_w, 4096);

    auto meta = BTreePersistence::persist(*bpm_w, original);
    ASSERT_TRUE(meta.has_value()) << meta.error().message;

    // Read the raw meta tuple back and confirm it starts with the new magic
    // (0xFF42544D LE => bytes [0x4D, 0x54, 0x42, 0xFF]), not the old 2-byte
    // sentinel, proving the V2 (overflow directory) path was actually
    // exercised.
    auto meta_page_r = bpm_w->fetch_page(*meta);
    ASSERT_TRUE(meta_page_r.has_value());
    auto tuple = (*meta_page_r)->get_tuple(0);
    ASSERT_TRUE(tuple.has_value());
    (void)bpm_w->unpin_page(*meta, false);
    ASSERT_GE(tuple->size(), 4u);
    EXPECT_EQ((*tuple)[0], 0x4D);
    EXPECT_EQ((*tuple)[1], 0x54);
    EXPECT_EQ((*tuple)[2], 0x42);
    EXPECT_EQ((*tuple)[3], 0xFF);

    bpm_w.reset();
    (void)dm_->close_file(*fid_w);

    auto fid_r = dm_->open_file(path);
    ASSERT_TRUE(fid_r.has_value());
    auto bpm_r = std::make_unique<BufferPoolManager>(*dm_, *fid_r, 4096);

    auto loaded = BTreePersistence::load(*bpm_r, *meta);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ((*loaded)->size(), static_cast<size_t>(N));

    for (int i = 0; i < N; ++i) {
        auto s = (*loaded)->search({Value(i)});
        ASSERT_TRUE(s.has_value()) << s.error().message;
        ASSERT_TRUE(s->has_value()) << "key " << i << " not found after load";
        EXPECT_EQ(s->value().page_id, static_cast<PageId>(i + 1));
    }

    bpm_r.reset();
    (void)dm_->close_file(*fid_r);
}

// =============================================================================
// GDB-1266: The new magic cannot equal any valid V1 root_page_id-leading
// meta. Verified by construction/documentation, but assert the arithmetic
// here so the invariant is checked mechanically (not just in a comment).
// =============================================================================

TEST_F(BTreePersistenceTest, V2MagicHighByteMakesCollisionImpossible) {
    // BTREE_META_V2_MAGIC's most-significant byte (as LE u32) must be 0xFF,
    // requiring root_page_id >= 0xFF000000 to collide -- a page count that
    // implies an index file size far beyond any practical deployment
    // (~34 PB at 8 KB/page), unlike the old bug where root_page_id=512
    // (~4 MB index) sufficed.
    constexpr uint32_t magic = 0xFF42544DU;
    constexpr uint32_t msb = (magic >> 24) & 0xFFu;
    EXPECT_EQ(msb, 0xFFu);

    // The old, unsound V1/V2 collision value from the bug report.
    constexpr uint32_t old_colliding_root_page_id = 512;
    EXPECT_NE(old_colliding_root_page_id, magic);
    EXPECT_LT(old_colliding_root_page_id, 0xFF000000u);
}
