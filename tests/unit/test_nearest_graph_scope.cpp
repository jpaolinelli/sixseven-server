/// @file test_nearest_graph_scope.cpp
/// @brief Unit tests for NEAREST ... WITHIN TRAVERSE with non-INT64 primary keys.
///
/// Regression tests for GDB-597: Graph-scoped NEAREST query crashes the server
/// when the table uses UUID primary keys. The crash was caused by the planner
/// calling Value::as_int64() on UUID values in the WITHIN TRAVERSE code path.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/uuid.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"
#include "sixseven/vector/embedding_column.h"
#include "sixseven/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class NearestGraphScopeTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_gdb597";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());
        provider_registry_ = std::make_unique<ProviderRegistry>(catalog_);
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        provider_registry_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return std::move(*result);
    }

    void exec_error(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value());
    }

    /// Register embedding metadata so NEAREST text queries work.
    void register_embedding(table_id_t table_id,
                            int32_t col_id,
                            int32_t dim,
                            const std::string& source,
                            const std::string& provider) {
        EmbeddingColumnDef emb_def;
        emb_def.table_id = table_id;
        emb_def.column_id = col_id;
        emb_def.dimension = dim;
        emb_def.source_expr = source;
        emb_def.provider = provider;
        auto reg = catalog_.register_embedding_column(emb_def);
        ASSERT_TRUE(reg.has_value()) << reg.error().message;

        // Only register the provider once.
        auto existing = catalog_.get_embedding_provider(provider);
        if (existing.has_value()) {
            return;
        }
        EmbeddingProviderConfig prov_config;
        prov_config.name = provider;
        prov_config.type = "builtin";
        prov_config.dimension = dim;
        auto prov_reg = catalog_.register_embedding_provider(prov_config);
        ASSERT_TRUE(prov_reg.has_value()) << prov_reg.error().message;
    }

    /// Insert a row with a pre-computed embedding directly into the heap.
    void insert_row_uuid(const std::string& table_name,
                         const std::string& uuid,
                         const std::string& text,
                         const Embedding& emb) {
        auto schema = catalog_.get_table(default_database_id, table_name);
        ASSERT_TRUE(schema.has_value());

        auto ts = storage_->get_table_storage(schema->table_id);
        ASSERT_TRUE(ts.has_value());
        auto* table_storage = *ts;

        auto parsed_uuid = parse_uuid(uuid);
        ASSERT_TRUE(parsed_uuid.has_value()) << parsed_uuid.error().message;

        std::vector<Value> values;
        values.push_back(Value(*parsed_uuid));
        values.push_back(Value(text));
        values.push_back(Value(emb));

        auto serialized = TupleSerializer::serialize(values, table_storage->storage_schema);
        ASSERT_TRUE(serialized.has_value()) << serialized.error().message;

        auto rid = table_storage->heap->insert_tuple(*serialized);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<ProviderRegistry> provider_registry_;
};

// =============================================================================
// GDB-597: NEAREST ... WITHIN TRAVERSE with UUID PKs must not crash
// =============================================================================

TEST_F(NearestGraphScopeTest, UuidPrimaryKeysDoNotCrash) {
    // Create a table with UUID PKs and an EMBEDDING column.
    exec_ok("CREATE TABLE posts (id UUID PRIMARY KEY, body VARCHAR, body_vec EMBEDDING)");

    auto schema = catalog_.get_table(default_database_id, "posts");
    ASSERT_TRUE(schema.has_value());

    // Register embedding metadata.
    register_embedding(schema->table_id, 2, 4, "body", "builtin/4");
    engine_->set_provider_registry(provider_registry_.get());

    // Insert rows with pre-computed embeddings.
    // UUIDs chosen to be distinct and easily identifiable.
    std::string uuid_a = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
    std::string uuid_b = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
    std::string uuid_c = "cccccccc-cccc-cccc-cccc-cccccccccccc";
    std::string uuid_d = "dddddddd-dddd-dddd-dddd-dddddddddddd";

    insert_row_uuid("posts", uuid_a, "alpha", {1.0F, 0.0F, 0.0F, 0.0F});
    insert_row_uuid("posts", uuid_b, "beta", {0.9F, 0.1F, 0.0F, 0.0F});
    insert_row_uuid("posts", uuid_c, "gamma", {0.0F, 1.0F, 0.0F, 0.0F});
    insert_row_uuid("posts", uuid_d, "delta", {0.0F, 0.0F, 1.0F, 0.0F});

    // Create an edge type and link some nodes.
    // Graph: A -> B -> C (D is not reachable from A via "follows")
    exec_ok("CREATE EDGE TYPE follows FROM posts TO posts");
    exec_ok("LINK posts('" + uuid_a + "') TO posts('" + uuid_b + "') VIA follows");
    exec_ok("LINK posts('" + uuid_b + "') TO posts('" + uuid_c + "') VIA follows");

    // Run NEAREST ... WITHIN TRAVERSE — this must not crash.
    auto result = engine_->execute("NEAREST 5 FROM posts.body_vec TO [1.0, 0.0, 0.0, 0.0] "
                                   "WITHIN TRAVERSE follows FROM posts('" +
                                   uuid_a + "') DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Should return results only from the reachable set {A, B, C}, not D.
    // A is the start node (included), B is depth 1, C is depth 2.
    // D is not reachable, so it must be excluded.
    EXPECT_LE(result->rows.size(), 3u);
    EXPECT_GE(result->rows.size(), 1u);

    // Verify that uuid_d is NOT in the results.
    auto parsed_d = parse_uuid(uuid_d);
    ASSERT_TRUE(parsed_d.has_value());
    for (const auto& row : result->rows) {
        ASSERT_FALSE(row.empty());
        if (row[0].type_id() == TypeId::UUID) {
            EXPECT_NE(row[0].as_uuid(), *parsed_d);
        }
    }
}

TEST_F(NearestGraphScopeTest, IntPrimaryKeysStillWork) {
    // Verify INT PK tables (the original working case) are not regressed.
    exec_ok("CREATE TABLE docs (id INT PRIMARY KEY, body VARCHAR, body_vec EMBEDDING)");

    auto schema = catalog_.get_table(default_database_id, "docs");
    ASSERT_TRUE(schema.has_value());

    register_embedding(schema->table_id, 2, 4, "body", "builtin/4");
    engine_->set_provider_registry(provider_registry_.get());

    // Insert rows via SQL.
    exec_ok("INSERT INTO docs VALUES (1, 'alpha', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (2, 'beta', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (3, 'gamma', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (4, 'delta', [0.0, 0.0, 1.0, 0.0])");

    // Create edges: 1 -> 2 -> 3 (4 unreachable).
    exec_ok("CREATE EDGE TYPE refs FROM docs TO docs");
    exec_ok("LINK docs(1) TO docs(2) VIA refs");
    exec_ok("LINK docs(2) TO docs(3) VIA refs");

    // NEAREST scoped to graph.
    auto result = engine_->execute("NEAREST 5 FROM docs.body_vec TO [1.0, 0.0, 0.0, 0.0] "
                                   "WITHIN TRAVERSE refs FROM docs(1) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_LE(result->rows.size(), 3u);
    EXPECT_GE(result->rows.size(), 1u);

    // Verify id=4 is not in the results.
    for (const auto& row : result->rows) {
        if (!row.empty() && row[0].type_id() == TypeId::INT32) {
            EXPECT_NE(row[0].as_int32(), 4);
        }
    }
}
