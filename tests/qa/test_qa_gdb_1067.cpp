// GDB-1067: Edge WAL replay crash-durability tests.
//
// These tests exercise the WAL recovery path directly without a real process
// crash (portable on Windows; no POSIX fork/kill needed).
//
// Each test simulates the "lost-buffer" crash scenario by:
//   1. Creating an edge type and LINKing an edge (WAL record written).
//   2. Constructing a FRESH GraphEngine + GraphEngineRecoveryHandler over the
//      SAME WAL but WITHOUT the on-disk edge heap loaded -- i.e. the edge is
//      not yet in the EdgeTable.
//   3. Running WAL recovery.
//   4. Asserting the edge is present / absent with correct pk/properties.
//
// Why each test FAILS on origin/main (before GDB-1067):
//   - Old log_edge_wal() sets txn_id=invalid_txn_id(0) on the WAL record.
//     WalRecovery::recover() emits WARN and SKIPS any data record whose
//     txn_id == invalid_txn_id (wal_recovery.cpp line ~186).
//     Therefore the EDGE_INSERT record is never passed to any handler,
//     and the edge is NOT recovered.
//   - Additionally, the old payload omits source_pk/target_pk/properties so
//     even if the record were passed to a handler, reconstruction would be
//     impossible.
//   - The new record types EDGE_INSERT/EDGE_DELETE did not exist, so
//     is_data_record() would return false (unreachable default branch) and
//     the records would be buffered but then the frozen_txn_id check would
//     skip them as there was no frozen_txn_id in the old code for edges.
//
// Windows-portable: no GTEST_SKIP needed; the direct-recovery-path test runs
// fully on Windows.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/graph/edge_table.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/graph_engine_wal.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/serialization.h"
#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_recovery.h"
#include "sixseven/storage/wal_record.h"
#include "sixseven/table/tuple.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class QA_GDB1067 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "qa_gdb1067";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        std::filesystem::create_directories(data_dir_ / "wal");
        std::filesystem::create_directories(data_dir_ / "tables");
        std::filesystem::create_directories(data_dir_ / "edges");
    }

    void TearDown() override {
        // Reset smart pointers in reverse order to release file handles before
        // remove_all (important on Windows where open files cannot be deleted).
        wal_writer_.reset();
        graph_engine_.reset();
        dm_.reset();
        std::error_code ec;
        std::filesystem::remove_all(data_dir_, ec);
    }

    // Create the GraphEngine and open the WAL writer.
    void open_engine() {
        dm_ = std::make_unique<DiskManager>();
        wal_writer_ = std::make_unique<WalWriter>(data_dir_ / "wal");
        ASSERT_TRUE(wal_writer_->open()) << "WalWriter::open failed";
        graph_engine_ = std::make_unique<GraphEngine>(catalog_, *dm_, data_dir_, wal_writer_.get());
    }

    // Create a simple source/target table in the catalog.
    void create_node_tables() {
        auto db_res = catalog_.create_database("testdb");
        ASSERT_TRUE(db_res) << db_res.error().message;
        db_id_ = *db_res;

        TableSchema src_schema;
        src_schema.name = "src_nodes";
        src_schema.columns.push_back({0, "id", TypeId::INT64, false, ""});
        src_schema.pk_columns = "id";
        auto src_res = catalog_.create_table(db_id_, src_schema);
        ASSERT_TRUE(src_res) << src_res.error().message;
        src_table_id_ = *src_res;

        TableSchema tgt_schema;
        tgt_schema.name = "tgt_nodes";
        tgt_schema.columns.push_back({0, "id", TypeId::INT64, false, ""});
        tgt_schema.pk_columns = "id";
        auto tgt_res = catalog_.create_table(db_id_, tgt_schema);
        ASSERT_TRUE(tgt_res) << tgt_res.error().message;
        tgt_table_id_ = *tgt_res;
    }

    // Create edge type "follows" (INT64->INT64, no properties).
    edge_id_t create_follows_edge() {
        auto res = graph_engine_->create_edge_type(
            db_id_, "follows", src_table_id_, tgt_table_id_,
            TypeId::INT64, TypeId::INT64, {});
        EXPECT_TRUE(res) << res.error().message;
        return res ? *res : -1;
    }

    // Create edge type "rated" with one FLOAT64 property "score".
    edge_id_t create_rated_edge() {
        ColumnDef prop_col;
        prop_col.name = "score";
        prop_col.type = TypeId::FLOAT64;
        auto res = graph_engine_->create_edge_type(
            db_id_, "rated", src_table_id_, tgt_table_id_,
            TypeId::INT64, TypeId::INT64, {prop_col});
        EXPECT_TRUE(res) << res.error().message;
        return res ? *res : -1;
    }

    // Flush + close the WAL writer so the segment is readable.
    void close_wal() {
        if (wal_writer_) {
            auto res = wal_writer_->close();
            EXPECT_TRUE(res) << res.error().message;
        }
    }

    // Build a fresh GraphEngineRecoveryHandler with the given EdgeTable.
    std::unique_ptr<GraphEngineRecoveryHandler>
    make_graph_handler(EdgeTable* table,
                       const std::string& edge_type_name,
                       const std::vector<TypeId>& prop_types) {
        auto h = std::make_unique<GraphEngineRecoveryHandler>();
        h->register_edge_table(db_id_, edge_type_name, table, prop_types);
        return h;
    }

    // Run WAL recovery with a composite handler over the data dir's WAL.
    RecoveryStats run_recovery(RecoveryHandler& handler) {
        WalRecovery recovery(data_dir_ / "wal", handler);
        auto res = recovery.recover();
        EXPECT_TRUE(res) << res.error().message;
        return res ? *res : RecoveryStats{};
    }

    std::filesystem::path data_dir_;
    Catalog catalog_;
    database_id_t db_id_ = 0;
    table_id_t src_table_id_ = 0;
    table_id_t tgt_table_id_ = 0;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<WalWriter> wal_writer_;
    std::unique_ptr<GraphEngine> graph_engine_;
};

// ---------------------------------------------------------------------------
// AC1: EDGE_INSERT WAL record is replayed -- edge is recovered after crash
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, InsertReplay_EdgeRecoveredWithCorrectPKs) {
    // --- Phase 1: write an EDGE_INSERT WAL record. ---
    create_node_tables();
    open_engine();
    create_follows_edge();

    Value src_pk = Value(static_cast<int64_t>(10));
    Value tgt_pk = Value(static_cast<int64_t>(20));
    auto link_res = graph_engine_->link(db_id_, "follows", src_pk, tgt_pk, {});
    ASSERT_TRUE(link_res) << link_res.error().message;

    // Flush WAL so the segment file is readable.
    close_wal();
    graph_engine_.reset();
    dm_.reset();
    wal_writer_.reset();

    // --- Phase 2: simulate the "lost-buffer" state. ---
    // Build a fresh EdgeTable NOT populated from heap -- represents the state
    // after a crash where the edge heap page was never flushed to disk.
    EdgeTableConfig cfg;
    cfg.edge_id = 1; // First edge type in the catalog.
    cfg.name = "follows";
    cfg.source_table_id = src_table_id_;
    cfg.target_table_id = tgt_table_id_;
    cfg.source_pk_type = TypeId::INT64;
    cfg.target_pk_type = TypeId::INT64;
    EdgeTable fresh_table(cfg);

    // Verify that before WAL replay the table is empty.
    ASSERT_EQ(fresh_table.size(), 0u) << "pre-condition: fresh table must be empty";

    // --- Phase 3: run WAL recovery. ---
    auto graph_handler = make_graph_handler(&fresh_table, "follows", {});
    auto stats = run_recovery(*graph_handler);
    EXPECT_GE(stats.records_redone, 1u) << "Expected at least one record redone";

    // --- Phase 4: assert edge is present with correct PKs. ---
    auto edges_res = fresh_table.get_edges_from(src_pk);
    ASSERT_TRUE(edges_res) << edges_res.error().message;
    ASSERT_EQ(edges_res->size(), 1u) << "Edge must be present after WAL replay";

    const auto& recovered_edge = (*edges_res)[0];
    EXPECT_EQ(recovered_edge.source_pk.as_int64(), 10);
    EXPECT_EQ(recovered_edge.target_pk.as_int64(), 20);
    EXPECT_TRUE(recovered_edge.properties.empty());
}

// ---------------------------------------------------------------------------
// AC2: Properties are recovered correctly after crash
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, InsertReplay_PropertiesRoundTrip) {
    create_node_tables();
    open_engine();
    create_rated_edge();

    Value src_pk = Value(static_cast<int64_t>(1));
    Value tgt_pk = Value(static_cast<int64_t>(2));
    std::vector<Value> props = {Value(3.14)};
    auto link_res = graph_engine_->link(db_id_, "rated", src_pk, tgt_pk, props);
    ASSERT_TRUE(link_res) << link_res.error().message;

    close_wal();
    graph_engine_.reset();
    dm_.reset();
    wal_writer_.reset();

    EdgeTableConfig cfg;
    cfg.edge_id = 1;
    cfg.name = "rated";
    cfg.source_table_id = src_table_id_;
    cfg.target_table_id = tgt_table_id_;
    cfg.source_pk_type = TypeId::INT64;
    cfg.target_pk_type = TypeId::INT64;
    ColumnDef prop_col;
    prop_col.name = "score";
    prop_col.type = TypeId::FLOAT64;
    cfg.property_columns = {prop_col};
    EdgeTable fresh_table(cfg);

    ASSERT_EQ(fresh_table.size(), 0u);

    auto graph_handler = make_graph_handler(&fresh_table, "rated", {TypeId::FLOAT64});
    run_recovery(*graph_handler);

    auto edges_res = fresh_table.get_edges_from(src_pk);
    ASSERT_TRUE(edges_res) << edges_res.error().message;
    ASSERT_EQ(edges_res->size(), 1u);

    const auto& edge = (*edges_res)[0];
    ASSERT_EQ(edge.properties.size(), 1u);
    EXPECT_NEAR(edge.properties[0].as_float64(), 3.14, 1e-9);
}

// ---------------------------------------------------------------------------
// AC3: EDGE_DELETE WAL record -- unlinked edge stays absent after replay
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, DeleteReplay_EdgeAbsentAfterReplay) {
    create_node_tables();
    open_engine();
    create_follows_edge();

    Value src_pk = Value(static_cast<int64_t>(100));
    Value tgt_pk = Value(static_cast<int64_t>(200));
    auto link_res = graph_engine_->link(db_id_, "follows", src_pk, tgt_pk, {});
    ASSERT_TRUE(link_res) << link_res.error().message;

    auto unlink_res = graph_engine_->unlink(db_id_, "follows", src_pk, tgt_pk);
    ASSERT_TRUE(unlink_res) << unlink_res.error().message;

    close_wal();
    graph_engine_.reset();
    dm_.reset();
    wal_writer_.reset();

    // Start with an EdgeTable that HAS the edge (as if loaded from a heap file
    // that was flushed before the unlink).
    EdgeTableConfig cfg;
    cfg.edge_id = 1;
    cfg.name = "follows";
    cfg.source_table_id = src_table_id_;
    cfg.target_table_id = tgt_table_id_;
    cfg.source_pk_type = TypeId::INT64;
    cfg.target_pk_type = TypeId::INT64;
    EdgeTable pre_loaded_table(cfg);

    // Pre-populate the edge as if the heap file had it.
    auto restore_res =
        pre_loaded_table.restore_edge(1, src_pk, tgt_pk, {});
    ASSERT_TRUE(restore_res) << restore_res.error().message;
    ASSERT_EQ(pre_loaded_table.size(), 1u) << "pre-condition: edge must be present";

    auto graph_handler = make_graph_handler(&pre_loaded_table, "follows", {});
    run_recovery(*graph_handler);

    // After replaying EDGE_INSERT then EDGE_DELETE, edge must be absent.
    auto edges_res = pre_loaded_table.get_edges_from(src_pk);
    ASSERT_TRUE(edges_res) << edges_res.error().message;
    EXPECT_EQ(edges_res->size(), 0u) << "Edge must be absent after UNLINK replay";
}

// ---------------------------------------------------------------------------
// AC4: Idempotency -- edge already in data file is NOT duplicated by WAL replay
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, Idempotency_NoDoubleInsertWhenEdgeAlreadyLoaded) {
    create_node_tables();
    open_engine();
    create_follows_edge();

    Value src_pk = Value(static_cast<int64_t>(7));
    Value tgt_pk = Value(static_cast<int64_t>(8));
    auto link_res = graph_engine_->link(db_id_, "follows", src_pk, tgt_pk, {});
    ASSERT_TRUE(link_res) << link_res.error().message;
    uint64_t edge_row_id = *link_res;

    close_wal();
    graph_engine_.reset();
    dm_.reset();
    wal_writer_.reset();

    // Simulate heap file flush: pre-populate with the edge (edge_row_id matches).
    EdgeTableConfig cfg;
    cfg.edge_id = 1;
    cfg.name = "follows";
    cfg.source_table_id = src_table_id_;
    cfg.target_table_id = tgt_table_id_;
    cfg.source_pk_type = TypeId::INT64;
    cfg.target_pk_type = TypeId::INT64;
    EdgeTable pre_loaded_table(cfg);
    auto restore_res = pre_loaded_table.restore_edge(edge_row_id, src_pk, tgt_pk, {});
    ASSERT_TRUE(restore_res) << restore_res.error().message;
    ASSERT_EQ(pre_loaded_table.size(), 1u);

    auto graph_handler = make_graph_handler(&pre_loaded_table, "follows", {});
    run_recovery(*graph_handler);

    // Must still have exactly 1 edge (no duplication).
    EXPECT_EQ(pre_loaded_table.size(), 1u) << "Idempotency: no duplicate after double replay";

    auto edges_res = pre_loaded_table.get_edges_from(src_pk);
    ASSERT_TRUE(edges_res) << edges_res.error().message;
    EXPECT_EQ(edges_res->size(), 1u);
}

// ---------------------------------------------------------------------------
// AC5: Payload round-trip -- serialize then deserialize produces identical edge
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, PayloadRoundTrip_FullEncoding) {
    Value src(static_cast<int64_t>(42));
    Value tgt(static_cast<int64_t>(99));
    Value prop(2.718);

    EdgeWalPayload payload;
    payload.edge_row_id = 123;
    payload.edge_type_name = "follows";
    payload.database_id = 5;
    payload.source_pk_type = TypeId::INT64;
    payload.source_pk = src;
    payload.target_pk_type = TypeId::INT64;
    payload.target_pk = tgt;
    payload.property_types = {TypeId::FLOAT64};
    payload.properties = {prop};

    auto bytes = serialize_edge_wal_payload(payload);
    ASSERT_GT(bytes.size(), 12u);

    auto result = deserialize_edge_wal_payload(bytes);
    ASSERT_TRUE(result) << result.error().message;

    EXPECT_EQ(result->edge_row_id, 123u);
    EXPECT_EQ(result->edge_type_name, "follows");
    EXPECT_EQ(result->database_id, 5);
    EXPECT_EQ(result->source_pk.as_int64(), 42);
    EXPECT_EQ(result->target_pk.as_int64(), 99);
    ASSERT_EQ(result->properties.size(), 1u);
    EXPECT_NEAR(result->properties[0].as_float64(), 2.718, 1e-9);
}

// ---------------------------------------------------------------------------
// AC6: Legacy short payload does not crash recovery -- skipped with WARN
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, LegacyShortPayload_SkippedNotCrashed) {
    // Build a WAL record with the OLD (too-short) payload format:
    // edge_row_id(8) + name_len(4) + name -- no database_id or PKs.
    std::string edge_name = "follows";
    std::vector<uint8_t> legacy_data(8 + 4 + edge_name.size());
    uint64_t row_id = 1;
    std::memcpy(legacy_data.data(), &row_id, 8);
    uint32_t nl = static_cast<uint32_t>(edge_name.size());
    std::memcpy(legacy_data.data() + 8, &nl, 4);
    std::memcpy(legacy_data.data() + 12, edge_name.data(), edge_name.size());

    auto result = deserialize_edge_wal_payload(legacy_data);
    // Must return an error (not crash), and recovery can skip it gracefully.
    EXPECT_FALSE(result) << "Legacy payload must fail deserialization with an error, not crash";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// AC7: EDGE_INSERT and EDGE_DELETE are recognized as data records by recovery
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, NewRecordTypes_AreDataRecords) {
    // is_data_record is private; verify indirectly: write an EDGE_INSERT with
    // frozen_txn_id and confirm it appears in records_redone (not skipped).
    create_node_tables();
    open_engine();
    create_follows_edge();

    Value src_pk = Value(static_cast<int64_t>(3));
    Value tgt_pk = Value(static_cast<int64_t>(4));
    auto link_res = graph_engine_->link(db_id_, "follows", src_pk, tgt_pk, {});
    ASSERT_TRUE(link_res) << link_res.error().message;

    close_wal();
    graph_engine_.reset();
    dm_.reset();
    wal_writer_.reset();

    EdgeTableConfig cfg;
    cfg.edge_id = 1;
    cfg.name = "follows";
    cfg.source_table_id = src_table_id_;
    cfg.target_table_id = tgt_table_id_;
    cfg.source_pk_type = TypeId::INT64;
    cfg.target_pk_type = TypeId::INT64;
    EdgeTable fresh_table(cfg);

    auto graph_handler = make_graph_handler(&fresh_table, "follows", {});
    auto stats = run_recovery(*graph_handler);

    // At least the EDGE_INSERT record must have been redone.
    EXPECT_GE(stats.records_redone, 1u)
        << "EDGE_INSERT record must be replayed; records_redone=" << stats.records_redone;
}

// ---------------------------------------------------------------------------
// AC8: Multiple edges round-trip correctly (batch insert / recovery)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, MultipleEdges_AllRecovered) {
    create_node_tables();
    open_engine();
    create_follows_edge();

    std::vector<std::pair<int64_t, int64_t>> pairs = {{1, 2}, {3, 4}, {5, 6}};
    for (auto [s, t] : pairs) {
        Value sv(s);
        Value tv(t);
        auto r = graph_engine_->link(db_id_, "follows", sv, tv, {});
        ASSERT_TRUE(r) << r.error().message;
    }

    close_wal();
    graph_engine_.reset();
    dm_.reset();
    wal_writer_.reset();

    EdgeTableConfig cfg;
    cfg.edge_id = 1;
    cfg.name = "follows";
    cfg.source_table_id = src_table_id_;
    cfg.target_table_id = tgt_table_id_;
    cfg.source_pk_type = TypeId::INT64;
    cfg.target_pk_type = TypeId::INT64;
    EdgeTable fresh_table(cfg);
    ASSERT_EQ(fresh_table.size(), 0u);

    auto graph_handler = make_graph_handler(&fresh_table, "follows", {});
    run_recovery(*graph_handler);

    EXPECT_EQ(fresh_table.size(), 3u) << "All 3 edges must be recovered";

    for (auto [s, t] : pairs) {
        Value sv(s);
        auto edges = fresh_table.get_edges_from(sv);
        ASSERT_TRUE(edges) << edges.error().message;
        ASSERT_EQ(edges->size(), 1u) << "Edge (" << s << "->" << t << ") not recovered";
        EXPECT_EQ((*edges)[0].target_pk.as_int64(), t);
    }
}

} // namespace
} // namespace sixseven
