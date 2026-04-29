/// @file test_qa_gdb_660.cpp
/// QA adversarial tests for GDB-660: DROP DATABASE CASCADE leaves orphaned
/// rows in sys_tables/sys_columns.
///
/// Verifies:
///   AC1: DROP DATABASE CASCADE removes sys_tables rows for tables in the
///        dropped database.
///   AC2: DROP DATABASE CASCADE removes sys_columns rows for tables in the
///        dropped database.
///   AC3: After restart, no warnings about orphaned tables from the dropped db.
///   AC4: Tables in OTHER databases are NOT affected by CASCADE drop.
///   AC5: sys_indexes rows for tables in the dropped db are also cleaned.
///
/// Adversarial: multiple tables, multiple databases, tables with indexes,
/// restart survival, empty database cascade, stress (many tables).

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/config.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace sixseven {
namespace {

class QA_GDB660 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "qa_gdb660";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        create_components();
    }

    void TearDown() override {
        destroy_components();
        std::filesystem::remove_all(data_dir_);
    }

    void create_components() {
        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        config_ = Config::load_defaults();
    }

    void destroy_components() {
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
    }

    void run_bootstrap() {
        auto result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }

    void restart() {
        destroy_components();
        create_components();
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << " => " << result.error().message;
        return std::move(*result);
    }

    database_id_t switch_to_db(const std::string& name) {
        auto db = catalog_->get_database(name);
        EXPECT_TRUE(db.has_value()) << "database '" << name << "' not found";
        engine_->set_current_database(db->database_id);
        return db->database_id;
    }

    void switch_to_default() { engine_->set_current_database(default_database_id); }

    table_id_t get_table_id(database_id_t db_id, const std::string& name) {
        auto tbl = catalog_->get_table(db_id, name);
        EXPECT_TRUE(tbl.has_value()) << "table '" << name << "' not found";
        return tbl->table_id;
    }

    struct SysTableRow {
        table_id_t table_id;
        database_id_t database_id;
        std::string name;
    };

    std::vector<SysTableRow> scan_sys_tables() {
        std::vector<SysTableRow> rows;
        auto ts = storage_->get_table_storage(sys_tables_table_id);
        if (!ts) return rows;
        auto schema = StorageManager::build_storage_schema(sys_tables_schema());
        auto it = (*ts)->heap->begin();
        if (!it) return rows;
        while (auto row = it->next()) {
            auto values = TupleSerializer::deserialize(row->second, schema);
            if (!values) continue;
            rows.push_back({static_cast<table_id_t>((*values)[0].as_int32()),
                            static_cast<database_id_t>((*values)[1].as_int32()),
                            (*values)[2].as_string()});
        }
        return rows;
    }

    struct SysColumnRow {
        table_id_t table_id;
        int32_t ordinal;
        std::string name;
    };

    std::vector<SysColumnRow> scan_sys_columns() {
        std::vector<SysColumnRow> rows;
        auto ts = storage_->get_table_storage(sys_columns_table_id);
        if (!ts) return rows;
        auto schema = StorageManager::build_storage_schema(sys_columns_schema());
        auto it = (*ts)->heap->begin();
        if (!it) return rows;
        while (auto row = it->next()) {
            auto values = TupleSerializer::deserialize(row->second, schema);
            if (!values) continue;
            rows.push_back({static_cast<table_id_t>((*values)[0].as_int32()),
                            (*values)[1].as_int32(),
                            (*values)[2].as_string()});
        }
        return rows;
    }

    struct SysIndexRow {
        int32_t index_id;
        table_id_t table_id;
        std::string name;
    };

    std::vector<SysIndexRow> scan_sys_indexes() {
        std::vector<SysIndexRow> rows;
        auto ts = storage_->get_table_storage(sys_indexes_table_id);
        if (!ts) return rows;
        auto schema = StorageManager::build_storage_schema(sys_indexes_schema());
        auto it = (*ts)->heap->begin();
        if (!it) return rows;
        while (auto row = it->next()) {
            auto values = TupleSerializer::deserialize(row->second, schema);
            if (!values) continue;
            rows.push_back({(*values)[0].as_int32(),
                            static_cast<table_id_t>((*values)[1].as_int32()),
                            (*values)[2].as_string()});
        }
        return rows;
    }

    bool sys_tables_contains(table_id_t tid) {
        auto rows = scan_sys_tables();
        return std::any_of(
            rows.begin(), rows.end(), [tid](const auto& r) { return r.table_id == tid; });
    }

    bool sys_columns_contains(table_id_t tid) {
        auto rows = scan_sys_columns();
        return std::any_of(
            rows.begin(), rows.end(), [tid](const auto& r) { return r.table_id == tid; });
    }

    bool sys_indexes_contains_table(table_id_t tid) {
        auto rows = scan_sys_indexes();
        return std::any_of(
            rows.begin(), rows.end(), [tid](const auto& r) { return r.table_id == tid; });
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    Config config_;
};

// ============================================================================
// AC1: sys_tables rows removed on CASCADE drop
// ============================================================================

TEST_F(QA_GDB660, CascadeRemovesSysTablesRows) {
    run_bootstrap();
    switch_to_default();
    exec_ok("CREATE DATABASE dropme");
    auto db_id = switch_to_db("dropme");
    exec_ok("CREATE TABLE t1 (id INT PRIMARY KEY, name VARCHAR)");

    auto t1_id = get_table_id(db_id, "t1");

    ASSERT_TRUE(sys_tables_contains(t1_id)) << "t1 should be in sys_tables before drop";

    switch_to_default();
    exec_ok("DROP DATABASE dropme CASCADE");

    EXPECT_FALSE(sys_tables_contains(t1_id)) << "t1 should NOT be in sys_tables after CASCADE drop";
}

// ============================================================================
// AC2: sys_columns rows removed on CASCADE drop
// ============================================================================

TEST_F(QA_GDB660, CascadeRemovesSysColumnsRows) {
    run_bootstrap();
    switch_to_default();
    exec_ok("CREATE DATABASE dropme2");
    auto db_id = switch_to_db("dropme2");
    exec_ok("CREATE TABLE t2 (id INT PRIMARY KEY, x FLOAT, y FLOAT, label VARCHAR)");

    auto t2_id = get_table_id(db_id, "t2");

    ASSERT_TRUE(sys_columns_contains(t2_id)) << "t2 columns should be in sys_columns before drop";

    switch_to_default();
    exec_ok("DROP DATABASE dropme2 CASCADE");

    EXPECT_FALSE(sys_columns_contains(t2_id))
        << "t2 columns should NOT be in sys_columns after CASCADE drop";
}

// ============================================================================
// AC3: Restart survival — no orphaned rows after restart
// ============================================================================

TEST_F(QA_GDB660, NoOrphanedRowsAfterRestart) {
    run_bootstrap();
    switch_to_default();
    exec_ok("CREATE DATABASE restart_test");
    auto db_id = switch_to_db("restart_test");
    exec_ok("CREATE TABLE rt1 (id INT PRIMARY KEY)");
    exec_ok("CREATE TABLE rt2 (id INT PRIMARY KEY, val VARCHAR)");

    auto rt1_id = get_table_id(db_id, "rt1");
    auto rt2_id = get_table_id(db_id, "rt2");

    switch_to_default();
    exec_ok("DROP DATABASE restart_test CASCADE");

    // Simulate restart.
    restart();
    run_bootstrap();

    EXPECT_FALSE(sys_tables_contains(rt1_id)) << "rt1 in sys_tables after restart";
    EXPECT_FALSE(sys_tables_contains(rt2_id)) << "rt2 in sys_tables after restart";
    EXPECT_FALSE(sys_columns_contains(rt1_id)) << "rt1 columns in sys_columns after restart";
    EXPECT_FALSE(sys_columns_contains(rt2_id)) << "rt2 columns in sys_columns after restart";

    auto db = catalog_->get_database("restart_test");
    EXPECT_FALSE(db.has_value()) << "database 'restart_test' should not exist after restart";
}

// ============================================================================
// AC4: Tables in OTHER databases are NOT affected
// ============================================================================

TEST_F(QA_GDB660, OtherDatabaseTablesUnaffected) {
    run_bootstrap();
    switch_to_default();
    exec_ok("CREATE DATABASE keep_db");
    exec_ok("CREATE DATABASE drop_db");

    auto keep_id = switch_to_db("keep_db");
    exec_ok("CREATE TABLE keeper (id INT PRIMARY KEY, data VARCHAR)");
    auto keeper_id = get_table_id(keep_id, "keeper");

    auto drop_id = switch_to_db("drop_db");
    exec_ok("CREATE TABLE goner (id INT PRIMARY KEY)");
    auto goner_id = get_table_id(drop_id, "goner");

    switch_to_default();
    exec_ok("DROP DATABASE drop_db CASCADE");

    EXPECT_FALSE(sys_tables_contains(goner_id)) << "goner should be removed";
    EXPECT_TRUE(sys_tables_contains(keeper_id)) << "keeper should be preserved";
    EXPECT_FALSE(sys_columns_contains(goner_id)) << "goner columns should be removed";
    EXPECT_TRUE(sys_columns_contains(keeper_id)) << "keeper columns should be preserved";

    // Verify keeper survives restart too.
    restart();
    run_bootstrap();
    EXPECT_TRUE(sys_tables_contains(keeper_id)) << "keeper should survive restart";

    auto db = catalog_->get_database("keep_db");
    EXPECT_TRUE(db.has_value()) << "keep_db should exist after restart";
}

// ============================================================================
// AC5: sys_indexes rows for tables in dropped db are also cleaned
// ============================================================================

TEST_F(QA_GDB660, CascadeRemovesSysIndexesRows) {
    run_bootstrap();
    switch_to_default();
    exec_ok("CREATE DATABASE idx_db");
    auto db_id = switch_to_db("idx_db");
    exec_ok("CREATE TABLE idx_table (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE INDEX idx_name ON idx_table (name)");

    auto tbl_id = get_table_id(db_id, "idx_table");

    ASSERT_TRUE(sys_indexes_contains_table(tbl_id)) << "index should exist before drop";

    switch_to_default();
    exec_ok("DROP DATABASE idx_db CASCADE");

    EXPECT_FALSE(sys_indexes_contains_table(tbl_id))
        << "sys_indexes rows should be removed after CASCADE drop";
}

// ============================================================================
// Edge case: Empty database CASCADE (no tables to clean up)
// ============================================================================

TEST_F(QA_GDB660, EmptyDatabaseCascade) {
    run_bootstrap();
    switch_to_default();
    exec_ok("CREATE DATABASE empty_db");

    auto tables_before = scan_sys_tables();

    exec_ok("DROP DATABASE empty_db CASCADE");

    auto tables_after = scan_sys_tables();
    EXPECT_EQ(tables_before.size(), tables_after.size())
        << "empty CASCADE should not affect sys_tables row count";
}

// ============================================================================
// Edge case: Multiple tables in the dropped database
// ============================================================================

TEST_F(QA_GDB660, MultipleTables) {
    run_bootstrap();
    switch_to_default();
    exec_ok("CREATE DATABASE multi_db");
    auto db_id = switch_to_db("multi_db");

    std::vector<table_id_t> table_ids;
    for (int i = 0; i < 5; ++i) {
        auto sql = "CREATE TABLE multi_t" + std::to_string(i) + " (id INT PRIMARY KEY, v INT)";
        exec_ok(sql);
        table_ids.push_back(get_table_id(db_id, "multi_t" + std::to_string(i)));
    }

    for (auto tid : table_ids) {
        ASSERT_TRUE(sys_tables_contains(tid));
        ASSERT_TRUE(sys_columns_contains(tid));
    }

    switch_to_default();
    exec_ok("DROP DATABASE multi_db CASCADE");

    for (auto tid : table_ids) {
        EXPECT_FALSE(sys_tables_contains(tid))
            << "table_id " << tid << " still in sys_tables after CASCADE";
        EXPECT_FALSE(sys_columns_contains(tid))
            << "table_id " << tid << " still in sys_columns after CASCADE";
    }
}

// ============================================================================
// Restart survival with multiple databases — only dropped db is cleaned
// ============================================================================

TEST_F(QA_GDB660, RestartWithMultipleDatabases) {
    run_bootstrap();
    switch_to_default();
    exec_ok("CREATE DATABASE survive_db");
    exec_ok("CREATE DATABASE doomed_db");

    auto survive_id = switch_to_db("survive_db");
    exec_ok("CREATE TABLE s_table (id INT PRIMARY KEY)");
    auto s_id = get_table_id(survive_id, "s_table");

    auto doomed_id = switch_to_db("doomed_db");
    exec_ok("CREATE TABLE d_table (id INT PRIMARY KEY)");
    auto d_id = get_table_id(doomed_id, "d_table");

    switch_to_default();
    exec_ok("DROP DATABASE doomed_db CASCADE");

    restart();
    run_bootstrap();

    EXPECT_TRUE(sys_tables_contains(s_id)) << "survive_db table should persist";
    EXPECT_FALSE(sys_tables_contains(d_id)) << "doomed_db table should be gone";

    auto survive = catalog_->get_database("survive_db");
    EXPECT_TRUE(survive.has_value());
    auto doomed = catalog_->get_database("doomed_db");
    EXPECT_FALSE(doomed.has_value());
}

// ============================================================================
// Stress: Drop database with many tables
// ============================================================================

TEST_F(QA_GDB660, StressManyTables) {
    run_bootstrap();
    switch_to_default();
    exec_ok("CREATE DATABASE stress_db");
    auto db_id = switch_to_db("stress_db");

    constexpr int NUM_TABLES = 20;
    std::vector<table_id_t> table_ids;

    for (int i = 0; i < NUM_TABLES; ++i) {
        auto sql =
            "CREATE TABLE stress_t" + std::to_string(i) + " (id INT PRIMARY KEY, data VARCHAR)";
        exec_ok(sql);
        table_ids.push_back(get_table_id(db_id, "stress_t" + std::to_string(i)));
    }

    switch_to_default();
    exec_ok("DROP DATABASE stress_db CASCADE");

    for (auto tid : table_ids) {
        EXPECT_FALSE(sys_tables_contains(tid)) << "stress table " << tid << " still in sys_tables";
        EXPECT_FALSE(sys_columns_contains(tid))
            << "stress table " << tid << " still in sys_columns";
    }

    // Verify clean restart.
    restart();
    run_bootstrap();

    for (auto tid : table_ids) {
        EXPECT_FALSE(sys_tables_contains(tid))
            << "stress table " << tid << " still in sys_tables after restart";
    }
}

// ============================================================================
// DROP DATABASE without CASCADE should not trigger table persistence cleanup
// (non-empty database should fail, not clean up)
// ============================================================================

TEST_F(QA_GDB660, DropWithoutCascadeFailsOnNonEmpty) {
    run_bootstrap();
    switch_to_default();
    exec_ok("CREATE DATABASE nonempty_db");
    auto db_id = switch_to_db("nonempty_db");
    exec_ok("CREATE TABLE blocker (id INT PRIMARY KEY)");

    auto blocker_id = get_table_id(db_id, "blocker");

    switch_to_default();
    auto result = engine_->execute("DROP DATABASE nonempty_db");
    EXPECT_FALSE(result.has_value()) << "DROP without CASCADE on non-empty db should fail";

    EXPECT_TRUE(sys_tables_contains(blocker_id))
        << "blocker table should still be in sys_tables after failed DROP";
}

// ============================================================================
// Sequential cascade drops: drop db A, create db B, drop db B — no leaks
// ============================================================================

TEST_F(QA_GDB660, SequentialCascadeDrops) {
    run_bootstrap();
    switch_to_default();

    exec_ok("CREATE DATABASE seq_a");
    auto a_db_id = switch_to_db("seq_a");
    exec_ok("CREATE TABLE a_tbl (id INT PRIMARY KEY)");
    auto a_id = get_table_id(a_db_id, "a_tbl");

    switch_to_default();
    exec_ok("DROP DATABASE seq_a CASCADE");
    EXPECT_FALSE(sys_tables_contains(a_id));

    exec_ok("CREATE DATABASE seq_b");
    auto b_db_id = switch_to_db("seq_b");
    exec_ok("CREATE TABLE b_tbl (id INT PRIMARY KEY)");
    auto b_id = get_table_id(b_db_id, "b_tbl");

    switch_to_default();
    exec_ok("DROP DATABASE seq_b CASCADE");
    EXPECT_FALSE(sys_tables_contains(b_id));

    restart();
    run_bootstrap();

    EXPECT_FALSE(sys_tables_contains(a_id));
    EXPECT_FALSE(sys_tables_contains(b_id));
}

}  // namespace
}  // namespace sixseven
