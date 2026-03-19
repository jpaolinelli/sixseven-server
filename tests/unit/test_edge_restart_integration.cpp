#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/config.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace sixseven;
namespace fs = std::filesystem;

/// Full-stack integration test that exercises the real restart path:
/// fresh DiskManager + Catalog + StorageManager + CatalogPersistence +
/// GraphEngine + QueryEngine, bootstrapped exactly like main.cpp.
TEST(EdgeRestartIntegrationTest, EdgesAndTablesRecoveredAfterFullRestart) {
    auto data_dir = fs::temp_directory_path() / "sixseven_test_edge_restart_integration";
    fs::remove_all(data_dir);
    fs::create_directories(data_dir);
    auto config = Config::load_defaults();
    config.data_dir = data_dir.string();

    // ===== FIRST RUN =====
    {
        DiskManager dm;
        Catalog catalog;
        StorageManager storage(dm, data_dir);
        CatalogPersistence persistence(catalog, storage);
        GraphEngine graph_engine(catalog, dm, data_dir);
        QueryEngine engine(catalog, storage, &graph_engine);
        engine.set_catalog_persistence(&persistence);

        auto boot =
            SystemBootstrap::bootstrap(engine, catalog, storage, persistence, config, data_dir);
        ASSERT_TRUE(boot.has_value()) << boot.error().message;

        auto edge_load = graph_engine.load_edges();
        ASSERT_TRUE(edge_load.has_value()) << edge_load.error().message;

        engine.set_current_database(default_database_id);

        auto r1 = engine.execute("CREATE TABLE users (id INT PRIMARY KEY, name TEXT)");
        ASSERT_TRUE(r1.has_value()) << r1.error().message;

        auto r2 = engine.execute("INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob')");
        ASSERT_TRUE(r2.has_value()) << r2.error().message;

        auto r3 = engine.execute("CREATE EDGE TYPE follows FROM users TO users");
        ASSERT_TRUE(r3.has_value()) << r3.error().message;

        auto r4 = engine.execute("LINK users(1) TO users(2) VIA follows");
        ASSERT_TRUE(r4.has_value()) << r4.error().message;

        auto r5 = engine.execute("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
        ASSERT_TRUE(r5.has_value()) << r5.error().message;
        EXPECT_EQ(r5->rows.size(), 1);

        // Verify edge storage file was created.
        bool found_edge_file = false;
        for (auto& p : fs::recursive_directory_iterator(data_dir)) {
            if (p.is_regular_file() && p.path().filename().string().starts_with("edge_")) {
                found_edge_file = true;
            }
        }
        EXPECT_TRUE(found_edge_file) << "No edge storage file found after LINK";
    }
    // All destructors run — files flushed and closed.

    // ===== SECOND RUN (fresh everything) =====
    {
        DiskManager dm;
        Catalog catalog;
        StorageManager storage(dm, data_dir);
        CatalogPersistence persistence(catalog, storage);
        GraphEngine graph_engine(catalog, dm, data_dir);
        QueryEngine engine(catalog, storage, &graph_engine);
        engine.set_catalog_persistence(&persistence);

        auto boot =
            SystemBootstrap::bootstrap(engine, catalog, storage, persistence, config, data_dir);
        ASSERT_TRUE(boot.has_value()) << boot.error().message;

        // Edge type metadata should be in the catalog after load_catalog.
        auto edge_types = catalog.list_edge_types(default_database_id);
        EXPECT_EQ(edge_types.size(), 1) << "Expected 1 edge type in catalog after restart";

        auto edge_load = graph_engine.load_edges();
        ASSERT_TRUE(edge_load.has_value()) << edge_load.error().message;

        // Edge table should exist in the graph engine.
        auto listed = graph_engine.list_edge_types();
        EXPECT_EQ(listed.size(), 1) << "Expected 1 edge table after load_edges";

        engine.set_current_database(default_database_id);

        // Tables should be recovered.
        auto r1 = engine.execute("SELECT * FROM users");
        ASSERT_TRUE(r1.has_value()) << "SELECT users failed: " << r1.error().message;
        EXPECT_EQ(r1->rows.size(), 2) << "Expected 2 rows in users table";

        // Edge type should exist and edges should be recovered.
        auto r2 = engine.execute("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT");
        ASSERT_TRUE(r2.has_value()) << "TRAVERSE failed: " << r2.error().message;
        EXPECT_EQ(r2->rows.size(), 1) << "Expected 1 edge after restart";
    }

    fs::remove_all(data_dir);
}

TEST(EdgeRestartIntegrationTest, DroppedEdgeTypeNotRecoveredAfterRestart) {
    auto data_dir = fs::temp_directory_path() / "sixseven_test_edge_drop_restart";
    fs::remove_all(data_dir);
    fs::create_directories(data_dir);
    auto config = Config::load_defaults();
    config.data_dir = data_dir.string();

    // ===== FIRST RUN: create then drop =====
    {
        DiskManager dm;
        Catalog catalog;
        StorageManager storage(dm, data_dir);
        CatalogPersistence persistence(catalog, storage);
        GraphEngine graph_engine(catalog, dm, data_dir);
        QueryEngine engine(catalog, storage, &graph_engine);
        engine.set_catalog_persistence(&persistence);

        auto boot =
            SystemBootstrap::bootstrap(engine, catalog, storage, persistence, config, data_dir);
        ASSERT_TRUE(boot.has_value()) << boot.error().message;
        engine.set_current_database(default_database_id);

        ASSERT_TRUE(engine.execute("CREATE TABLE nodes (id INT PRIMARY KEY)").has_value());
        ASSERT_TRUE(engine.execute("CREATE EDGE TYPE connects FROM nodes TO nodes").has_value());
        ASSERT_TRUE(engine.execute("DROP EDGE TYPE connects").has_value());
    }

    // ===== SECOND RUN: verify it's gone =====
    {
        DiskManager dm;
        Catalog catalog;
        StorageManager storage(dm, data_dir);
        CatalogPersistence persistence(catalog, storage);
        GraphEngine graph_engine(catalog, dm, data_dir);
        QueryEngine engine(catalog, storage, &graph_engine);
        engine.set_catalog_persistence(&persistence);

        auto boot =
            SystemBootstrap::bootstrap(engine, catalog, storage, persistence, config, data_dir);
        ASSERT_TRUE(boot.has_value()) << boot.error().message;

        auto edge_load = graph_engine.load_edges();
        ASSERT_TRUE(edge_load.has_value()) << edge_load.error().message;

        auto edge_types = catalog.list_edge_types(default_database_id);
        EXPECT_EQ(edge_types.size(), 0) << "Dropped edge type should not be in catalog";

        auto listed = graph_engine.list_edge_types();
        EXPECT_EQ(listed.size(), 0) << "Dropped edge type should not be in graph engine";
    }

    fs::remove_all(data_dir);
}
