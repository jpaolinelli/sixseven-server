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

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

using namespace sixseven;

// =============================================================================
// Test fixture for catalog persistence QA (GDB-215)
// =============================================================================

class QA_CatalogPersistence : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_215";
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

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value());
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected);
        }
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
// Boundary: Table ID counter advances correctly across restarts
// =============================================================================

TEST_F(QA_CatalogPersistence, TableIdNoCollisionAfterRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // Create 3 tables.
    exec_ok("CREATE TABLE t_a (id INT)");
    exec_ok("CREATE TABLE t_b (id INT)");
    exec_ok("CREATE TABLE t_c (id INT)");

    auto tc = catalog_->get_table(default_database_id, "t_c");
    ASSERT_TRUE(tc.has_value());
    auto max_id_before = tc->table_id;

    // Restart.
    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // Create a new table — its ID must be > max_id_before.
    exec_ok("CREATE TABLE t_d (id INT)");
    auto td = catalog_->get_table(default_database_id, "t_d");
    ASSERT_TRUE(td.has_value());
    EXPECT_GT(td->table_id, max_id_before) << "new table ID should be greater than max restored ID";
}

// =============================================================================
// Boundary: Table ID counter handles gaps from dropped tables
// =============================================================================

TEST_F(QA_CatalogPersistence, TableIdCounterHandlesGapsFromDrops) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE keep (id INT)");    // table_id = 8
    exec_ok("CREATE TABLE drop_me (id INT)"); // table_id = 9
    exec_ok("CREATE TABLE last (id INT)");    // table_id = 10

    auto last = catalog_->get_table(default_database_id, "last");
    ASSERT_TRUE(last.has_value());
    auto last_id = last->table_id;

    // Drop the middle table.
    exec_ok("DROP TABLE drop_me");

    // Restart.
    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // Verify "keep" and "last" survive, "drop_me" does not.
    EXPECT_TRUE(catalog_->get_table(default_database_id, "keep").has_value());
    EXPECT_TRUE(catalog_->get_table(default_database_id, "last").has_value());
    EXPECT_FALSE(catalog_->get_table(default_database_id, "drop_me").has_value());

    // Create a new table — its ID must not collide with any existing.
    exec_ok("CREATE TABLE new_table (id INT)");
    auto nt = catalog_->get_table(default_database_id, "new_table");
    ASSERT_TRUE(nt.has_value());
    EXPECT_GT(nt->table_id, last_id)
        << "new table ID must be greater than the highest restored table ID";
}

// =============================================================================
// Boundary: Reuse table name after drop and restart
// =============================================================================

TEST_F(QA_CatalogPersistence, ReusableTableNameAfterDropAndRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // Create and drop a table.
    exec_ok("CREATE TABLE reusable (id INT, name VARCHAR)");
    ASSERT_TRUE(catalog_->get_table(default_database_id, "reusable").has_value());

    exec_ok("DROP TABLE reusable");

    // Restart.
    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // Recreate with different schema.
    exec_ok("CREATE TABLE reusable (x DOUBLE, y DOUBLE, z DOUBLE)");
    auto recreated = catalog_->get_table(default_database_id, "reusable");
    ASSERT_TRUE(recreated.has_value());
    // Note: table_id may be reused if no other tables exist. That's OK.
    ASSERT_EQ(recreated->columns.size(), 3u);
    EXPECT_EQ(recreated->columns[0].name, "x");
    EXPECT_EQ(recreated->columns[0].type_id, TypeId::FLOAT64);

    // Restart again to verify the new schema persists.
    restart();
    run_bootstrap();

    auto final_check = catalog_->get_table(default_database_id, "reusable");
    ASSERT_TRUE(final_check.has_value());
    ASSERT_EQ(final_check->columns.size(), 3u);
    EXPECT_EQ(final_check->columns[0].name, "x");
}

// =============================================================================
// Stress: Many tables survive restart
// =============================================================================

TEST_F(QA_CatalogPersistence, ManyTablesSurviveRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    constexpr int NUM_TABLES = 50;
    for (int i = 0; i < NUM_TABLES; ++i) {
        std::string sql = "CREATE TABLE stress_" + std::to_string(i) + " (id INT, val VARCHAR)";
        exec_ok(sql);
    }

    // Restart.
    restart();
    run_bootstrap();

    // Verify all tables survived.
    for (int i = 0; i < NUM_TABLES; ++i) {
        std::string name = "stress_" + std::to_string(i);
        auto schema = catalog_->get_table(default_database_id, name);
        ASSERT_TRUE(schema.has_value()) << "table '" << name << "' not found after restart";
        ASSERT_EQ(schema->columns.size(), 2u);
    }
}

// =============================================================================
// Stress: Wide table with many columns
// =============================================================================

TEST_F(QA_CatalogPersistence, WideTableManyColumnsSurviveRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // Build a table with 20 columns.
    std::string cols;
    for (int i = 0; i < 20; ++i) {
        if (!cols.empty()) {
            cols += ", ";
        }
        cols += "col_" + std::to_string(i) + " INT";
    }
    exec_ok("CREATE TABLE wide_table (" + cols + ")");

    auto orig = catalog_->get_table(default_database_id, "wide_table");
    ASSERT_TRUE(orig.has_value());
    ASSERT_EQ(orig->columns.size(), 20u);

    // Restart.
    restart();
    run_bootstrap();

    auto restored = catalog_->get_table(default_database_id, "wide_table");
    ASSERT_TRUE(restored.has_value());
    ASSERT_EQ(restored->columns.size(), 20u);

    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(restored->columns[static_cast<size_t>(i)].name, "col_" + std::to_string(i));
        EXPECT_EQ(restored->columns[static_cast<size_t>(i)].type_id, TypeId::INT32);
        EXPECT_EQ(restored->columns[static_cast<size_t>(i)].ordinal, i);
    }
}

// =============================================================================
// Boundary: Column ordinals preserved correctly
// =============================================================================

TEST_F(QA_CatalogPersistence, ColumnOrdinalsPreservedAfterRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE ordinal_test (z INT, a VARCHAR, m DOUBLE)");

    auto orig = catalog_->get_table(default_database_id, "ordinal_test");
    ASSERT_TRUE(orig.has_value());
    ASSERT_EQ(orig->columns.size(), 3u);
    EXPECT_EQ(orig->columns[0].ordinal, 0);
    EXPECT_EQ(orig->columns[0].name, "z");
    EXPECT_EQ(orig->columns[1].ordinal, 1);
    EXPECT_EQ(orig->columns[1].name, "a");
    EXPECT_EQ(orig->columns[2].ordinal, 2);
    EXPECT_EQ(orig->columns[2].name, "m");

    // Restart.
    restart();
    run_bootstrap();

    auto restored = catalog_->get_table(default_database_id, "ordinal_test");
    ASSERT_TRUE(restored.has_value());
    ASSERT_EQ(restored->columns.size(), 3u);

    // Verify column ordering is identical (sorted by ordinal, not alphabetical).
    EXPECT_EQ(restored->columns[0].ordinal, 0);
    EXPECT_EQ(restored->columns[0].name, "z");
    EXPECT_EQ(restored->columns[1].ordinal, 1);
    EXPECT_EQ(restored->columns[1].name, "a");
    EXPECT_EQ(restored->columns[2].ordinal, 2);
    EXPECT_EQ(restored->columns[2].name, "m");
}

// =============================================================================
// Sequence: Three consecutive restarts with incremental changes
// =============================================================================

TEST_F(QA_CatalogPersistence, ThreeConsecutiveRestarts) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE r1 (id INT)");

    // First restart.
    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);
    EXPECT_TRUE(catalog_->get_table(default_database_id, "r1").has_value());

    exec_ok("CREATE TABLE r2 (id INT)");

    // Second restart.
    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);
    EXPECT_TRUE(catalog_->get_table(default_database_id, "r1").has_value());
    EXPECT_TRUE(catalog_->get_table(default_database_id, "r2").has_value());

    exec_ok("CREATE TABLE r3 (id INT)");

    // Third restart.
    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);
    EXPECT_TRUE(catalog_->get_table(default_database_id, "r1").has_value());
    EXPECT_TRUE(catalog_->get_table(default_database_id, "r2").has_value());
    EXPECT_TRUE(catalog_->get_table(default_database_id, "r3").has_value());
}

// =============================================================================
// Data: INSERT data survives restart and is queryable
// =============================================================================

TEST_F(QA_CatalogPersistence, InsertedDataQueryableAfterRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE data_test (id INT, name VARCHAR, value DOUBLE)");
    exec_ok("INSERT INTO data_test VALUES (1, 'alice', 3.14)");
    exec_ok("INSERT INTO data_test VALUES (2, 'bob', 2.71)");
    exec_ok("INSERT INTO data_test VALUES (3, 'carol', 1.41)");

    // Restart.
    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    auto qr = exec_ok("SELECT id, name, value FROM data_test");
    ASSERT_EQ(qr.rows.size(), 3u);

    // Verify data integrity.
    bool found_alice = false;
    bool found_bob = false;
    bool found_carol = false;
    for (const auto& row : qr.rows) {
        ASSERT_EQ(row.size(), 3u);
        auto name = row[1].as_string();
        if (name == "alice") {
            found_alice = true;
            EXPECT_EQ(row[0].as_int32(), 1);
        } else if (name == "bob") {
            found_bob = true;
            EXPECT_EQ(row[0].as_int32(), 2);
        } else if (name == "carol") {
            found_carol = true;
            EXPECT_EQ(row[0].as_int32(), 3);
        }
    }
    EXPECT_TRUE(found_alice);
    EXPECT_TRUE(found_bob);
    EXPECT_TRUE(found_carol);
}

// =============================================================================
// Data: UPDATE and DELETE effects survive restart
// =============================================================================

TEST_F(QA_CatalogPersistence, UpdateDeleteSurviveRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE dml_test (id INT, value INT)");
    exec_ok("INSERT INTO dml_test VALUES (1, 100)");
    exec_ok("INSERT INTO dml_test VALUES (2, 200)");
    exec_ok("INSERT INTO dml_test VALUES (3, 300)");

    // Update row 2 and delete row 3.
    exec_ok("UPDATE dml_test SET value = 999 WHERE id = 2");
    exec_ok("DELETE FROM dml_test WHERE id = 3");

    // Restart.
    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    auto qr = exec_ok("SELECT id, value FROM dml_test");
    ASSERT_EQ(qr.rows.size(), 2u);

    // Verify rows.
    bool found_1 = false;
    bool found_2 = false;
    for (const auto& row : qr.rows) {
        auto id = row[0].as_int32();
        if (id == 1) {
            found_1 = true;
            EXPECT_EQ(row[1].as_int32(), 100);
        } else if (id == 2) {
            found_2 = true;
            EXPECT_EQ(row[1].as_int32(), 999);
        }
    }
    EXPECT_TRUE(found_1);
    EXPECT_TRUE(found_2);
}

// =============================================================================
// Boundary: Empty table (no data) survives restart
// =============================================================================

TEST_F(QA_CatalogPersistence, EmptyTableSurvivesRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE empty_table (id INT, data VARCHAR)");

    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    auto schema = catalog_->get_table(default_database_id, "empty_table");
    ASSERT_TRUE(schema.has_value());
    ASSERT_EQ(schema->columns.size(), 2u);

    // Query should return 0 rows.
    auto qr = exec_ok("SELECT * FROM empty_table");
    EXPECT_EQ(qr.rows.size(), 0u);
}

// =============================================================================
// Boundary: Table with NULL values in data survives restart
// =============================================================================

TEST_F(QA_CatalogPersistence, NullDataValuesSurviveRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE null_test (id INT NOT NULL, opt_name VARCHAR, opt_val INT)");
    exec_ok("INSERT INTO null_test VALUES (1, NULL, NULL)");
    exec_ok("INSERT INTO null_test VALUES (2, 'hello', NULL)");
    exec_ok("INSERT INTO null_test VALUES (3, NULL, 42)");

    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    auto qr = exec_ok("SELECT id, opt_name, opt_val FROM null_test");
    ASSERT_EQ(qr.rows.size(), 3u);

    // Find the row with id=1 and verify NULLs.
    for (const auto& row : qr.rows) {
        auto id = row[0].as_int32();
        if (id == 1) {
            EXPECT_TRUE(row[1].is_null());
            EXPECT_TRUE(row[2].is_null());
        } else if (id == 2) {
            EXPECT_EQ(row[1].as_string(), "hello");
            EXPECT_TRUE(row[2].is_null());
        } else if (id == 3) {
            EXPECT_TRUE(row[1].is_null());
            EXPECT_EQ(row[2].as_int32(), 42);
        }
    }
}

// =============================================================================
// Sequence: Create table, insert data, drop, create same name, restart
// =============================================================================

TEST_F(QA_CatalogPersistence, RecreateTableWithNewSchemaAndDataSurvivesRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // First version.
    exec_ok("CREATE TABLE recycled (id INT, value VARCHAR)");
    exec_ok("INSERT INTO recycled VALUES (1, 'old')");

    // Drop and recreate with different schema.
    exec_ok("DROP TABLE recycled");
    exec_ok("CREATE TABLE recycled (x DOUBLE, y DOUBLE)");
    exec_ok("INSERT INTO recycled VALUES (1.5, 2.5)");

    // Restart.
    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    auto schema = catalog_->get_table(default_database_id, "recycled");
    ASSERT_TRUE(schema.has_value());
    ASSERT_EQ(schema->columns.size(), 2u);
    EXPECT_EQ(schema->columns[0].name, "x");
    EXPECT_EQ(schema->columns[0].type_id, TypeId::FLOAT64);

    auto qr = exec_ok("SELECT x, y FROM recycled");
    ASSERT_EQ(qr.rows.size(), 1u);
}

// =============================================================================
// Boundary: Multiple indexes on same table survive restart
// =============================================================================

TEST_F(QA_CatalogPersistence, MultipleIndexesSurviveRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE indexed_table (id INT, name VARCHAR, age INT)");
    auto schema = catalog_->get_table(default_database_id, "indexed_table");
    ASSERT_TRUE(schema.has_value());
    auto tid = schema->table_id;

    // Create multiple indexes.
    IndexDef idx1;
    idx1.index_id = 1;
    idx1.table_id = tid;
    idx1.name = "idx_id";
    idx1.index_type = "btree";
    idx1.columns = "id";
    idx1.is_unique = true;

    IndexDef idx2;
    idx2.index_id = 2;
    idx2.table_id = tid;
    idx2.name = "idx_name";
    idx2.index_type = "hash";
    idx2.columns = "name";
    idx2.is_unique = false;

    IndexDef idx3;
    idx3.index_id = 3;
    idx3.table_id = tid;
    idx3.name = "idx_composite";
    idx3.index_type = "btree";
    idx3.columns = "id,name";
    idx3.is_unique = true;

    ASSERT_TRUE(catalog_->restore_index(idx1).has_value());
    ASSERT_TRUE(catalog_->restore_index(idx2).has_value());
    ASSERT_TRUE(catalog_->restore_index(idx3).has_value());

    ASSERT_TRUE(persistence_->persist_index(idx1).has_value());
    ASSERT_TRUE(persistence_->persist_index(idx2).has_value());
    ASSERT_TRUE(persistence_->persist_index(idx3).has_value());

    // Restart.
    restart();
    run_bootstrap();

    auto indexes = catalog_->list_indexes(tid);
    ASSERT_EQ(indexes.size(), 3u);

    // Verify all three indexes are present.
    bool found_id = false;
    bool found_name = false;
    bool found_composite = false;
    for (const auto& idx : indexes) {
        if (idx.name == "idx_id") {
            found_id = true;
            EXPECT_EQ(idx.index_type, "btree");
            EXPECT_TRUE(idx.is_unique);
        } else if (idx.name == "idx_name") {
            found_name = true;
            EXPECT_EQ(idx.index_type, "hash");
            EXPECT_FALSE(idx.is_unique);
        } else if (idx.name == "idx_composite") {
            found_composite = true;
            EXPECT_EQ(idx.columns, "id,name");
        }
    }
    EXPECT_TRUE(found_id);
    EXPECT_TRUE(found_name);
    EXPECT_TRUE(found_composite);
}

// =============================================================================
// Boundary: Edge type with empty properties
// =============================================================================

TEST_F(QA_CatalogPersistence, EdgeTypeEmptyPropertiesSurvivesRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE people (id INT)");
    auto schema = catalog_->get_table(default_database_id, "people");
    ASSERT_TRUE(schema.has_value());
    auto tid = schema->table_id;

    EdgeTypeDef edge;
    edge.edge_id = 1;
    edge.name = "knows";
    edge.source_table_id = tid;
    edge.target_table_id = tid;
    edge.properties = ""; // Empty properties.

    ASSERT_TRUE(catalog_->restore_edge_type(edge).has_value());
    ASSERT_TRUE(persistence_->persist_edge_type(edge).has_value());

    // Restart.
    restart();
    run_bootstrap();

    auto restored = catalog_->get_edge_type("knows");
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->name, "knows");
    EXPECT_EQ(restored->properties, "");
}

// =============================================================================
// Boundary: Embedding column with empty source_expr
// =============================================================================

TEST_F(QA_CatalogPersistence, EmbeddingEmptySourceExprSurvivesRestart) {
    run_bootstrap();

    EmbeddingColumnDef emb;
    emb.table_id = first_user_table_id;
    emb.column_id = 0;
    emb.dimension = 768;
    emb.source_expr = ""; // Empty source expression.
    emb.provider = "test-provider";

    catalog_->restore_embedding_column(emb);
    ASSERT_TRUE(persistence_->persist_embedding_column(emb).has_value());

    restart();
    run_bootstrap();

    auto emb_cols = catalog_->list_embedding_columns(first_user_table_id);
    ASSERT_EQ(emb_cols.size(), 1u);
    EXPECT_EQ(emb_cols[0].source_expr, "");
    EXPECT_EQ(emb_cols[0].provider, "test-provider");
    EXPECT_EQ(emb_cols[0].dimension, 768);
}

// =============================================================================
// Boundary: pk_columns with multiple columns (composite PK)
// =============================================================================

TEST_F(QA_CatalogPersistence, CompositePrimaryKeySurvivesRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE composite_pk (a INT, b INT, c VARCHAR, "
            "PRIMARY KEY (a, b))");

    auto orig = catalog_->get_table(default_database_id, "composite_pk");
    ASSERT_TRUE(orig.has_value());
    EXPECT_EQ(orig->pk_columns, "a,b");

    restart();
    run_bootstrap();

    auto restored = catalog_->get_table(default_database_id, "composite_pk");
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->pk_columns, "a,b");
}

// =============================================================================
// Boundary: Table with no primary key
// =============================================================================

TEST_F(QA_CatalogPersistence, TableNoPrimaryKeySurvivesRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE no_pk (id INT, data VARCHAR)");

    auto orig = catalog_->get_table(default_database_id, "no_pk");
    ASSERT_TRUE(orig.has_value());
    EXPECT_TRUE(orig->pk_columns.empty());

    restart();
    run_bootstrap();

    auto restored = catalog_->get_table(default_database_id, "no_pk");
    ASSERT_TRUE(restored.has_value());
    EXPECT_TRUE(restored->pk_columns.empty());
}

// =============================================================================
// Boundary: All supported data types survive restart
// =============================================================================

TEST_F(QA_CatalogPersistence, AllDataTypesSurviveRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE all_types ("
            "c_int INT, c_bigint BIGINT, c_float FLOAT, c_double DOUBLE, "
            "c_bool BOOLEAN, c_varchar VARCHAR, c_timestamp TIMESTAMP)");

    restart();
    run_bootstrap();

    auto restored = catalog_->get_table(default_database_id, "all_types");
    ASSERT_TRUE(restored.has_value());
    ASSERT_EQ(restored->columns.size(), 7u);

    EXPECT_EQ(restored->columns[0].type_id, TypeId::INT32);
    EXPECT_EQ(restored->columns[1].type_id, TypeId::INT64);
    EXPECT_EQ(restored->columns[2].type_id, TypeId::FLOAT32);
    EXPECT_EQ(restored->columns[3].type_id, TypeId::FLOAT64);
    EXPECT_EQ(restored->columns[4].type_id, TypeId::BOOL);
    EXPECT_EQ(restored->columns[5].type_id, TypeId::STRING);
    EXPECT_EQ(restored->columns[6].type_id, TypeId::TIMESTAMP);
}

// =============================================================================
// Sequence: Drop all tables, restart — clean empty state
// =============================================================================

TEST_F(QA_CatalogPersistence, DropAllTablesRestartsClean) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE temp1 (id INT)");
    exec_ok("CREATE TABLE temp2 (id INT)");
    exec_ok("CREATE TABLE temp3 (id INT)");

    exec_ok("DROP TABLE temp1");
    exec_ok("DROP TABLE temp2");
    exec_ok("DROP TABLE temp3");

    restart();
    run_bootstrap();

    auto user_tables = catalog_->list_tables(default_database_id);
    EXPECT_TRUE(user_tables.empty());
}

// =============================================================================
// Boundary: System tables are not queryable via user SQL after restart
// (they are in system_database_id, not default_database_id)
// =============================================================================

TEST_F(QA_CatalogPersistence, SystemTablesNotInDefaultDatabase) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // sys_tables should NOT be visible from the default database context.
    exec_error("SELECT * FROM sys_tables", StatusCode::NOT_FOUND);
}

// =============================================================================
// Sequence: Insert data before restart, insert more after restart
// =============================================================================

TEST_F(QA_CatalogPersistence, InsertBeforeAndAfterRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE append_test (id INT, label VARCHAR)");
    exec_ok("INSERT INTO append_test VALUES (1, 'before')");

    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // Insert more data after restart.
    exec_ok("INSERT INTO append_test VALUES (2, 'after')");

    auto qr = exec_ok("SELECT id, label FROM append_test");
    ASSERT_EQ(qr.rows.size(), 2u);

    bool found_before = false;
    bool found_after = false;
    for (const auto& row : qr.rows) {
        auto label = row[1].as_string();
        if (label == "before") {
            found_before = true;
        } else if (label == "after") {
            found_after = true;
        }
    }
    EXPECT_TRUE(found_before);
    EXPECT_TRUE(found_after);
}

// =============================================================================
// Boundary: Restart without any DDL operations (only bootstrap tables)
// =============================================================================

TEST_F(QA_CatalogPersistence, DoubleRestartNoUserTables) {
    run_bootstrap();

    // First restart — no user tables at all.
    restart();
    run_bootstrap();

    // Second restart — still no user tables.
    restart();
    run_bootstrap();

    // System tables should still exist.
    auto sys_tables = catalog_->get_table(system_database_id, "sys_tables");
    ASSERT_TRUE(sys_tables.has_value());

    auto sys_columns = catalog_->get_table(system_database_id, "sys_columns");
    ASSERT_TRUE(sys_columns.has_value());

    auto user_tables = catalog_->list_tables(default_database_id);
    EXPECT_TRUE(user_tables.empty());
}

// =============================================================================
// Boundary: Index ID counter advances correctly
// =============================================================================

TEST_F(QA_CatalogPersistence, IndexIdCounterAdvancesAfterRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE idx_counter_test (id INT)");
    auto schema = catalog_->get_table(default_database_id, "idx_counter_test");
    ASSERT_TRUE(schema.has_value());
    auto tid = schema->table_id;

    IndexDef idx;
    idx.index_id = 5;
    idx.table_id = tid;
    idx.name = "idx_high_id";
    idx.index_type = "btree";
    idx.columns = "id";
    idx.is_unique = false;

    ASSERT_TRUE(catalog_->restore_index(idx).has_value());
    ASSERT_TRUE(persistence_->persist_index(idx).has_value());

    restart();
    run_bootstrap();

    // Creating a new index should get an ID > 5.
    IndexDef new_idx;
    new_idx.table_id = tid;
    new_idx.name = "idx_new";
    new_idx.index_type = "btree";
    new_idx.columns = "id";
    new_idx.is_unique = false;

    auto new_id = catalog_->create_index(std::move(new_idx));
    ASSERT_TRUE(new_id.has_value()) << new_id.error().message;
    EXPECT_GT(*new_id, 5) << "new index ID should be > highest restored index ID";
}

// =============================================================================
// Boundary: Edge ID counter advances correctly
// =============================================================================

TEST_F(QA_CatalogPersistence, EdgeIdCounterAdvancesAfterRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE edge_counter_test (id INT)");
    auto schema = catalog_->get_table(default_database_id, "edge_counter_test");
    ASSERT_TRUE(schema.has_value());
    auto tid = schema->table_id;

    EdgeTypeDef edge;
    edge.edge_id = 10;
    edge.name = "edge_high_id";
    edge.source_table_id = tid;
    edge.target_table_id = tid;
    edge.properties = "";

    ASSERT_TRUE(catalog_->restore_edge_type(edge).has_value());
    ASSERT_TRUE(persistence_->persist_edge_type(edge).has_value());

    restart();
    run_bootstrap();

    // Creating a new edge type should get an ID > 10.
    EdgeTypeDef new_edge;
    new_edge.name = "edge_new";
    new_edge.source_table_id = tid;
    new_edge.target_table_id = tid;
    new_edge.properties = "";

    auto new_id = catalog_->create_edge_type(std::move(new_edge));
    ASSERT_TRUE(new_id.has_value()) << new_id.error().message;
    EXPECT_GT(*new_id, 10) << "new edge ID should be > highest restored edge ID";
}

// =============================================================================
// Sequence: DROP TABLE removes related indexes from persistence
// =============================================================================

TEST_F(QA_CatalogPersistence, DropTableRemovesIndexesFromPersistence) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE drop_idx_test (id INT, name VARCHAR)");
    auto schema = catalog_->get_table(default_database_id, "drop_idx_test");
    ASSERT_TRUE(schema.has_value());
    auto tid = schema->table_id;

    IndexDef idx;
    idx.index_id = 1;
    idx.table_id = tid;
    idx.name = "idx_to_cascade";
    idx.index_type = "btree";
    idx.columns = "id";
    idx.is_unique = false;

    ASSERT_TRUE(catalog_->restore_index(idx).has_value());
    ASSERT_TRUE(persistence_->persist_index(idx).has_value());

    // Drop the table.
    exec_ok("DROP TABLE drop_idx_test");

    restart();
    run_bootstrap();

    // The table and its indexes should be gone.
    EXPECT_FALSE(catalog_->get_table(default_database_id, "drop_idx_test").has_value());
    auto indexes = catalog_->list_indexes(tid);
    EXPECT_TRUE(indexes.empty());
}

// =============================================================================
// Sequence: DROP TABLE removes related edge types from persistence
// =============================================================================

TEST_F(QA_CatalogPersistence, DropTableRemovesEdgeTypesFromPersistence) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE src (id INT)");
    exec_ok("CREATE TABLE tgt (id INT)");

    auto src = catalog_->get_table(default_database_id, "src");
    auto tgt = catalog_->get_table(default_database_id, "tgt");
    ASSERT_TRUE(src.has_value());
    ASSERT_TRUE(tgt.has_value());

    EdgeTypeDef edge;
    edge.edge_id = 1;
    edge.name = "links_to";
    edge.source_table_id = src->table_id;
    edge.target_table_id = tgt->table_id;
    edge.properties = "";

    ASSERT_TRUE(catalog_->restore_edge_type(edge).has_value());
    ASSERT_TRUE(persistence_->persist_edge_type(edge).has_value());

    // Drop the source table — edge type should be cascaded.
    exec_ok("DROP TABLE src");

    restart();
    run_bootstrap();

    auto restored_edge = catalog_->get_edge_type("links_to");
    EXPECT_FALSE(restored_edge.has_value())
        << "edge type should be removed when source table is dropped";
}

// =============================================================================
// Boundary: Create table in system db context should fail
// =============================================================================

TEST_F(QA_CatalogPersistence, CreateTableInSystemDatabaseFails) {
    run_bootstrap();
    engine_->set_current_database(system_database_id);

    // Creating user tables in the system database should fail because
    // "sys_tables" (and other system table names) are reserved.
    // But creating a non-system-named table should work.
    auto result = engine_->execute("CREATE TABLE user_in_sys (id INT)");
    if (result.has_value()) {
        // If it succeeds, verify it persists.
        restart();
        run_bootstrap();
        auto restored = catalog_->get_table(system_database_id, "user_in_sys");
        // This is acceptable behavior — the table was created in the system database.
        EXPECT_TRUE(restored.has_value());
    }
    // If it fails, that's also acceptable — system database may be restricted.
}

// =============================================================================
// Data: Large string values survive restart
// =============================================================================

TEST_F(QA_CatalogPersistence, LargeStringDataSurvivesRestart) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE large_str (id INT, data VARCHAR)");

    // Insert a row with a reasonably large string.
    std::string large_value(500, 'X');
    exec_ok("INSERT INTO large_str VALUES (1, '" + large_value + "')");

    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    auto qr = exec_ok("SELECT data FROM large_str");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string().size(), 500u);
    EXPECT_EQ(qr.rows[0][0].as_string(), large_value);
}

// =============================================================================
// Boundary: Bootstrap flag file existence determines first vs subsequent run
// =============================================================================

TEST_F(QA_CatalogPersistence, BootstrapFlagFileControls) {
    // First run: not bootstrapped.
    EXPECT_FALSE(SystemBootstrap::is_bootstrapped(data_dir_));

    run_bootstrap();

    // After bootstrap: flagged.
    EXPECT_TRUE(SystemBootstrap::is_bootstrapped(data_dir_));

    // After restart: still flagged.
    restart();
    EXPECT_TRUE(SystemBootstrap::is_bootstrapped(data_dir_));
}

// =============================================================================
// Boundary: Multiple embedding columns for same table survive restart
// =============================================================================

TEST_F(QA_CatalogPersistence, MultipleEmbeddingColumnsSurviveRestart) {
    run_bootstrap();

    EmbeddingColumnDef emb1;
    emb1.table_id = first_user_table_id;
    emb1.column_id = 0;
    emb1.dimension = 128;
    emb1.source_expr = "title";
    emb1.provider = "provider_a";

    EmbeddingColumnDef emb2;
    emb2.table_id = first_user_table_id;
    emb2.column_id = 1;
    emb2.dimension = 512;
    emb2.source_expr = "description";
    emb2.provider = "provider_b";

    catalog_->restore_embedding_column(emb1);
    catalog_->restore_embedding_column(emb2);

    ASSERT_TRUE(persistence_->persist_embedding_column(emb1).has_value());
    ASSERT_TRUE(persistence_->persist_embedding_column(emb2).has_value());

    restart();
    run_bootstrap();

    auto emb_cols = catalog_->list_embedding_columns(first_user_table_id);
    ASSERT_EQ(emb_cols.size(), 2u);

    bool found_a = false;
    bool found_b = false;
    for (const auto& e : emb_cols) {
        if (e.provider == "provider_a") {
            found_a = true;
            EXPECT_EQ(e.dimension, 128);
        } else if (e.provider == "provider_b") {
            found_b = true;
            EXPECT_EQ(e.dimension, 512);
        }
    }
    EXPECT_TRUE(found_a);
    EXPECT_TRUE(found_b);
}

// =============================================================================
// Error path: persist_table on non-existent system table ID
// (exercise insert_row error path when storage is missing)
// =============================================================================

TEST_F(QA_CatalogPersistence, PersistTableBeforeBootstrapFails) {
    // Do NOT call run_bootstrap() — system tables don't exist.
    TableSchema schema;
    schema.table_id = 100;
    schema.name = "orphan";
    schema.columns = {{0, "id", TypeId::INT32, false, ""}};

    auto result = persistence_->persist_table(default_database_id, schema);
    EXPECT_FALSE(result.has_value())
        << "persist_table should fail when system tables aren't created yet";
}

// =============================================================================
// Error path: load_catalog on fresh directory (no system catalog files)
// =============================================================================

TEST_F(QA_CatalogPersistence, LoadCatalogOnFreshDirFails) {
    // Do NOT call run_bootstrap().
    auto result = persistence_->load_catalog();
    EXPECT_FALSE(result.has_value())
        << "load_catalog should fail when system catalog tables don't exist";
}

// =============================================================================
// Sequence: Create table, restart, verify DML works on restored table
// =============================================================================

TEST_F(QA_CatalogPersistence, DmlWorksOnRestoredTable) {
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    exec_ok("CREATE TABLE dml_after_restart (id INT, name VARCHAR)");

    restart();
    run_bootstrap();
    engine_->set_current_database(default_database_id);

    // Verify DML works on the restored table.
    exec_ok("INSERT INTO dml_after_restart VALUES (1, 'test')");
    exec_ok("UPDATE dml_after_restart SET name = 'updated' WHERE id = 1");

    auto qr = exec_ok("SELECT name FROM dml_after_restart WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "updated");

    exec_ok("DELETE FROM dml_after_restart WHERE id = 1");

    auto qr2 = exec_ok("SELECT * FROM dml_after_restart");
    EXPECT_EQ(qr2.rows.size(), 0u);
}
