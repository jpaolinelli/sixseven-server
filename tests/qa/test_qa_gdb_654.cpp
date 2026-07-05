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
        if (!ts)
            return result;

        auto storage_schema = StorageManager::build_storage_schema(sys_databases_schema());
        auto it = (*ts)->heap->begin();
        if (!it)
            return result;

        for (;;) {
            auto row_result = it->next();
            if (!row_result.has_value())
                break;
            if (!row_result->has_value())
                break;
            auto values = TupleSerializer::deserialize((*row_result)->second, storage_schema);
            if (!values)
                continue;
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
// AC2: first_user_table_id sits above all reserved system table ids.
// (Originally 10; bumped to 12 when sys_users reserved id 11 was added.)
// ============================================================================

TEST_F(QA_GDB654, AC2_FirstUserTableIdAboveSystemIds) {
    EXPECT_EQ(first_user_table_id, 12);
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
        sys_users_table_id,
    };
    EXPECT_EQ(ids.size(), 10u) << "Duplicate system table IDs detected";
    EXPECT_LT(*ids.rbegin(), first_user_table_id)
        << "System table ID leaked into the user table ID range";
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
    // GDB-1224: run_bootstrap() always persists the default database (id=1,
    // "demo") as part of SystemBootstrap::bootstrap()'s first-run path (see
    // src/executor/system_bootstrap.cpp), so read_all_databases() always
    // has that row in addition to whatever the test explicitly persists.
    // These tests predate that always-persisted default row and asserted
    // exact counts / a fixed dbs[0] index that didn't account for it.
    run_bootstrap();

    auto result = persistence_->persist_database(100, "my_db");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 2u);
    auto it = std::find_if(dbs.begin(), dbs.end(), [](const auto& p) { return p.first == 100; });
    ASSERT_NE(it, dbs.end());
    EXPECT_EQ(it->second, "my_db");
}

TEST_F(QA_GDB654, AC4_PersistDatabaseSurvivesRestart) {
    run_bootstrap();

    ASSERT_TRUE(persistence_->persist_database(2, "alpha").has_value());
    ASSERT_TRUE(persistence_->persist_database(3, "beta").has_value());

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    EXPECT_EQ(dbs.size(), 3u); // default (id=1) + alpha (2) + beta (3)

    // Verify both entries are present (order may vary).
    std::set<int32_t> ids;
    for (auto& [id, name] : dbs)
        ids.insert(id);
    EXPECT_TRUE(ids.count(1)); // default database
    EXPECT_TRUE(ids.count(2));
    EXPECT_TRUE(ids.count(3));
}

// ============================================================================
// AC5: remove_database() deletes a row from sys_databases
// ============================================================================

TEST_F(QA_GDB654, AC5_RemoveDatabaseBasic) {
    // GDB-1224: see the AC4_PersistDatabaseBasic comment -- run_bootstrap()
    // always leaves the default database (id=1) persisted.
    run_bootstrap();

    ASSERT_TRUE(persistence_->persist_database(10, "db_a").has_value());
    ASSERT_TRUE(persistence_->persist_database(20, "db_b").has_value());

    auto result = persistence_->remove_database(10);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 2u); // default (id=1) + db_b (20)
    std::set<int32_t> ids;
    for (auto& [id, name] : dbs)
        ids.insert(id);
    EXPECT_TRUE(ids.count(1));
    EXPECT_TRUE(ids.count(20));
    EXPECT_FALSE(ids.count(10));
}

TEST_F(QA_GDB654, AC5_RemoveDatabaseSurvivesRestart) {
    run_bootstrap();

    ASSERT_TRUE(persistence_->persist_database(10, "db_a").has_value());
    ASSERT_TRUE(persistence_->persist_database(20, "db_b").has_value());
    ASSERT_TRUE(persistence_->remove_database(10).has_value());

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 2u); // default (id=1) + db_b (20)
    std::set<int32_t> ids;
    for (auto& [id, name] : dbs)
        ids.insert(id);
    EXPECT_TRUE(ids.count(1));
    EXPECT_TRUE(ids.count(20));
    EXPECT_FALSE(ids.count(10));
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
    // GDB-1224: see the AC4_PersistDatabaseBasic comment -- run_bootstrap()
    // always leaves the default database (id=1) persisted.
    run_bootstrap();

    auto result = persistence_->persist_database(0, "zero_db");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 2u);
    auto it = std::find_if(dbs.begin(), dbs.end(), [](const auto& p) { return p.first == 0; });
    ASSERT_NE(it, dbs.end());
}

TEST_F(QA_GDB654, Adversarial_NegativeDatabaseId) {
    run_bootstrap();

    auto result = persistence_->persist_database(-1, "neg_db");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 2u);
    auto it = std::find_if(dbs.begin(), dbs.end(), [](const auto& p) { return p.first == -1; });
    ASSERT_NE(it, dbs.end());
}

TEST_F(QA_GDB654, Adversarial_MaxInt32DatabaseId) {
    run_bootstrap();

    auto result = persistence_->persist_database(INT32_MAX, "max_db");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 2u);
    auto it =
        std::find_if(dbs.begin(), dbs.end(), [](const auto& p) { return p.first == INT32_MAX; });
    ASSERT_NE(it, dbs.end());
    EXPECT_EQ(it->second, "max_db");
}

TEST_F(QA_GDB654, Adversarial_MinInt32DatabaseId) {
    run_bootstrap();

    auto result = persistence_->persist_database(INT32_MIN, "min_db");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 2u);
    auto it =
        std::find_if(dbs.begin(), dbs.end(), [](const auto& p) { return p.first == INT32_MIN; });
    ASSERT_NE(it, dbs.end());
}

// ============================================================================
// Adversarial: Empty and special strings
// ============================================================================

TEST_F(QA_GDB654, Adversarial_EmptyDatabaseName) {
    // GDB-1224: switched the persisted id from 1 to 2 -- id=1 collides with
    // default_database_id, which run_bootstrap() already persists as
    // "demo"; asserting against a duplicate id=1 row was not this test's
    // intent (it wants to probe an empty *name*, not id collisions). See
    // the AC4_PersistDatabaseBasic comment for the general default-row
    // accounting this file's tests needed.
    run_bootstrap();

    auto result = persistence_->persist_database(2, "");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 2u);
    auto it = std::find_if(dbs.begin(), dbs.end(), [](const auto& p) { return p.first == 2; });
    ASSERT_NE(it, dbs.end());
    EXPECT_EQ(it->second, "");
}

TEST_F(QA_GDB654, Adversarial_LongDatabaseName) {
    run_bootstrap();

    std::string long_name(1024, 'x');
    auto result = persistence_->persist_database(2, long_name);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 2u);
    auto it = std::find_if(dbs.begin(), dbs.end(), [](const auto& p) { return p.first == 2; });
    ASSERT_NE(it, dbs.end());
    EXPECT_EQ(it->second, long_name);
}

TEST_F(QA_GDB654, Adversarial_SpecialCharactersInName) {
    run_bootstrap();

    std::string special = "db with spaces & 'quotes' \"double\" \ttab \nnewline";
    auto result = persistence_->persist_database(2, special);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    ASSERT_EQ(dbs.size(), 2u);
    auto it = std::find_if(dbs.begin(), dbs.end(), [](const auto& p) { return p.first == 2; });
    ASSERT_NE(it, dbs.end());
    EXPECT_EQ(it->second, special);
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
    // GDB-1224: "empty table" in the original comment predates
    // run_bootstrap() always persisting the default database (id=1); the
    // table has exactly that one row, not zero, at this point.
    run_bootstrap();

    // Remove a non-existent id — should succeed (no rows match predicate)
    // and must not disturb the default database row.
    auto result = persistence_->remove_database(999);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto dbs = read_all_databases();
    EXPECT_EQ(dbs.size(), 1u);
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
    for (auto& [id, name] : dbs)
        ids.insert(id);

    EXPECT_EQ(ids.size(), 2u);
    EXPECT_TRUE(ids.count(3));
    EXPECT_TRUE(ids.count(4));
}

// ============================================================================
// Adversarial: Multiple restart cycles
// ============================================================================

TEST_F(QA_GDB654, Adversarial_MultipleRestartCycles) {
    // GDB-1224: switched ids from 1,2,3 to 2,3,4 -- id=1 collides with
    // default_database_id, which run_bootstrap()'s first-run path already
    // persists as "demo". The expected final count is bumped from 3 to 4
    // (default + db2 + db3 + db4) to match.
    run_bootstrap();
    ASSERT_TRUE(persistence_->persist_database(2, "db2").has_value());

    // Restart cycle 1
    restart();
    run_bootstrap();
    ASSERT_TRUE(persistence_->persist_database(3, "db3").has_value());

    // Restart cycle 2
    restart();
    run_bootstrap();
    ASSERT_TRUE(persistence_->persist_database(4, "db4").has_value());

    // Restart cycle 3 — verify all four survive
    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    EXPECT_EQ(dbs.size(), 4u);
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
    // GDB-1224: id range shifted from [0, N) to [1000, 1000+N) to avoid
    // colliding with default_database_id (1), which run_bootstrap() already
    // persists as "demo" -- persist_database() does not enforce id
    // uniqueness (see Adversarial_DuplicateDatabaseId), so id=1 would have
    // silently produced a second, duplicate row rather than being skipped,
    // throwing off the exact-count assertion below by one.
    run_bootstrap();

    constexpr int N = 100;
    constexpr int base = 1000;
    for (int i = 0; i < N; ++i) {
        auto result = persistence_->persist_database(base + i, "db_" + std::to_string(i));
        ASSERT_TRUE(result.has_value()) << "Failed at i=" << i << ": " << result.error().message;
    }

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    EXPECT_EQ(dbs.size(), static_cast<size_t>(N + 1)); // +1 for the default database

    // Verify all IDs present (plus the default database).
    std::set<int32_t> ids;
    for (auto& [id, name] : dbs)
        ids.insert(id);
    EXPECT_TRUE(ids.count(default_database_id));
    for (int i = 0; i < N; ++i) {
        EXPECT_TRUE(ids.count(base + i)) << "Missing database ID " << (base + i);
    }
}

TEST_F(QA_GDB654, Adversarial_StressRemoveHalf) {
    // GDB-1224: id range shifted from [0, N) to [1000, 1000+N) for the same
    // default_database_id-collision reason as Adversarial_StressManyDatabases.
    run_bootstrap();

    constexpr int N = 50;
    constexpr int base = 1000;
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(
            persistence_->persist_database(base + i, "db_" + std::to_string(i)).has_value());
    }

    // Remove even-offset IDs.
    for (int i = 0; i < N; i += 2) {
        ASSERT_TRUE(persistence_->remove_database(base + i).has_value());
    }

    restart();
    run_bootstrap();

    auto dbs = read_all_databases();
    EXPECT_EQ(dbs.size(), static_cast<size_t>(N / 2 + 1)); // +1 for the default database

    std::set<int32_t> ids;
    for (auto& [id, name] : dbs)
        ids.insert(id);
    EXPECT_TRUE(ids.count(default_database_id));
    for (int i = 1; i < N; i += 2) {
        EXPECT_TRUE(ids.count(base + i)) << "Missing odd-offset ID " << (base + i);
    }
    for (int i = 0; i < N; i += 2) {
        EXPECT_FALSE(ids.count(base + i))
            << "Even-offset ID " << (base + i) << " should have been removed";
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
