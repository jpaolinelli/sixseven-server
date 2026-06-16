#pragma once

/// @file test_qa_gdb_656_657_660_fixture.h
/// @brief Shared bootstrap QA fixture base for GDB-656, GDB-657, and GDB-660
///        adversarial tests.
///
/// Included by test_qa_gdb_656.cpp, test_qa_gdb_657.cpp, and
/// test_qa_gdb_660.cpp — all compiled into the same sixseven_qa_tests binary.
/// All member functions are defined inline/in-class so that including this
/// header in three translation units does not cause duplicate-symbol link
/// errors (ODR-safe).  Use #pragma once.
///
/// What is extracted here (byte-identical across all three files):
///   - All shared #includes
///   - Member variables
///   - create_components() / destroy_components()
///   - run_bootstrap()
///   - restart()
///   - exec_ok()
///   - scan_sys_databases()   <-- byte-identical helper called out in GDB-823
///
/// What is NOT extracted (per-file differences):
///   - The data_dir_ subdirectory suffix (set by each derived fixture's SetUp)
///   - GDB-657's exec_error() and sys_databases_contains() helpers
///   - GDB-660's switch_to_db(), switch_to_default(), get_table_id(),
///     scan_sys_tables(), scan_sys_columns(), scan_sys_indexes(), and the
///     associated bool-query helpers

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
#include <memory>
#include <string>
#include <vector>

namespace sixseven {

/// Base fixture shared by QA_GDB656, QA_GDB657, and QA_GDB660.
///
/// Each derived fixture's SetUp() must:
///   1. Set data_dir_ to the desired temp subdirectory.
///   2. Call std::filesystem::remove_all(data_dir_) and
///      std::filesystem::create_directories(data_dir_).
///   3. Call create_components().
///
/// Each derived fixture's TearDown() must:
///   1. Call destroy_components().
///   2. Call std::filesystem::remove_all(data_dir_).
class QA_GDB_Bootstrap_Base : public ::testing::Test {
protected:
    // ------------------------------------------------------------------
    // Component lifecycle
    // ------------------------------------------------------------------

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
        return result ? std::move(*result) : QueryResult{};
    }

    /// Scan the sys_databases heap and return all (id, name) pairs.
    /// Byte-identical helper shared by GDB-656, GDB-657, and GDB-660.
    std::vector<std::pair<database_id_t, std::string>> scan_sys_databases() {
        std::vector<std::pair<database_id_t, std::string>> entries;
        auto ts = storage_->get_table_storage(sys_databases_table_id);
        if (!ts)
            return entries;
        auto schema = StorageManager::build_storage_schema(sys_databases_schema());
        auto it = (*ts)->heap->begin();
        if (!it)
            return entries;
        while (auto row = it->next()) {
            auto values = TupleSerializer::deserialize(row->second, schema);
            if (!values)
                continue;
            entries.emplace_back(static_cast<database_id_t>((*values)[0].as_int32()),
                                 (*values)[1].as_string());
        }
        return entries;
    }

    // ------------------------------------------------------------------
    // Shared members
    // ------------------------------------------------------------------

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    Config config_;
};

} // namespace sixseven
