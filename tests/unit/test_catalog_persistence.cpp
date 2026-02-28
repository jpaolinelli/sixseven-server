#include "giodb/catalog/catalog.h"
#include "giodb/catalog/schema.h"
#include "giodb/common/config.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/catalog_persistence.h"
#include "giodb/executor/query_engine.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/executor/system_bootstrap.h"
#include "giodb/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

using namespace giodb;

// =============================================================================
// Test fixture for catalog persistence (GDB-215)
// =============================================================================

class CatalogPersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "giodb_test_catalog_persistence";
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

    /// Run full bootstrap and assert success.
    void run_bootstrap() {
        auto result = SystemBootstrap::bootstrap(
            *engine_, *catalog_, *storage_, *persistence_, config_, data_dir_);
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }

    /// Simulate a server restart: destroy all in-memory state and recreate from disk.
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

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, expected);
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<DiskManager> dm_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<CatalogPersistence> persistence_;
    std::unique_ptr<QueryEngine> engine_;
    Config config_;
};

// =============================================================================
// AC: System catalog tables created on first run
// =============================================================================

TEST_F(CatalogPersistenceTest, CreateSystemCatalogTables) {
    run_bootstrap();

    // All 5 system catalog tables should exist in the catalog.
    auto sys_tables = catalog_->get_table(system_database_id, "sys_tables");
    ASSERT_TRUE(sys_tables.has_value()) << sys_tables.error().message;
    EXPECT_EQ(sys_tables->table_id, sys_tables_table_id);

    auto sys_columns = catalog_->get_table(system_database_id, "sys_columns");
    ASSERT_TRUE(sys_columns.has_value()) << sys_columns.error().message;
    EXPECT_EQ(sys_columns->table_id, sys_columns_table_id);

    auto sys_indexes = catalog_->get_table(system_database_id, "sys_indexes");
    ASSERT_TRUE(sys_indexes.has_value()) << sys_indexes.error().message;
    EXPECT_EQ(sys_indexes->table_id, sys_indexes_table_id);

    auto sys_edge_types = catalog_->get_table(system_database_id, "sys_edge_types");
    ASSERT_TRUE(sys_edge_types.has_value()) << sys_edge_types.error().message;
    EXPECT_EQ(sys_edge_types->table_id, sys_edge_types_table_id);

    auto sys_emb = catalog_->get_table(system_database_id, "sys_embedding_columns");
    ASSERT_TRUE(sys_emb.has_value()) << sys_emb.error().message;
    EXPECT_EQ(sys_emb->table_id, sys_embedding_columns_table_id);
}

TEST_F(CatalogPersistenceTest, SystemCatalogTablesHaveCorrectSchemas) {
    run_bootstrap();

    auto sys_tables = catalog_->get_table(system_database_id, "sys_tables");
    ASSERT_TRUE(sys_tables.has_value());
    ASSERT_EQ(sys_tables->columns.size(), 4u);
    EXPECT_EQ(sys_tables->columns[0].name, "table_id");
    EXPECT_EQ(sys_tables->columns[1].name, "database_id");
    EXPECT_EQ(sys_tables->columns[2].name, "name");
    EXPECT_EQ(sys_tables->columns[3].name, "pk_columns");

    auto sys_columns = catalog_->get_table(system_database_id, "sys_columns");
    ASSERT_TRUE(sys_columns.has_value());
    ASSERT_EQ(sys_columns->columns.size(), 6u);
    EXPECT_EQ(sys_columns->columns[0].name, "table_id");
    EXPECT_EQ(sys_columns->columns[1].name, "ordinal");
    EXPECT_EQ(sys_columns->columns[2].name, "name");
    EXPECT_EQ(sys_columns->columns[3].name, "type_id");
    EXPECT_EQ(sys_columns->columns[4].name, "nullable");
    EXPECT_EQ(sys_columns->columns[5].name, "default_expr");
}

// =============================================================================
// AC: User table IDs start after system tables
// =============================================================================

TEST_F(CatalogPersistenceTest, UserTableIdsStartAtFirstUserTableId) {
    run_bootstrap();

    EXPECT_GE(catalog_->next_table_id(), first_user_table_id);

    // Create a user table and verify its ID.
    engine_->set_current_database(default_database_id);
    exec_ok("CREATE TABLE test_table (id INT, name VARCHAR)");

    auto schema = catalog_->get_table(default_database_id, "test_table");
    ASSERT_TRUE(schema.has_value());
    EXPECT_GE(schema->table_id, first_user_table_id);
}

// =============================================================================
// AC: persist_table writes to sys_tables and sys_columns
// =============================================================================

TEST_F(CatalogPersistenceTest, PersistTableWritesToSystemHeaps) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // Create a table with table-level PRIMARY KEY constraint.
    exec_ok("CREATE TABLE users (id INT, name VARCHAR, age INT, PRIMARY KEY (id))");

    auto schema = catalog_->get_table(default_database_id, "users");
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ(schema->pk_columns, "id");

    // Simulate restart and load catalog.
    restart();
    run_bootstrap(); // Second run: loads from disk.

    // Verify the table was restored.
    auto restored = catalog_->get_table(default_database_id, "users");
    ASSERT_TRUE(restored.has_value()) << restored.error().message;
    EXPECT_EQ(restored->name, "users");
    EXPECT_EQ(restored->table_id, schema->table_id);
    ASSERT_EQ(restored->columns.size(), 3u);
    EXPECT_EQ(restored->columns[0].name, "id");
    EXPECT_EQ(restored->columns[0].type_id, TypeId::INT32);
    EXPECT_EQ(restored->columns[1].name, "name");
    EXPECT_EQ(restored->columns[1].type_id, TypeId::STRING);
    EXPECT_EQ(restored->columns[2].name, "age");
    EXPECT_EQ(restored->columns[2].type_id, TypeId::INT32);
    EXPECT_EQ(restored->pk_columns, "id");
}

// =============================================================================
// AC: Catalog survives server restart
// =============================================================================

TEST_F(CatalogPersistenceTest, CatalogSurvivesRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // Create multiple tables.
    exec_ok("CREATE TABLE products (product_id INT, title VARCHAR, price DOUBLE)");
    exec_ok("CREATE TABLE orders (order_id INT, product_id INT, quantity INT)");

    auto products = catalog_->get_table(default_database_id, "products");
    auto orders = catalog_->get_table(default_database_id, "orders");
    ASSERT_TRUE(products.has_value());
    ASSERT_TRUE(orders.has_value());

    auto products_id = products->table_id;
    auto orders_id = orders->table_id;

    // Restart.
    restart();
    run_bootstrap();

    // Both tables should be restored.
    auto restored_products = catalog_->get_table(default_database_id, "products");
    ASSERT_TRUE(restored_products.has_value()) << restored_products.error().message;
    EXPECT_EQ(restored_products->table_id, products_id);
    EXPECT_EQ(restored_products->name, "products");
    ASSERT_EQ(restored_products->columns.size(), 3u);

    auto restored_orders = catalog_->get_table(default_database_id, "orders");
    ASSERT_TRUE(restored_orders.has_value()) << restored_orders.error().message;
    EXPECT_EQ(restored_orders->table_id, orders_id);
    EXPECT_EQ(restored_orders->name, "orders");
    ASSERT_EQ(restored_orders->columns.size(), 3u);
}

// =============================================================================
// AC: DROP TABLE removes from persistence
// =============================================================================

TEST_F(CatalogPersistenceTest, DropTableRemovesFromPersistence) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE temp_table (id INT, data VARCHAR)");

    auto schema = catalog_->get_table(default_database_id, "temp_table");
    ASSERT_TRUE(schema.has_value());

    // Drop the table.
    exec_ok("DROP TABLE temp_table");

    // Verify table is gone from catalog.
    auto gone = catalog_->get_table(default_database_id, "temp_table");
    EXPECT_FALSE(gone.has_value());

    // Restart and verify the table is NOT restored.
    restart();
    run_bootstrap();

    auto after_restart = catalog_->get_table(default_database_id, "temp_table");
    EXPECT_FALSE(after_restart.has_value());
}

// =============================================================================
// AC: Data inserted into persisted tables survives restart
// =============================================================================

TEST_F(CatalogPersistenceTest, TableDataSurvivesRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE scores (name VARCHAR, score INT)");
    exec_ok("INSERT INTO scores VALUES ('Alice', 100)");
    exec_ok("INSERT INTO scores VALUES ('Bob', 200)");

    // Restart.
    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // Table schema should be restored.
    auto schema = catalog_->get_table(default_database_id, "scores");
    ASSERT_TRUE(schema.has_value());

    // Data should be queryable.
    auto qr = exec_ok("SELECT name, score FROM scores");
    ASSERT_EQ(qr.rows.size(), 2u);
}

// =============================================================================
// AC: Multiple restarts preserve catalog
// =============================================================================

TEST_F(CatalogPersistenceTest, MultipleRestarts) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE t1 (id INT, val VARCHAR)");

    // First restart.
    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    auto t1 = catalog_->get_table(default_database_id, "t1");
    ASSERT_TRUE(t1.has_value());

    // Create another table.
    exec_ok("CREATE TABLE t2 (id INT, data VARCHAR)");

    // Second restart.
    restart();
    run_bootstrap();

    // Both tables should exist.
    auto restored_t1 = catalog_->get_table(default_database_id, "t1");
    ASSERT_TRUE(restored_t1.has_value()) << restored_t1.error().message;

    auto restored_t2 = catalog_->get_table(default_database_id, "t2");
    ASSERT_TRUE(restored_t2.has_value()) << restored_t2.error().message;
}

// =============================================================================
// AC: Column metadata preserved correctly across restart
// =============================================================================

TEST_F(CatalogPersistenceTest, ColumnMetadataPreserved) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE typed_cols ("
            "a INT, b BIGINT, c FLOAT, d DOUBLE, "
            "e BOOLEAN, f VARCHAR, g TIMESTAMP)");

    auto orig = catalog_->get_table(default_database_id, "typed_cols");
    ASSERT_TRUE(orig.has_value());
    ASSERT_EQ(orig->columns.size(), 7u);

    restart();
    run_bootstrap();

    auto restored = catalog_->get_table(default_database_id, "typed_cols");
    ASSERT_TRUE(restored.has_value());
    ASSERT_EQ(restored->columns.size(), 7u);

    // Verify each column type is correct.
    EXPECT_EQ(restored->columns[0].type_id, TypeId::INT32);
    EXPECT_EQ(restored->columns[0].name, "a");
    EXPECT_EQ(restored->columns[1].type_id, TypeId::INT64);
    EXPECT_EQ(restored->columns[1].name, "b");
    EXPECT_EQ(restored->columns[2].type_id, TypeId::FLOAT32);
    EXPECT_EQ(restored->columns[2].name, "c");
    EXPECT_EQ(restored->columns[3].type_id, TypeId::FLOAT64);
    EXPECT_EQ(restored->columns[3].name, "d");
    EXPECT_EQ(restored->columns[4].type_id, TypeId::BOOL);
    EXPECT_EQ(restored->columns[4].name, "e");
    EXPECT_EQ(restored->columns[5].type_id, TypeId::STRING);
    EXPECT_EQ(restored->columns[5].name, "f");
    EXPECT_EQ(restored->columns[6].type_id, TypeId::TIMESTAMP);
    EXPECT_EQ(restored->columns[6].name, "g");
}

// =============================================================================
// AC: persist_index / remove_index work correctly
// =============================================================================

TEST_F(CatalogPersistenceTest, PersistAndRemoveIndex) {
    run_bootstrap();

    IndexDef idx;
    idx.index_id = 1;
    idx.table_id = first_user_table_id;
    idx.name = "idx_test";
    idx.index_type = "btree";
    idx.columns = "id";
    idx.is_unique = true;

    // Restore index in catalog so it can be verified.
    auto restore = catalog_->restore_index(idx);
    ASSERT_TRUE(restore.has_value()) << restore.error().message;

    auto persist = persistence_->persist_index(idx);
    ASSERT_TRUE(persist.has_value()) << persist.error().message;

    // Restart and load.
    restart();
    run_bootstrap();

    auto indexes = catalog_->list_indexes(first_user_table_id);
    bool found = false;
    for (const auto& i : indexes) {
        if (i.name == "idx_test") {
            found = true;
            EXPECT_EQ(i.index_type, "btree");
            EXPECT_EQ(i.columns, "id");
            EXPECT_TRUE(i.is_unique);
        }
    }
    EXPECT_TRUE(found) << "index idx_test not restored after restart";

    // Now remove it.
    auto remove = persistence_->remove_index(idx.index_id);
    ASSERT_TRUE(remove.has_value()) << remove.error().message;

    // Restart and verify it's gone.
    restart();
    run_bootstrap();

    auto indexes_after = catalog_->list_indexes(first_user_table_id);
    for (const auto& i : indexes_after) {
        EXPECT_NE(i.name, "idx_test") << "index should have been removed";
    }
}

// =============================================================================
// AC: persist_edge_type / remove_edge_type work correctly
// =============================================================================

TEST_F(CatalogPersistenceTest, PersistAndRemoveEdgeType) {
    run_bootstrap();

    EdgeTypeDef edge;
    edge.edge_id = 1;
    edge.name = "follows";
    edge.source_table_id = first_user_table_id;
    edge.target_table_id = first_user_table_id;
    edge.properties = "weight:FLOAT64";

    auto restore = catalog_->restore_edge_type(edge);
    ASSERT_TRUE(restore.has_value()) << restore.error().message;

    auto persist = persistence_->persist_edge_type(edge);
    ASSERT_TRUE(persist.has_value()) << persist.error().message;

    // Restart and load.
    restart();
    run_bootstrap();

    auto restored = catalog_->get_edge_type("follows");
    ASSERT_TRUE(restored.has_value()) << restored.error().message;
    EXPECT_EQ(restored->name, "follows");
    EXPECT_EQ(restored->source_table_id, first_user_table_id);
    EXPECT_EQ(restored->target_table_id, first_user_table_id);
    EXPECT_EQ(restored->properties, "weight:FLOAT64");

    // Remove and verify.
    auto remove = persistence_->remove_edge_type(edge.edge_id);
    ASSERT_TRUE(remove.has_value()) << remove.error().message;

    restart();
    run_bootstrap();

    auto gone = catalog_->get_edge_type("follows");
    EXPECT_FALSE(gone.has_value());
}

// =============================================================================
// AC: persist_embedding_column works correctly
// =============================================================================

TEST_F(CatalogPersistenceTest, PersistEmbeddingColumn) {
    run_bootstrap();

    EmbeddingColumnDef emb;
    emb.table_id = first_user_table_id;
    emb.column_id = 2;
    emb.dimension = 384;
    emb.source_expr = "title || ' ' || description";
    emb.provider = "ollama/all-minilm";

    catalog_->restore_embedding_column(emb);
    auto persist = persistence_->persist_embedding_column(emb);
    ASSERT_TRUE(persist.has_value()) << persist.error().message;

    // Restart and verify.
    restart();
    run_bootstrap();

    auto emb_cols = catalog_->list_embedding_columns(first_user_table_id);
    ASSERT_EQ(emb_cols.size(), 1u) << "embedding column not restored after restart";
    EXPECT_EQ(emb_cols[0].column_id, 2);
    EXPECT_EQ(emb_cols[0].dimension, 384);
    EXPECT_EQ(emb_cols[0].source_expr, "title || ' ' || description");
    EXPECT_EQ(emb_cols[0].provider, "ollama/all-minilm");
}

// =============================================================================
// AC: remove_table cascades to indexes, edge types, embedding columns
// =============================================================================

TEST_F(CatalogPersistenceTest, RemoveTableCascadesToRelatedEntries) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE cascade_test (id INT, data VARCHAR)");
    auto schema = catalog_->get_table(default_database_id, "cascade_test");
    ASSERT_TRUE(schema.has_value());
    auto tid = schema->table_id;

    // Add an index for this table.
    IndexDef idx;
    idx.index_id = 1;
    idx.table_id = tid;
    idx.name = "idx_cascade";
    idx.index_type = "hash";
    idx.columns = "data";
    idx.is_unique = false;
    auto r1 = catalog_->restore_index(idx);
    ASSERT_TRUE(r1.has_value());
    auto r2 = persistence_->persist_index(idx);
    ASSERT_TRUE(r2.has_value());

    // Add an embedding column for this table.
    EmbeddingColumnDef emb;
    emb.table_id = tid;
    emb.column_id = 1;
    emb.dimension = 128;
    emb.source_expr = "data";
    emb.provider = "test";
    catalog_->restore_embedding_column(emb);
    auto r3 = persistence_->persist_embedding_column(emb);
    ASSERT_TRUE(r3.has_value());

    // Drop the table (which calls remove_table internally).
    exec_ok("DROP TABLE cascade_test");

    // Restart and verify nothing related is restored.
    restart();
    run_bootstrap();

    auto gone = catalog_->get_table(default_database_id, "cascade_test");
    EXPECT_FALSE(gone.has_value());

    auto indexes = catalog_->list_indexes(tid);
    EXPECT_TRUE(indexes.empty());

    auto emb_cols = catalog_->list_embedding_columns(tid);
    EXPECT_TRUE(emb_cols.empty());
}

// =============================================================================
// AC: Create and drop multiple tables, then restart
// =============================================================================

TEST_F(CatalogPersistenceTest, CreateDropMultipleThenRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE keep1 (id INT, data VARCHAR)");
    exec_ok("CREATE TABLE drop1 (id INT, data VARCHAR)");
    exec_ok("CREATE TABLE keep2 (id INT, data VARCHAR)");
    exec_ok("CREATE TABLE drop2 (id INT, data VARCHAR)");

    exec_ok("DROP TABLE drop1");
    exec_ok("DROP TABLE drop2");

    restart();
    run_bootstrap();

    EXPECT_TRUE(catalog_->get_table(default_database_id, "keep1").has_value());
    EXPECT_TRUE(catalog_->get_table(default_database_id, "keep2").has_value());
    EXPECT_FALSE(catalog_->get_table(default_database_id, "drop1").has_value());
    EXPECT_FALSE(catalog_->get_table(default_database_id, "drop2").has_value());
}

// =============================================================================
// AC: Reserved system table IDs are stable
// =============================================================================

TEST_F(CatalogPersistenceTest, SystemTableIdsAreStable) {
    EXPECT_EQ(sys_settings_table_id, 1);
    EXPECT_EQ(sys_providers_table_id, 2);
    EXPECT_EQ(sys_tables_table_id, 3);
    EXPECT_EQ(sys_columns_table_id, 4);
    EXPECT_EQ(sys_indexes_table_id, 5);
    EXPECT_EQ(sys_edge_types_table_id, 6);
    EXPECT_EQ(sys_embedding_columns_table_id, 7);
    EXPECT_EQ(sys_embedding_jobs_table_id, 8);
    EXPECT_EQ(first_user_table_id, 9);
}

TEST_F(CatalogPersistenceTest, SystemTableSchemaIdsMatch) {
    EXPECT_EQ(sys_settings_schema().table_id, sys_settings_table_id);
    EXPECT_EQ(sys_providers_schema().table_id, sys_providers_table_id);
    EXPECT_EQ(sys_tables_schema().table_id, sys_tables_table_id);
    EXPECT_EQ(sys_columns_schema().table_id, sys_columns_table_id);
    EXPECT_EQ(sys_indexes_schema().table_id, sys_indexes_table_id);
    EXPECT_EQ(sys_edge_types_schema().table_id, sys_edge_types_table_id);
    EXPECT_EQ(sys_embedding_columns_schema().table_id, sys_embedding_columns_table_id);
    EXPECT_EQ(sys_embedding_jobs_schema().table_id, sys_embedding_jobs_table_id);
}

// =============================================================================
// Edge case: Empty catalog loads cleanly
// =============================================================================

TEST_F(CatalogPersistenceTest, EmptyCatalogLoadsCleanly) {
    run_bootstrap();

    // No user tables created — just restart and verify it works.
    restart();
    run_bootstrap();

    // System tables should still exist.
    auto sys_tables = catalog_->get_table(system_database_id, "sys_tables");
    ASSERT_TRUE(sys_tables.has_value());

    // No user tables should exist.
    auto user_tables = catalog_->list_tables(default_database_id);
    EXPECT_TRUE(user_tables.empty());
}

// =============================================================================
// Edge case: Table with nullable columns
// =============================================================================

TEST_F(CatalogPersistenceTest, NullableColumnsPreserved) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE nullable_test (id INT NOT NULL, optional_data VARCHAR)");

    restart();
    run_bootstrap();

    auto restored = catalog_->get_table(default_database_id, "nullable_test");
    ASSERT_TRUE(restored.has_value());
    ASSERT_EQ(restored->columns.size(), 2u);
    EXPECT_FALSE(restored->columns[0].nullable); // PK is not nullable.
    EXPECT_TRUE(restored->columns[1].nullable);  // Default is nullable.
}
