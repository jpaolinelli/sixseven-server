// GDB-884: Edge WAL logging is write-only theater; durability claim in
// comments is false (audit finding H14).
//
// This is a regression test verifying that the fix already delivered under
// GDB-1067 (PR #438) actually closes the H14 finding described in GDB-884:
//   - log_edge_wal() now serializes source_pk/target_pk/properties into the
//     WAL record payload (not just edge_row_id + type name), so the edge can
//     be reconstructed at replay.
//   - The record is tagged with frozen_txn_id (a real, always-committed
//     sentinel) rather than invalid_txn_id (0), so WalRecovery::recover()
//     includes it in the redo set instead of unconditionally skipping it.
//   - GraphEngineRecoveryHandler is wired in as a real RecoveryHandler that
//     decodes EDGE_INSERT/EDGE_DELETE payloads and reconstructs/removes edges
//     during startup recovery (not just the replication wal_receiver path).
//
// Per GDB-884's AC, this test drives recovery deterministically at the API
// level (append WAL via LINK/UNLINK, then construct a fresh GraphEngine
// recovery view over the same WAL without going through clean shutdown or a
// heap flush) so it is fully portable on Windows -- no POSIX crash/fork
// simulation is required.
//
// Against pre-GDB-1067 code these tests fail because:
//   - the WAL payload lacked source_pk/target_pk/properties (reconstruction
//     was impossible even if the record reached a handler), and
//   - the record's txn_id was left at invalid_txn_id(0), so
//     WalRecovery::recover() skipped it outright (wal_recovery.cpp ~line 213).

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/graph/edge_table.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/graph/graph_engine_wal.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_recovery.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sixseven {
namespace {

class QA_GDB884 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "qa_gdb884";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        std::filesystem::create_directories(data_dir_ / "wal");
        std::filesystem::create_directories(data_dir_ / "tables");
        std::filesystem::create_directories(data_dir_ / "edges");
    }

    void TearDown() override {
        // Reset in reverse-construction order so file handles are released
        // before remove_all (Windows cannot delete open files).
        wal_writer_.reset();
        graph_engine_.reset();
        dm_.reset();
        std::error_code ec;
        std::filesystem::remove_all(data_dir_, ec);
    }

    void open_engine() {
        dm_ = std::make_unique<DiskManager>();
        wal_writer_ = std::make_unique<WalWriter>(data_dir_ / "wal");
        ASSERT_TRUE(wal_writer_->open()) << "WalWriter::open failed";
        graph_engine_ = std::make_unique<GraphEngine>(catalog_, *dm_, data_dir_, wal_writer_.get());
    }

    void create_node_tables() {
        auto db_res = catalog_.create_database("gdb884db");
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

    // Flush and close the WAL writer so its segment file is durably readable,
    // WITHOUT going through a clean-shutdown / edge-heap-flush / checkpoint
    // path -- this is the "crash after LINK/UNLINK returns success" scenario.
    void close_wal_without_clean_shutdown() {
        if (wal_writer_) {
            auto res = wal_writer_->close();
            EXPECT_TRUE(res) << res.error().message;
        }
    }

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
// AC: a LINKed edge survives a simulated crash (WAL-only, no clean shutdown,
// no checkpoint, no eviction) followed by reopen + WAL recovery.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB884, LinkedEdgeSurvivesCrashAndRecovery) {
    create_node_tables();
    open_engine();

    auto edge_res = graph_engine_->create_edge_type(
        db_id_, "follows", src_table_id_, tgt_table_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(edge_res) << edge_res.error().message;

    Value src_pk = Value(static_cast<int64_t>(42));
    Value tgt_pk = Value(static_cast<int64_t>(99));
    auto link_res = graph_engine_->link(db_id_, "follows", src_pk, tgt_pk, {});
    ASSERT_TRUE(link_res) << link_res.error().message;

    // Simulate crash: tear down without any clean-shutdown edge heap flush.
    close_wal_without_clean_shutdown();
    graph_engine_.reset();
    dm_.reset();
    wal_writer_.reset();

    // Fresh, empty EdgeTable represents the post-crash state where the edge
    // heap page holding this edge was never flushed to disk.
    EdgeTableConfig cfg;
    cfg.edge_id = *edge_res;
    cfg.name = "follows";
    cfg.source_table_id = src_table_id_;
    cfg.target_table_id = tgt_table_id_;
    cfg.source_pk_type = TypeId::INT64;
    cfg.target_pk_type = TypeId::INT64;
    EdgeTable fresh_table(cfg);
    ASSERT_EQ(fresh_table.size(), 0u) << "pre-condition: fresh table must be empty pre-recovery";

    GraphEngineRecoveryHandler graph_handler;
    graph_handler.register_edge_table(db_id_, "follows", &fresh_table, {});
    auto stats = run_recovery(graph_handler);
    EXPECT_GE(stats.records_redone, 1u) << "EDGE_INSERT record must be redone, not skipped";

    auto edges_res = fresh_table.get_edges_from(src_pk);
    ASSERT_TRUE(edges_res) << edges_res.error().message;
    ASSERT_EQ(edges_res->size(), 1u) << "edge must be present after recovery (was lost pre-fix)";
    EXPECT_EQ((*edges_res)[0].source_pk.as_int64(), 42);
    EXPECT_EQ((*edges_res)[0].target_pk.as_int64(), 99);
}

// ---------------------------------------------------------------------------
// AC: an edge with properties survives crash + recovery with properties
// intact (correct types and values).
// ---------------------------------------------------------------------------
TEST_F(QA_GDB884, LinkedEdgeWithPropertiesSurvivesCrashAndRecovery) {
    create_node_tables();
    open_engine();

    ColumnDef weight_col;
    weight_col.name = "weight";
    weight_col.type = TypeId::FLOAT64;
    ColumnDef label_col;
    label_col.name = "label";
    label_col.type = TypeId::STRING;

    auto edge_res = graph_engine_->create_edge_type(db_id_,
                                                    "connects",
                                                    src_table_id_,
                                                    tgt_table_id_,
                                                    TypeId::INT64,
                                                    TypeId::INT64,
                                                    {weight_col, label_col});
    ASSERT_TRUE(edge_res) << edge_res.error().message;

    Value src_pk = Value(static_cast<int64_t>(5));
    Value tgt_pk = Value(static_cast<int64_t>(7));
    std::vector<Value> props = {Value(2.5), Value(std::string("strong"))};
    auto link_res = graph_engine_->link(db_id_, "connects", src_pk, tgt_pk, props);
    ASSERT_TRUE(link_res) << link_res.error().message;

    close_wal_without_clean_shutdown();
    graph_engine_.reset();
    dm_.reset();
    wal_writer_.reset();

    EdgeTableConfig cfg;
    cfg.edge_id = *edge_res;
    cfg.name = "connects";
    cfg.source_table_id = src_table_id_;
    cfg.target_table_id = tgt_table_id_;
    cfg.source_pk_type = TypeId::INT64;
    cfg.target_pk_type = TypeId::INT64;
    cfg.property_columns = {weight_col, label_col};
    EdgeTable fresh_table(cfg);
    ASSERT_EQ(fresh_table.size(), 0u);

    GraphEngineRecoveryHandler graph_handler;
    graph_handler.register_edge_table(
        db_id_, "connects", &fresh_table, {TypeId::FLOAT64, TypeId::STRING});
    run_recovery(graph_handler);

    auto edges_res = fresh_table.get_edges_from(src_pk);
    ASSERT_TRUE(edges_res) << edges_res.error().message;
    ASSERT_EQ(edges_res->size(), 1u);

    const auto& edge = (*edges_res)[0];
    ASSERT_EQ(edge.properties.size(), 2u)
        << "properties must survive recovery (were dropped pre-fix)";
    EXPECT_NEAR(edge.properties[0].as_float64(), 2.5, 1e-9);
    EXPECT_EQ(edge.properties[1].as_string(), "strong");
}

// ---------------------------------------------------------------------------
// AC: an UNLINKed edge stays deleted after crash + recovery.
// ---------------------------------------------------------------------------
TEST_F(QA_GDB884, UnlinkedEdgeStaysDeletedAfterCrashAndRecovery) {
    create_node_tables();
    open_engine();

    auto edge_res = graph_engine_->create_edge_type(
        db_id_, "follows", src_table_id_, tgt_table_id_, TypeId::INT64, TypeId::INT64, {});
    ASSERT_TRUE(edge_res) << edge_res.error().message;

    Value src_pk = Value(static_cast<int64_t>(1));
    Value tgt_pk = Value(static_cast<int64_t>(2));
    auto link_res = graph_engine_->link(db_id_, "follows", src_pk, tgt_pk, {});
    ASSERT_TRUE(link_res) << link_res.error().message;

    auto unlink_res = graph_engine_->unlink(db_id_, "follows", src_pk, tgt_pk);
    ASSERT_TRUE(unlink_res) << unlink_res.error().message;

    close_wal_without_clean_shutdown();
    graph_engine_.reset();
    dm_.reset();
    wal_writer_.reset();

    // Pre-load the edge as if the heap file had it flushed before the delete
    // (i.e. the state that must be corrected by replaying EDGE_DELETE).
    EdgeTableConfig cfg;
    cfg.edge_id = *edge_res;
    cfg.name = "follows";
    cfg.source_table_id = src_table_id_;
    cfg.target_table_id = tgt_table_id_;
    cfg.source_pk_type = TypeId::INT64;
    cfg.target_pk_type = TypeId::INT64;
    EdgeTable pre_loaded_table(cfg);
    auto restore_res = pre_loaded_table.restore_edge(1, src_pk, tgt_pk, {});
    ASSERT_TRUE(restore_res) << restore_res.error().message;
    ASSERT_EQ(pre_loaded_table.size(), 1u);

    GraphEngineRecoveryHandler graph_handler;
    graph_handler.register_edge_table(db_id_, "follows", &pre_loaded_table, {});
    run_recovery(graph_handler);

    auto edges_res = pre_loaded_table.get_edges_from(src_pk);
    ASSERT_TRUE(edges_res) << edges_res.error().message;
    EXPECT_EQ(edges_res->size(), 0u) << "edge must remain deleted after recovery replays UNLINK";
}

} // namespace
} // namespace sixseven
