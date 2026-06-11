#include "sixseven/catalog/catalog.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace sixseven;

// =============================================================================
// QA Adversarial Tests for GDB-18: Catalog & System Tables
// =============================================================================

// -- Helper -------------------------------------------------------------------

static TableSchema make_test_schema(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", TypeId::INT32, false, ""},
        {1, "name", TypeId::STRING, true, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

// =============================================================================
// Catalog: Empty/whitespace name validation
// =============================================================================

TEST(QA_Catalog, CreateTableEmptyName) {
    Catalog catalog;
    auto result = catalog.create_table(default_database_id, make_test_schema(""));
    // Empty name should ideally be rejected, but if it succeeds,
    // verify it's retrievable.
    if (result.has_value()) {
        auto get = catalog.get_table(default_database_id, "");
        EXPECT_TRUE(get.has_value());
    }
}

TEST(QA_Catalog, CreateDatabaseEmptyName) {
    Catalog catalog;
    auto result = catalog.create_database("");
    if (result.has_value()) {
        auto get = catalog.get_database("");
        EXPECT_TRUE(get.has_value());
    }
}

TEST(QA_Catalog, CreateIndexEmptyName) {
    Catalog catalog;
    auto tid = catalog.create_table(default_database_id, make_test_schema("t"));
    ASSERT_TRUE(tid.has_value());

    IndexDef def;
    def.table_id = *tid;
    def.name = "";
    def.index_type = "btree";
    def.columns = "id";

    auto result = catalog.create_index(def);
    if (result.has_value()) {
        auto get = catalog.get_index("");
        EXPECT_TRUE(get.has_value());
    }
}

TEST(QA_Catalog, CreateEdgeTypeEmptyName) {
    Catalog catalog;
    auto t1 = catalog.create_table(default_database_id, make_test_schema("a"));
    auto t2 = catalog.create_table(default_database_id, make_test_schema("b"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    EdgeTypeDef def;
    def.name = "";
    def.source_table_id = *t1;
    def.target_table_id = *t2;

    auto result = catalog.create_edge_type(default_database_id, def);
    if (result.has_value()) {
        auto get = catalog.get_edge_type(default_database_id, "");
        EXPECT_TRUE(get.has_value());
    }
}

// =============================================================================
// Catalog: Zero-column table creation
// =============================================================================

TEST(QA_Catalog, CreateTableZeroColumns) {
    Catalog catalog;
    TableSchema schema;
    schema.name = "empty_cols";
    // No columns at all.

    auto result = catalog.create_table(default_database_id, schema);
    // A zero-column table is questionable but may be allowed.
    if (result.has_value()) {
        auto get = catalog.get_table(default_database_id, "empty_cols");
        ASSERT_TRUE(get.has_value());
        EXPECT_EQ(get->columns.size(), 0u);
    }
}

// =============================================================================
// Catalog: Embedding column validation gaps
// =============================================================================

TEST(QA_Catalog, RegisterEmbeddingColumnZeroDimension) {
    Catalog catalog;
    auto tid = catalog.create_table(default_database_id, make_test_schema("docs"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.table_id = *tid;
    def.column_id = 0;
    def.dimension = 0; // Zero dimension — should this be valid?
    def.source_expr = "name";
    def.provider = "test";

    auto result = catalog.register_embedding_column(def);
    // Documenting current behavior: no dimension validation.
    if (result.has_value()) {
        auto embs = catalog.list_embedding_columns(*tid);
        ASSERT_EQ(embs.size(), 1u);
        EXPECT_EQ(embs[0].dimension, 0);
    }
}

TEST(QA_Catalog, RegisterEmbeddingColumnNegativeDimension) {
    Catalog catalog;
    auto tid = catalog.create_table(default_database_id, make_test_schema("docs"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.table_id = *tid;
    def.column_id = 0;
    def.dimension = -1; // Negative dimension.
    def.source_expr = "name";
    def.provider = "test";

    auto result = catalog.register_embedding_column(def);
    if (result.has_value()) {
        auto embs = catalog.list_embedding_columns(*tid);
        ASSERT_EQ(embs.size(), 1u);
        EXPECT_EQ(embs[0].dimension, -1);
    }
}

TEST(QA_Catalog, RegisterEmbeddingColumnInvalidColumnId) {
    Catalog catalog;
    auto tid = catalog.create_table(default_database_id, make_test_schema("docs"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.table_id = *tid;
    def.column_id = 999; // Column doesn't exist in the schema.
    def.dimension = 128;
    def.source_expr = "name";
    def.provider = "test";

    auto result = catalog.register_embedding_column(def);
    // Documenting: no column_id validation against table schema.
    if (result.has_value()) {
        auto embs = catalog.list_embedding_columns(*tid);
        ASSERT_EQ(embs.size(), 1u);
        EXPECT_EQ(embs[0].column_id, 999);
    }
}

// =============================================================================
// Catalog: Index column validation
// =============================================================================

TEST(QA_Catalog, CreateIndexWithNonexistentColumns) {
    Catalog catalog;
    auto tid = catalog.create_table(default_database_id, make_test_schema("users"));
    ASSERT_TRUE(tid.has_value());

    IndexDef def;
    def.table_id = *tid;
    def.name = "idx_bogus";
    def.index_type = "btree";
    def.columns = "nonexistent_column"; // Column doesn't exist.

    auto result = catalog.create_index(def);
    // Documenting: no column validation.
    if (result.has_value()) {
        auto get = catalog.get_index("idx_bogus");
        ASSERT_TRUE(get.has_value());
        EXPECT_EQ(get->columns, "nonexistent_column");
    }
}

TEST(QA_Catalog, CreateIndexWithEmptyColumns) {
    Catalog catalog;
    auto tid = catalog.create_table(default_database_id, make_test_schema("users"));
    ASSERT_TRUE(tid.has_value());

    IndexDef def;
    def.table_id = *tid;
    def.name = "idx_empty_cols";
    def.index_type = "btree";
    def.columns = ""; // No columns specified.

    auto result = catalog.create_index(def);
    // Documenting: no empty column validation.
    if (result.has_value()) {
        auto get = catalog.get_index("idx_empty_cols");
        ASSERT_TRUE(get.has_value());
        EXPECT_EQ(get->columns, "");
    }
}

// =============================================================================
// Catalog: Double-drop operations
// =============================================================================

TEST(QA_Catalog, DropTableTwiceFails) {
    Catalog catalog;
    auto tid = catalog.create_table(default_database_id, make_test_schema("temp"));
    ASSERT_TRUE(tid.has_value());

    auto drop1 = catalog.drop_table(default_database_id, "temp");
    ASSERT_TRUE(drop1.has_value());

    auto drop2 = catalog.drop_table(default_database_id, "temp");
    ASSERT_FALSE(drop2.has_value());
    EXPECT_EQ(drop2.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_Catalog, DropIndexTwiceFails) {
    Catalog catalog;
    auto tid = catalog.create_table(default_database_id, make_test_schema("t"));
    ASSERT_TRUE(tid.has_value());

    IndexDef def;
    def.table_id = *tid;
    def.name = "idx";
    def.index_type = "btree";
    def.columns = "id";
    ASSERT_TRUE(catalog.create_index(def).has_value());

    auto drop1 = catalog.drop_index("idx");
    ASSERT_TRUE(drop1.has_value());

    auto drop2 = catalog.drop_index("idx");
    ASSERT_FALSE(drop2.has_value());
    EXPECT_EQ(drop2.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_Catalog, DropEdgeTypeTwiceFails) {
    Catalog catalog;
    auto t1 = catalog.create_table(default_database_id, make_test_schema("a"));
    auto t2 = catalog.create_table(default_database_id, make_test_schema("b"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    EdgeTypeDef def;
    def.name = "edge";
    def.source_table_id = *t1;
    def.target_table_id = *t2;
    ASSERT_TRUE(catalog.create_edge_type(default_database_id, def).has_value());

    auto drop1 = catalog.drop_edge_type(default_database_id, "edge");
    ASSERT_TRUE(drop1.has_value());

    auto drop2 = catalog.drop_edge_type(default_database_id, "edge");
    ASSERT_FALSE(drop2.has_value());
    EXPECT_EQ(drop2.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_Catalog, DropDatabaseTwiceFails) {
    Catalog catalog;
    auto db = catalog.create_database("tempdb");
    ASSERT_TRUE(db.has_value());

    auto drop1 = catalog.drop_database(*db, false);
    ASSERT_TRUE(drop1.has_value());

    auto drop2 = catalog.drop_database(*db, false);
    ASSERT_FALSE(drop2.has_value());
    EXPECT_EQ(drop2.error().code, StatusCode::NOT_FOUND);
}

// =============================================================================
// Catalog: Global index naming across databases
// =============================================================================

TEST(QA_Catalog, IndexNameGlobalNotScopedPerDatabase) {
    Catalog catalog;
    auto db1 = catalog.create_database("db1");
    auto db2 = catalog.create_database("db2");
    ASSERT_TRUE(db1.has_value());
    ASSERT_TRUE(db2.has_value());

    auto t1 = catalog.create_table(*db1, make_test_schema("t"));
    auto t2 = catalog.create_table(*db2, make_test_schema("t"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    IndexDef def1;
    def1.table_id = *t1;
    def1.name = "idx_same_name";
    def1.index_type = "btree";
    def1.columns = "id";
    auto r1 = catalog.create_index(def1);
    ASSERT_TRUE(r1.has_value());

    // Same index name in different database should fail because names are global.
    IndexDef def2;
    def2.table_id = *t2;
    def2.name = "idx_same_name";
    def2.index_type = "btree";
    def2.columns = "id";
    auto r2 = catalog.create_index(def2);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, StatusCode::ALREADY_EXISTS);
}

// =============================================================================
// Catalog: Edge type validation edge cases
// =============================================================================

TEST(QA_Catalog, CreateEdgeTypeBothTablesNonexistent) {
    Catalog catalog;

    EdgeTypeDef def;
    def.name = "broken";
    def.source_table_id = 888;
    def.target_table_id = 999;

    auto result = catalog.create_edge_type(default_database_id, def);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_Catalog, CreateEdgeTypeAfterSourceDropped) {
    Catalog catalog;
    auto t1 = catalog.create_table(default_database_id, make_test_schema("src"));
    auto t2 = catalog.create_table(default_database_id, make_test_schema("tgt"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    // Drop source table first.
    ASSERT_TRUE(catalog.drop_table(default_database_id, "src").has_value());

    EdgeTypeDef def;
    def.name = "broken";
    def.source_table_id = *t1;
    def.target_table_id = *t2;

    auto result = catalog.create_edge_type(default_database_id, def);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// =============================================================================
// Catalog: list operations on invalid/empty databases
// =============================================================================

TEST(QA_Catalog, ListTablesNonexistentDatabase) {
    Catalog catalog;
    auto tables = catalog.list_tables(9999);
    EXPECT_TRUE(tables.empty());
}

TEST(QA_Catalog, ListIndexesNonexistentTable) {
    Catalog catalog;
    auto indexes = catalog.list_indexes(9999);
    EXPECT_TRUE(indexes.empty());
}

TEST(QA_Catalog, ListEmbeddingColumnsNonexistentTable) {
    Catalog catalog;
    auto embs = catalog.list_embedding_columns(9999);
    EXPECT_TRUE(embs.empty());
}

// =============================================================================
// Catalog: Stress test — many tables
// =============================================================================

TEST(QA_Catalog, StressCreateManyTables) {
    Catalog catalog;
    constexpr int N = 500;

    for (int i = 0; i < N; ++i) {
        auto result =
            catalog.create_table(default_database_id, make_test_schema("t" + std::to_string(i)));
        ASSERT_TRUE(result.has_value())
            << "Failed at table " << i << ": " << result.error().message;
    }

    auto tables = catalog.list_tables(default_database_id);
    EXPECT_EQ(tables.size(), static_cast<size_t>(N));

    // Verify all tables are retrievable by name.
    for (int i = 0; i < N; ++i) {
        auto get = catalog.get_table(default_database_id, "t" + std::to_string(i));
        ASSERT_TRUE(get.has_value()) << "Failed to get table t" << i;
    }
}

TEST(QA_Catalog, StressCreateAndDropManyTables) {
    Catalog catalog;
    constexpr int N = 200;

    // Create N tables.
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(
            catalog.create_table(default_database_id, make_test_schema("s" + std::to_string(i)))
                .has_value());
    }

    // Drop all.
    for (int i = 0; i < N; ++i) {
        auto drop = catalog.drop_table(default_database_id, "s" + std::to_string(i));
        ASSERT_TRUE(drop.has_value()) << "Drop failed for s" << i;
    }

    auto tables = catalog.list_tables(default_database_id);
    EXPECT_TRUE(tables.empty());
}

// =============================================================================
// Catalog: Very long names
// =============================================================================

TEST(QA_Catalog, CreateTableVeryLongName) {
    Catalog catalog;
    std::string long_name(10000, 'x');

    auto result = catalog.create_table(default_database_id, make_test_schema(long_name));
    // Should succeed (no length limit enforced).
    if (result.has_value()) {
        auto get = catalog.get_table(default_database_id, long_name);
        ASSERT_TRUE(get.has_value());
        EXPECT_EQ(get->name, long_name);
    }
}

// =============================================================================
// Catalog: get_table_by_id returns correct database-scoped table
// =============================================================================

TEST(QA_Catalog, GetTableByIdCrossDatabase) {
    Catalog catalog;
    auto db1 = catalog.create_database("db1");
    auto db2 = catalog.create_database("db2");
    ASSERT_TRUE(db1.has_value());
    ASSERT_TRUE(db2.has_value());

    auto t1 = catalog.create_table(*db1, make_test_schema("shared"));
    auto t2 = catalog.create_table(*db2, make_test_schema("shared"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    // get_table_by_id should return the correct table regardless of database.
    auto s1 = catalog.get_table_by_id(*t1);
    auto s2 = catalog.get_table_by_id(*t2);
    ASSERT_TRUE(s1.has_value());
    ASSERT_TRUE(s2.has_value());
    EXPECT_EQ(s1->table_id, *t1);
    EXPECT_EQ(s2->table_id, *t2);
    EXPECT_NE(s1->table_id, s2->table_id);
}

// =============================================================================
// Catalog: drop_table in non-existent database
// =============================================================================

TEST(QA_Catalog, DropTableNonexistentDatabase) {
    Catalog catalog;
    auto result = catalog.drop_table(9999, "anything");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// =============================================================================
// Catalog: System tables protection
// =============================================================================

TEST(QA_Catalog, CannotDropSystemDatabase) {
    Catalog catalog;
    auto drop = catalog.drop_database(system_database_id, false);
    ASSERT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST(QA_Catalog, CannotDropSystemDatabaseWithCascade) {
    Catalog catalog;
    auto drop = catalog.drop_database(system_database_id, true);
    ASSERT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

TEST(QA_Catalog, CannotDropSystemTables) {
    Catalog catalog;

    // Try to create and then drop a table in the system database.
    auto tid = catalog.create_table(system_database_id, make_test_schema("sys_test"));
    ASSERT_TRUE(tid.has_value()) << tid.error().message;

    auto drop = catalog.drop_table(system_database_id, "sys_test");
    ASSERT_FALSE(drop.has_value());
    EXPECT_EQ(drop.error().code, StatusCode::CONSTRAINT_VIOLATION);
}

// =============================================================================
// Catalog: Embedding provider operations
// =============================================================================

TEST(QA_Catalog, RegisterEmbeddingProviderEmptyName) {
    Catalog catalog;

    EmbeddingProviderConfig config;
    config.name = "";
    config.type = "openai";
    config.base_url = "http://localhost";
    config.model = "test";

    auto result = catalog.register_embedding_provider(config);
    if (result.has_value()) {
        auto get = catalog.get_embedding_provider("");
        EXPECT_TRUE(get.has_value());
    }
}

TEST(QA_Catalog, RemoveEmbeddingProviderNotFound) {
    Catalog catalog;
    auto result = catalog.remove_embedding_provider("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_Catalog, RemoveEmbeddingProviderTwice) {
    Catalog catalog;

    EmbeddingProviderConfig config;
    config.name = "test_provider";
    config.type = "openai";
    ASSERT_TRUE(catalog.register_embedding_provider(config).has_value());

    auto drop1 = catalog.remove_embedding_provider("test_provider");
    ASSERT_TRUE(drop1.has_value());

    auto drop2 = catalog.remove_embedding_provider("test_provider");
    ASSERT_FALSE(drop2.has_value());
    EXPECT_EQ(drop2.error().code, StatusCode::NOT_FOUND);
}

TEST(QA_Catalog, RegisterDuplicateEmbeddingProvider) {
    Catalog catalog;

    EmbeddingProviderConfig config;
    config.name = "dup_provider";
    config.type = "openai";
    ASSERT_TRUE(catalog.register_embedding_provider(config).has_value());

    auto dup = catalog.register_embedding_provider(config);
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(QA_Catalog, ListEmbeddingProvidersEmpty) {
    Catalog catalog;
    auto providers = catalog.list_embedding_providers();
    EXPECT_TRUE(providers.empty());
}

// =============================================================================
// Catalog: Thread safety smoke test
// =============================================================================

TEST(QA_Catalog, ConcurrentCreateTables) {
    Catalog catalog;
    constexpr int threads_count = 8;
    constexpr int tables_per_thread = 50;

    std::vector<std::thread> threads;
    threads.reserve(threads_count);
    for (int t = 0; t < threads_count; ++t) {
        threads.emplace_back([&catalog, t]() {
            for (int i = 0; i < tables_per_thread; ++i) {
                std::string name = "t_" + std::to_string(t) + "_" + std::to_string(i);
                auto result = catalog.create_table(default_database_id, make_test_schema(name));
                EXPECT_TRUE(result.has_value())
                    << "Thread " << t << " table " << i << ": " << result.error().message;
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    auto tables = catalog.list_tables(default_database_id);
    EXPECT_EQ(tables.size(), static_cast<size_t>(threads_count * tables_per_thread));
}

// =============================================================================
// Catalog: Drop database cascade with edge types and embedding columns
// =============================================================================

TEST(QA_Catalog, DropDatabaseCascadeRemovesEdgeTypesAndEmbeddings) {
    Catalog catalog;

    auto db_id = catalog.create_database("complex_db");
    ASSERT_TRUE(db_id.has_value());

    auto t1 = catalog.create_table(*db_id, make_test_schema("src"));
    auto t2 = catalog.create_table(*db_id, make_test_schema("tgt"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    // Create edge type.
    EdgeTypeDef edge;
    edge.name = "cascade_edge";
    edge.source_table_id = *t1;
    edge.target_table_id = *t2;
    ASSERT_TRUE(catalog.create_edge_type(*db_id, edge).has_value());

    // Register embedding column.
    EmbeddingColumnDef emb;
    emb.table_id = *t1;
    emb.column_id = 0;
    emb.dimension = 128;
    ASSERT_TRUE(catalog.register_embedding_column(emb).has_value());

    // Cascade drop the database.
    auto drop = catalog.drop_database(*db_id, true);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // Edge type should be gone.
    EXPECT_TRUE(catalog.list_edge_types(*db_id).empty());
    EXPECT_FALSE(catalog.get_edge_type(*db_id, "cascade_edge").has_value());

    // Embedding columns should be gone.
    EXPECT_TRUE(catalog.list_all_embedding_columns().empty());

    // Tables should be gone.
    EXPECT_FALSE(catalog.get_table_by_id(*t1).has_value());
    EXPECT_FALSE(catalog.get_table_by_id(*t2).has_value());
}

// =============================================================================
// Catalog: Recreate after drop preserves ID monotonicity
// =============================================================================

TEST(QA_Catalog, RecreateAfterDropPreservesIdMonotonicity) {
    Catalog catalog;

    auto id1 = catalog.create_table(default_database_id, make_test_schema("t1"));
    ASSERT_TRUE(id1.has_value());

    auto id2 = catalog.create_table(default_database_id, make_test_schema("t2"));
    ASSERT_TRUE(id2.has_value());

    // Drop t1 and recreate.
    ASSERT_TRUE(catalog.drop_table(default_database_id, "t1").has_value());
    auto id3 = catalog.create_table(default_database_id, make_test_schema("t1"));
    ASSERT_TRUE(id3.has_value());

    // New ID should be greater than any previous ID.
    EXPECT_GT(*id3, *id2);
}

// =============================================================================
// QA Adversarial Tests for GDB-18/GDB-100: TableHeap
// =============================================================================

class QA_TableHeapTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb18_heap.db";
        std::filesystem::remove(path_);

        auto fid = dm_.create_file(path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        file_id_ = *fid;

        bpm_ = std::make_unique<BufferPoolManager>(dm_, file_id_, 64);
    }

    void TearDown() override {
        bpm_.reset();
        auto close = dm_.close_file(file_id_);
        (void)close;
        std::filesystem::remove(path_);
    }

    static std::vector<uint8_t> make_tuple(size_t len, uint8_t fill = 0xAA) {
        return std::vector<uint8_t>(len, fill);
    }

    std::filesystem::path path_;
    DiskManager dm_;
    FileId file_id_ = 0;
    std::unique_ptr<BufferPoolManager> bpm_;
};

// -- Minimum-size tuple -------------------------------------------------------

TEST_F(QA_TableHeapTest, InsertOneByteTuple) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto rid = heap.insert_tuple(make_tuple(1, 0xFF));
    ASSERT_TRUE(rid.has_value()) << rid.error().message;

    auto get = heap.get_tuple(*rid);
    ASSERT_TRUE(get.has_value()) << get.error().message;
    ASSERT_EQ(get->size(), 1u);
    EXPECT_EQ((*get)[0], 0xFF);
}

// -- Maximum tuple that fits a page -------------------------------------------

TEST_F(QA_TableHeapTest, InsertMaxSizeTuple) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Page is 8192 bytes, header 24 bytes, each slot entry 4 bytes.
    // Max tuple = 8192 - 24 - 4 = 8164 bytes.
    auto rid = heap.insert_tuple(make_tuple(8164, 0xBB));
    ASSERT_TRUE(rid.has_value()) << rid.error().message;

    auto get = heap.get_tuple(*rid);
    ASSERT_TRUE(get.has_value()) << get.error().message;
    EXPECT_EQ(get->size(), 8164u);
    EXPECT_EQ((*get)[0], 0xBB);
}

TEST_F(QA_TableHeapTest, InsertOversizedTupleFails) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // One byte over max should fail.
    auto result = heap.insert_tuple(make_tuple(8165, 0xCC));
    EXPECT_FALSE(result.has_value());
}

// -- Double delete ------------------------------------------------------------

TEST_F(QA_TableHeapTest, DeleteTupleTwice) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto rid = heap.insert_tuple(make_tuple(50, 0x11));
    ASSERT_TRUE(rid.has_value());

    auto del1 = heap.delete_tuple(*rid);
    ASSERT_TRUE(del1.has_value());

    // Second delete should fail (already deleted).
    auto del2 = heap.delete_tuple(*rid);
    EXPECT_FALSE(del2.has_value());
}

// -- Update deleted tuple -----------------------------------------------------

TEST_F(QA_TableHeapTest, UpdateDeletedTupleFails) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto rid = heap.insert_tuple(make_tuple(50, 0x11));
    ASSERT_TRUE(rid.has_value());

    ASSERT_TRUE(heap.delete_tuple(*rid).has_value());

    auto upd = heap.update_tuple(*rid, make_tuple(50, 0x22));
    EXPECT_FALSE(upd.has_value());
}

// -- Get deleted tuple --------------------------------------------------------

TEST_F(QA_TableHeapTest, GetDeletedTupleFails) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto rid = heap.insert_tuple(make_tuple(50, 0x11));
    ASSERT_TRUE(rid.has_value());

    ASSERT_TRUE(heap.delete_tuple(*rid).has_value());

    auto get = heap.get_tuple(*rid);
    ASSERT_FALSE(get.has_value());
    EXPECT_EQ(get.error().code, StatusCode::NOT_FOUND);
}

// -- Sequential scan: all deleted ---------------------------------------------

TEST_F(QA_TableHeapTest, SequentialScanAllDeleted) {
    TableHeap heap(*bpm_, dm_, file_id_);

    std::vector<RID> rids;
    for (int i = 0; i < 10; ++i) {
        auto r = heap.insert_tuple(make_tuple(50, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value());
        rids.push_back(*r);
    }

    for (auto& rid : rids) {
        ASSERT_TRUE(heap.delete_tuple(rid).has_value());
    }

    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value());
    auto it = std::move(*it_result);

    int count = 0;
    while (it.next()) {
        count++;
    }
    EXPECT_EQ(count, 0);
}

// -- Sequential scan: multiple calls to next() after exhaustion ---------------

TEST_F(QA_TableHeapTest, SequentialScanMultipleCallsAfterExhaustion) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto r = heap.insert_tuple(make_tuple(50, 0x01));
    ASSERT_TRUE(r.has_value());

    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value());
    auto it = std::move(*it_result);

    auto r1 = it.next();
    EXPECT_TRUE(r1.has_value());

    auto r2 = it.next();
    EXPECT_FALSE(r2.has_value());

    // Calling next() again after exhaustion should be safe.
    auto r3 = it.next();
    EXPECT_FALSE(r3.has_value());

    auto r4 = it.next();
    EXPECT_FALSE(r4.has_value());
}

// -- Insert, delete all, insert again -----------------------------------------

TEST_F(QA_TableHeapTest, InsertDeleteAllInsertAgain) {
    TableHeap heap(*bpm_, dm_, file_id_);

    std::vector<RID> rids;
    for (int i = 0; i < 5; ++i) {
        auto r = heap.insert_tuple(make_tuple(100, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value());
        rids.push_back(*r);
    }

    for (auto& rid : rids) {
        ASSERT_TRUE(heap.delete_tuple(rid).has_value());
    }

    // Insert new tuples into the space freed by deletions.
    std::vector<RID> new_rids;
    for (int i = 0; i < 5; ++i) {
        auto r = heap.insert_tuple(make_tuple(100, static_cast<uint8_t>(0x80 + i)));
        ASSERT_TRUE(r.has_value()) << "Re-insert " << i << " failed: " << r.error().message;
        new_rids.push_back(*r);
    }

    // Verify new tuples are readable.
    for (int i = 0; i < 5; ++i) {
        auto get = heap.get_tuple(new_rids[static_cast<size_t>(i)]);
        ASSERT_TRUE(get.has_value());
        EXPECT_EQ((*get)[0], static_cast<uint8_t>(0x80 + i));
    }

    // Sequential scan should return exactly 5.
    auto it = heap.begin();
    ASSERT_TRUE(it.has_value());
    auto iter = std::move(*it);
    int count = 0;
    while (iter.next()) {
        count++;
    }
    EXPECT_EQ(count, 5);
}

// -- Stress: many small inserts -----------------------------------------------

TEST_F(QA_TableHeapTest, StressManySmallInserts) {
    TableHeap heap(*bpm_, dm_, file_id_);
    constexpr int N = 1000;

    std::vector<RID> rids;
    rids.reserve(N);
    for (int i = 0; i < N; ++i) {
        auto r = heap.insert_tuple(make_tuple(20, static_cast<uint8_t>(i & 0xFF)));
        ASSERT_TRUE(r.has_value()) << "Insert " << i << " failed: " << r.error().message;
        rids.push_back(*r);
    }

    // Verify all via sequential scan.
    auto it_result = heap.begin();
    ASSERT_TRUE(it_result.has_value());
    auto it = std::move(*it_result);
    int count = 0;
    while (it.next()) {
        count++;
    }
    EXPECT_EQ(count, N);
}

// -- Update triggering compaction ---------------------------------------------

TEST_F(QA_TableHeapTest, UpdateTriggersCompaction) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Fill a page with several tuples.
    std::vector<RID> rids;
    for (int i = 0; i < 20; ++i) {
        auto r = heap.insert_tuple(make_tuple(300, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value());
        rids.push_back(*r);
    }

    // Delete several tuples to create gaps.
    for (int i = 0; i < 15; ++i) {
        ASSERT_TRUE(heap.delete_tuple(rids[static_cast<size_t>(i)]).has_value());
    }

    // Update a remaining tuple to be larger — should trigger compaction.
    auto upd = heap.update_tuple(rids[15], make_tuple(600, 0xDD));
    ASSERT_TRUE(upd.has_value()) << upd.error().message;

    auto get = heap.get_tuple(rids[15]);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ(get->size(), 600u);
    EXPECT_EQ((*get)[0], 0xDD);
}

// -- Update same-size (no growth, no shrink) ----------------------------------

TEST_F(QA_TableHeapTest, UpdateSameSize) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto rid = heap.insert_tuple(make_tuple(100, 0x11));
    ASSERT_TRUE(rid.has_value());

    auto upd = heap.update_tuple(*rid, make_tuple(100, 0x22));
    ASSERT_TRUE(upd.has_value());

    auto get = heap.get_tuple(*rid);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ(get->size(), 100u);
    EXPECT_EQ((*get)[0], 0x22);
}

// -- Interleaved insert and delete operations ---------------------------------

TEST_F(QA_TableHeapTest, InterleavedInsertDelete) {
    TableHeap heap(*bpm_, dm_, file_id_);

    // Insert 10, delete odd-numbered, insert 5 more.
    std::vector<RID> rids;
    for (int i = 0; i < 10; ++i) {
        auto r = heap.insert_tuple(make_tuple(80, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value());
        rids.push_back(*r);
    }

    for (size_t i = 1; i < rids.size(); i += 2) {
        ASSERT_TRUE(heap.delete_tuple(rids[i]).has_value());
    }

    for (int i = 10; i < 15; ++i) {
        auto r = heap.insert_tuple(make_tuple(80, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value());
        rids.push_back(*r);
    }

    // Count via scan: 5 remaining from first batch + 5 new = 10.
    auto it = heap.begin();
    ASSERT_TRUE(it.has_value());
    auto iter = std::move(*it);
    int count = 0;
    while (iter.next()) {
        count++;
    }
    EXPECT_EQ(count, 10);
}

// -- Different tuple sizes on same page ---------------------------------------

TEST_F(QA_TableHeapTest, VariousTupleSizesOnSamePage) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto r1 = heap.insert_tuple(make_tuple(1, 0x01));
    auto r2 = heap.insert_tuple(make_tuple(100, 0x02));
    auto r3 = heap.insert_tuple(make_tuple(500, 0x03));
    auto r4 = heap.insert_tuple(make_tuple(2000, 0x04));
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r3.has_value());
    ASSERT_TRUE(r4.has_value());

    auto g1 = heap.get_tuple(*r1);
    auto g2 = heap.get_tuple(*r2);
    auto g3 = heap.get_tuple(*r3);
    auto g4 = heap.get_tuple(*r4);
    ASSERT_TRUE(g1.has_value());
    ASSERT_TRUE(g2.has_value());
    ASSERT_TRUE(g3.has_value());
    ASSERT_TRUE(g4.has_value());

    EXPECT_EQ(g1->size(), 1u);
    EXPECT_EQ(g2->size(), 100u);
    EXPECT_EQ(g3->size(), 500u);
    EXPECT_EQ(g4->size(), 2000u);
}

// -- page_count reflects actual data pages ------------------------------------

TEST_F(QA_TableHeapTest, PageCountReflectsInserts) {
    TableHeap heap(*bpm_, dm_, file_id_);

    auto pc_before = heap.page_count();
    ASSERT_TRUE(pc_before.has_value());

    // Insert max-size tuples to force new page allocations.
    for (int i = 0; i < 5; ++i) {
        auto r = heap.insert_tuple(make_tuple(8164, static_cast<uint8_t>(i)));
        ASSERT_TRUE(r.has_value());
    }

    auto pc_after = heap.page_count();
    ASSERT_TRUE(pc_after.has_value());
    // Each max-size tuple takes an entire page, so we need at least 5 pages.
    EXPECT_GE(*pc_after, 5u);
}
