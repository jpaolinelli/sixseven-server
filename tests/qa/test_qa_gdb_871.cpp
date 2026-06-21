/// @file test_qa_gdb_871.cpp
/// @brief QA regression tests for GDB-871.
///
/// Bug: an edge type created with prevent_duplicates=true silently loses the
/// unique-edge constraint after a server restart.  The root cause was that
/// load_edges() hardcoded config.prevent_duplicates = false instead of
/// restoring it from the catalog.
///
/// Fix: the "__uniq__" sentinel token is encoded in the EdgeTypeDef::properties
/// string at create time and decoded back at load time so the flag survives
/// the full restart path (catalog persistence -> load_catalog -> load_edges ->
/// load_edge_indexes).
///
/// Mutation-grade test: the key assertion (duplicate insert MUST fail after
/// restart) FAILS on the pre-fix main branch and PASSES with the fix.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/config.h"
#include "sixseven/common/status.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/catalog_persistence.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/system_bootstrap.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "test_catalog_helpers.h"

using namespace sixseven;
namespace fs = std::filesystem;

static Value pk(int64_t v) {
    return Value(v);
}

// =============================================================================
// QA_GDB871: prevent_duplicates persists across restart
// =============================================================================

/// Reproduces the original bug: before the fix, the duplicate insert after
/// restart SUCCEEDED (no constraint enforced).  After the fix it FAILS with
/// CONSTRAINT_VIOLATION.
///
/// The restart is fully real: all in-memory objects are destroyed, a fresh
/// DiskManager + Catalog + StorageManager + CatalogPersistence + GraphEngine +
/// QueryEngine are created, and SystemBootstrap::bootstrap + load_edges() are
/// called exactly as in production (same path as test_edge_restart_integration).
TEST(QA_GDB871, UniqueConstraintSurvivesRestart) {
    auto data_dir = fs::temp_directory_path() / "sixseven_qa_gdb871_uniq_restart";
    fs::remove_all(data_dir);
    fs::create_directories(data_dir);
    auto cfg = Config::load_defaults();
    cfg.data_dir = data_dir.string();

    // ===== FIRST RUN =====
    {
        DiskManager dm;
        Catalog catalog;
        init_test_catalog(catalog);
        StorageManager storage(dm, data_dir);
        CatalogPersistence persistence(catalog, storage);
        GraphEngine graph_engine(catalog, dm, data_dir);
        QueryEngine engine(catalog, storage, &graph_engine);
        engine.set_catalog_persistence(&persistence);

        auto boot =
            SystemBootstrap::bootstrap(engine, catalog, storage, persistence, cfg, data_dir);
        ASSERT_TRUE(boot.has_value()) << boot.error().message;

        auto le = graph_engine.load_edges();
        ASSERT_TRUE(le.has_value()) << le.error().message;

        engine.set_current_database(default_database_id);

        // Create the tables that the edge type links.
        ASSERT_TRUE(engine.execute("CREATE TABLE nodes (id BIGINT PRIMARY KEY)").has_value());

        // Create the unique edge type via the C++ GraphEngine API, since the SQL
        // CREATE EDGE TYPE syntax does not yet expose prevent_duplicates.
        auto from_schema = catalog.get_table(default_database_id, "nodes");
        ASSERT_TRUE(from_schema.has_value());
        table_id_t node_tid = from_schema->table_id;

        auto eid = graph_engine.create_edge_type(default_database_id,
                                                 "connects",
                                                 node_tid,
                                                 node_tid,
                                                 TypeId::INT64,
                                                 TypeId::INT64,
                                                 {},
                                                 /*prevent_duplicates=*/true);
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Verify the EdgeTypeDef in the catalog has the flag set and the sentinel.
        auto et = catalog.get_edge_type(default_database_id, "connects");
        ASSERT_TRUE(et.has_value());
        EXPECT_TRUE(et->prevent_duplicates);
        EXPECT_NE(et->properties.find("__uniq__"), std::string::npos)
            << "properties must contain __uniq__ sentinel";

        // Persist the edge type metadata to sys_edge_types.
        auto persist = persistence.persist_edge_type(*et);
        ASSERT_TRUE(persist.has_value()) << persist.error().message;

        // Insert first edge (1->2): must succeed.
        auto r1 = graph_engine.link(default_database_id, "connects", pk(1), pk(2), {});
        ASSERT_TRUE(r1.has_value()) << r1.error().message;

        // Baseline: same-session duplicate must already fail with CONSTRAINT_VIOLATION.
        auto r2 = graph_engine.link(default_database_id, "connects", pk(1), pk(2), {});
        ASSERT_FALSE(r2.has_value()) << "same-session duplicate must fail";
        EXPECT_EQ(r2.error().code, StatusCode::CONSTRAINT_VIOLATION);
    }
    // All destructors run — edge heap pages are flushed, files are closed.

    // ===== SECOND RUN (simulated restart) =====
    {
        DiskManager dm;
        Catalog catalog;
        init_test_catalog(catalog);
        StorageManager storage(dm, data_dir);
        CatalogPersistence persistence(catalog, storage);
        GraphEngine graph_engine(catalog, dm, data_dir);
        QueryEngine engine(catalog, storage, &graph_engine);
        engine.set_catalog_persistence(&persistence);

        auto boot =
            SystemBootstrap::bootstrap(engine, catalog, storage, persistence, cfg, data_dir);
        ASSERT_TRUE(boot.has_value()) << boot.error().message;

        // Verify the catalog correctly restored prevent_duplicates from the
        // __uniq__ sentinel in the properties field.
        auto et = catalog.get_edge_type(default_database_id, "connects");
        ASSERT_TRUE(et.has_value()) << "edge type 'connects' must survive restart";
        EXPECT_TRUE(et->prevent_duplicates)
            << "prevent_duplicates must be restored from catalog after restart (GDB-871)";

        // load_edges: rebuilds in-memory EdgeTable with correct config + loads
        // the persisted unique index from disk.
        auto le = graph_engine.load_edges();
        ASSERT_TRUE(le.has_value()) << le.error().message;

        // MUTATION-GRADE ASSERTION:
        // On the pre-fix main branch this INSERT SUCCEEDS (bug: constraint lost).
        // With the fix it MUST fail with CONSTRAINT_VIOLATION.
        auto r = graph_engine.link(default_database_id, "connects", pk(1), pk(2), {});
        ASSERT_FALSE(r.has_value())
            << "GDB-871 regression: duplicate edge insert must fail with CONSTRAINT_VIOLATION "
               "after server restart; before the fix this insert incorrectly succeeded";
        EXPECT_EQ(r.error().code, StatusCode::CONSTRAINT_VIOLATION);

        // A different (non-duplicate) edge must still succeed.
        auto r_new = graph_engine.link(default_database_id, "connects", pk(1), pk(3), {});
        ASSERT_TRUE(r_new.has_value())
            << "non-duplicate edge must be accepted after restart: " << r_new.error().message;
    }

    fs::remove_all(data_dir);
}

/// Verify that an edge type created WITHOUT prevent_duplicates still allows
/// duplicate edges after restart (the false flag also round-trips correctly;
/// the __uniq__ sentinel is absent from the properties string).
TEST(QA_GDB871, NonUniqueEdgeTypeAllowsDuplicatesAfterRestart) {
    auto data_dir = fs::temp_directory_path() / "sixseven_qa_gdb871_nonuniq_restart";
    fs::remove_all(data_dir);
    fs::create_directories(data_dir);
    auto cfg = Config::load_defaults();
    cfg.data_dir = data_dir.string();

    // ===== FIRST RUN =====
    {
        DiskManager dm;
        Catalog catalog;
        init_test_catalog(catalog);
        StorageManager storage(dm, data_dir);
        CatalogPersistence persistence(catalog, storage);
        GraphEngine graph_engine(catalog, dm, data_dir);
        QueryEngine engine(catalog, storage, &graph_engine);
        engine.set_catalog_persistence(&persistence);

        auto boot =
            SystemBootstrap::bootstrap(engine, catalog, storage, persistence, cfg, data_dir);
        ASSERT_TRUE(boot.has_value()) << boot.error().message;

        auto le = graph_engine.load_edges();
        ASSERT_TRUE(le.has_value()) << le.error().message;

        engine.set_current_database(default_database_id);
        ASSERT_TRUE(engine.execute("CREATE TABLE nodes2 (id BIGINT PRIMARY KEY)").has_value());

        auto from_schema = catalog.get_table(default_database_id, "nodes2");
        ASSERT_TRUE(from_schema.has_value());
        table_id_t node_tid = from_schema->table_id;

        // Non-unique edge type (default).
        auto eid = graph_engine.create_edge_type(default_database_id,
                                                 "links",
                                                 node_tid,
                                                 node_tid,
                                                 TypeId::INT64,
                                                 TypeId::INT64,
                                                 {},
                                                 /*prevent_duplicates=*/false);
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        auto et = catalog.get_edge_type(default_database_id, "links");
        ASSERT_TRUE(et.has_value());
        EXPECT_FALSE(et->prevent_duplicates);
        EXPECT_EQ(et->properties.find("__uniq__"), std::string::npos)
            << "non-unique edge type must not have __uniq__ sentinel";

        auto persist = persistence.persist_edge_type(*et);
        ASSERT_TRUE(persist.has_value()) << persist.error().message;

        ASSERT_TRUE(
            graph_engine.link(default_database_id, "links", pk(10), pk(20), {}).has_value());
    }

    // ===== SECOND RUN =====
    {
        DiskManager dm;
        Catalog catalog;
        init_test_catalog(catalog);
        StorageManager storage(dm, data_dir);
        CatalogPersistence persistence(catalog, storage);
        GraphEngine graph_engine(catalog, dm, data_dir);
        QueryEngine engine(catalog, storage, &graph_engine);
        engine.set_catalog_persistence(&persistence);

        auto boot =
            SystemBootstrap::bootstrap(engine, catalog, storage, persistence, cfg, data_dir);
        ASSERT_TRUE(boot.has_value()) << boot.error().message;

        auto le = graph_engine.load_edges();
        ASSERT_TRUE(le.has_value()) << le.error().message;

        auto et = catalog.get_edge_type(default_database_id, "links");
        ASSERT_TRUE(et.has_value()) << "non-unique edge type must survive restart";
        EXPECT_FALSE(et->prevent_duplicates)
            << "prevent_duplicates must remain false after restart";

        // Duplicate insert must SUCCEED for a non-unique edge type.
        auto r = graph_engine.link(default_database_id, "links", pk(10), pk(20), {});
        ASSERT_TRUE(r.has_value())
            << "duplicate insert on non-unique edge type must succeed: " << r.error().message;
    }

    fs::remove_all(data_dir);
}
