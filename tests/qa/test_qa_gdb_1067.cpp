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

// ===========================================================================
// ADVERSARIAL TESTS (added by QA for GDB-1067)
// ===========================================================================

// ---------------------------------------------------------------------------
// PK TYPE: STRING primary keys round-trip through WAL encode->decode->replay
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialPKType_StringPKRoundTrip) {
    // Create edge type with STRING source/target PKs.
    auto db_res = catalog_.create_database("testdb_str");
    ASSERT_TRUE(db_res) << db_res.error().message;
    database_id_t db_id = *db_res;

    TableSchema src_schema;
    src_schema.name = "str_src";
    src_schema.columns.push_back({0, "name", TypeId::STRING, false, ""});
    src_schema.pk_columns = "name";
    auto src_res = catalog_.create_table(db_id, src_schema);
    ASSERT_TRUE(src_res);
    table_id_t src_tid = *src_res;

    TableSchema tgt_schema;
    tgt_schema.name = "str_tgt";
    tgt_schema.columns.push_back({0, "name", TypeId::STRING, false, ""});
    tgt_schema.pk_columns = "name";
    auto tgt_res = catalog_.create_table(db_id, tgt_schema);
    ASSERT_TRUE(tgt_res);
    table_id_t tgt_tid = *tgt_res;

    dm_ = std::make_unique<DiskManager>();
    wal_writer_ = std::make_unique<WalWriter>(data_dir_ / "wal");
    ASSERT_TRUE(wal_writer_->open());
    graph_engine_ = std::make_unique<GraphEngine>(catalog_, *dm_, data_dir_, wal_writer_.get());

    auto et_res = graph_engine_->create_edge_type(
        db_id, "str_edge", src_tid, tgt_tid, TypeId::STRING, TypeId::STRING, {});
    ASSERT_TRUE(et_res) << et_res.error().message;

    Value src_pk(std::string("alice"));
    Value tgt_pk(std::string("bob"));
    auto link_res = graph_engine_->link(db_id, "str_edge", src_pk, tgt_pk, {});
    ASSERT_TRUE(link_res) << link_res.error().message;

    close_wal();
    graph_engine_.reset();
    dm_.reset();
    wal_writer_.reset();

    EdgeTableConfig cfg;
    cfg.edge_id = 1;
    cfg.name = "str_edge";
    cfg.source_table_id = src_tid;
    cfg.target_table_id = tgt_tid;
    cfg.source_pk_type = TypeId::STRING;
    cfg.target_pk_type = TypeId::STRING;
    EdgeTable fresh_table(cfg);
    ASSERT_EQ(fresh_table.size(), 0u);

    auto handler = std::make_unique<GraphEngineRecoveryHandler>();
    handler->register_edge_table(db_id, "str_edge", &fresh_table, {});
    auto stats = run_recovery(*handler);
    EXPECT_GE(stats.records_redone, 1u);

    auto edges = fresh_table.get_edges_from(src_pk);
    ASSERT_TRUE(edges) << edges.error().message;
    ASSERT_EQ(edges->size(), 1u) << "STRING PK edge must be recovered";
    EXPECT_EQ((*edges)[0].source_pk.as_string(), "alice");
    EXPECT_EQ((*edges)[0].target_pk.as_string(), "bob");
}

// ---------------------------------------------------------------------------
// PK TYPE: Negative INT64 and INT64_MIN/MAX boundary PKs
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialPKType_NegativeAndBoundaryInt64PKs) {
    EdgeWalPayload p;
    p.edge_row_id = 7;
    p.edge_type_name = "bound_edge";
    p.database_id = 1;
    p.source_pk_type = TypeId::INT64;
    p.source_pk = Value(std::numeric_limits<int64_t>::min());
    p.target_pk_type = TypeId::INT64;
    p.target_pk = Value(std::numeric_limits<int64_t>::max());
    p.property_types = {};
    p.properties = {};

    auto bytes = serialize_edge_wal_payload(p);
    auto result = deserialize_edge_wal_payload(bytes);
    ASSERT_TRUE(result) << result.error().message;

    EXPECT_EQ(result->source_pk.as_int64(), std::numeric_limits<int64_t>::min());
    EXPECT_EQ(result->target_pk.as_int64(), std::numeric_limits<int64_t>::max());
}

// ---------------------------------------------------------------------------
// PK TYPE: UUID primary key round-trip through WAL codec
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialPKType_UuidPKRoundTrip) {
    Uuid src_uuid = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                     0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
    Uuid tgt_uuid = {0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
                     0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00};

    EdgeWalPayload p;
    p.edge_row_id = 42;
    p.edge_type_name = "uuid_edge";
    p.database_id = 2;
    p.source_pk_type = TypeId::UUID;
    p.source_pk = Value(src_uuid);
    p.target_pk_type = TypeId::UUID;
    p.target_pk = Value(tgt_uuid);

    auto bytes = serialize_edge_wal_payload(p);
    auto result = deserialize_edge_wal_payload(bytes);
    ASSERT_TRUE(result) << result.error().message;

    EXPECT_EQ(result->source_pk.as_uuid(), src_uuid);
    EXPECT_EQ(result->target_pk.as_uuid(), tgt_uuid);
}

// ---------------------------------------------------------------------------
// PROPERTIES: Empty properties round-trip
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialProperties_EmptyPropertiesRoundTrip) {
    EdgeWalPayload p;
    p.edge_row_id = 1;
    p.edge_type_name = "no_props";
    p.database_id = 1;
    p.source_pk_type = TypeId::INT64;
    p.source_pk = Value(static_cast<int64_t>(1));
    p.target_pk_type = TypeId::INT64;
    p.target_pk = Value(static_cast<int64_t>(2));
    // No properties.

    auto bytes = serialize_edge_wal_payload(p);
    auto result = deserialize_edge_wal_payload(bytes);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->properties.size(), 0u);
    EXPECT_EQ(result->property_types.size(), 0u);
}

// ---------------------------------------------------------------------------
// PROPERTIES: Many properties of varied types
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialProperties_ManyMixedTypeProperties) {
    EdgeWalPayload p;
    p.edge_row_id = 99;
    p.edge_type_name = "multi_prop";
    p.database_id = 1;
    p.source_pk_type = TypeId::INT64;
    p.source_pk = Value(static_cast<int64_t>(10));
    p.target_pk_type = TypeId::INT64;
    p.target_pk = Value(static_cast<int64_t>(20));

    // Varied property types: INT32, FLOAT64, BOOL, STRING
    p.property_types = {TypeId::INT32, TypeId::FLOAT64, TypeId::BOOL, TypeId::STRING};
    p.properties = {
        Value(static_cast<int32_t>(42)),
        Value(3.14159),
        Value(true),
        Value(std::string("hello"))
    };

    auto bytes = serialize_edge_wal_payload(p);
    auto result = deserialize_edge_wal_payload(bytes);
    ASSERT_TRUE(result) << result.error().message;

    ASSERT_EQ(result->properties.size(), 4u);
    EXPECT_EQ(result->properties[0].as_int32(), 42);
    EXPECT_NEAR(result->properties[1].as_float64(), 3.14159, 1e-9);
    EXPECT_EQ(result->properties[2].as_bool(), true);
    EXPECT_EQ(result->properties[3].as_string(), "hello");
}

// ---------------------------------------------------------------------------
// PROPERTIES: STRING property with empty string value
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialProperties_EmptyStringPropertyValue) {
    EdgeWalPayload p;
    p.edge_row_id = 55;
    p.edge_type_name = "str_prop";
    p.database_id = 1;
    p.source_pk_type = TypeId::INT64;
    p.source_pk = Value(static_cast<int64_t>(1));
    p.target_pk_type = TypeId::INT64;
    p.target_pk = Value(static_cast<int64_t>(2));
    p.property_types = {TypeId::STRING};
    p.properties = {Value(std::string(""))};  // Empty string property

    auto bytes = serialize_edge_wal_payload(p);
    auto result = deserialize_edge_wal_payload(bytes);
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->properties.size(), 1u);
    EXPECT_EQ(result->properties[0].as_string(), "");
}

// ---------------------------------------------------------------------------
// INSERT-THEN-DELETE in same WAL: replay must end with edge ABSENT
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialSequence_InsertThenDeleteSameEdge) {
    create_node_tables();
    open_engine();
    create_follows_edge();

    Value src_pk = Value(static_cast<int64_t>(500));
    Value tgt_pk = Value(static_cast<int64_t>(600));

    // LINK then UNLINK -- both WAL records present in log.
    auto link_res = graph_engine_->link(db_id_, "follows", src_pk, tgt_pk, {});
    ASSERT_TRUE(link_res) << link_res.error().message;
    uint64_t edge_row_id = *link_res;

    auto unlink_res = graph_engine_->unlink(db_id_, "follows", src_pk, tgt_pk);
    ASSERT_TRUE(unlink_res) << unlink_res.error().message;

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

    auto handler = make_graph_handler(&fresh_table, "follows", {});
    run_recovery(*handler);

    // Delete wins: edge must be absent.
    auto edges = fresh_table.get_edges_from(src_pk);
    ASSERT_TRUE(edges) << edges.error().message;
    EXPECT_EQ(edges->size(), 0u)
        << "INSERT-then-DELETE: edge must be absent after replay (edge_row_id=" << edge_row_id << ")";
}

// ---------------------------------------------------------------------------
// Multiple link/unlink cycles of the same (src,tgt)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialSequence_MultipleRelinkCycles) {
    create_node_tables();
    open_engine();
    create_follows_edge();

    Value src_pk = Value(static_cast<int64_t>(11));
    Value tgt_pk = Value(static_cast<int64_t>(22));

    // Cycle 1: link then unlink.
    auto link1 = graph_engine_->link(db_id_, "follows", src_pk, tgt_pk, {});
    ASSERT_TRUE(link1);
    auto unlink1 = graph_engine_->unlink(db_id_, "follows", src_pk, tgt_pk);
    ASSERT_TRUE(unlink1);

    // Cycle 2: link again (final state = linked).
    auto link2 = graph_engine_->link(db_id_, "follows", src_pk, tgt_pk, {});
    ASSERT_TRUE(link2);

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

    auto handler = make_graph_handler(&fresh_table, "follows", {});
    run_recovery(*handler);

    // Final state: link2 leaves the edge present.
    auto edges = fresh_table.get_edges_from(src_pk);
    ASSERT_TRUE(edges) << edges.error().message;
    EXPECT_EQ(edges->size(), 1u)
        << "After link-unlink-link cycles, exactly one edge must remain after recovery";
}

// ---------------------------------------------------------------------------
// IDEMPOTENCY: Double recovery over the same WAL -- no duplicate edges
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialIdempotency_DoubleRecovery) {
    create_node_tables();
    open_engine();
    create_follows_edge();

    Value src_pk = Value(static_cast<int64_t>(77));
    Value tgt_pk = Value(static_cast<int64_t>(88));
    auto link_res = graph_engine_->link(db_id_, "follows", src_pk, tgt_pk, {});
    ASSERT_TRUE(link_res);

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

    auto handler = make_graph_handler(&fresh_table, "follows", {});

    // First recovery pass.
    run_recovery(*handler);
    ASSERT_EQ(fresh_table.size(), 1u) << "After first recovery: must have exactly 1 edge";

    // Second recovery pass over the same WAL -- must be idempotent.
    run_recovery(*handler);
    EXPECT_EQ(fresh_table.size(), 1u)
        << "After second (double) recovery: must still have exactly 1 edge -- ALREADY_EXISTS must be a no-op";
}

// ---------------------------------------------------------------------------
// IDEMPOTENCY: WAL DELETE for an edge not in table -- no error (NOT_FOUND ok)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialIdempotency_DeleteForAbsentEdge) {
    // Build a WAL record for EDGE_DELETE of an edge that is NOT in the table.
    EdgeWalPayload payload;
    payload.edge_row_id = 9999;
    payload.edge_type_name = "ghost_edge";
    payload.database_id = 1;
    payload.source_pk_type = TypeId::INT64;
    payload.source_pk = Value(static_cast<int64_t>(1));
    payload.target_pk_type = TypeId::INT64;
    payload.target_pk = Value(static_cast<int64_t>(2));

    WalRecord record;
    record.type = WalRecordType::EDGE_DELETE;
    record.txn_id = frozen_txn_id;
    record.data = serialize_edge_wal_payload(payload);

    EdgeTableConfig cfg;
    cfg.edge_id = 1;
    cfg.name = "ghost_edge";
    cfg.source_table_id = src_table_id_;
    cfg.target_table_id = tgt_table_id_;
    cfg.source_pk_type = TypeId::INT64;
    cfg.target_pk_type = TypeId::INT64;
    EdgeTable empty_table(cfg);

    GraphEngineRecoveryHandler handler;
    handler.register_edge_table(1, "ghost_edge", &empty_table, {});

    // Must succeed (NOT_FOUND is a no-op in delete redo).
    auto res = handler.redo(record);
    EXPECT_TRUE(res) << "EDGE_DELETE for absent edge must be a no-op (idempotent), got: "
                     << (res ? "" : res.error().message);
    EXPECT_EQ(empty_table.size(), 0u);
}

// ---------------------------------------------------------------------------
// MIGRATION: Truncated payload (< 12 bytes) must not crash
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialMigration_TruncatedPayloadUnder12Bytes) {
    // 5-byte payload -- too short for even the header.
    std::vector<uint8_t> tiny = {0x01, 0x02, 0x03, 0x04, 0x05};
    auto result = deserialize_edge_wal_payload(tiny);
    EXPECT_FALSE(result) << "Truncated payload must fail gracefully";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// MIGRATION: Empty payload must not crash
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialMigration_EmptyPayload) {
    std::vector<uint8_t> empty;
    auto result = deserialize_edge_wal_payload(empty);
    EXPECT_FALSE(result) << "Empty payload must fail gracefully";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// MIGRATION: Payload with valid header but truncated PK section
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialMigration_PayloadTruncatedAfterName) {
    // edge_row_id(8) + name_len(4) + name(7) = 19 bytes -- no database_id or PKs.
    std::string name = "follows";
    std::vector<uint8_t> data;
    uint64_t row_id = 1;
    uint32_t nl = static_cast<uint32_t>(name.size());
    // append edge_row_id
    data.resize(8);
    std::memcpy(data.data(), &row_id, 8);
    // append name_len
    uint8_t tmp[4];
    std::memcpy(tmp, &nl, 4);
    data.insert(data.end(), tmp, tmp + 4);
    // append name bytes
    data.insert(data.end(), name.begin(), name.end());
    // NO database_id, NO PKs -- exactly the legacy short format.

    auto result = deserialize_edge_wal_payload(data);
    EXPECT_FALSE(result)
        << "Payload truncated after name must fail with INVALID_ARGUMENT";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// MIGRATION: Unrelated WAL record type interleaved in EDGE_INSERT handler
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialMigration_UnrelatedRecordTypeSkipped) {
    // Feed an INSERT (table DML) record into GraphEngineRecoveryHandler.
    // It should return ok() and not touch any EdgeTable.
    WalRecord record;
    record.type = WalRecordType::INSERT;
    record.txn_id = frozen_txn_id;
    record.data = {0xDE, 0xAD, 0xBE, 0xEF}; // Arbitrary non-edge payload.

    EdgeTableConfig cfg;
    cfg.edge_id = 1;
    cfg.name = "follows";
    cfg.source_table_id = 0;
    cfg.target_table_id = 0;
    cfg.source_pk_type = TypeId::INT64;
    cfg.target_pk_type = TypeId::INT64;
    EdgeTable dummy(cfg);

    GraphEngineRecoveryHandler handler;
    handler.register_edge_table(1, "follows", &dummy, {});

    // Non-edge record type must be a pass-through no-op.
    auto res = handler.redo(record);
    EXPECT_TRUE(res)
        << "Unrelated record type must be pass-through no-op in GraphEngineRecoveryHandler";
    EXPECT_EQ(dummy.size(), 0u);
}

// ---------------------------------------------------------------------------
// prevent_duplicates: duplicate WAL EDGE_INSERT is ALREADY_EXISTS no-op
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialDuplicates_PreventDuplicatesIdempotent) {
    create_node_tables();
    open_engine();

    // Create edge type WITH prevent_duplicates.
    ColumnDef no_props_placeholder; // unused but API requires vector
    auto et_res = graph_engine_->create_edge_type(
        db_id_, "unique_edge", src_table_id_, tgt_table_id_,
        TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(et_res) << et_res.error().message;

    Value src_pk = Value(static_cast<int64_t>(1));
    Value tgt_pk = Value(static_cast<int64_t>(2));
    auto link_res = graph_engine_->link(db_id_, "unique_edge", src_pk, tgt_pk, {});
    ASSERT_TRUE(link_res);
    uint64_t eid = *link_res;

    close_wal();
    graph_engine_.reset();
    dm_.reset();
    wal_writer_.reset();

    // Pre-populate table (simulating heap was flushed -- edge is already present).
    EdgeTableConfig cfg;
    cfg.edge_id = 1;
    cfg.name = "unique_edge";
    cfg.source_table_id = src_table_id_;
    cfg.target_table_id = tgt_table_id_;
    cfg.source_pk_type = TypeId::INT64;
    cfg.target_pk_type = TypeId::INT64;
    cfg.prevent_duplicates = true;
    EdgeTable pre_loaded(cfg);
    auto restore_res = pre_loaded.restore_edge(eid, src_pk, tgt_pk, {});
    ASSERT_TRUE(restore_res) << restore_res.error().message;
    ASSERT_EQ(pre_loaded.size(), 1u);

    auto handler = make_graph_handler(&pre_loaded, "unique_edge", {});
    // WAL has EDGE_INSERT; table already has the edge -> must be ALREADY_EXISTS no-op.
    run_recovery(*handler);

    EXPECT_EQ(pre_loaded.size(), 1u)
        << "prevent_duplicates edge: WAL replay must be idempotent (ALREADY_EXISTS -> no-op)";
}

// ---------------------------------------------------------------------------
// EDGE_INSERT/EDGE_DELETE WAL record type bound -- max type byte is 12 (EDGE_DELETE)
// A corrupt type byte (e.g. 0xFF) in the recovery dispatcher must not crash
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialFormat_CorruptRecordTypeByte) {
    // Directly feed a WalRecord with a corrupt type byte to the
    // CompositeRecoveryHandler. The graph handler returns ok() for non-edge
    // types; the table handler may log or return ok(). Neither must crash.

    // Build a minimal EDGE_INSERT payload to make the record look plausible.
    EdgeWalPayload payload;
    payload.edge_row_id = 1;
    payload.edge_type_name = "x";
    payload.database_id = 1;
    payload.source_pk_type = TypeId::INT64;
    payload.source_pk = Value(static_cast<int64_t>(1));
    payload.target_pk_type = TypeId::INT64;
    payload.target_pk = Value(static_cast<int64_t>(2));

    WalRecord record;
    // Use a value beyond EDGE_DELETE (12) -- simulates a corrupt or future type byte.
    record.type = static_cast<WalRecordType>(0xFF);
    record.txn_id = frozen_txn_id;
    record.data = serialize_edge_wal_payload(payload);

    EdgeTableConfig cfg;
    cfg.edge_id = 1;
    cfg.name = "x";
    cfg.source_table_id = 0;
    cfg.target_table_id = 0;
    cfg.source_pk_type = TypeId::INT64;
    cfg.target_pk_type = TypeId::INT64;
    EdgeTable dummy(cfg);

    GraphEngineRecoveryHandler graph_h;
    graph_h.register_edge_table(1, "x", &dummy, {});

    // GraphEngineRecoveryHandler must not crash on unknown type (returns ok pass-through).
    auto res = graph_h.redo(record);
    EXPECT_TRUE(res)
        << "GraphEngineRecoveryHandler must tolerate corrupt/unknown type byte without crashing";
    EXPECT_EQ(dummy.size(), 0u);
}

// ---------------------------------------------------------------------------
// HETEROGENEOUS: source and target from DIFFERENT tables with different PK types
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialHeterogeneous_DifferentSourceTargetPKTypes) {
    EdgeWalPayload p;
    p.edge_row_id = 200;
    p.edge_type_name = "hetero_edge";
    p.database_id = 3;
    p.source_pk_type = TypeId::STRING;
    p.source_pk = Value(std::string("node_A"));
    p.target_pk_type = TypeId::INT64;
    p.target_pk = Value(static_cast<int64_t>(12345));

    auto bytes = serialize_edge_wal_payload(p);
    auto result = deserialize_edge_wal_payload(bytes);
    ASSERT_TRUE(result) << result.error().message;

    EXPECT_EQ(result->source_pk_type, TypeId::STRING);
    EXPECT_EQ(result->source_pk.as_string(), "node_A");
    EXPECT_EQ(result->target_pk_type, TypeId::INT64);
    EXPECT_EQ(result->target_pk.as_int64(), 12345);
}

// ---------------------------------------------------------------------------
// EDGE TYPE NOT REGISTERED: WAL record for unknown edge type is silently skipped
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialRecovery_UnregisteredEdgeTypeSkipped) {
    EdgeWalPayload payload;
    payload.edge_row_id = 1;
    payload.edge_type_name = "nonexistent_type";
    payload.database_id = 42;
    payload.source_pk_type = TypeId::INT64;
    payload.source_pk = Value(static_cast<int64_t>(1));
    payload.target_pk_type = TypeId::INT64;
    payload.target_pk = Value(static_cast<int64_t>(2));

    WalRecord record;
    record.type = WalRecordType::EDGE_INSERT;
    record.txn_id = frozen_txn_id;
    record.data = serialize_edge_wal_payload(payload);

    // Handler with NO registered edge tables.
    GraphEngineRecoveryHandler empty_handler;
    auto res = empty_handler.redo(record);
    EXPECT_TRUE(res)
        << "Unregistered edge type must be silently skipped (no crash, no error)";
}

// ---------------------------------------------------------------------------
// PAYLOAD: Long edge type name and long string property (stress codec)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialPayload_LongNameAndLongStringProperty) {
    std::string long_name(500, 'x');
    std::string long_val(10000, 'z');

    EdgeWalPayload p;
    p.edge_row_id = 1;
    p.edge_type_name = long_name;
    p.database_id = 1;
    p.source_pk_type = TypeId::INT64;
    p.source_pk = Value(static_cast<int64_t>(1));
    p.target_pk_type = TypeId::INT64;
    p.target_pk = Value(static_cast<int64_t>(2));
    p.property_types = {TypeId::STRING};
    p.properties = {Value(long_val)};

    auto bytes = serialize_edge_wal_payload(p);
    auto result = deserialize_edge_wal_payload(bytes);
    ASSERT_TRUE(result) << result.error().message;

    EXPECT_EQ(result->edge_type_name, long_name);
    ASSERT_EQ(result->properties.size(), 1u);
    EXPECT_EQ(result->properties[0].as_string(), long_val);
}

// ---------------------------------------------------------------------------
// ORDERING: CompositeRecoveryHandler routes edge records to graph handler,
//           non-edge records to table handler -- both handlers called correctly
// ---------------------------------------------------------------------------

TEST_F(QA_GDB1067, AdversarialComposite_RoutingCorrect) {
    // Build a simple stub table handler that counts redo calls.
    struct CountingHandler : RecoveryHandler {
        int redo_calls = 0;
        int undo_calls = 0;
        Result<void> redo(const WalRecord&) override { ++redo_calls; return ok(); }
        Result<void> undo(const WalRecord&) override { ++undo_calls; return ok(); }
    };

    CountingHandler table_handler;

    EdgeTableConfig cfg;
    cfg.edge_id = 1;
    cfg.name = "follows";
    cfg.source_table_id = 0;
    cfg.target_table_id = 0;
    cfg.source_pk_type = TypeId::INT64;
    cfg.target_pk_type = TypeId::INT64;
    EdgeTable edge_tbl(cfg);

    GraphEngineRecoveryHandler graph_handler;
    graph_handler.register_edge_table(1, "follows", &edge_tbl, {});

    CompositeRecoveryHandler composite(table_handler, graph_handler);

    // Build a valid EDGE_INSERT record.
    EdgeWalPayload ep;
    ep.edge_row_id = 1;
    ep.edge_type_name = "follows";
    ep.database_id = 1;
    ep.source_pk_type = TypeId::INT64;
    ep.source_pk = Value(static_cast<int64_t>(10));
    ep.target_pk_type = TypeId::INT64;
    ep.target_pk = Value(static_cast<int64_t>(20));

    WalRecord edge_rec;
    edge_rec.type = WalRecordType::EDGE_INSERT;
    edge_rec.txn_id = frozen_txn_id;
    edge_rec.data = serialize_edge_wal_payload(ep);

    auto r1 = composite.redo(edge_rec);
    EXPECT_TRUE(r1);
    EXPECT_EQ(table_handler.redo_calls, 0) << "EDGE_INSERT must NOT be routed to table handler";
    EXPECT_EQ(edge_tbl.size(), 1u) << "EDGE_INSERT must be applied to EdgeTable";

    // Non-edge record goes to table handler.
    WalRecord dml_rec;
    dml_rec.type = WalRecordType::INSERT;
    dml_rec.txn_id = frozen_txn_id;
    auto r2 = composite.redo(dml_rec);
    EXPECT_TRUE(r2);
    EXPECT_EQ(table_handler.redo_calls, 1) << "DML INSERT must be routed to table handler";
    EXPECT_EQ(edge_tbl.size(), 1u) << "DML INSERT must not touch edge table";
}

} // namespace
} // namespace sixseven
