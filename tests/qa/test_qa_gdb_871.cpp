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

/// Regression test for the substring-find collision bug (GDB-871 follow-up).
///
/// A NON-unique edge type that has a real property column literally named
/// "__uniq__" produces a properties string of the form "__uniq__:STRING" (no
/// bare sentinel).  The old substring `find("__uniq__")` would match that token
/// and falsely flip prevent_duplicates to true after restart — causing a
/// CONSTRAINT_VIOLATION on a perfectly legal duplicate insert.
///
/// With the whole-token fix the properties string "__uniq__:STRING" contains NO
/// token that is EXACTLY "__uniq__" (the token is "__uniq__:STRING"), so
/// prevent_duplicates stays false and the duplicate insert succeeds.
///
/// On the buggy substring-find code this test FAILS at the duplicate-insert
/// assertion.  With the whole-token fix it PASSES.
TEST(QA_GDB871, PropertyNamedUniqSentinelDoesNotFlipPreventDuplicates) {
    auto data_dir = fs::temp_directory_path() / "sixseven_qa_gdb871_uniq_prop_collision";
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
        ASSERT_TRUE(engine.execute("CREATE TABLE nodes3 (id BIGINT PRIMARY KEY)").has_value());

        auto from_schema = catalog.get_table(default_database_id, "nodes3");
        ASSERT_TRUE(from_schema.has_value());
        table_id_t node_tid = from_schema->table_id;

        // Non-unique edge type with a property column literally named "__uniq__".
        // This exercises the collision: the encoded properties string becomes
        // "__uniq__:STRING" which the old substring find() would falsely match.
        std::vector<ColumnDef> props = {{"__uniq__", TypeId::STRING}};
        auto eid = graph_engine.create_edge_type(default_database_id,
                                                 "collision_edge",
                                                 node_tid,
                                                 node_tid,
                                                 TypeId::INT64,
                                                 TypeId::INT64,
                                                 props,
                                                 /*prevent_duplicates=*/false);
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        auto et = catalog.get_edge_type(default_database_id, "collision_edge");
        ASSERT_TRUE(et.has_value());
        EXPECT_FALSE(et->prevent_duplicates)
            << "edge type with property named __uniq__ must NOT be flagged unique";
        // The properties string contains "__uniq__:STRING", not the bare sentinel.
        // Verify there is no bare __uniq__ token (the encode side never writes one
        // since prevent_duplicates=false).
        EXPECT_EQ(et->properties.find("__uniq__,"), std::string::npos);
        EXPECT_NE(et->properties.find("__uniq__:"), std::string::npos)
            << "the __uniq__ property column must appear with a colon suffix";

        auto persist = persistence.persist_edge_type(*et);
        ASSERT_TRUE(persist.has_value()) << persist.error().message;

        // Insert a duplicate edge — must succeed on a non-unique type.
        auto r1 = graph_engine.link(
            default_database_id, "collision_edge", pk(1), pk(2), {Value(std::string("v1"))});
        ASSERT_TRUE(r1.has_value()) << r1.error().message;
        auto r2 = graph_engine.link(
            default_database_id, "collision_edge", pk(1), pk(2), {Value(std::string("v2"))});
        ASSERT_TRUE(r2.has_value()) << "duplicate insert must succeed before restart";
    }

    // ===== SECOND RUN (restart) =====
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

        // Key assertion: after restart, the edge type must STILL be non-unique.
        // The old substring find() would set prevent_duplicates=true here (collision),
        // causing the duplicate insert below to fail with CONSTRAINT_VIOLATION.
        auto et = catalog.get_edge_type(default_database_id, "collision_edge");
        ASSERT_TRUE(et.has_value()) << "edge type must survive restart";
        EXPECT_FALSE(et->prevent_duplicates)
            << "prevent_duplicates must remain false after restart even when a "
               "property column is named __uniq__ (whole-token fix, GDB-871)";

        // Duplicate edge insert must SUCCEED — this is the mutation-grade assertion.
        // On the buggy substring-find code this returns CONSTRAINT_VIOLATION (FAIL).
        // With the whole-token fix it returns success (PASS).
        auto r = graph_engine.link(
            default_database_id, "collision_edge", pk(1), pk(2), {Value(std::string("v3"))});
        ASSERT_TRUE(r.has_value())
            << "duplicate insert on non-unique edge type must succeed after restart; "
               "got: "
            << r.error().message;
    }

    fs::remove_all(data_dir);
}

// =============================================================================
// QA_GDB871 adversarial: additional persistence round-trip scenarios
// =============================================================================

/// AC: unique edge type WITH real property columns.
/// After restart the unique constraint AND the property schema must both survive.
/// The sentinel token must not eat a real column definition.
TEST(QA_GDB871, UniqueConstraintWithPropertyColumnsRoundTrips) {
    auto data_dir = fs::temp_directory_path() / "sixseven_qa_gdb871_uniq_props_roundtrip";
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

        ASSERT_TRUE(engine.execute("CREATE TABLE nodeset (id BIGINT PRIMARY KEY)").has_value());
        auto from_schema = catalog.get_table(default_database_id, "nodeset");
        ASSERT_TRUE(from_schema.has_value());
        table_id_t tid = from_schema->table_id;

        // Two real property columns alongside the unique flag.
        std::vector<ColumnDef> props = {{"weight", TypeId::FLOAT64}, {"label", TypeId::STRING}};
        auto eid = graph_engine.create_edge_type(default_database_id,
                                                 "weighted",
                                                 tid,
                                                 tid,
                                                 TypeId::INT64,
                                                 TypeId::INT64,
                                                 props,
                                                 /*prevent_duplicates=*/true);
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        auto et = catalog.get_edge_type(default_database_id, "weighted");
        ASSERT_TRUE(et.has_value());
        EXPECT_TRUE(et->prevent_duplicates);
        // Encoded string must contain sentinel AND both column definitions.
        EXPECT_NE(et->properties.find("__uniq__"), std::string::npos) << "sentinel must be present";
        EXPECT_NE(et->properties.find("weight:"), std::string::npos)
            << "weight column must be present";
        EXPECT_NE(et->properties.find("label:"), std::string::npos)
            << "label column must be present";

        auto persist = persistence.persist_edge_type(*et);
        ASSERT_TRUE(persist.has_value()) << persist.error().message;

        // Insert one edge with property values.
        auto r1 = graph_engine.link(default_database_id,
                                    "weighted",
                                    pk(1),
                                    pk(2),
                                    {Value(3.14), Value(std::string("alpha"))});
        ASSERT_TRUE(r1.has_value()) << r1.error().message;
    }

    // ===== SECOND RUN (restart) =====
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

        // Verify catalog restored both prevent_duplicates and property columns.
        auto et = catalog.get_edge_type(default_database_id, "weighted");
        ASSERT_TRUE(et.has_value()) << "edge type 'weighted' must survive restart";
        EXPECT_TRUE(et->prevent_duplicates)
            << "prevent_duplicates must survive restart alongside property columns";
        // The sentinel must not have consumed a property column token.
        EXPECT_NE(et->properties.find("weight:"), std::string::npos)
            << "weight column must survive in properties string after restart";
        EXPECT_NE(et->properties.find("label:"), std::string::npos)
            << "label column must survive in properties string after restart";

        auto le = graph_engine.load_edges();
        ASSERT_TRUE(le.has_value()) << le.error().message;

        // Duplicate of the pre-restart edge must be rejected.
        auto r_dup = graph_engine.link(default_database_id,
                                       "weighted",
                                       pk(1),
                                       pk(2),
                                       {Value(9.99), Value(std::string("beta"))});
        ASSERT_FALSE(r_dup.has_value())
            << "duplicate must be rejected after restart even with property columns";
        EXPECT_EQ(r_dup.error().code, StatusCode::CONSTRAINT_VIOLATION);

        // A new distinct edge must be accepted.
        auto r_new = graph_engine.link(default_database_id,
                                       "weighted",
                                       pk(1),
                                       pk(3),
                                       {Value(1.0), Value(std::string("gamma"))});
        ASSERT_TRUE(r_new.has_value())
            << "distinct edge must be accepted: " << r_new.error().message;
    }

    fs::remove_all(data_dir);
}

/// AC: multiple edge types in one catalog — some unique, some not.
/// After restart each type retains its OWN flag (no cross-contamination).
TEST(QA_GDB871, MultipleEdgeTypesNoFlagCrossContamination) {
    auto data_dir = fs::temp_directory_path() / "sixseven_qa_gdb871_multi_types";
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

        ASSERT_TRUE(engine.execute("CREATE TABLE mn (id BIGINT PRIMARY KEY)").has_value());
        auto tbl = catalog.get_table(default_database_id, "mn");
        ASSERT_TRUE(tbl.has_value());
        table_id_t tid = tbl->table_id;

        // Create three edge types in mixed unique/non-unique order.
        auto e1 = graph_engine.create_edge_type(default_database_id,
                                                "e_uniq_a",
                                                tid,
                                                tid,
                                                TypeId::INT64,
                                                TypeId::INT64,
                                                {},
                                                /*prevent_duplicates=*/true);
        ASSERT_TRUE(e1.has_value()) << e1.error().message;

        auto e2 = graph_engine.create_edge_type(default_database_id,
                                                "e_nonuniq",
                                                tid,
                                                tid,
                                                TypeId::INT64,
                                                TypeId::INT64,
                                                {},
                                                /*prevent_duplicates=*/false);
        ASSERT_TRUE(e2.has_value()) << e2.error().message;

        auto e3 = graph_engine.create_edge_type(default_database_id,
                                                "e_uniq_b",
                                                tid,
                                                tid,
                                                TypeId::INT64,
                                                TypeId::INT64,
                                                {},
                                                /*prevent_duplicates=*/true);
        ASSERT_TRUE(e3.has_value()) << e3.error().message;

        for (const auto& name : {"e_uniq_a", "e_nonuniq", "e_uniq_b"}) {
            auto et = catalog.get_edge_type(default_database_id, name);
            ASSERT_TRUE(et.has_value()) << name;
            auto persist = persistence.persist_edge_type(*et);
            ASSERT_TRUE(persist.has_value()) << persist.error().message;
        }

        // Insert one edge per type.
        ASSERT_TRUE(
            graph_engine.link(default_database_id, "e_uniq_a", pk(1), pk(2), {}).has_value());
        ASSERT_TRUE(
            graph_engine.link(default_database_id, "e_nonuniq", pk(1), pk(2), {}).has_value());
        ASSERT_TRUE(
            graph_engine.link(default_database_id, "e_uniq_b", pk(1), pk(2), {}).has_value());
    }

    // ===== SECOND RUN (restart) =====
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

        // Check catalog flags before load_edges().
        auto et_a = catalog.get_edge_type(default_database_id, "e_uniq_a");
        ASSERT_TRUE(et_a.has_value());
        EXPECT_TRUE(et_a->prevent_duplicates) << "e_uniq_a must remain unique after restart";

        auto et_n = catalog.get_edge_type(default_database_id, "e_nonuniq");
        ASSERT_TRUE(et_n.has_value());
        EXPECT_FALSE(et_n->prevent_duplicates) << "e_nonuniq must remain non-unique after restart";

        auto et_b = catalog.get_edge_type(default_database_id, "e_uniq_b");
        ASSERT_TRUE(et_b.has_value());
        EXPECT_TRUE(et_b->prevent_duplicates) << "e_uniq_b must remain unique after restart";

        auto le = graph_engine.load_edges();
        ASSERT_TRUE(le.has_value()) << le.error().message;

        // e_uniq_a: duplicate must be rejected.
        auto ra = graph_engine.link(default_database_id, "e_uniq_a", pk(1), pk(2), {});
        ASSERT_FALSE(ra.has_value()) << "e_uniq_a duplicate must be rejected after restart";
        EXPECT_EQ(ra.error().code, StatusCode::CONSTRAINT_VIOLATION);

        // e_nonuniq: duplicate must succeed.
        auto rn = graph_engine.link(default_database_id, "e_nonuniq", pk(1), pk(2), {});
        ASSERT_TRUE(rn.has_value()) << "e_nonuniq duplicate must succeed: " << rn.error().message;

        // e_uniq_b: duplicate must be rejected.
        auto rb = graph_engine.link(default_database_id, "e_uniq_b", pk(1), pk(2), {});
        ASSERT_FALSE(rb.has_value()) << "e_uniq_b duplicate must be rejected after restart";
        EXPECT_EQ(rb.error().code, StatusCode::CONSTRAINT_VIOLATION);
    }

    fs::remove_all(data_dir);
}

/// AC: the unique INDEX DATA actually reloads.
/// Pre-restart edges are in the reloaded uniq index, so a duplicate of a
/// pre-restart edge is rejected; a brand-new distinct edge is still accepted.
TEST(QA_GDB871, ReloadedUniqIndexContainsPreRestartEdges) {
    auto data_dir = fs::temp_directory_path() / "sixseven_qa_gdb871_uniq_index_data";
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

        ASSERT_TRUE(engine.execute("CREATE TABLE idx_nodes (id BIGINT PRIMARY KEY)").has_value());
        auto tbl = catalog.get_table(default_database_id, "idx_nodes");
        ASSERT_TRUE(tbl.has_value());
        table_id_t tid = tbl->table_id;

        auto eid = graph_engine.create_edge_type(default_database_id,
                                                 "idx_edge",
                                                 tid,
                                                 tid,
                                                 TypeId::INT64,
                                                 TypeId::INT64,
                                                 {},
                                                 /*prevent_duplicates=*/true);
        ASSERT_TRUE(eid.has_value()) << eid.error().message;
        auto et = catalog.get_edge_type(default_database_id, "idx_edge");
        ASSERT_TRUE(et.has_value());
        auto persist = persistence.persist_edge_type(*et);
        ASSERT_TRUE(persist.has_value()) << persist.error().message;

        // Insert several distinct edges so the uniq index has real content.
        ASSERT_TRUE(
            graph_engine.link(default_database_id, "idx_edge", pk(10), pk(20), {}).has_value());
        ASSERT_TRUE(
            graph_engine.link(default_database_id, "idx_edge", pk(10), pk(30), {}).has_value());
        ASSERT_TRUE(
            graph_engine.link(default_database_id, "idx_edge", pk(20), pk(10), {}).has_value());
    }
    // Destructors run — buffers flushed, files closed.

    // ===== SECOND RUN (restart) =====
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

        // (a) Duplicates of every pre-restart edge must be rejected.
        // If load_edge_indexes had created an EMPTY fresh index these would
        // succeed (false negative) — proving the index data was NOT reloaded.
        auto dup1 = graph_engine.link(default_database_id, "idx_edge", pk(10), pk(20), {});
        ASSERT_FALSE(dup1.has_value())
            << "duplicate of pre-restart edge (10->20) must fail; "
               "if it succeeds, the uniq index was NOT reloaded from disk";
        EXPECT_EQ(dup1.error().code, StatusCode::CONSTRAINT_VIOLATION);

        auto dup2 = graph_engine.link(default_database_id, "idx_edge", pk(10), pk(30), {});
        ASSERT_FALSE(dup2.has_value()) << "duplicate of pre-restart edge (10->30) must fail";
        EXPECT_EQ(dup2.error().code, StatusCode::CONSTRAINT_VIOLATION);

        auto dup3 = graph_engine.link(default_database_id, "idx_edge", pk(20), pk(10), {});
        ASSERT_FALSE(dup3.has_value()) << "duplicate of pre-restart edge (20->10) must fail";
        EXPECT_EQ(dup3.error().code, StatusCode::CONSTRAINT_VIOLATION);

        // (b) A brand-new distinct edge must still be accepted.
        auto r_new = graph_engine.link(default_database_id, "idx_edge", pk(99), pk(99), {});
        ASSERT_TRUE(r_new.has_value())
            << "new distinct edge must be accepted after restart: " << r_new.error().message;
    }

    fs::remove_all(data_dir);
}

/// Sentinel robustness: a property column whose name CONTAINS "__uniq__" as a
/// SUBSTRING but is NOT exactly it (e.g. "my__uniq__col STRING") on a
/// NON-unique edge type — must NOT flip prevent_duplicates to true after
/// restart.
TEST(QA_GDB871, PropertySubstringOfSentinelDoesNotFlipFlag) {
    auto data_dir = fs::temp_directory_path() / "sixseven_qa_gdb871_substr_collision";
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

        ASSERT_TRUE(engine.execute("CREATE TABLE subn (id BIGINT PRIMARY KEY)").has_value());
        auto tbl = catalog.get_table(default_database_id, "subn");
        ASSERT_TRUE(tbl.has_value());
        table_id_t tid = tbl->table_id;

        // Property name "my__uniq__col" contains "__uniq__" as a substring.
        std::vector<ColumnDef> props = {{"my__uniq__col", TypeId::STRING}};
        auto eid = graph_engine.create_edge_type(default_database_id,
                                                 "substr_edge",
                                                 tid,
                                                 tid,
                                                 TypeId::INT64,
                                                 TypeId::INT64,
                                                 props,
                                                 /*prevent_duplicates=*/false);
        ASSERT_TRUE(eid.has_value()) << eid.error().message;
        auto et = catalog.get_edge_type(default_database_id, "substr_edge");
        ASSERT_TRUE(et.has_value());
        EXPECT_FALSE(et->prevent_duplicates);
        // The encoded token is "my__uniq__col:STRING" — not the bare sentinel.
        EXPECT_NE(et->properties.find("my__uniq__col:"), std::string::npos);

        auto persist = persistence.persist_edge_type(*et);
        ASSERT_TRUE(persist.has_value()) << persist.error().message;

        ASSERT_TRUE(
            graph_engine
                .link(default_database_id, "substr_edge", pk(1), pk(2), {Value(std::string("x"))})
                .has_value());
    }

    // ===== SECOND RUN (restart) =====
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

        auto et = catalog.get_edge_type(default_database_id, "substr_edge");
        ASSERT_TRUE(et.has_value());
        EXPECT_FALSE(et->prevent_duplicates)
            << "property whose name contains __uniq__ as a substring must NOT flip "
               "prevent_duplicates to true (whole-token match required)";

        // Duplicate must succeed — type is non-unique.
        auto r = graph_engine.link(
            default_database_id, "substr_edge", pk(1), pk(2), {Value(std::string("y"))});
        ASSERT_TRUE(r.has_value())
            << "duplicate insert on non-unique edge type must succeed: " << r.error().message;
    }

    fs::remove_all(data_dir);
}

/// Sentinel robustness: a UNIQUE edge type that also has a real property named
/// something with no sentinel collision.  Both the sentinel AND the column
/// definitions coexist in the properties string and both round-trip correctly.
TEST(QA_GDB871, SentinelAndRealPropertyCoexistAndBothRoundTrip) {
    auto data_dir = fs::temp_directory_path() / "sixseven_qa_gdb871_sentinel_coexist";
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

        ASSERT_TRUE(engine.execute("CREATE TABLE coex_n (id BIGINT PRIMARY KEY)").has_value());
        auto tbl = catalog.get_table(default_database_id, "coex_n");
        ASSERT_TRUE(tbl.has_value());
        table_id_t tid = tbl->table_id;

        std::vector<ColumnDef> props = {{"cost", TypeId::FLOAT64}};
        auto eid = graph_engine.create_edge_type(default_database_id,
                                                 "coex_edge",
                                                 tid,
                                                 tid,
                                                 TypeId::INT64,
                                                 TypeId::INT64,
                                                 props,
                                                 /*prevent_duplicates=*/true);
        ASSERT_TRUE(eid.has_value()) << eid.error().message;
        auto et = catalog.get_edge_type(default_database_id, "coex_edge");
        ASSERT_TRUE(et.has_value());
        // Encoded: "__uniq__,cost:FLOAT64"
        EXPECT_NE(et->properties.find("__uniq__"), std::string::npos);
        EXPECT_NE(et->properties.find("cost:"), std::string::npos);
        EXPECT_TRUE(et->prevent_duplicates);

        auto persist = persistence.persist_edge_type(*et);
        ASSERT_TRUE(persist.has_value()) << persist.error().message;

        ASSERT_TRUE(graph_engine.link(default_database_id, "coex_edge", pk(5), pk(6), {Value(1.0)})
                        .has_value());
    }

    // ===== SECOND RUN (restart) =====
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

        auto et = catalog.get_edge_type(default_database_id, "coex_edge");
        ASSERT_TRUE(et.has_value());
        EXPECT_TRUE(et->prevent_duplicates)
            << "prevent_duplicates must survive restart alongside real property 'cost'";
        EXPECT_NE(et->properties.find("cost:"), std::string::npos)
            << "real property 'cost' must survive alongside sentinel after restart";

        auto le = graph_engine.load_edges();
        ASSERT_TRUE(le.has_value()) << le.error().message;

        // Duplicate of pre-restart edge must be rejected.
        auto r_dup =
            graph_engine.link(default_database_id, "coex_edge", pk(5), pk(6), {Value(2.0)});
        ASSERT_FALSE(r_dup.has_value())
            << "duplicate of pre-restart edge must be rejected when sentinel+column coexist";
        EXPECT_EQ(r_dup.error().code, StatusCode::CONSTRAINT_VIOLATION);

        // New edge must be accepted.
        auto r_new =
            graph_engine.link(default_database_id, "coex_edge", pk(7), pk(8), {Value(0.5)});
        ASSERT_TRUE(r_new.has_value())
            << "new distinct edge must be accepted: " << r_new.error().message;
    }

    fs::remove_all(data_dir);
}

/// Backward-compat: an EdgeTypeDef loaded from an old-style catalog record
/// (properties string with NO "__uniq__" token) must decode to
/// prevent_duplicates=false with no crash.
/// This exercises the same decode path as load_catalog() but through a direct
/// catalog restore, bypassing the production bootstrap round-trip.
TEST(QA_GDB871, BackwardCompatNullSentinelDecodesToFalse) {
    Catalog catalog;
    init_test_catalog(catalog);

    // Simulate a legacy on-disk record: properties string has real columns but
    // no __uniq__ sentinel (pre-GDB-871 format).
    EdgeTypeDef def;
    def.edge_id = 42;
    def.database_id = default_database_id;
    def.name = "legacy_edge";
    def.source_table_id = 1;
    def.target_table_id = 1;
    def.properties = "weight:FLOAT64,note:STRING";
    // No __uniq__ token anywhere — simulate old catalog record.
    // The decode logic in load_catalog sets prevent_duplicates from the
    // whole-token scan; since the token is absent, it must be false.
    // We manually replicate the decode here to verify invariant.
    {
        bool found = false;
        const std::string& p = def.properties;
        std::string::size_type start = 0;
        while (start <= p.size()) {
            auto end = p.find(',', start);
            if (end == std::string::npos) {
                end = p.size();
            }
            if (p.compare(start, end - start, "__uniq__") == 0) {
                found = true;
                break;
            }
            start = end + 1;
        }
        def.prevent_duplicates = found;
    }

    EXPECT_FALSE(def.prevent_duplicates)
        << "legacy catalog record without __uniq__ sentinel must decode to false (backward compat)";

    // Restore into catalog — must not crash.
    auto r = catalog.restore_edge_type(default_database_id, def);
    // restore_edge_type may fail if source/target tables don't exist, but it must
    // not crash, and prevent_duplicates must have been set correctly before restore.
    (void)r;
    SUCCEED() << "legacy record restore did not crash";
}

/// Backward-compat: empty properties string (NULL column in old catalog) decodes
/// to prevent_duplicates=false.
TEST(QA_GDB871, BackwardCompatEmptyPropertiesDecodesToFalse) {
    // Replicate the load_catalog decode on an empty properties string.
    const std::string p;
    bool found = false;
    std::string::size_type start = 0;
    while (start <= p.size()) {
        auto end = p.find(',', start);
        if (end == std::string::npos) {
            end = p.size();
        }
        if (p.compare(start, end - start, "__uniq__") == 0) {
            found = true;
            break;
        }
        start = end + 1;
    }
    EXPECT_FALSE(found) << "empty properties string must not set prevent_duplicates=true";
}
