#pragma once

// Shared QueryEngine test fixture used by BM25 and AutoIncrement test suites.
//
// exec_ok uses the safe guarded form: it checks has_value() before dereferencing
// and never calls ADD_FAILURE()-then-deref (which would be UB on an errored
// Result). exec_error asserts !has_value() and checks the StatusCode.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/config.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "test_catalog_helpers.h"

namespace sixseven {

/// Base fixture providing a full QueryEngine stack (DiskManager, Catalog,
/// StorageManager, CatalogPersistence, QueryEngine, IndexManager) plus helpers
/// for bootstrap, index rebuild, and simulated restarts.
///
/// Derived fixtures must supply their own data_dir_ value in SetUp (before
/// calling make_stack / run_bootstrap) to keep temp directories unique per
/// suite and avoid cross-suite collisions.
class QueryEngineFixture : public ::testing::Test {
protected:
    void make_stack() {
        dm_ = std::make_unique<DiskManager>();
        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(*dm_, data_dir_);
        persistence_ = std::make_unique<CatalogPersistence>(*catalog_, *storage_);
        engine_ = std::make_unique<QueryEngine>(*catalog_, *storage_);
        engine_->set_catalog_persistence(persistence_.get());
        config_ = Config::load_defaults();
    }

    void reset_stack() {
        index_manager_.reset();
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

    void rebuild_indexes() {
        index_manager_ = std::make_unique<IndexManager>(*catalog_, *storage_);
        index_manager_->set_catalog_persistence(persistence_.get());
        auto r = index_manager_->rebuild_all_indexes();
        ASSERT_TRUE(r.has_value()) << r.error().message;
        engine_->set_index_manager(index_manager_.get());
    }

    /// Simulate a server restart: flush indexes, tear down all in-memory state,
    /// recreate the stack, reload the persisted catalog, then rebuild indexes.
    void restart_and_reload() {
        ASSERT_TRUE(index_manager_->flush_all_indexes().has_value());
        reset_stack();
        make_stack();
        run_bootstrap();
        rebuild_indexes();
    }

    /// Execute SQL and assert success. Returns an empty QueryResult on failure
    /// so the test can continue (non-fatal). Never dereferences an errored Result.
    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    /// Execute SQL and assert failure with the given StatusCode.
    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "expected error but got success for: " << sql;
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected)
                << "expected " << static_cast<int>(expected) << " but got "
                << static_cast<int>(result.error().code) << ": " << result.error().message;
        }
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<IndexManager> index_manager_;
    Config config_;
};

} // namespace sixseven
