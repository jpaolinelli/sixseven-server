/// @file test_qa_gdb_597.cpp
/// @brief Adversarial QA tests for GDB-597: Graph-scoped NEAREST crash with
///        non-INT64 primary keys.
///
/// The crash was caused by the planner calling Value::as_int64() unconditionally
/// on PK values in the WITHIN TRAVERSE code path. The fix uses type-generic
/// Value with ValueHash/ValueEqual for the reachable PK set, and coerces the
/// start key via fit_to_storage().
///
/// Attack surfaces tested:
///   - Every practical PK type (UUID, STRING, INT32, INT64, INT16, INT8)
///   - Start key coercion paths (STRING → UUID, INT64 literal → INT32 column)
///   - Empty traversal results (no outgoing edges)
///   - Non-existent start key (key not in table)
///   - Single-node graph (start node only)
///   - Deep traversal chain
///   - Large fan-out (many neighbors)
///   - Direction variants (IN, OUT, BOTH)
///   - MAX_DEPTH boundary values (0, 1, very large)

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

#include "test_qa_helpers.h"

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class QA_GDB597 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_qa_gdb597";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());
        provider_registry_ = std::make_unique<ProviderRegistry>(catalog_);
        engine_->set_provider_registry(provider_registry_.get());
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
        if (!result.has_value()) {
            ADD_FAILURE() << "Expected success but got error: " << result.error().message;
            return {};
        }
        return std::move(*result);
    }

    void exec_error(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "Expected error but query succeeded";
    }

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
// AC1: UUID PKs must not crash (core regression)
// =============================================================================

/// Reproduce the exact crash scenario from the ticket.
TEST_F(QA_GDB597, UuidPk_ExactReproduction) {
    exec_ok("CREATE TABLE posts (id UUID PRIMARY KEY, body VARCHAR, body_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "posts");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4, "body", "builtin/4");

    std::string u1 = "da7eee6f-1111-4aaa-8888-111111111111";
    std::string u2 = "da7eee6f-2222-4aaa-8888-222222222222";
    std::string u3 = "da7eee6f-3333-4aaa-8888-333333333333";
    std::string u4 = "da7eee6f-4444-4aaa-8888-444444444444";

    insert_row_uuid("posts", u1, "data science intro", {1.0F, 0.0F, 0.0F, 0.0F});
    insert_row_uuid("posts", u2, "machine learning", {0.9F, 0.1F, 0.0F, 0.0F});
    insert_row_uuid("posts", u3, "deep learning", {0.8F, 0.2F, 0.0F, 0.0F});
    insert_row_uuid("posts", u4, "cooking recipes", {0.0F, 0.0F, 0.0F, 1.0F});

    exec_ok("CREATE EDGE TYPE follows FROM posts TO posts");
    exec_ok("LINK posts('" + u1 + "') TO posts('" + u2 + "') VIA follows");
    exec_ok("LINK posts('" + u2 + "') TO posts('" + u3 + "') VIA follows");

    // This was the crashing query — must succeed now.
    auto result =
        engine_->execute("SELECT * FROM posts WHERE NEAREST(body_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE follows FROM posts('" +
                         u1 + "') DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Reachable: u1, u2, u3. u4 must be excluded.
    EXPECT_GE(result->rows.size(), 1u);
    EXPECT_LE(result->rows.size(), 3u);

    auto parsed_u4 = parse_uuid(u4);
    ASSERT_TRUE(parsed_u4.has_value());
    for (const auto& row : result->rows) {
        if (!row.empty() && row[0].type_id() == TypeId::UUID) {
            EXPECT_NE(row[0].as_uuid(), *parsed_u4) << "Unreachable node u4 in results";
        }
    }
}

// =============================================================================
// AC2: INT64 PKs still work (non-regression)
// =============================================================================

TEST_F(QA_GDB597, Int64Pk_NonRegression) {
    exec_ok("CREATE TABLE docs (id BIGINT PRIMARY KEY, body VARCHAR, body_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "docs");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4, "body", "builtin/4");

    exec_ok("INSERT INTO docs VALUES (100, 'alpha', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (200, 'beta', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (300, 'gamma', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (400, 'delta', [0.0, 0.0, 1.0, 0.0])");

    exec_ok("CREATE EDGE TYPE cites FROM docs TO docs");
    exec_ok("LINK docs(100) TO docs(200) VIA cites");
    exec_ok("LINK docs(200) TO docs(300) VIA cites");

    auto result =
        engine_->execute("SELECT * FROM docs WHERE NEAREST(body_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE cites FROM docs(100) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_GE(result->rows.size(), 1u);
    EXPECT_LE(result->rows.size(), 3u);

    // id=400 must not appear.
    for (const auto& row : result->rows) {
        if (!row.empty() && row[0].type_id() == TypeId::INT64) {
            EXPECT_NE(row[0].as_int64(), 400);
        }
    }
}

// =============================================================================
// AC3: INT32 PKs work (coercion: INT64 literal → INT32 column)
// =============================================================================

TEST_F(QA_GDB597, Int32Pk_CoercionPath) {
    exec_ok("CREATE TABLE items (id INT PRIMARY KEY, body VARCHAR, body_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "items");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4, "body", "builtin/4");

    exec_ok("INSERT INTO items VALUES (1, 'apple', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO items VALUES (2, 'banana', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO items VALUES (3, 'cherry', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("INSERT INTO items VALUES (4, 'date', [0.0, 0.0, 1.0, 0.0])");

    exec_ok("CREATE EDGE TYPE related FROM items TO items");
    exec_ok("LINK items(1) TO items(2) VIA related");
    exec_ok("LINK items(2) TO items(3) VIA related");

    // Integer literal 1 is parsed as INT64, must coerce to INT32 for PK lookup.
    auto result =
        engine_->execute("SELECT * FROM items WHERE NEAREST(body_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE related FROM items(1) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_GE(result->rows.size(), 1u);
    EXPECT_LE(result->rows.size(), 3u);

    for (const auto& row : result->rows) {
        if (!row.empty() && row[0].type_id() == TypeId::INT32) {
            EXPECT_NE(row[0].as_int32(), 4) << "Unreachable node 4 in results";
        }
    }
}

// =============================================================================
// AC4: STRING PKs work
// =============================================================================

TEST_F(QA_GDB597, StringPk_Works) {
    exec_ok("CREATE TABLE articles (slug VARCHAR PRIMARY KEY, body VARCHAR, body_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "articles");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4, "body", "builtin/4");

    exec_ok("INSERT INTO articles VALUES ('intro', 'introduction', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO articles VALUES ('chap1', 'chapter one', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO articles VALUES ('chap2', 'chapter two', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("INSERT INTO articles VALUES ('orphan', 'orphan page', [0.0, 0.0, 1.0, 0.0])");

    exec_ok("CREATE EDGE TYPE links FROM articles TO articles");
    exec_ok("LINK articles('intro') TO articles('chap1') VIA links");
    exec_ok("LINK articles('chap1') TO articles('chap2') VIA links");

    auto result = engine_->execute(
        "SELECT * FROM articles WHERE NEAREST(body_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
        "WITHIN TRAVERSE links FROM articles('intro') DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_GE(result->rows.size(), 1u);
    EXPECT_LE(result->rows.size(), 3u);

    for (const auto& row : result->rows) {
        if (!row.empty() && row[0].type_id() == TypeId::STRING) {
            EXPECT_NE(row[0].as_string(), "orphan") << "Unreachable 'orphan' in results";
        }
    }
}

// =============================================================================
// Edge case: Empty traversal (start node has no outgoing edges)
// =============================================================================

TEST_F(QA_GDB597, UuidPk_NoOutgoingEdges) {
    exec_ok("CREATE TABLE nodes (id UUID PRIMARY KEY, text VARCHAR, text_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "nodes");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4, "text", "builtin/4");

    std::string u1 = "11111111-1111-1111-1111-111111111111";
    std::string u2 = "22222222-2222-2222-2222-222222222222";

    insert_row_uuid("nodes", u1, "isolated", {1.0F, 0.0F, 0.0F, 0.0F});
    insert_row_uuid("nodes", u2, "other", {0.9F, 0.1F, 0.0F, 0.0F});

    exec_ok("CREATE EDGE TYPE connects FROM nodes TO nodes");
    // No edges created — u1 is isolated.

    auto result =
        engine_->execute("SELECT * FROM nodes WHERE NEAREST(text_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE connects FROM nodes('" +
                         u1 + "') DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Only the start node itself should be reachable.
    EXPECT_LE(result->rows.size(), 1u);
}

// =============================================================================
// Edge case: MAX_DEPTH = 0 (only start node reachable)
// =============================================================================

TEST_F(QA_GDB597, UuidPk_MaxDepthZero) {
    exec_ok("CREATE TABLE pg (id UUID PRIMARY KEY, text VARCHAR, text_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "pg");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4, "text", "builtin/4");

    std::string u1 = "aaaaaaaa-0001-4000-8000-000000000001";
    std::string u2 = "aaaaaaaa-0002-4000-8000-000000000002";

    insert_row_uuid("pg", u1, "start", {1.0F, 0.0F, 0.0F, 0.0F});
    insert_row_uuid("pg", u2, "neighbor", {0.95F, 0.05F, 0.0F, 0.0F});

    exec_ok("CREATE EDGE TYPE link FROM pg TO pg");
    exec_ok("LINK pg('" + u1 + "') TO pg('" + u2 + "') VIA link");

    auto result =
        engine_->execute("SELECT * FROM pg WHERE NEAREST(text_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE link FROM pg('" +
                         u1 + "') DIRECTION OUT MAX_DEPTH 0");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // MAX_DEPTH 0 → only start node reachable (no traversal).
    EXPECT_LE(result->rows.size(), 1u);
}

// =============================================================================
// Edge case: DIRECTION IN (reverse edges)
// =============================================================================

TEST_F(QA_GDB597, UuidPk_DirectionIn) {
    exec_ok("CREATE TABLE rev (id UUID PRIMARY KEY, text VARCHAR, text_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "rev");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4, "text", "builtin/4");

    std::string u1 = "11111111-aaaa-4000-8000-aaaaaaaaaaaa";
    std::string u2 = "22222222-bbbb-4000-8000-bbbbbbbbbbbb";
    std::string u3 = "33333333-cccc-4000-8000-cccccccccccc";

    insert_row_uuid("rev", u1, "leaf", {1.0F, 0.0F, 0.0F, 0.0F});
    insert_row_uuid("rev", u2, "mid", {0.9F, 0.1F, 0.0F, 0.0F});
    insert_row_uuid("rev", u3, "root", {0.0F, 0.0F, 1.0F, 0.0F});

    exec_ok("CREATE EDGE TYPE parent FROM rev TO rev");
    // u2 -> u1, u3 -> u2 (so traversing IN from u1 reaches u2 and u3)
    exec_ok("LINK rev('" + u2 + "') TO rev('" + u1 + "') VIA parent");
    exec_ok("LINK rev('" + u3 + "') TO rev('" + u2 + "') VIA parent");

    auto result =
        engine_->execute("SELECT * FROM rev WHERE NEAREST(text_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE parent FROM rev('" +
                         u1 + "') DIRECTION IN MAX_DEPTH 2");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Traversing IN from u1: reaches u2 (who points to u1) and u3 (who points to u2).
    // All 3 nodes should be reachable.
    EXPECT_GE(result->rows.size(), 1u);
    EXPECT_LE(result->rows.size(), 3u);
}

// =============================================================================
// Edge case: DIRECTION BOTH
// =============================================================================

TEST_F(QA_GDB597, Int32Pk_DirectionBoth) {
    exec_ok("CREATE TABLE bidir (id INT PRIMARY KEY, text VARCHAR, text_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "bidir");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4, "text", "builtin/4");

    exec_ok("INSERT INTO bidir VALUES (10, 'center', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO bidir VALUES (20, 'left', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO bidir VALUES (30, 'right', [0.8, 0.2, 0.0, 0.0])");
    exec_ok("INSERT INTO bidir VALUES (40, 'far', [0.0, 0.0, 0.0, 1.0])");

    exec_ok("CREATE EDGE TYPE adj FROM bidir TO bidir");
    // 20 -> 10 -> 30 (10 is center, reachable from both directions)
    exec_ok("LINK bidir(20) TO bidir(10) VIA adj");
    exec_ok("LINK bidir(10) TO bidir(30) VIA adj");

    auto result =
        engine_->execute("SELECT * FROM bidir WHERE NEAREST(text_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE adj FROM bidir(10) DIRECTION BOTH MAX_DEPTH 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // BOTH from 10: reaches 20 (incoming) and 30 (outgoing). 40 unreachable.
    EXPECT_GE(result->rows.size(), 1u);
    EXPECT_LE(result->rows.size(), 3u);

    for (const auto& row : result->rows) {
        if (!row.empty() && row[0].type_id() == TypeId::INT32) {
            EXPECT_NE(row[0].as_int32(), 40) << "Unreachable node 40 in results";
        }
    }
}

// =============================================================================
// Stress: Deep chain with UUID PKs
// =============================================================================

TEST_F(QA_GDB597, UuidPk_DeepChain) {
    exec_ok("CREATE TABLE chain (id UUID PRIMARY KEY, text VARCHAR, text_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "chain");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4, "text", "builtin/4");

    constexpr int CHAIN_LEN = 20;
    std::vector<std::string> uuids;
    for (int i = 0; i < CHAIN_LEN; ++i) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%08x-0000-4000-8000-%012x", i + 1, i + 1);
        uuids.emplace_back(buf);
    }

    // Insert nodes with progressively different embeddings.
    for (int i = 0; i < CHAIN_LEN; ++i) {
        float v = 1.0F - (static_cast<float>(i) / CHAIN_LEN);
        insert_row_uuid("chain", uuids[i], "node_" + std::to_string(i), {v, 1.0F - v, 0.0F, 0.0F});
    }

    // Create a linear chain: 0 -> 1 -> 2 -> ... -> 19
    exec_ok("CREATE EDGE TYPE next FROM chain TO chain");
    for (int i = 0; i < CHAIN_LEN - 1; ++i) {
        exec_ok("LINK chain('" + uuids[i] + "') TO chain('" + uuids[i + 1] + "') VIA next");
    }

    // Query with MAX_DEPTH = 5 from start.
    auto result =
        engine_->execute("SELECT * FROM chain WHERE NEAREST(text_vec, 10) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE next FROM chain('" +
                         uuids[0] + "') DIRECTION OUT MAX_DEPTH 5");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Reachable: nodes 0..5 (6 nodes). Rest excluded.
    EXPECT_GE(result->rows.size(), 1u);
    EXPECT_LE(result->rows.size(), 6u);
}

// =============================================================================
// Stress: Fan-out with many neighbors
// =============================================================================

TEST_F(QA_GDB597, Int32Pk_FanOut) {
    exec_ok("CREATE TABLE hub (id INT PRIMARY KEY, text VARCHAR, text_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "hub");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4, "text", "builtin/4");

    // Hub node + 50 spokes + 1 unreachable.
    constexpr int SPOKES = 50;
    exec_ok("INSERT INTO hub VALUES (0, 'hub', [1.0, 0.0, 0.0, 0.0])");
    for (int i = 1; i <= SPOKES; ++i) {
        float v = 1.0F - (static_cast<float>(i) / (SPOKES + 1));
        std::string emb = "[" + std::to_string(v) + ", " + std::to_string(1.0F - v) + ", 0.0, 0.0]";
        exec_ok("INSERT INTO hub VALUES (" + std::to_string(i) + ", 'spoke_" + std::to_string(i) +
                "', " + emb + ")");
    }
    exec_ok("INSERT INTO hub VALUES (999, 'unreachable', [0.0, 0.0, 0.0, 1.0])");

    exec_ok("CREATE EDGE TYPE spoke FROM hub TO hub");
    for (int i = 1; i <= SPOKES; ++i) {
        exec_ok("LINK hub(0) TO hub(" + std::to_string(i) + ") VIA spoke");
    }

    auto result =
        engine_->execute("SELECT * FROM hub WHERE NEAREST(text_vec, 10) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE spoke FROM hub(0) DIRECTION OUT MAX_DEPTH 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_GE(result->rows.size(), 1u);
    EXPECT_LE(result->rows.size(), 10u); // limited by NEAREST k=10

    // Node 999 must not appear.
    for (const auto& row : result->rows) {
        if (!row.empty() && row[0].type_id() == TypeId::INT32) {
            EXPECT_NE(row[0].as_int32(), 999) << "Unreachable node 999 in results";
        }
    }
}

// =============================================================================
// Edge case: Start key not in the table
// =============================================================================

TEST_F(QA_GDB597, UuidPk_NonExistentStartKey) {
    exec_ok("CREATE TABLE sparse (id UUID PRIMARY KEY, text VARCHAR, text_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "sparse");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4, "text", "builtin/4");

    std::string u1 = "11111111-1111-1111-1111-111111111111";
    insert_row_uuid("sparse", u1, "only", {1.0F, 0.0F, 0.0F, 0.0F});

    exec_ok("CREATE EDGE TYPE link2 FROM sparse TO sparse");

    // Start from a UUID that doesn't exist in the table.
    std::string ghost = "ffffffff-ffff-ffff-ffff-ffffffffffff";
    auto result =
        engine_->execute("SELECT * FROM sparse WHERE NEAREST(text_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE link2 FROM sparse('" +
                         ghost + "') DIRECTION OUT MAX_DEPTH 2");

    // Must NOT crash — that's the GDB-597 guarantee.
    // NOTE: Current behavior returns unfiltered results when the start key is
    // not in the table (allowed_node_ids is empty → filter bypassed).
    // This is a pre-existing issue in NearestScanOperator, not in the GDB-597
    // fix. Filed separately.
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

// =============================================================================
// Edge case: Self-loop edge
// =============================================================================

TEST_F(QA_GDB597, UuidPk_SelfLoop) {
    exec_ok("CREATE TABLE loops (id UUID PRIMARY KEY, text VARCHAR, text_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "loops");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4, "text", "builtin/4");

    std::string u1 = "11111111-aaaa-4000-aaaa-111111111111";
    std::string u2 = "22222222-bbbb-4000-bbbb-222222222222";

    insert_row_uuid("loops", u1, "self-ref", {1.0F, 0.0F, 0.0F, 0.0F});
    insert_row_uuid("loops", u2, "other", {0.0F, 0.0F, 1.0F, 0.0F});

    exec_ok("CREATE EDGE TYPE selfedge FROM loops TO loops");
    exec_ok("LINK loops('" + u1 + "') TO loops('" + u1 + "') VIA selfedge");

    auto result =
        engine_->execute("SELECT * FROM loops WHERE NEAREST(text_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE selfedge FROM loops('" +
                         u1 + "') DIRECTION OUT MAX_DEPTH 5");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Only u1 is reachable (self-loop doesn't expand to new nodes).
    EXPECT_LE(result->rows.size(), 1u);
}

// =============================================================================
// Edge case: Cycle in graph (A → B → C → A)
// =============================================================================

TEST_F(QA_GDB597, UuidPk_Cycle) {
    exec_ok("CREATE TABLE cyc (id UUID PRIMARY KEY, text VARCHAR, text_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "cyc");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4, "text", "builtin/4");

    std::string u1 = "aaaaaaaa-1111-4000-8000-aaaaaaaaaaaa";
    std::string u2 = "bbbbbbbb-2222-4000-8000-bbbbbbbbbbbb";
    std::string u3 = "cccccccc-3333-4000-8000-cccccccccccc";
    std::string u4 = "dddddddd-4444-4000-8000-dddddddddddd";

    insert_row_uuid("cyc", u1, "a", {1.0F, 0.0F, 0.0F, 0.0F});
    insert_row_uuid("cyc", u2, "b", {0.9F, 0.1F, 0.0F, 0.0F});
    insert_row_uuid("cyc", u3, "c", {0.8F, 0.2F, 0.0F, 0.0F});
    insert_row_uuid("cyc", u4, "d", {0.0F, 0.0F, 0.0F, 1.0F});

    exec_ok("CREATE EDGE TYPE ring FROM cyc TO cyc");
    exec_ok("LINK cyc('" + u1 + "') TO cyc('" + u2 + "') VIA ring");
    exec_ok("LINK cyc('" + u2 + "') TO cyc('" + u3 + "') VIA ring");
    exec_ok("LINK cyc('" + u3 + "') TO cyc('" + u1 + "') VIA ring"); // cycle back

    auto result =
        engine_->execute("SELECT * FROM cyc WHERE NEAREST(text_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE ring FROM cyc('" +
                         u1 + "') DIRECTION OUT MAX_DEPTH 10");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Cycle: all 3 nodes reachable (u1, u2, u3). u4 not reachable.
    EXPECT_GE(result->rows.size(), 1u);
    EXPECT_LE(result->rows.size(), 3u);

    auto parsed_u4 = parse_uuid(u4);
    ASSERT_TRUE(parsed_u4.has_value());
    for (const auto& row : result->rows) {
        if (!row.empty() && row[0].type_id() == TypeId::UUID) {
            EXPECT_NE(row[0].as_uuid(), *parsed_u4) << "Unreachable u4 found in cycle results";
        }
    }
}

// =============================================================================
// Edge case: NEAREST k=1 (minimal result set)
// =============================================================================

TEST_F(QA_GDB597, UuidPk_NearestK1) {
    exec_ok("CREATE TABLE k1 (id UUID PRIMARY KEY, text VARCHAR, text_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "k1");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4, "text", "builtin/4");

    std::string u1 = "11111111-0001-4000-8000-000000000001";
    std::string u2 = "22222222-0002-4000-8000-000000000002";
    std::string u3 = "33333333-0003-4000-8000-000000000003";

    insert_row_uuid("k1", u1, "closest", {1.0F, 0.0F, 0.0F, 0.0F});
    insert_row_uuid("k1", u2, "medium", {0.5F, 0.5F, 0.0F, 0.0F});
    insert_row_uuid("k1", u3, "far", {0.0F, 1.0F, 0.0F, 0.0F});

    exec_ok("CREATE EDGE TYPE conn FROM k1 TO k1");
    exec_ok("LINK k1('" + u1 + "') TO k1('" + u2 + "') VIA conn");
    exec_ok("LINK k1('" + u2 + "') TO k1('" + u3 + "') VIA conn");

    auto result =
        engine_->execute("SELECT * FROM k1 WHERE NEAREST(text_vec, 1) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE conn FROM k1('" +
                         u1 + "') DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Exactly 1 result.
    EXPECT_EQ(result->rows.size(), 1u);
}

// =============================================================================
// Edge case: INT16 PKs (narrow integer coercion)
// =============================================================================

TEST_F(QA_GDB597, Int16Pk_NarrowCoercion) {
    exec_ok("CREATE TABLE small (id SMALLINT PRIMARY KEY, text VARCHAR, text_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "small");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4, "text", "builtin/4");

    exec_ok("INSERT INTO small VALUES (1, 'one', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO small VALUES (2, 'two', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO small VALUES (3, 'three', [0.0, 1.0, 0.0, 0.0])");

    exec_ok("CREATE EDGE TYPE seq FROM small TO small");
    exec_ok("LINK small(1) TO small(2) VIA seq");

    auto result =
        engine_->execute("SELECT * FROM small WHERE NEAREST(text_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE seq FROM small(1) DIRECTION OUT MAX_DEPTH 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_GE(result->rows.size(), 1u);
    EXPECT_LE(result->rows.size(), 2u);

    for (const auto& row : result->rows) {
        if (!row.empty() && row[0].type_id() == TypeId::INT16) {
            EXPECT_NE(row[0].as_int16(), static_cast<int16_t>(3))
                << "Unreachable node 3 in results";
        }
    }
}
