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
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"
#include "sixseven/vector/embedding_column.h"
#include "sixseven/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

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

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());
        provider_registry_ = std::make_unique<ProviderRegistry>(catalog_);
    }

    void TearDown() override {
        index_manager_.reset();
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        provider_registry_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return result ? std::move(*result) : QueryResult{};
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

    /// Create an HNSW IndexDef on the given column and rebuild all indexes
    /// from current table data. After this, NEAREST uses the HNSW fast path
    /// (node_id → RID map) instead of the brute-force scan.
    void enable_hnsw(const std::string& table_name, const std::string& column_name) {
        auto schema = catalog_.get_table(default_database_id, table_name);
        ASSERT_TRUE(schema.has_value());

        IndexDef def;
        def.table_id = schema->table_id;
        def.name = "hnsw_" + table_name + "_" + column_name;
        def.index_type = "hnsw";
        def.columns = column_name;
        def.is_unique = false;
        auto idx = catalog_.create_index(def);
        ASSERT_TRUE(idx.has_value()) << idx.error().message;

        index_manager_ = std::make_unique<IndexManager>(catalog_, *storage_);
        engine_->set_index_manager(index_manager_.get());
        engine_->set_hnsw_indexes(index_manager_->hnsw_map());
        auto r = index_manager_->rebuild_all_indexes();
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<ProviderRegistry> provider_registry_;
    std::unique_ptr<IndexManager> index_manager_;
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
    auto result = engine_->execute("SELECT * FROM posts "
                                   "WHERE NEAREST(body_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
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
    auto result = engine_->execute("SELECT * FROM docs "
                                   "WHERE NEAREST(body_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
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

// =============================================================================
// GDB-745: Graph-scoped NEAREST with an HNSW index must scope by RID
// =============================================================================

TEST_F(NearestGraphScopeTest, HnswGraphScopeWithNullEmbeddingRow) {
    exec_ok("CREATE TABLE notes (id INT PRIMARY KEY, body VARCHAR, body_vec EMBEDDING)");

    auto schema = catalog_.get_table(default_database_id, "notes");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4, "body", "builtin/4");

    // Row 1 has a NULL embedding (steady state for async embedding
    // generation). It sits BEFORE the in-scope rows, so heap ordinals and
    // HNSW node ids diverge: HNSW assigns node ids only to rows 2-4.
    exec_ok("INSERT INTO notes (id, body) VALUES (1, 'pending')");
    exec_ok("INSERT INTO notes VALUES (2, 'alpha', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO notes VALUES (3, 'beta', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO notes VALUES (4, 'gamma', [0.0, 1.0, 0.0, 0.0])");

    // Build the HNSW index from the table (3 nodes: rows 2, 3, 4).
    enable_hnsw("notes", "body_vec");

    // Graph: 2 -> 3. Reachable set from 2 is exactly {2, 3}.
    exec_ok("CREATE EDGE TYPE links FROM notes TO notes");
    exec_ok("LINK notes(2) TO notes(3) VIA links");

    auto result = engine_->execute("SELECT * FROM notes "
                                   "WHERE NEAREST(body_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                                   "WITHIN TRAVERSE links FROM notes(2) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Exactly rows 2 and 3 — row 4 is out of scope, row 1 has no embedding.
    std::vector<int32_t> ids;
    for (const auto& row : result->rows) {
        ASSERT_FALSE(row.empty());
        ids.push_back(row[0].as_int32());
    }
    std::sort(ids.begin(), ids.end());
    ASSERT_EQ(ids.size(), 2u) << "scope filter selected the wrong rows";
    EXPECT_EQ(ids[0], 2);
    EXPECT_EQ(ids[1], 3);
}

TEST_F(NearestGraphScopeTest, HnswGraphScopeWithDeletedRow) {
    exec_ok("CREATE TABLE pages (id INT PRIMARY KEY, body VARCHAR, body_vec EMBEDDING)");

    auto schema = catalog_.get_table(default_database_id, "pages");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4, "body", "builtin/4");

    exec_ok("INSERT INTO pages VALUES (1, 'alpha', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO pages VALUES (2, 'beta', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO pages VALUES (3, 'gamma', [0.8, 0.2, 0.0, 0.0])");
    exec_ok("INSERT INTO pages VALUES (4, 'delta', [0.0, 1.0, 0.0, 0.0])");

    // Graph: 1 -> 2 -> 3. Reachable set from 1 is {1, 2, 3}.
    exec_ok("CREATE EDGE TYPE links2 FROM pages TO pages");
    exec_ok("LINK pages(1) TO pages(2) VIA links2");
    exec_ok("LINK pages(2) TO pages(3) VIA links2");

    // Build the HNSW index with all four rows, THEN delete row 2. The HNSW
    // node for row 2 is now stale; scoping must not shift onto other rows.
    enable_hnsw("pages", "body_vec");
    exec_ok("DELETE FROM pages WHERE id = 2");

    auto result =
        engine_->execute("SELECT * FROM pages "
                         "WHERE NEAREST(body_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE links2 FROM pages(1) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Exactly rows 1 and 3 — row 2 is deleted, row 4 is out of scope.
    std::vector<int32_t> ids;
    for (const auto& row : result->rows) {
        ASSERT_FALSE(row.empty());
        ids.push_back(row[0].as_int32());
    }
    std::sort(ids.begin(), ids.end());
    ASSERT_EQ(ids.size(), 2u) << "scope filter selected the wrong rows";
    EXPECT_EQ(ids[0], 1);
    EXPECT_EQ(ids[1], 3);
}

// =============================================================================
// GDB-1251: WITHIN TRAVERSE must validate the traversal reaches the table that
// owns the EMBEDDING column. Otherwise reachable PKs are intersected with the
// vector table's PKs by raw numeric equality — a silent garbage scope.
// =============================================================================

class NearestTraverseScopeValidationTest : public NearestGraphScopeTest {
protected:
    /// books(id INT64 PK, title, description_vec EMBEDDING),
    /// readers(id INT64 PK, name STRING),
    /// edges: reads(readers→books), follows(readers→readers),
    /// similar_to(books→books).
    void setup_schema() {
        exec_ok(
            "CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR, description_vec EMBEDDING)");
        exec_ok("CREATE TABLE readers (id INT PRIMARY KEY, name VARCHAR)");

        auto books = catalog_.get_table(default_database_id, "books");
        ASSERT_TRUE(books.has_value());
        register_embedding(books->table_id, 2, 4, "title", "builtin/4");
        engine_->set_provider_registry(provider_registry_.get());

        exec_ok("CREATE EDGE TYPE reads FROM readers TO books");
        exec_ok("CREATE EDGE TYPE follows FROM readers TO readers");
        exec_ok("CREATE EDGE TYPE similar_to FROM books TO books");
    }
};

// AC1: the canonical repro — follows reaches readers, NEAREST is on books.
TEST_F(NearestTraverseScopeValidationTest, ReproFollowsReachesWrongTableErrors) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");

    auto result =
        engine_->execute("SELECT title FROM books "
                         "WHERE NEAREST(description_vec, 3) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE follows FROM readers(1) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    const std::string& msg = result.error().message;
    EXPECT_NE(msg.find("follows"), std::string::npos) << msg;
    EXPECT_NE(msg.find("readers"), std::string::npos) << msg;
    EXPECT_NE(msg.find("books"), std::string::npos) << msg;
}

// AC2: heterogeneous edge ENDING at the vector table is allowed, and the
// start-seed fix means a book whose id equals reader 1's id is NOT admitted
// unless actually reached.
TEST_F(NearestTraverseScopeValidationTest, ReadsHeterogeneousEndingAtBooksAllowed) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    // Book id 1 numerically collides with reader 1's id but is NOT reached.
    exec_ok("INSERT INTO books VALUES (1, 'collision', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (10, 'reached_a', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (11, 'reached_b', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (12, 'unreached', [0.0, 0.0, 1.0, 0.0])");

    // reader 1 reads books 10 and 11 only.
    exec_ok("LINK readers(1) TO books(10) VIA reads");
    exec_ok("LINK readers(1) TO books(11) VIA reads");

    auto result =
        engine_->execute("SELECT id FROM books "
                         "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    std::vector<int32_t> ids;
    for (const auto& row : result->rows) {
        ASSERT_FALSE(row.empty());
        ids.push_back(row[0].as_int32());
    }
    std::sort(ids.begin(), ids.end());
    ASSERT_EQ(ids.size(), 2u) << "expected exactly the reached books {10, 11}";
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 11);
    // The PK-collision book (id 1) and the unreached book (id 12) are excluded.
    EXPECT_EQ(std::count(ids.begin(), ids.end(), 1), 0)
        << "start-node PK collision leaked into scope";
    EXPECT_EQ(std::count(ids.begin(), ids.end(), 12), 0);
}

// AC3 direction matrix: reads DIRECTION IN from a book reaches readers → error.
TEST_F(NearestTraverseScopeValidationTest, ReadsDirectionInReachesReadersErrors) {
    setup_schema();
    exec_ok("INSERT INTO books VALUES (1, 'b', [1.0, 0.0, 0.0, 0.0])");

    auto result = engine_->execute("SELECT title FROM books "
                                   "WHERE NEAREST(description_vec, 3) TO [1.0, 0.0, 0.0, 0.0] "
                                   "WITHIN TRAVERSE reads FROM books(1) DIRECTION IN MAX_DEPTH 2");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(result.error().message.find("readers"), std::string::npos) << result.error().message;
}

// AC3: similar_to DIRECTION IN (books→books) reaches books → allowed.
TEST_F(NearestTraverseScopeValidationTest, SimilarToDirectionInAllowed) {
    setup_schema();
    exec_ok("INSERT INTO books VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (2, 'b', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("LINK books(1) TO books(2) VIA similar_to");

    // DIRECTION IN from book 2 reaches book 1 (the source side).
    auto result =
        engine_->execute("SELECT id FROM books "
                         "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE similar_to FROM books(2) DIRECTION IN MAX_DEPTH 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // Scope is {2 (start, same table), 1 (reached)}.
    EXPECT_GE(result->rows.size(), 1u);
}

// AC3: DIRECTION BOTH on a heterogeneous edge (reads) → error.
TEST_F(NearestTraverseScopeValidationTest, ReadsDirectionBothErrors) {
    setup_schema();
    exec_ok("INSERT INTO books VALUES (1, 'b', [1.0, 0.0, 0.0, 0.0])");

    auto result =
        engine_->execute("SELECT title FROM books "
                         "WHERE NEAREST(description_vec, 3) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE reads FROM books(1) DIRECTION BOTH MAX_DEPTH 2");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// AC3: DIRECTION BOTH on a homogeneous edge (similar_to) → allowed.
TEST_F(NearestTraverseScopeValidationTest, SimilarToDirectionBothAllowed) {
    setup_schema();
    exec_ok("INSERT INTO books VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (2, 'b', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("LINK books(1) TO books(2) VIA similar_to");

    auto result =
        engine_->execute("SELECT id FROM books "
                         "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE similar_to FROM books(1) DIRECTION BOTH MAX_DEPTH 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_GE(result->rows.size(), 1u);
}

// =============================================================================
// GDB-1257: a WITHIN TRAVERSE scope that resolves to ZERO reachable rows must
// emit zero rows, not fall back to the global nearest neighbors. The empty
// reachable set is an explicit scope, not an absent scope.
// =============================================================================

// Brute-force path: reader 1 reads no books, so the scope is empty.
TEST_F(NearestTraverseScopeValidationTest, EmptyScopeEmitsZeroRowsBruteForce) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    exec_ok("INSERT INTO books VALUES (1, 'collide', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (2, 'other', [0.0, 1.0, 0.0, 0.0])");
    // Deliberately do NOT link reader 1 to any book.

    auto result =
        engine_->execute("SELECT id FROM books "
                         "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 0u) << "empty graph scope leaked global NEAREST results";
}

// HNSW path: same empty scope, but with an HNSW index built so the operator
// takes the index search path rather than brute-force. The early-out must
// still suppress the global results.
TEST_F(NearestTraverseScopeValidationTest, EmptyScopeEmitsZeroRowsHnsw) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    exec_ok("INSERT INTO books VALUES (1, 'collide', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (2, 'other', [0.0, 1.0, 0.0, 0.0])");
    enable_hnsw("books", "description_vec");
    // No reads edge from reader 1.

    auto result =
        engine_->execute("SELECT id FROM books "
                         "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 0u)
        << "empty graph scope leaked global NEAREST results via HNSW path";
}

// Guard against over-correction: a NEAREST with NO WITHIN TRAVERSE clause is
// unscoped and must still return the global nearest neighbors.
TEST_F(NearestTraverseScopeValidationTest, UnscopedNearestStillReturnsGlobalResults) {
    setup_schema();
    exec_ok("INSERT INTO books VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (2, 'b', [0.0, 1.0, 0.0, 0.0])");

    auto result = engine_->execute("SELECT id FROM books "
                                   "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0]");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->rows.size(), 2u)
        << "unscoped NEAREST must not be suppressed by the empty-scope guard";
}

// A non-empty scope must still return exactly the reached rows (the fix must
// not regress the populated-scope case).
TEST_F(NearestTraverseScopeValidationTest, NonEmptyScopeStillReturnsReachedRows) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    exec_ok("INSERT INTO books VALUES (10, 'reached', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (11, 'unreached', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("LINK readers(1) TO books(10) VIA reads");

    auto result =
        engine_->execute("SELECT id FROM books "
                         "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 1u);
    EXPECT_EQ(result->rows[0][0].as_int32(), 10);
}

// AC4: heterogeneous start key typed by the START table. A STRING-PK readers
// variant: TRAVERSE reads FROM readers('alice') must coerce against readers'
// PK type (STRING), not books' PK type (INT).
TEST_F(NearestGraphScopeTest, HeterogeneousStartKeyTypedByStartTable) {
    exec_ok("CREATE TABLE books2 (id INT PRIMARY KEY, title VARCHAR, description_vec EMBEDDING)");
    exec_ok("CREATE TABLE readers2 (id VARCHAR PRIMARY KEY, name VARCHAR)");

    auto books = catalog_.get_table(default_database_id, "books2");
    ASSERT_TRUE(books.has_value());
    register_embedding(books->table_id, 2, 4, "title", "builtin/4");
    engine_->set_provider_registry(provider_registry_.get());

    exec_ok("CREATE EDGE TYPE reads2 FROM readers2 TO books2");
    exec_ok("INSERT INTO readers2 VALUES ('alice', 'Alice')");
    exec_ok("INSERT INTO books2 VALUES (10, 'reached', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books2 VALUES (11, 'unreached', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("LINK readers2('alice') TO books2(10) VIA reads2");

    // Start key 'alice' is a STRING; coercion must target readers2's STRING PK,
    // not books2's INT PK (which would fail or produce garbage).
    auto result =
        engine_->execute("SELECT id FROM books2 "
                         "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE reads2 FROM readers2('alice') DIRECTION OUT MAX_DEPTH 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    std::vector<int32_t> ids;
    for (const auto& row : result->rows) {
        ASSERT_FALSE(row.empty());
        ids.push_back(row[0].as_int32());
    }
    ASSERT_EQ(ids.size(), 1u) << "expected exactly the reached book {10}";
    EXPECT_EQ(ids[0], 10);
}
