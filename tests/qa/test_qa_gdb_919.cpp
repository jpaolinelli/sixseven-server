/// @file test_qa_gdb_919.cpp
/// @brief QA regression tests for GDB-919: HnswIndex::remove() wired into DELETE.
///
/// Verifies that SQL DELETE tombstones the deleted row's vector in the HNSW
/// index (not merely skips it post-hoc), that the node_count decreases, and
/// that NEAREST queries after DELETE return correct results without the deleted
/// rows.

#include "sixseven/executor/delete.h"
#include "sixseven/executor/iterator.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/index/rid.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"
#include "sixseven/vector/hnsw_index.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

using namespace sixseven;

// =============================================================================
// Helpers
// =============================================================================

namespace {

// Two-dimensional test vectors.
static const std::vector<float> kVec0 = {1.0F, 0.0F};
static const std::vector<float> kVec1 = {0.0F, 1.0F};
static const std::vector<float> kVec2 = {-1.0F, 0.0F};
static const std::vector<float> kVec3 = {0.0F, -1.0F};

// Simple child iterator that yields a single tuple (used to feed DeleteOperator).
class SingleTupleIterator : public Iterator {
public:
    explicit SingleTupleIterator(RID rid) : rid_(rid) {
        schema_ = OutputSchema({{"", "id", TypeId::INT32, false, 0}});
    }

    const OutputSchema& output_schema() const override { return schema_; }
    std::string plan_node_name() const override { return "SingleTuple"; }
    std::string plan_node_detail() const override { return ""; }
    std::vector<const Iterator*> plan_children() const override { return {}; }

protected:
    Result<void> do_open() override {
        yielded_ = false;
        return ok();
    }

    Result<std::optional<Tuple>> do_next() override {
        if (yielded_) {
            return ok(std::optional<Tuple>(std::nullopt));
        }
        yielded_ = true;
        Tuple t;
        t.values.push_back(Value(int32_t{42}));
        t.rid = rid_;
        return ok(std::optional<Tuple>(std::move(t)));
    }

    void do_close() override {}

    std::vector<Iterator*> plan_children_mutable() override { return {}; }

private:
    RID rid_;
    OutputSchema schema_;
    bool yielded_ = false;
};

} // namespace

// =============================================================================
// Fixture
// =============================================================================

class GDB919Test : public ::testing::Test {
protected:
    void SetUp() override {
        table_path_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb919_table.db";
        hnsw_path_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb919_hnsw.db";
        std::filesystem::remove(table_path_);
        std::filesystem::remove(hnsw_path_);

        auto table_fid = dm_.create_file(table_path_, false, true);
        ASSERT_TRUE(table_fid.has_value()) << table_fid.error().message;
        table_fid_ = *table_fid;
        table_bpm_ = std::make_unique<BufferPoolManager>(dm_, table_fid_, 64);

        auto hnsw_fid = dm_.create_file(hnsw_path_, false, true);
        ASSERT_TRUE(hnsw_fid.has_value()) << hnsw_fid.error().message;
        hnsw_fid_ = *hnsw_fid;
        hnsw_bpm_ = std::make_unique<BufferPoolManager>(dm_, hnsw_fid_, 64);

        storage_schema_ = Schema({
            {"id", TypeId::INT32},
            {"vec", TypeId::EMBEDDING},
        });

        heap_ = std::make_unique<TableHeap>(*table_bpm_, dm_, table_fid_);

        hnsw_ = std::make_unique<HnswIndex>(*hnsw_bpm_);
        HnswIndexConfig cfg;
        cfg.dimension = 2;
        cfg.metric = DistanceMetric::L2;
        auto hnsw_create = hnsw_->create(cfg);
        ASSERT_TRUE(hnsw_create.has_value()) << hnsw_create.error().message;
    }

    void TearDown() override {
        heap_.reset();
        hnsw_.reset();
        table_bpm_.reset();
        hnsw_bpm_.reset();
        (void)dm_.close_file(table_fid_);
        (void)dm_.close_file(hnsw_fid_);
        std::filesystem::remove(table_path_);
        std::filesystem::remove(hnsw_path_);
    }

    // Insert a row into the heap, insert the vector into HNSW, and record the
    // node_id -> RID mapping. Returns the RID of the inserted row.
    RID insert_row_and_vector(int32_t id, const std::vector<float>& vec) {
        std::vector<Value> vals = {Value(id), Value(Embedding(vec))};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        EXPECT_TRUE(bytes.has_value()) << bytes.error().message;
        if (!bytes.has_value()) {
            return RID::invalid();
        }
        auto rid_result = heap_->insert_tuple(*bytes);
        EXPECT_TRUE(rid_result.has_value()) << rid_result.error().message;
        if (!rid_result.has_value()) {
            return RID::invalid();
        }
        RID rid = *rid_result;

        auto node_result = hnsw_->insert(std::span<const float>(vec));
        EXPECT_TRUE(node_result.has_value()) << node_result.error().message;

        rid_map_.push_back(rid);
        return rid;
    }

    DiskManager dm_;
    FileId table_fid_ = 0;
    FileId hnsw_fid_ = 0;
    std::unique_ptr<BufferPoolManager> table_bpm_;
    std::unique_ptr<BufferPoolManager> hnsw_bpm_;
    std::filesystem::path table_path_;
    std::filesystem::path hnsw_path_;

    Schema storage_schema_;
    std::unique_ptr<TableHeap> heap_;
    std::unique_ptr<HnswIndex> hnsw_;

    // Mirrors the IndexManager hnsw_rid_maps_ entry for this index.
    std::vector<RID> rid_map_;
};

// =============================================================================
// AC1: Deleting a row tombstones its HNSW node.
//
// After DELETE, node_count() must decrease (distinguishes "removed" from
// "skipped"), and the rid_map slot must be invalidated (RID::invalid()).
// =============================================================================

TEST_F(GDB919Test, DeleteTombstonesHnswNodeAndDecrementsNodeCount) {
    RID rid0 = insert_row_and_vector(0, kVec0);
    RID rid1 = insert_row_and_vector(1, kVec1);
    RID rid2 = insert_row_and_vector(2, kVec2);
    RID rid3 = insert_row_and_vector(3, kVec3);

    ASSERT_NE(rid0, RID::invalid());
    ASSERT_NE(rid1, RID::invalid());
    ASSERT_NE(rid2, RID::invalid());
    ASSERT_NE(rid3, RID::invalid());

    // All four nodes are live before any delete.
    EXPECT_EQ(hnsw_->node_count(), 4u);

    // Set up DeleteOperator targeting rid1.
    const index_id_t kFakeIndexId = 1;
    HnswMaintenanceTarget target{hnsw_.get(), kFakeIndexId, &rid_map_};

    auto child = std::make_unique<SingleTupleIterator>(rid1);
    DeleteOperator del_op(*heap_, std::move(child));
    del_op.hnsw_targets_.push_back(target);

    auto open_res = del_op.open();
    ASSERT_TRUE(open_res.has_value()) << open_res.error().message;

    auto next_res = del_op.next();
    ASSERT_TRUE(next_res.has_value()) << next_res.error().message;
    ASSERT_TRUE(next_res->has_value());
    EXPECT_EQ((*next_res)->values[0].as_int64(), 1); // 1 row deleted

    del_op.close();

    // node_count() decrements: proves removal, not just skip.
    EXPECT_EQ(hnsw_->node_count(), 3u);

    // rid_map slot for rid1 (node_id==1) is invalidated.
    EXPECT_EQ(rid_map_[1], RID::invalid());

    // Other slots untouched.
    EXPECT_EQ(rid_map_[0], rid0);
    EXPECT_EQ(rid_map_[2], rid2);
    EXPECT_EQ(rid_map_[3], rid3);
}

// =============================================================================
// AC2: NEAREST search after DELETE does not return the deleted node.
//
// After tombstoning, the deleted node_id is absent from HNSW search results.
// =============================================================================

TEST_F(GDB919Test, HnswSearchDoesNotReturnDeletedNode) {
    RID rid0 = insert_row_and_vector(0, kVec0);
    RID rid1 = insert_row_and_vector(1, kVec1);
    RID rid2 = insert_row_and_vector(2, kVec2);
    (void)rid0;
    (void)rid2;

    // Delete rid1 (node_id 1).
    const index_id_t kFakeIndexId = 1;
    HnswMaintenanceTarget target{hnsw_.get(), kFakeIndexId, &rid_map_};

    auto child = std::make_unique<SingleTupleIterator>(rid1);
    DeleteOperator del_op(*heap_, std::move(child));
    del_op.hnsw_targets_.push_back(target);

    auto open_res = del_op.open();
    ASSERT_TRUE(open_res.has_value());
    auto next_res = del_op.next();
    ASSERT_TRUE(next_res.has_value());
    del_op.close();

    // Search for the vector closest to kVec1 (which was deleted).
    // The deleted node must not appear in results.
    std::vector<float> query = {0.0F, 1.0F};
    auto results = hnsw_->search(std::span<const float>(query), 3u);
    ASSERT_TRUE(results.has_value()) << results.error().message;

    for (const auto& sr : *results) {
        EXPECT_NE(sr.node_id, 1u) << "deleted node_id 1 must not appear in NEAREST results";
    }
}

// =============================================================================
// AC3: Deleting multiple rows tombstones all their nodes.
// =============================================================================

TEST_F(GDB919Test, MultipleDeletesTombstoneAllNodes) {
    RID rid0 = insert_row_and_vector(0, kVec0);
    RID rid1 = insert_row_and_vector(1, kVec1);
    RID rid2 = insert_row_and_vector(2, kVec2);
    RID rid3 = insert_row_and_vector(3, kVec3);
    (void)rid0;
    (void)rid3;

    ASSERT_EQ(hnsw_->node_count(), 4u);

    const index_id_t kFakeIndexId = 1;

    // Delete rid1.
    {
        HnswMaintenanceTarget target{hnsw_.get(), kFakeIndexId, &rid_map_};
        auto child = std::make_unique<SingleTupleIterator>(rid1);
        DeleteOperator del_op(*heap_, std::move(child));
        del_op.hnsw_targets_.push_back(target);
        auto open_r = del_op.open();
        ASSERT_TRUE(open_r.has_value());
        auto next_r = del_op.next();
        ASSERT_TRUE(next_r.has_value());
        del_op.close();
    }

    EXPECT_EQ(hnsw_->node_count(), 3u);

    // Delete rid2.
    {
        HnswMaintenanceTarget target{hnsw_.get(), kFakeIndexId, &rid_map_};
        auto child = std::make_unique<SingleTupleIterator>(rid2);
        DeleteOperator del_op(*heap_, std::move(child));
        del_op.hnsw_targets_.push_back(target);
        auto open_r = del_op.open();
        ASSERT_TRUE(open_r.has_value());
        auto next_r = del_op.next();
        ASSERT_TRUE(next_r.has_value());
        del_op.close();
    }

    EXPECT_EQ(hnsw_->node_count(), 2u);
    EXPECT_EQ(rid_map_[1], RID::invalid());
    EXPECT_EQ(rid_map_[2], RID::invalid());
}

// =============================================================================
// AC4: DELETE on a table with no HNSW targets is a no-op for HNSW (regression:
// tables without an embedding column must not be affected).
// =============================================================================

TEST_F(GDB919Test, DeleteWithNoHnswTargetsDoesNotTouchIndex) {
    insert_row_and_vector(0, kVec0);
    ASSERT_EQ(hnsw_->node_count(), 1u);

    // Insert an extra row to delete (a row with no vector just uses the heap).
    std::vector<Value> vals = {Value(int32_t{99}), Value(Embedding(kVec0))};
    auto bytes = TupleSerializer::serialize(vals, storage_schema_);
    ASSERT_TRUE(bytes.has_value());
    auto rid_extra = heap_->insert_tuple(*bytes);
    ASSERT_TRUE(rid_extra.has_value());

    // DeleteOperator with NO hnsw_targets_.
    auto child = std::make_unique<SingleTupleIterator>(*rid_extra);
    DeleteOperator del_op(*heap_, std::move(child));
    // hnsw_targets_ intentionally empty.

    auto open_res = del_op.open();
    ASSERT_TRUE(open_res.has_value());
    auto next_res = del_op.next();
    ASSERT_TRUE(next_res.has_value());
    ASSERT_TRUE(next_res->has_value());
    EXPECT_EQ((*next_res)->values[0].as_int64(), 1);
    del_op.close();

    // HNSW index is untouched: the extra heap row was not in the HNSW, so
    // node_count stays at 1 and rid_map is unchanged.
    EXPECT_EQ(hnsw_->node_count(), 1u);
    ASSERT_EQ(rid_map_.size(), 1u);
    EXPECT_NE(rid_map_[0], RID::invalid());
}

// =============================================================================
// AC5: Deleting a RID that is not in the rid_map does not crash and does not
// corrupt the index (e.g. row inserted after HNSW last rebuilt).
// =============================================================================

TEST_F(GDB919Test, DeleteRidNotInRidMapIsNoop) {
    RID rid0 = insert_row_and_vector(0, kVec0);
    ASSERT_EQ(hnsw_->node_count(), 1u);

    // Insert a heap row that was NOT added to the HNSW (simulates a row
    // inserted after the last REINDEX, when the HNSW rid_map is stale).
    std::vector<Value> vals = {Value(int32_t{99}), Value(Embedding(kVec1))};
    auto bytes = TupleSerializer::serialize(vals, storage_schema_);
    ASSERT_TRUE(bytes.has_value());
    auto phantom_rid = heap_->insert_tuple(*bytes);
    ASSERT_TRUE(phantom_rid.has_value());
    // Note: we do NOT add *phantom_rid to rid_map_ or hnsw_.

    const index_id_t kFakeIndexId = 1;
    HnswMaintenanceTarget target{hnsw_.get(), kFakeIndexId, &rid_map_};

    auto child = std::make_unique<SingleTupleIterator>(*phantom_rid);
    DeleteOperator del_op(*heap_, std::move(child));
    del_op.hnsw_targets_.push_back(target);

    auto open_res = del_op.open();
    ASSERT_TRUE(open_res.has_value());
    auto next_res = del_op.next();
    ASSERT_TRUE(next_res.has_value());
    ASSERT_TRUE(next_res->has_value());
    EXPECT_EQ((*next_res)->values[0].as_int64(), 1); // heap delete succeeded
    del_op.close();

    // HNSW and rid_map are intact: the phantom RID was not found in rid_map.
    EXPECT_EQ(hnsw_->node_count(), 1u);
    ASSERT_EQ(rid_map_.size(), 1u);
    EXPECT_EQ(rid_map_[0], rid0);
}
