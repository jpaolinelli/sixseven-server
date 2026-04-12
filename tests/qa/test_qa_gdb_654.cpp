/// @file test_qa_gdb_654.cpp
/// QA adversarial tests for GDB-654: Add sys_databases system table and
/// persistence methods.
///
/// Acceptance Criteria:
///   AC1: sys_databases_table_id = 9
///   AC2: first_user_table_id bumped to 10
///   AC3: sys_databases_schema() uses sys_databases_table_id
///   AC4: persist_database() inserts a row into sys_databases
///   AC5: remove_database() deletes a row from sys_databases
///   AC6: create_system_catalog_tables() includes sys_databases
///   AC7: Unit tests pass
///
/// Adversarial categories: boundary values, duplicate inserts, remove
/// non-existent, empty strings, large IDs, multiple restart cycles,
/// idempotent removal, interleaved persist/remove, stress.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/config.h"
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
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Test fixture (mirrors CatalogPersistenceTest from unit tests)
// ============================================================================

class QA_GDB654 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb654";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        config_ = Config::load_defaults();
    }

    void TearDown() override {
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void run_bootstrap() {
        auto result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }

    void restart() {
        engine_.reset();
        persistence_.reset();
        storage_.reset();
        catalog_.reset();
        dm_.reset();

        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
    }

    /// Read all (db_id, name) pairs from sys_databases heap.
    std::vector<std::pair<int32_t, std::string>> read_all_databases() {
        std::vector<std::pair<int32_t, std::string>> result;
        auto ts = storage_->get_table_storage(sys_databases_table_id);
        if (!ts) return result;

        auto storage_schema = StorageManager::build_storage_schema(sys_databases_schema());
        auto it = (*ts)->heap->begin();
        if (!it) return result;

        while (auto row = it->next()) {
            auto values = TupleSerializer::deserialize(row->second, storage_schema);
            if (!values) continue;
            result.emplace_back((*values)[0].as_int32(), (*values)[1].as_string());
        }
        return result;
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
// AC1: sys_databases_table_id = 9
// ============================================================================

TEST_F(QA_GDB654, AC1_SysDatabasesTableIdIs9) {
    EXPECT_EQ(sys_databases_table_id, 9);
}

// ============================================================================
// AC2: first_user_table_id bumped to 10
// ============================================================================

TEST_F(QA_GDB654, AC2_FirstUserTableIdIs10) {
    EXPECT_EQ(first_user_table_id, 10);
}

// AC2 boundary: sys_databases_table_id must be strictly less than first_user_table_id
TEST_F(QA_GDB654, AC2_SysDatabasesIdLessThanFirstUserId) {
    EXPECT_LT(sys_databases_table_id, first_user_table_id);
}

// All system table IDs must be unique.
TEST_F(QA_GDB654, AC2_AllSystemTableIdsUnique) {
    std::set<table_id_t> ids = {
        sys_settings_table_id,
        sys_providers_table_id,
        sys_tables_table_id,
        sys_columns_table_id,
        sys_indexes_table_id,
        sys_edge_types_table_id,
        sys_embedding_columns_table_id,
        sys_embedding_jobs_table_id,
        sys_databases_table_id,
    };
    EXPECT_EQ(ids.size(), 9u) << "Duplicate system table IDs detected";
}

// ============================================================================
// AC3: sys_databases_schema() uses sys_databases_table_id
// ============================================================================

TEST_F(QA_GDB654, AC3_SchemaUsesCorrectTableId) {
    auto schema = sys_databases_schema();
    EXPECT_EQ(schema.table_id, sys_databases_table_id);
    EXPECT_EQ(schema.name, "sys_databases");
}

TEST_F(QA_GDB654, AC3_SchemaHasExpectedColumns) {
    auto schema = sys_databases_schema();
    ASSERT_EQ(schema.columns.size(), 2u);
    EXPECT_EQ(schema.columns[0].name, "database_id");
    EXPECT_EQ(schema.columns[0].type_id, TypeId::INT32);
    EXPECT_EQ(schema.columns[1].name, "name");
    EXPECT_EQ(schema.columns[1].type_id, TypeId::STRING);
}

TEST_F(QA_GDB654, AC3_SchemaHasPrimaryKey) {
    auto schema = sys_databases_schema();
    EXPECT_EQ(schema.pk_columns, "database_id");
}

// ============================================================================
// AC4: persist_database() inserts a row into sys_databases
// ============================================================================

TEST_F(QA_GDB654, AC4_PersistDatabaseBasic) {
    run_bootstrap();

    auto result = persistence_->persist_database(100, "my_db");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 1u);
    EXPECT_EQ(dbs[0].first, 100);
    EXPECT_EQ(dbs[0].second, "my_db");
}

TEST_F(QA_GDB654, AC4_PersistDatabaseSurvivesRestart) {
    run_bootstrap();

    ASSERT_TRUE(persistence_->persist_database(1, "alpha").has_value());
    ASSERT_TRUE(persistence_->persist_database(2, "beta").has_value());

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    EXPECT_EQ(dbs.size(), 2u);

    // Verify both entries are present (order may vary).
    std::set<int32_t> ids;
    for (auto& [id, name] : dbs) ids.insert(id);
    EXPECT_TRUE(ids.count(1));
    EXPECT_TRUE(ids.count(2));
}

// ============================================================================
// AC5: remove_database() deletes a row from sys_databases
// ============================================================================

TEST_F(QA_GDB654, AC5_RemoveDatabaseBasic) {
    run_bootstrap();

    ASSERT_TRUE(persistence_->persist_database(10, "db_a").has_value());
    ASSERT_TRUE(persistence_->persist_database(20, "db_b").has_value());

    auto result = persistence_->remove_database(10);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 1u);
    EXPECT_EQ(dbs[0].first, 20);
}

TEST_F(QA_GDB654, AC5_RemoveDatabaseSurvivesRestart) {
    run_bootstrap();

    ASSERT_TRUE(persistence_->persist_database(10, "db_a").has_value());
    ASSERT_TRUE(persistence_->persist_database(20, "db_b").has_value());
    ASSERT_TRUE(persistence_->remove_database(10).has_value());

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 1u);
    EXPECT_EQ(dbs[0].first, 20);
}

// ============================================================================
// AC6: create_system_catalog_tables() includes sys_databases
// ============================================================================

TEST_F(QA_GDB654, AC6_SysDatabasesCreatedDuringBootstrap) {
    run_bootstrap();

    auto result = catalog_->get_table(system_database_id, "sys_databases");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->table_id, sys_databases_table_id);
}

TEST_F(QA_GDB654, AC6_SysDatabasesOpenedOnRestart) {
    run_bootstrap();
    restart();
    run_bootstrap();

    auto result = catalog_->get_table(system_database_id, "sys_databases");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->table_id, sys_databases_table_id);
}

TEST_F(QA_GDB654, AC6_SysDatabasesStorageAccessible) {
    run_bootstrap();

    auto ts = storage_->get_table_storage(sys_databases_table_id);
    ASSERT_TRUE(ts.has_value()) << ts.error().message;
    // Verify we can iterate (should be empty initially).
    auto it = (*ts)->heap->begin();
    ASSERT_TRUE(it.has_value()) << it.error().message;
}

// ============================================================================
// Adversarial: Boundary values
// ============================================================================

TEST_F(QA_GDB654, Adversarial_ZeroDatabaseId) {
    run_bootstrap();

    auto result = persistence_->persist_database(0, "zero_db");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 1u);
    EXPECT_EQ(dbs[0].first, 0);
}

TEST_F(QA_GDB654, Adversarial_NegativeDatabaseId) {
    run_bootstrap();

    auto result = persistence_->persist_database(-1, "neg_db");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 1u);
    EXPECT_EQ(dbs[0].first, -1);
}

TEST_F(QA_GDB654, Adversarial_MaxInt32DatabaseId) {
    run_bootstrap();

    auto result = persistence_->persist_database(INT32_MAX, "max_db");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 1u);
    EXPECT_EQ(dbs[0].first, INT32_MAX);
    EXPECT_EQ(dbs[0].second, "max_db");
}

TEST_F(QA_GDB654, Adversarial_MinInt32DatabaseId) {
    run_bootstrap();

    auto result = persistence_->persist_database(INT32_MIN, "min_db");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 1u);
    EXPECT_EQ(dbs[0].first, INT32_MIN);
}

// ============================================================================
// Adversarial: Empty and special strings
// ============================================================================

TEST_F(QA_GDB654, Adversarial_EmptyDatabaseName) {
    run_bootstrap();

    auto result = persistence_->persist_database(1, "");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 1u);
    EXPECT_EQ(dbs[0].second, "");
}

TEST_F(QA_GDB654, Adversarial_LongDatabaseName) {
    run_bootstrap();

    std::string long_name(1024, 'x');
    auto result = persistence_->persist_database(1, long_name);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 1u);
    EXPECT_EQ(dbs[0].second, long_name);
}

TEST_F(QA_GDB654, Adversarial_SpecialCharactersInName) {
    run_bootstrap();

    std::string special = "db with spaces & 'quotes' \"double\" \ttab \nnewline";
    auto result = persistence_->persist_database(1, special);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 1u);
    EXPECT_EQ(dbs[0].second, special);
}

// ============================================================================
// Adversarial: Duplicate inserts (same ID)
// ============================================================================

TEST_F(QA_GDB654, Adversarial_DuplicateDatabaseId) {
    run_bootstrap();

    ASSERT_TRUE(persistence_->persist_database(1, "first").has_value());
    // Second insert with same ID — no uniqueness enforcement at storage level,
    // so this should succeed (or fail if enforced). Either way, no crash.
    auto result = persistence_->persist_database(1, "second");
    // We just verify no crash / no undefined behavior.
    // If it succeeds, both rows exist; if it fails, that's also acceptable.
    (void)result;

    auto dbs = read_all_databases();
    EXPECT_GE(dbs.size(), 1u);
}

// ============================================================================
// Adversarial: Remove non-existent database
// ============================================================================

TEST_F(QA_GDB654, Adversarial_RemoveNonExistentDatabase) {
    run_bootstrap();

    // Remove from empty table — should succeed (no rows match predicate).
    auto result = persistence_->remove_database(999);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto dbs = read_all_databases();
    EXPECT_EQ(dbs.size(), 0u);
}

TEST_F(QA_GDB654, Adversarial_RemoveAlreadyRemoved) {
    run_bootstrap();

    ASSERT_TRUE(persistence_->persist_database(1, "db1").has_value());
    ASSERT_TRUE(persistence_->remove_database(1).has_value());

    // Double remove — should be a no-op.
    auto result = persistence_->remove_database(1);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto dbs = read_all_databases();
    EXPECT_EQ(dbs.size(), 0u);
}

// ============================================================================
// Adversarial: Interleaved persist and remove
// ============================================================================

TEST_F(QA_GDB654, Adversarial_InterleavedPersistRemove) {
    run_bootstrap();

    ASSERT_TRUE(persistence_->persist_database(1, "a").has_value());
    ASSERT_TRUE(persistence_->persist_database(2, "b").has_value());
    ASSERT_TRUE(persistence_->remove_database(1).has_value());
    ASSERT_TRUE(persistence_->persist_database(3, "c").has_value());
    ASSERT_TRUE(persistence_->remove_database(2).has_value());
    ASSERT_TRUE(persistence_->persist_database(4, "d").has_value());

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    std::set<int32_t> ids;
    for (auto& [id, name] : dbs) ids.insert(id);

    EXPECT_EQ(ids.size(), 2u);
    EXPECT_TRUE(ids.count(3));
    EXPECT_TRUE(ids.count(4));
}

// ============================================================================
// Adversarial: Multiple restart cycles
// ============================================================================

TEST_F(QA_GDB654, Adversarial_MultipleRestartCycles) {
    run_bootstrap();
    ASSERT_TRUE(persistence_->persist_database(1, "db1").has_value());

    // Restart cycle 1
    restart();
    run_bootstrap();
    ASSERT_TRUE(persistence_->persist_database(2, "db2").has_value());

    // Restart cycle 2
    restart();
    run_bootstrap();
    ASSERT_TRUE(persistence_->persist_database(3, "db3").has_value());

    // Restart cycle 3 — verify all three survive
    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    EXPECT_EQ(dbs.size(), 3u);
}

// ============================================================================
// Adversarial: Remove then re-add same ID
// ============================================================================

TEST_F(QA_GDB654, Adversarial_RemoveThenReAddSameId) {
    run_bootstrap();

    ASSERT_TRUE(persistence_->persist_database(1, "original").has_value());
    ASSERT_TRUE(persistence_->remove_database(1).has_value());
    ASSERT_TRUE(persistence_->persist_database(1, "replacement").has_value());

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 1u);
    EXPECT_EQ(dbs[0].first, 1);
    EXPECT_EQ(dbs[0].second, "replacement");
}

// ============================================================================
// Adversarial: Stress — many databases
// ============================================================================

TEST_F(QA_GDB654, Adversarial_StressManyDatabases) {
    run_bootstrap();

    constexpr int N = 100;
    for (int i = 0; i < N; ++i) {
        auto result = persistence_->persist_database(i, "db_" + std::to_string(i));
        ASSERT_TRUE(result.has_value()) << "Failed at i=" << i << ": " << result.error().message;
    }

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    EXPECT_EQ(dbs.size(), static_cast<size_t>(N));

    // Verify all IDs present.
    std::set<int32_t> ids;
    for (auto& [id, name] : dbs) ids.insert(id);
    for (int i = 0; i < N; ++i) {
        EXPECT_TRUE(ids.count(i)) << "Missing database ID " << i;
    }
}

TEST_F(QA_GDB654, Adversarial_StressRemoveHalf) {
    run_bootstrap();

    constexpr int N = 50;
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(persistence_->persist_database(i, "db_" + std::to_string(i)).has_value());
    }

    // Remove even IDs.
    for (int i = 0; i < N; i += 2) {
        ASSERT_TRUE(persistence_->remove_database(i).has_value());
    }

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    EXPECT_EQ(dbs.size(), static_cast<size_t>(N / 2));

    std::set<int32_t> ids;
    for (auto& [id, name] : dbs) ids.insert(id);
    for (int i = 1; i < N; i += 2) {
        EXPECT_TRUE(ids.count(i)) << "Missing odd ID " << i;
    }
    for (int i = 0; i < N; i += 2) {
        EXPECT_FALSE(ids.count(i)) << "Even ID " << i << " should have been removed";
    }
}

// ============================================================================
// Adversarial: Remove all databases leaves table empty
// ============================================================================

TEST_F(QA_GDB654, Adversarial_RemoveAllDatabases) {
    run_bootstrap();

    ASSERT_TRUE(persistence_->persist_database(1, "a").has_value());
    ASSERT_TRUE(persistence_->persist_database(2, "b").has_value());
    ASSERT_TRUE(persistence_->persist_database(3, "c").has_value());

    ASSERT_TRUE(persistence_->remove_database(1).has_value());
    ASSERT_TRUE(persistence_->remove_database(2).has_value());
    ASSERT_TRUE(persistence_->remove_database(3).has_value());

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    EXPECT_EQ(dbs.size(), 0u);
}

} // namespace
} // namespace sixseven
