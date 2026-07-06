/// @file test_qa_gdb_1263.cpp
/// QA adversarial tests for GDB-1263: case-insensitive edge-type name resolution.
///
/// GDB-1263 normalizes edge-type name lookups via to_lower() at two choke points:
///   - Catalog::edge_name_to_id_ (create/get/drop/restore_edge_type)
///   - GraphEngine::make_edge_key (edge_tables_ / edge_storage_ access)
/// The *display* name (EdgeTypeDef::name) is left in its original casing.
///
/// This file focuses on the attack surface NOT already covered by the updated
/// tests/qa/test_qa_gdb_807.cpp (TRAVERSE with various casings of edge type name
/// and property) and tests/unit/test_catalog.cpp (Catalog-level CRUD case tests):
///   1. CREATE EDGE TYPE collision across case (rated vs RATED must be ONE type).
///   2. DROP EDGE TYPE with different case than declared, and that TRAVERSE then
///      fails cleanly afterward.
///   3. Persistence / restart simulation via Catalog::restore_edge_type -- the
///      normalization must survive a fresh GraphEngine/Catalog reload path.
///   4. MATCH and LINK with mixed-case edge type names.
///   5. Display name (list_edge_types) preserves original casing -- normalization
///      is lookup-only, not a data mutation.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "test_qa_helpers.h"

namespace sixseven {
namespace {

// -- Helper: build a simple TableSchema (mirrors tests/unit/test_catalog.cpp) --

TableSchema make_users_schema() {
    TableSchema schema;
    schema.name = "users";
    schema.columns = {
        {0, "id", TypeId::INT32, false, ""},
        {1, "name", TypeId::STRING, true, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

TableSchema make_products_schema() {
    TableSchema schema;
    schema.name = "products";
    schema.columns = {
        {0, "id", TypeId::INT32, false, ""},
        {1, "name", TypeId::STRING, true, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

class QA_GDB1263 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1263";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");
        exec_ok("INSERT INTO users VALUES (3, 'Carol')");

        exec_ok("CREATE TABLE products (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO products VALUES (10, 'Widget')");
        exec_ok("INSERT INTO products VALUES (20, 'Gadget')");
        exec_ok("INSERT INTO products VALUES (30, 'Gizmo')");

        exec_ok("CREATE EDGE TYPE rated (score DOUBLE, review VARCHAR) "
                "FROM users TO products");

        exec_ok("LINK users(1) TO products(10) VIA rated (score = 4.5, review = 'excellent')");
        exec_ok("LINK users(1) TO products(20) VIA rated (score = 1.5, review = 'terrible')");
        exec_ok("LINK users(1) TO products(30) VIA rated (score = 3.0, review = 'average')");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void exec_err(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << ": expected error but got success";
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// 1. TRAVERSE resolves regardless of case (sanity re-check at the QueryEngine
//    layer, complementing test_qa_gdb_807.cpp's coverage)
// ============================================================================

TEST_F(QA_GDB1263, TraverseUppercaseEdgeTypeReturnsSameRowsAsDeclaredCase) {
    auto lower = exec_ok("SELECT rated.score "
                         "FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");
    auto upper = exec_ok("SELECT rated.score "
                         "FROM TRAVERSE RATED FROM users(1) DIRECTION OUT FETCH AS t");
    auto mixed = exec_ok("SELECT rated.score "
                         "FROM TRAVERSE Rated FROM users(1) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(lower.rows.size(), 3u);
    ASSERT_EQ(upper.rows.size(), 3u);
    ASSERT_EQ(mixed.rows.size(), 3u);
}

// ============================================================================
// 2. CREATE / DUPLICATE -- second CREATE EDGE TYPE differing only in case must
//    collide with the first, not create a second, unreachable edge type.
// ============================================================================

TEST_F(QA_GDB1263, CreateEdgeTypeUppercaseAfterLowercaseCollides) {
    // 'rated' already created in SetUp(). Creating 'RATED' must fail as a
    // duplicate, not silently succeed as a second, distinct edge type.
    exec_err("CREATE EDGE TYPE RATED (score DOUBLE) FROM users TO products");
}

TEST_F(QA_GDB1263, CreateEdgeTypeCaseCollisionDoesNotCorruptExistingType) {
    // After the failed duplicate create, the original 'rated' edge type and its
    // existing links must still be fully intact and traversable.
    exec_err("CREATE EDGE TYPE RATED (score DOUBLE) FROM users TO products");
    auto qr = exec_ok("SELECT rated.score "
                      "FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(qr.rows.size(), 3u);
}

TEST_F(QA_GDB1263, CatalogListEdgeTypesHasExactlyOneEntryAfterCaseCollisionAttempt) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);
    auto t1 = catalog.create_table(default_database_id, make_users_schema());
    auto t2 = catalog.create_table(default_database_id, make_products_schema());
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    EdgeTypeDef def;
    def.name = "rated";
    def.source_table_id = *t1;
    def.target_table_id = *t2;
    ASSERT_TRUE(catalog.create_edge_type(default_database_id, def).has_value());

    EdgeTypeDef dup;
    dup.name = "RATED";
    dup.source_table_id = *t1;
    dup.target_table_id = *t2;
    auto dup_result = catalog.create_edge_type(default_database_id, dup);
    ASSERT_FALSE(dup_result.has_value());
    EXPECT_EQ(dup_result.error().code, StatusCode::ALREADY_EXISTS);

    EXPECT_EQ(catalog.list_edge_types(default_database_id).size(), 1u);
}

// ============================================================================
// 3. DROP -- DROP EDGE TYPE with different case drops the declared-case type,
//    and subsequent TRAVERSE fails cleanly (not a crash, not silent no-op).
// ============================================================================

TEST_F(QA_GDB1263, DropEdgeTypeUppercaseDropsLowercaseDeclaredType) {
    exec_ok("DROP EDGE TYPE RATED");
    // Subsequent TRAVERSE, in any case, must now fail cleanly (edge type gone).
    exec_err("SELECT rated.score "
             "FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t");
    exec_err("SELECT rated.score "
             "FROM TRAVERSE RATED FROM users(1) DIRECTION OUT FETCH AS t");
}

TEST_F(QA_GDB1263, DropEdgeTypeThenRecreateWithDifferentCaseSucceeds) {
    exec_ok("DROP EDGE TYPE RATED");
    // Recreating with a brand new casing is a fresh create, not a duplicate.
    exec_ok("CREATE EDGE TYPE RaTeD (score DOUBLE, review VARCHAR) FROM users TO products");
    exec_ok("LINK users(2) TO products(10) VIA RaTeD (score = 5.0, review = 'perfect')");
    auto qr = exec_ok("SELECT rated.score "
                      "FROM TRAVERSE rated FROM users(2) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_DOUBLE_EQ(qr.rows[0][0].as_float64(), 5.0);
}

// ============================================================================
// 4. PERSISTENCE / RESTART -- restore_edge_type must normalize the same way
//    create_edge_type does, so a "restart" (fresh Catalog + restore) preserves
//    case-insensitive lookup.
// ============================================================================

TEST(QA_GDB1263_Persistence, RestoreEdgeTypeNormalizesNameForLookup) {
    Catalog original;
    bootstrap_qa_catalog(original);
    auto t1 = original.create_table(default_database_id, make_users_schema());
    auto t2 = original.create_table(default_database_id, make_products_schema());
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    EdgeTypeDef def;
    def.name = "rated";
    def.source_table_id = *t1;
    def.target_table_id = *t2;
    auto created = original.create_edge_type(default_database_id, def);
    ASSERT_TRUE(created.has_value());

    // Capture the persisted definition exactly as it would be read back from
    // the catalog's on-disk representation (original casing retained on the
    // EdgeTypeDef, since that's the field durability code round-trips).
    auto persisted = original.get_edge_type(default_database_id, "rated");
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->name, "rated");

    // Simulate a restart: fresh Catalog instance, restore from the "persisted" def.
    Catalog restarted;
    bootstrap_qa_catalog(restarted);
    // Recreate the same tables (ids must line up for this unit-level simulation).
    auto rt1 = restarted.create_table(default_database_id, make_users_schema());
    auto rt2 = restarted.create_table(default_database_id, make_products_schema());
    ASSERT_TRUE(rt1.has_value());
    ASSERT_TRUE(rt2.has_value());

    EdgeTypeDef restore_def = *persisted;
    auto restored = restarted.restore_edge_type(default_database_id, restore_def);
    ASSERT_TRUE(restored.has_value()) << restored.error().message;

    // Post-restart, uppercase lookup must still resolve case-insensitively.
    auto looked_up_upper = restarted.get_edge_type(default_database_id, "RATED");
    ASSERT_TRUE(looked_up_upper.has_value()) << looked_up_upper.error().message;
    EXPECT_EQ(looked_up_upper->name, "rated");  // display name preserved, not lowercased

    auto looked_up_mixed = restarted.get_edge_type(default_database_id, "RaTeD");
    ASSERT_TRUE(looked_up_mixed.has_value());
}

TEST(QA_GDB1263_Persistence, RestoreEdgeTypeRejectsCaseCollisionWithExisting) {
    Catalog catalog;
    bootstrap_qa_catalog(catalog);
    auto t1 = catalog.create_table(default_database_id, make_users_schema());
    auto t2 = catalog.create_table(default_database_id, make_products_schema());
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    EdgeTypeDef def;
    def.name = "rated";
    def.source_table_id = *t1;
    def.target_table_id = *t2;
    ASSERT_TRUE(catalog.create_edge_type(default_database_id, def).has_value());

    // Simulate restoring a second edge type from a WAL/snapshot that, due to a
    // pre-fix bug or external corruption, differs only in case from an
    // already-restored type. This must be rejected, not silently overwrite.
    EdgeTypeDef dup_restore;
    dup_restore.edge_id = 9999;
    dup_restore.name = "RATED";
    dup_restore.source_table_id = *t1;
    dup_restore.target_table_id = *t2;
    auto result = catalog.restore_edge_type(default_database_id, dup_restore);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::ALREADY_EXISTS);
}

// ============================================================================
// 5. MATCH / LINK with mixed-case edge type names
// ============================================================================

TEST_F(QA_GDB1263, MatchWithUppercaseEdgeTypeNameResolves) {
    auto qr = exec_ok("SELECT b.name FROM MATCH (a:users)-[e:RATED]->(b:products) "
                      "WHERE a.id = 1");
    ASSERT_EQ(qr.rows.size(), 3u);
}

TEST_F(QA_GDB1263, MatchWithMixedCaseEdgeTypeNameResolves) {
    auto qr = exec_ok("SELECT b.name FROM MATCH (a:users)-[e:Rated]->(b:products) "
                      "WHERE a.id = 1");
    ASSERT_EQ(qr.rows.size(), 3u);
}

TEST_F(QA_GDB1263, LinkWithUppercaseEdgeTypeNameResolvesToLowercaseDeclaredType) {
    exec_ok("LINK users(3) TO products(20) VIA RATED (score = 2.5, review = 'ok')");
    auto qr = exec_ok("SELECT rated.score "
                      "FROM TRAVERSE rated FROM users(3) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_DOUBLE_EQ(qr.rows[0][0].as_float64(), 2.5);
}

TEST_F(QA_GDB1263, LinkWithMixedCaseThenTraverseWithDifferentMixedCase) {
    exec_ok("LINK users(3) TO products(30) VIA RaTeD (score = 1.0, review = 'meh')");
    auto qr = exec_ok("SELECT rated.score "
                      "FROM TRAVERSE rAtEd FROM users(3) DIRECTION OUT FETCH AS t");
    ASSERT_EQ(qr.rows.size(), 1u);
}

// ============================================================================
// 6. DISPLAY -- list_edge_types / get_edge_type must show ORIGINAL casing.
//    Normalization is lookup-only, never mutates the stored display name.
// ============================================================================

TEST_F(QA_GDB1263, GetEdgeTypeDisplayNamePreservesOriginalCasingViaUppercaseLookup) {
    auto def = catalog_.get_edge_type(default_database_id, "RATED");
    ASSERT_TRUE(def.has_value()) << def.error().message;
    EXPECT_EQ(def->name, "rated");
}

TEST_F(QA_GDB1263, ListEdgeTypesShowsOriginalCasingNotLowercased) {
    auto types = catalog_.list_edge_types(default_database_id);
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0].name, "rated");
}

TEST_F(QA_GDB1263, CreatedWithMixedCaseNamePreservesExactCasingInListing) {
    exec_ok("DROP EDGE TYPE rated");
    exec_ok("CREATE EDGE TYPE RaTeD (score DOUBLE, review VARCHAR) FROM users TO products");
    auto types = catalog_.list_edge_types(default_database_id);
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0].name, "RaTeD");

    // And it's still reachable via any other case.
    auto qr = exec_ok("SELECT rated.score "
                      "FROM TRAVERSE RATED FROM users(1) DIRECTION OUT FETCH AS t");
    // No links exist yet under the recreated type; this must succeed with 0 rows,
    // not error (edge type resolves fine, there's just no data).
    EXPECT_EQ(qr.rows.size(), 0u);
}

}  // namespace
}  // namespace sixseven
