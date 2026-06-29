// QA adversarial tests for GDB-984: Per-row index maintenance API in IndexManager.
//
// Focus areas:
//   1. Ordinal-out-of-bounds silent truncation for composite keys
//   2. Duplicate HNSW insert for the same RID produces orphaned node
//   3. BM25: null-text skip + subsequent non-null insert still indexed
//   4. HNSW duplicate vector removal targets the correct node
//   5. Error propagation: first sub-index failure propagates (no swallow)
//   6. Real insert_entry / remove_entry on IndexManager -- not just stub
//   7. Concurrent insert_entry + remove_entry on real IndexManager (no crash / UB)

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/index/bm25_index.h"
#include "sixseven/index/btree_index.h"
#include "sixseven/index/btree_key.h"
#include "sixseven/index/hash_index.h"
#include "sixseven/index/rid.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/hnsw_index.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static RID qa_rid(uint32_t page, uint16_t slot) {
    return RID{page, slot};
}

// ===========================================================================
// QA_GDB984_BtreeCompositeKey
// Tests the composite-key (multi-ordinal) code path.
// The production code has: "if (ordinal < values.size()) key.push_back(...)".
// A composite key target with ordinal [0, 5] on a 3-value row silently
// produces a 1-element key instead of a 2-element key.  This test verifies
// that behavior is observable (i.e., the test is NOT vacuous for the normal
// case) and documents the gap.
// ===========================================================================

class QA_GDB984_BtreeCompositeKey : public ::testing::Test {
protected:
    void SetUp() override {
        // Two-column INT32 key (ordinals 0 and 1).
        BTreeConfig cfg;
        cfg.key_types = {TypeId::INT32, TypeId::INT32};
        cfg.is_unique = false;
        btree_ = std::make_unique<BTreeIndex>(std::move(cfg));
    }

    std::unique_ptr<BTreeIndex> btree_;
};

TEST_F(QA_GDB984_BtreeCompositeKey, TwoColumnKeyInsertSearchRemove) {
    BtreeMaintenanceTarget tgt;
    tgt.index = btree_.get();
    tgt.key_column_ordinals = {0, 1}; // composite: col0=10, col1=20

    auto rid = qa_rid(1, 0);
    std::vector<Value> values = {Value(int32_t{10}), Value(int32_t{20})};

    // Build key and insert (mirrors insert_entry_btree logic).
    KeyType key;
    for (size_t ord : tgt.key_column_ordinals) {
        if (ord < values.size()) {
            key.push_back(values[ord]);
        }
    }
    ASSERT_EQ(key.size(), 2u) << "Composite key must have 2 elements";
    ASSERT_TRUE(tgt.index->insert(key, rid).has_value());

    // Correct search must use full composite key.
    auto found = btree_->search({Value(int32_t{10}), Value(int32_t{20})});
    ASSERT_TRUE(found.has_value());
    ASSERT_TRUE(found->has_value()) << "Expected to find inserted composite key";
    EXPECT_EQ(**found, rid);

    // Remove with full composite key.
    KeyType rm_key = {Value(int32_t{10}), Value(int32_t{20})};
    ASSERT_TRUE(tgt.index->remove(rm_key, rid).has_value());

    auto after = btree_->search({Value(int32_t{10}), Value(int32_t{20})});
    ASSERT_TRUE(after.has_value());
    EXPECT_FALSE(after->has_value()) << "Key must not be present after remove";
}

TEST_F(QA_GDB984_BtreeCompositeKey, OrdinalOutOfBoundsSilentlyTruncatesKey) {
    // This test documents the gap: if key_column_ordinals references an ordinal
    // beyond the values vector, the key is silently shorter.  A 2-ordinal target
    // with ordinals [0, 99] on a 2-value row produces a 1-element key.
    BtreeMaintenanceTarget tgt;
    tgt.index = btree_.get(); // expects 2-column INT32 key
    tgt.key_column_ordinals = {0, 99}; // ordinal 99 is out-of-bounds

    auto rid = qa_rid(2, 0);
    std::vector<Value> values = {Value(int32_t{7}), Value(int32_t{8})};

    // Mirror the production path.
    KeyType key;
    for (size_t ord : tgt.key_column_ordinals) {
        if (ord < values.size()) {
            key.push_back(values[ord]);
        }
    }
    // Only ordinal 0 is in-bounds: key has 1 element, not 2.
    EXPECT_EQ(key.size(), 1u)
        << "Gap: out-of-bounds ordinal silently produces a shorter key";
    // Inserting a 1-element key into a 2-key-type index may produce inconsistent state.
    // We do not assert success here -- the important thing is it does not crash.
    (void)tgt.index->insert(key, rid);
}

// ===========================================================================
// QA_GDB984_BtreeRealInsertEntry
// Exercises the REAL IndexManager::insert_entry / remove_entry for B-tree.
// The unit tests use IndexManagerStub which bypasses the actual dispatch.
// This test verifies the real code path is not a no-op.
// ===========================================================================

class QA_GDB984_BtreeRealInsertEntry : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ =
            std::filesystem::temp_directory_path() / "sixseven_qa_984_btree_real";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        im_ = std::make_unique<IndexManager>(*catalog_, *storage_);

        BTreeConfig cfg;
        cfg.key_types = {TypeId::INT32};
        cfg.is_unique = false;
        btree_ = std::make_unique<BTreeIndex>(std::move(cfg));

        BtreeMaintenanceTarget tgt;
        tgt.index = btree_.get();
        tgt.key_column_ordinals = {0};
        im_->register_btree_target(/*table_id=*/42, std::move(tgt));
    }

    void TearDown() override {
        im_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        btree_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<IndexManager> im_;
    std::unique_ptr<BTreeIndex> btree_;
};

TEST_F(QA_GDB984_BtreeRealInsertEntry, InsertEntryActuallyInsertsIntoBtree) {
    auto rid = qa_rid(5, 1);
    std::vector<Value> values = {Value(int32_t{123})};

    // Call real insert_entry -- not a stub.
    auto r = im_->insert_entry(42, rid, values);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    // The real btree must now contain the key.
    auto found = btree_->search({Value(int32_t{123})});
    ASSERT_TRUE(found.has_value()) << found.error().message;
    ASSERT_TRUE(found->has_value()) << "Real insert_entry must insert into btree (was it a no-op?)";
    EXPECT_EQ(**found, rid);
}

TEST_F(QA_GDB984_BtreeRealInsertEntry, RemoveEntryActuallyRemovesFromBtree) {
    auto rid = qa_rid(6, 2);
    std::vector<Value> values = {Value(int32_t{456})};

    ASSERT_TRUE(im_->insert_entry(42, rid, values).has_value());

    auto before = btree_->search({Value(int32_t{456})});
    ASSERT_TRUE(before.has_value() && before->has_value());

    // Call real remove_entry.
    auto r = im_->remove_entry(42, rid, values);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto after = btree_->search({Value(int32_t{456})});
    ASSERT_TRUE(after.has_value());
    EXPECT_FALSE(after->has_value()) << "Real remove_entry must remove from btree (was it a no-op?)";
}

TEST_F(QA_GDB984_BtreeRealInsertEntry, UnregisteredTableIdIsNoOp) {
    // Calling insert_entry for a table_id with no registered targets must
    // return ok() and not crash.
    auto rid = qa_rid(99, 0);
    std::vector<Value> values = {Value(int32_t{999})};
    auto r = im_->insert_entry(/*table_id=*/9999, rid, values);
    EXPECT_TRUE(r.has_value()) << "Unregistered table_id must be a silent no-op";
}

// ===========================================================================
// QA_GDB984_HashRealInsertEntry
// Same as above for real HashIndex dispatch.
// ===========================================================================

class QA_GDB984_HashRealInsertEntry : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ =
            std::filesystem::temp_directory_path() / "sixseven_qa_984_hash_real";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        im_ = std::make_unique<IndexManager>(*catalog_, *storage_);

        HashIndexConfig cfg;
        cfg.key_types = {TypeId::INT32};
        cfg.is_unique = false;
        hash_ = std::make_unique<HashIndex>(std::move(cfg));

        HashMaintenanceTarget tgt;
        tgt.index = hash_.get();
        tgt.key_column_ordinals = {0};
        im_->register_hash_target(/*table_id=*/43, std::move(tgt));
    }

    void TearDown() override {
        im_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        hash_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<IndexManager> im_;
    std::unique_ptr<HashIndex> hash_;
};

TEST_F(QA_GDB984_HashRealInsertEntry, InsertEntryActuallyInsertsIntoHash) {
    auto rid = qa_rid(7, 1);
    std::vector<Value> values = {Value(int32_t{777})};

    auto r = im_->insert_entry(43, rid, values);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto found = hash_->search({Value(int32_t{777})});
    ASSERT_TRUE(found.has_value());
    ASSERT_TRUE(found->has_value())
        << "Real insert_entry must insert into hash (was it a no-op?)";
    EXPECT_EQ(**found, rid);
}

TEST_F(QA_GDB984_HashRealInsertEntry, RemoveEntryActuallyRemovesFromHash) {
    auto rid = qa_rid(8, 2);
    std::vector<Value> values = {Value(int32_t{888})};

    ASSERT_TRUE(im_->insert_entry(43, rid, values).has_value());
    auto r = im_->remove_entry(43, rid, values);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto after = hash_->search({Value(int32_t{888})});
    ASSERT_TRUE(after.has_value());
    EXPECT_FALSE(after->has_value())
        << "Real remove_entry must remove from hash (was it a no-op?)";
}

// ===========================================================================
// QA_GDB984_Bm25RealInsertEntry
// Exercises real BM25 dispatch through IndexManager.
// ===========================================================================

class QA_GDB984_Bm25RealInsertEntry : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ =
            std::filesystem::temp_directory_path() / "sixseven_qa_984_bm25_real";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        im_ = std::make_unique<IndexManager>(*catalog_, *storage_);

        bm25_ = std::make_unique<Bm25Index>();
        bm25_->create(Bm25Config{});

        Bm25MaintenanceTarget tgt;
        tgt.index = bm25_.get();
        tgt.text_column_index = 0;
        im_->register_bm25_target(/*table_id=*/44, std::move(tgt));
    }

    void TearDown() override {
        im_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        bm25_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<IndexManager> im_;
    std::unique_ptr<Bm25Index> bm25_;
};

TEST_F(QA_GDB984_Bm25RealInsertEntry, NullThenNonNullInsert) {
    // Insert null -- should be skipped, doc_count stays 0.
    auto rid_null = qa_rid(1, 0);
    std::vector<Value> null_values = {Value()}; // null
    auto r1 = im_->insert_entry(44, rid_null, null_values);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    EXPECT_EQ(bm25_->doc_count(), 0u) << "Null text must not be indexed";

    // Insert non-null -- must actually index the text.
    auto rid_real = qa_rid(2, 0);
    std::vector<Value> real_values = {Value(std::string("adversarial quantum"))};
    auto r2 = im_->insert_entry(44, rid_real, real_values);
    ASSERT_TRUE(r2.has_value()) << r2.error().message;

    EXPECT_EQ(bm25_->doc_count(), 1u) << "Non-null text must be indexed";

    auto hits = bm25_->search("quantum", 10);
    ASSERT_FALSE(hits.empty()) << "Indexed text must be searchable";
    EXPECT_EQ(hits[0].rid, rid_real);
}

TEST_F(QA_GDB984_Bm25RealInsertEntry, RemoveEntryRemovesDocument) {
    auto rid = qa_rid(3, 0);
    std::vector<Value> values = {Value(std::string("unique zither phrase"))};

    ASSERT_TRUE(im_->insert_entry(44, rid, values).has_value());
    ASSERT_EQ(bm25_->doc_count(), 1u);

    auto r = im_->remove_entry(44, rid, values);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto hits = bm25_->search("zither", 10);
    EXPECT_TRUE(hits.empty()) << "Document must be removed after remove_entry";
}

// ===========================================================================
// QA_GDB984_HnswDuplicateVector
// Two rows with identical float vectors must get distinct node_ids.
// Removing one RID must invalidate only its slot, leaving the other intact.
// ===========================================================================

class QA_GDB984_HnswDuplicateVector : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ =
            std::filesystem::temp_directory_path() / "sixseven_qa_984_hnsw_dup";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        auto path = data_dir_ / "hnsw_dup.db";
        auto fid = dm_->create_file(path, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        fid_ = *fid;
        bpm_ = std::make_unique<BufferPoolManager>(*dm_, fid_, 256);

        hnsw_ = std::make_unique<HnswIndex>(*bpm_);
        HnswIndexConfig cfg;
        cfg.dimension = 3;
        ASSERT_TRUE(hnsw_->create(cfg).has_value());

        rid_map_.clear();
    }

    void TearDown() override {
        hnsw_.reset();
        bpm_.reset();
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    Result<uint32_t> maintenance_insert(const RID& rid, const std::vector<float>& vec) {
        std::span<const float> sp{vec.data(), vec.size()};
        auto r = hnsw_->insert(sp);
        if (!r)
            return tl::unexpected(r.error());
        uint32_t node_id = *r;
        if (node_id >= rid_map_.size()) {
            rid_map_.resize(static_cast<size_t>(node_id) + 1, RID::invalid());
        }
        rid_map_[node_id] = rid;
        return ok(node_id);
    }

    Result<void> maintenance_remove(const RID& rid) {
        for (size_t node_id = 0; node_id < rid_map_.size(); ++node_id) {
            if (rid_map_[node_id] == rid) {
                auto r = hnsw_->remove(static_cast<uint32_t>(node_id));
                if (!r)
                    return tl::unexpected(r.error());
                rid_map_[node_id] = RID::invalid();
                return ok();
            }
        }
        return ok(); // not found -- no-op
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    FileId fid_{};
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<HnswIndex> hnsw_;
    std::vector<RID> rid_map_;
};

TEST_F(QA_GDB984_HnswDuplicateVector, DuplicateVectorGetDistinctNodeIds) {
    std::vector<float> vec = {1.0F, 1.0F, 1.0F};
    auto rid_a = qa_rid(10, 0);
    auto rid_b = qa_rid(11, 0);

    auto r1 = maintenance_insert(rid_a, vec);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    uint32_t node_a = *r1;

    auto r2 = maintenance_insert(rid_b, vec);
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    uint32_t node_b = *r2;

    EXPECT_NE(node_a, node_b) << "Identical vectors must get distinct node_ids";
    EXPECT_EQ(rid_map_[node_a], rid_a);
    EXPECT_EQ(rid_map_[node_b], rid_b);
}

TEST_F(QA_GDB984_HnswDuplicateVector, RemoveOneLeavesSibling) {
    std::vector<float> vec = {0.5F, 0.5F, 0.5F};
    auto rid_a = qa_rid(12, 0);
    auto rid_b = qa_rid(13, 0);

    auto r1 = maintenance_insert(rid_a, vec);
    ASSERT_TRUE(r1.has_value());
    uint32_t node_a = *r1;

    auto r2 = maintenance_insert(rid_b, vec);
    ASSERT_TRUE(r2.has_value());
    uint32_t node_b = *r2;

    // Remove rid_a only.
    ASSERT_TRUE(maintenance_remove(rid_a).has_value());

    // rid_a slot must be invalidated.
    EXPECT_EQ(rid_map_[node_a], RID::invalid())
        << "Removed RID slot must be tombstoned";

    // rid_b slot must still be valid.
    EXPECT_EQ(rid_map_[node_b], rid_b)
        << "Sibling RID must survive removal of duplicate-vector peer";
}

// ===========================================================================
// QA_GDB984_HnswSamRidInsertedTwice
// If insert_entry is called twice for the same RID (e.g., idempotent retry),
// the second call creates a second node_id mapped to the same RID, leaving
// the first slot orphaned.  This tests that both node_ids map to the same RID
// (documenting the orphan gap, not asserting failure).
// ===========================================================================

TEST(QA_GDB984_HnswDuplicate, SameRidInsertedTwiceCreatesOrphan) {
    auto data_dir =
        std::filesystem::temp_directory_path() / "sixseven_qa_984_hnsw_samerid";
    std::filesystem::remove_all(data_dir);
    std::filesystem::create_directories(data_dir);

    auto dm = std::make_unique<DiskManager>();
    auto path = data_dir / "hnsw.db";
    auto fid_r = dm->create_file(path, false, true);
    ASSERT_TRUE(fid_r.has_value());
    FileId fid = *fid_r;
    auto bpm = std::make_unique<BufferPoolManager>(*dm, fid, 256);

    std::vector<RID> rid_map;
    uint32_t node1 = 0;
    uint32_t node2 = 0;
    RID rid = qa_rid(100, 5);

    {
        HnswIndex hnsw(*bpm);
        HnswIndexConfig cfg;
        cfg.dimension = 2;
        ASSERT_TRUE(hnsw.create(cfg).has_value());

        auto do_insert = [&](float x, float y) -> uint32_t {
            std::vector<float> vec = {x, y};
            auto r = hnsw.insert(std::span<const float>{vec.data(), vec.size()});
            EXPECT_TRUE(r.has_value());
            uint32_t node_id = *r;
            if (node_id >= rid_map.size()) {
                rid_map.resize(node_id + 1, RID::invalid());
            }
            rid_map[node_id] = rid;
            return node_id;
        };

        node1 = do_insert(0.1F, 0.2F);
        node2 = do_insert(0.1F, 0.2F); // same RID, same vector
    }

    // Both node_ids must be distinct (HNSW always assigns a new node).
    // node1 slot is now overwritten if they are the same id -- but if distinct,
    // node1 is an orphan that points to rid but will never be cleaned up.
    if (node1 != node2) {
        // Document the orphan gap.
        EXPECT_EQ(rid_map[node1], rid) << "First node still points to rid (orphaned)";
        EXPECT_EQ(rid_map[node2], rid) << "Second node also points to rid";
    }
    // If by chance node1 == node2 (implementation-specific), the slot is simply overwritten.
    // Either way, no crash.

    // Reset BPM and DiskManager before removing directory (Windows file handle cleanup).
    bpm.reset();
    dm.reset();
    std::filesystem::remove_all(data_dir);
}

// ===========================================================================
// QA_GDB984_ErrorPropagation
// Verify that a failing sub-index insert propagates an error immediately
// and does not silently swallow it.
// ===========================================================================

TEST(QA_GDB984_ErrorPropagation, BtreeInsertFailurePropagates) {
    // Create a BTreeIndex with a UNIQUE constraint.  Inserting the same key
    // twice should cause the second insert to fail.
    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.is_unique = true;
    BTreeIndex btree(std::move(cfg));

    auto rid1 = qa_rid(1, 0);
    auto rid2 = qa_rid(2, 0);
    KeyType key = {Value(int32_t{42})};

    ASSERT_TRUE(btree.insert(key, rid1).has_value());

    // Second insert of the same key into a unique index should fail.
    auto r = btree.insert(key, rid2);
    EXPECT_FALSE(r.has_value())
        << "Unique constraint violation must propagate an error, not be swallowed";
}

TEST(QA_GDB984_ErrorPropagation, RealInsertEntryPropagatesUniqueConstraintViolation) {
    auto data_dir =
        std::filesystem::temp_directory_path() / "sixseven_qa_984_errprop";
    std::filesystem::remove_all(data_dir);
    std::filesystem::create_directories(data_dir);

    DiskManager dm;
    Catalog catalog;
    StorageManager storage(dm, data_dir);
    IndexManager im(catalog, storage);

    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.is_unique = true;
    auto btree = std::make_unique<BTreeIndex>(std::move(cfg));

    BtreeMaintenanceTarget tgt;
    tgt.index = btree.get();
    tgt.key_column_ordinals = {0};
    im.register_btree_target(/*table_id=*/50, std::move(tgt));

    std::vector<Value> values = {Value(int32_t{55})};
    auto rid1 = qa_rid(1, 0);
    auto rid2 = qa_rid(2, 0);

    ASSERT_TRUE(im.insert_entry(50, rid1, values).has_value());

    // Second insert violates uniqueness -- must NOT return ok().
    auto r = im.insert_entry(50, rid2, values);
    EXPECT_FALSE(r.has_value())
        << "insert_entry must propagate unique constraint error, not swallow it";

    std::filesystem::remove_all(data_dir);
}

// ===========================================================================
// QA_GDB984_ConcurrentRealIndexManager
// Concurrent insert_entry and remove_entry on a real IndexManager.
// This is the adversarial version of the unit test's BtreeWriterReaderNoRace:
// it uses the REAL insert_entry / remove_entry API (not direct index calls),
// which involves maintenance_latch_ acquisition/release.
// ===========================================================================

TEST(QA_GDB984_ConcurrentReal, InsertRemoveConcurrentNoRace) {
    auto data_dir =
        std::filesystem::temp_directory_path() / "sixseven_qa_984_concurrent";
    std::filesystem::remove_all(data_dir);
    std::filesystem::create_directories(data_dir);

    DiskManager dm;
    Catalog catalog;
    StorageManager storage(dm, data_dir);
    IndexManager im(catalog, storage);

    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.is_unique = false;
    BTreeIndex btree(std::move(cfg));

    BtreeMaintenanceTarget tgt;
    tgt.index = &btree;
    tgt.key_column_ordinals = {0};
    im.register_btree_target(/*table_id=*/60, std::move(tgt));

    constexpr int ITERATIONS = 300;
    std::atomic<bool> writer_done{false};

    std::thread writer([&]() {
        for (int i = 0; i < ITERATIONS; ++i) {
            auto rid = qa_rid(static_cast<uint32_t>(i), 0);
            std::vector<Value> values = {Value(static_cast<int32_t>(i))};
            (void)im.insert_entry(60, rid, values);
            (void)im.remove_entry(60, rid, values);
        }
        writer_done.store(true);
    });

    std::thread reader([&]() {
        while (!writer_done.load()) {
            for (int i = 0; i < ITERATIONS; ++i) {
                KeyType key = {Value(static_cast<int32_t>(i))};
                (void)btree.search(key);
            }
        }
    });

    writer.join();
    reader.join();

    // Post-condition: all removes completed -- index should be empty (or at most
    // contain some entries if a remove lost a race, which is acceptable here
    // since correctness of the lock protocol is the goal, not ordering).
    // The important assertion is: no crash, no UB detected by ASan/TSan.
    SUCCEED() << "Concurrent insert_entry/remove_entry completed without crash";
    std::filesystem::remove_all(data_dir);
}

// ===========================================================================
// QA_GDB984_MultipleTargetsSameTable
// Two BTree targets registered for the same table_id -- both must be
// maintained on insert_entry and remove_entry.
// ===========================================================================

TEST(QA_GDB984_MultipleTargets, TwoBtreeTargetsBothMaintained) {
    auto data_dir =
        std::filesystem::temp_directory_path() / "sixseven_qa_984_multi";
    std::filesystem::remove_all(data_dir);
    std::filesystem::create_directories(data_dir);

    DiskManager dm;
    Catalog catalog;
    StorageManager storage(dm, data_dir);
    IndexManager im(catalog, storage);

    // Index A: keyed on column 0.
    BTreeConfig cfgA;
    cfgA.key_types = {TypeId::INT32};
    cfgA.is_unique = false;
    BTreeIndex btreeA(std::move(cfgA));

    // Index B: keyed on column 1.
    BTreeConfig cfgB;
    cfgB.key_types = {TypeId::INT32};
    cfgB.is_unique = false;
    BTreeIndex btreeB(std::move(cfgB));

    BtreeMaintenanceTarget tgtA;
    tgtA.index = &btreeA;
    tgtA.key_column_ordinals = {0};
    im.register_btree_target(/*table_id=*/70, std::move(tgtA));

    BtreeMaintenanceTarget tgtB;
    tgtB.index = &btreeB;
    tgtB.key_column_ordinals = {1};
    im.register_btree_target(/*table_id=*/70, std::move(tgtB));

    auto rid = qa_rid(1, 0);
    std::vector<Value> values = {Value(int32_t{10}), Value(int32_t{20})};

    ASSERT_TRUE(im.insert_entry(70, rid, values).has_value());

    auto foundA = btreeA.search({Value(int32_t{10})});
    ASSERT_TRUE(foundA.has_value() && foundA->has_value())
        << "First btree target must be maintained";
    EXPECT_EQ(**foundA, rid);

    auto foundB = btreeB.search({Value(int32_t{20})});
    ASSERT_TRUE(foundB.has_value() && foundB->has_value())
        << "Second btree target must be maintained";
    EXPECT_EQ(**foundB, rid);

    // Remove.
    ASSERT_TRUE(im.remove_entry(70, rid, values).has_value());

    EXPECT_FALSE(btreeA.search({Value(int32_t{10})})->has_value())
        << "First target must reflect removal";
    EXPECT_FALSE(btreeB.search({Value(int32_t{20})})->has_value())
        << "Second target must reflect removal";

    std::filesystem::remove_all(data_dir);
}

// ===========================================================================
// QA_GDB984_EmptyValuesVector
// insert_entry/remove_entry with an empty values vector must not crash.
// Out-of-bounds check in "if (ordinal < values.size())" must protect us.
// ===========================================================================

TEST(QA_GDB984_EdgeCases, EmptyValuesNoOp) {
    auto data_dir =
        std::filesystem::temp_directory_path() / "sixseven_qa_984_empty";
    std::filesystem::remove_all(data_dir);
    std::filesystem::create_directories(data_dir);

    DiskManager dm;
    Catalog catalog;
    StorageManager storage(dm, data_dir);
    IndexManager im(catalog, storage);

    BTreeConfig cfg;
    cfg.key_types = {TypeId::INT32};
    cfg.is_unique = false;
    BTreeIndex btree(std::move(cfg));

    BtreeMaintenanceTarget tgt;
    tgt.index = &btree;
    tgt.key_column_ordinals = {0};
    im.register_btree_target(80, std::move(tgt));

    auto rid = qa_rid(1, 0);
    std::vector<Value> empty_values;

    // Must not crash or UB despite ordinal 0 being out-of-bounds for empty vector.
    auto r = im.insert_entry(80, rid, empty_values);
    // Result could be ok() (key is empty and insert is no-op) or an error; either is acceptable.
    // The key assertion is: no crash.
    (void)r;

    SUCCEED() << "insert_entry with empty values did not crash";
    std::filesystem::remove_all(data_dir);
}
