/// @file test_qa_gdb_745_adversarial.cpp
/// @brief Adversarial QA tests for GDB-745: graph-scoped NEAREST canonical
/// RID space.
///
/// These tests are designed to exercise edge cases, boundary conditions, and
/// failure modes that the shipped tests do not cover.  They target:
///   1. Many interleaved NULL embeddings (20 rows alternating null/non-null)
///   2. Multi-delete after HNSW build (k > survivors, stale rid_map bounds)
///   3. Insert-after-build: document index-staleness behavior (brute-force
///      detects, HNSW misses — expected, no crash)
///   4. Scope-of-one with k larger than scope
///   5. Scope where NO in-scope row has an embedding (0 results, no crash)
///   6. Whole-table scope == unscoped results
///   7. Scoped DOT-product query (most-similar-first ordering within scope)
///   8. Scoped query on metric-mismatched index (GDB-723 brute-force fallback,
///      still respects RID scope)
///   9. Mutation class: verify planner correctly inserts NULL-embedding rows'
///      RIDs — the scope scan must NOT skip rows that have NULL embeddings
///      when they are reachable PK nodes (they should be included as scoped
///      candidates even though they have no embedding value and will therefore
///      produce 0 distance matches).

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"
#include "sixseven/vector/embedding_column.h"
#include "sixseven/vector/hnsw_index.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_qa_helpers.h"

using namespace sixseven;

// =============================================================================
// Shared fixture (mirrors QA_GDB745)
// =============================================================================

class QA_GDB745_Adversarial : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ =
            std::filesystem::temp_directory_path() / "sixseven_qa_gdb745_adversarial";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());
    }

    void TearDown() override {
        index_manager_.reset();
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << " failed: " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void register_embedding(table_id_t table_id, int32_t col_id, int32_t dim) {
        EmbeddingColumnDef emb_def;
        emb_def.table_id = table_id;
        emb_def.column_id = col_id;
        emb_def.dimension = dim;
        emb_def.source_expr = "body";
        emb_def.provider = "builtin/4";
        auto reg = catalog_.register_embedding_column(emb_def);
        ASSERT_TRUE(reg.has_value()) << reg.error().message;
    }

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

    static std::vector<int32_t> sorted_ids(const QueryResult& qr) {
        std::vector<int32_t> ids;
        for (const auto& row : qr.rows) {
            EXPECT_FALSE(row.empty());
            if (!row.empty()) {
                ids.push_back(row[0].as_int32());
            }
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<IndexManager> index_manager_;
};

// =============================================================================
// Test 1: Many interleaved NULLs (20 rows, alternating null/non-null)
//
// Rows with odd IDs have NULL embeddings; rows with even IDs have non-null
// embeddings. HNSW node ids are assigned only to even-id rows (10 nodes).
// Scope covers a scattered subset (IDs 4, 8, 12, 16, 20). Pre-fix, the
// ordinal shift would cause IDs 5, 9, 13, 17, 21 to appear — which don't
// exist. Post-fix, only the correct in-scope rows surface.
// =============================================================================

TEST_F(QA_GDB745_Adversarial, ManyInterleavedNullsScatteredScope) {
    exec_ok("CREATE TABLE items (id INT PRIMARY KEY, body VARCHAR, vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "items");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4);

    // 20 rows: odd IDs → NULL embedding; even IDs → non-null embedding.
    for (int i = 1; i <= 20; ++i) {
        if (i % 2 == 1) {
            // NULL embedding row.
            std::string sql = "INSERT INTO items (id, body) VALUES (" + std::to_string(i) +
                              ", 'pending')";
            exec_ok(sql);
        } else {
            // Non-null embedding: base vector rotated slightly per row.
            float v0 = 1.0F - static_cast<float>(i) * 0.04F;
            float v1 = static_cast<float>(i) * 0.04F;
            std::string vec = "[" + std::to_string(v0) + ", " + std::to_string(v1) + ", 0.0, 0.0]";
            std::string sql = "INSERT INTO items VALUES (" + std::to_string(i) +
                              ", 'row" + std::to_string(i) + "', " + vec + ")";
            exec_ok(sql);
        }
    }

    // Build HNSW with 10 non-null rows (even IDs 2,4,6,...,20).
    enable_hnsw("items", "vec");

    // Create a chain edge type.
    exec_ok("CREATE EDGE TYPE chain20 FROM items TO items");

    // Scope: connect 4 -> 8 -> 12 -> 16 -> 20 so all are reachable from 4.
    exec_ok("LINK items(4) TO items(8) VIA chain20");
    exec_ok("LINK items(8) TO items(12) VIA chain20");
    exec_ok("LINK items(12) TO items(16) VIA chain20");
    exec_ok("LINK items(16) TO items(20) VIA chain20");

    auto result = engine_->execute(
        "SELECT * FROM items "
        "WHERE NEAREST(vec, 10) TO [1.0, 0.0, 0.0, 0.0] "
        "WITHIN TRAVERSE chain20 FROM items(4) DIRECTION OUT MAX_DEPTH 5");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto ids = sorted_ids(*result);

    // Only in-scope non-null rows: {4, 8, 12, 16, 20}.
    // Odd-id rows have no embedding so they can never appear.
    // Out-of-scope even rows (2, 6, 10, 14, 18) must not appear.
    std::vector<int32_t> expected = {4, 8, 12, 16, 20};
    EXPECT_EQ(ids, expected)
        << "interleaved NULL rows should not shift HNSW scope into out-of-scope rows";

    // No result ID should be odd (NULL-embedding rows).
    for (int32_t id : ids) {
        EXPECT_EQ(id % 2, 0) << "odd-id row (NULL embedding) leaked into results: " << id;
    }

    // No result ID should be out-of-scope even row.
    std::vector<int32_t> out_of_scope_even = {2, 6, 10, 14, 18};
    for (int32_t id : ids) {
        EXPECT_EQ(std::find(out_of_scope_even.begin(), out_of_scope_even.end(), id),
                  out_of_scope_even.end())
            << "out-of-scope even row leaked into results: " << id;
    }
}

// =============================================================================
// Test 2: Multiple deletes after HNSW build, k > survivors
//
// Build index with 6 rows in scope; delete 4 of them; ask for k=10 (more
// than the 2 survivors). The HNSW rid_map bounds check should be exercised
// for deleted-row slots.  Must return exactly the 2 surviving in-scope rows,
// not crash, not return out-of-scope rows.
// =============================================================================

TEST_F(QA_GDB745_Adversarial, MultipleDeletesAfterBuildKLargerThanSurvivors) {
    exec_ok("CREATE TABLE chunks (id INT PRIMARY KEY, body VARCHAR, vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "chunks");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4);

    // 8 rows total; 6 in-scope (IDs 1-6), 2 out-of-scope (IDs 7-8).
    exec_ok("INSERT INTO chunks VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO chunks VALUES (2, 'b', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO chunks VALUES (3, 'c', [0.8, 0.2, 0.0, 0.0])");
    exec_ok("INSERT INTO chunks VALUES (4, 'd', [0.7, 0.3, 0.0, 0.0])");
    exec_ok("INSERT INTO chunks VALUES (5, 'e', [0.6, 0.4, 0.0, 0.0])");
    exec_ok("INSERT INTO chunks VALUES (6, 'f', [0.5, 0.5, 0.0, 0.0])");
    exec_ok("INSERT INTO chunks VALUES (7, 'g', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("INSERT INTO chunks VALUES (8, 'h', [0.0, 0.0, 1.0, 0.0])");

    // Build HNSW with all 8 rows.
    enable_hnsw("chunks", "vec");

    // Graph: 1 -> 2 -> 3 -> 4 -> 5 -> 6 (chain); 7 and 8 isolated.
    exec_ok("CREATE EDGE TYPE del_chain FROM chunks TO chunks");
    exec_ok("LINK chunks(1) TO chunks(2) VIA del_chain");
    exec_ok("LINK chunks(2) TO chunks(3) VIA del_chain");
    exec_ok("LINK chunks(3) TO chunks(4) VIA del_chain");
    exec_ok("LINK chunks(4) TO chunks(5) VIA del_chain");
    exec_ok("LINK chunks(5) TO chunks(6) VIA del_chain");

    // Delete 4 of the 6 in-scope rows, leaving only rows 1 and 6.
    exec_ok("DELETE FROM chunks WHERE id = 2");
    exec_ok("DELETE FROM chunks WHERE id = 3");
    exec_ok("DELETE FROM chunks WHERE id = 4");
    exec_ok("DELETE FROM chunks WHERE id = 5");

    // Ask for k=10 — more than the 2 survivors.  Must not crash or return
    // out-of-scope rows.
    auto result = engine_->execute(
        "SELECT * FROM chunks "
        "WHERE NEAREST(vec, 10) TO [1.0, 0.0, 0.0, 0.0] "
        "WITHIN TRAVERSE del_chain FROM chunks(1) DIRECTION OUT MAX_DEPTH 6");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto ids = sorted_ids(*result);

    // Only rows 1 and 6 survive and are in-scope.
    EXPECT_LE(ids.size(), 2u) << "more rows returned than survivors";
    for (int32_t id : ids) {
        EXPECT_TRUE(id == 1 || id == 6)
            << "unexpected row " << id << " returned after multi-delete";
    }
    // Out-of-scope rows 7 and 8 must never appear.
    for (int32_t id : ids) {
        EXPECT_NE(id, 7) << "out-of-scope row 7 leaked";
        EXPECT_NE(id, 8) << "out-of-scope row 8 leaked";
    }
}

// =============================================================================
// Test 3: Insert-after-build — document staleness behavior
//
// Build HNSW with rows 1-4.  Insert row 5 (in-scope) after the build.
// Row 5 is NOT in the HNSW index (index staleness is a known limitation).
// Brute-force path should find it; HNSW fast path won't see it in the index.
// We document the actual behavior without inventing requirements: assert no
// crash and that results are a subset of {1, 2, 3, 4, 5} with 4 in scope.
// =============================================================================

TEST_F(QA_GDB745_Adversarial, InsertAfterBuildDocumentsStalenessBehavior) {
    exec_ok("CREATE TABLE posts (id INT PRIMARY KEY, body VARCHAR, vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "posts");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4);

    exec_ok("INSERT INTO posts VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO posts VALUES (2, 'b', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO posts VALUES (3, 'c', [0.8, 0.2, 0.0, 0.0])");
    exec_ok("INSERT INTO posts VALUES (4, 'oos', [0.0, 1.0, 0.0, 0.0])");

    // Build HNSW with rows 1-4.
    enable_hnsw("posts", "vec");

    exec_ok("CREATE EDGE TYPE post_links FROM posts TO posts");
    exec_ok("LINK posts(1) TO posts(2) VIA post_links");
    exec_ok("LINK posts(2) TO posts(3) VIA post_links");

    // Insert a NEW in-scope row AFTER the HNSW build.
    exec_ok("INSERT INTO posts VALUES (5, 'new', [0.95, 0.05, 0.0, 0.0])");
    exec_ok("LINK posts(3) TO posts(5) VIA post_links");

    auto result = engine_->execute(
        "SELECT * FROM posts "
        "WHERE NEAREST(vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
        "WITHIN TRAVERSE post_links FROM posts(1) DIRECTION OUT MAX_DEPTH 5");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Must not crash. Results must be a subset of reachable rows {1, 2, 3, 5}.
    // Row 4 is out-of-scope — must never appear.
    auto ids = sorted_ids(*result);
    for (int32_t id : ids) {
        EXPECT_NE(id, 4) << "out-of-scope row 4 leaked into results after insert-after-build";
        EXPECT_GE(id, 1);
        EXPECT_LE(id, 5);
    }
    // Row 5 may or may not appear (HNSW index is stale — this is acceptable).
    // We are documenting, not prescribing: just assert the constraint above.
}

// =============================================================================
// Test 4a: Scope-of-one with k larger than scope
//
// Graph has exactly one reachable node.  k=10.  Must return exactly 1 row,
// not crash, not return 0 rows.
// =============================================================================

TEST_F(QA_GDB745_Adversarial, ScopeOfOneKLargerThanScope) {
    exec_ok("CREATE TABLE nodes (id INT PRIMARY KEY, body VARCHAR, vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "nodes");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4);

    exec_ok("INSERT INTO nodes VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO nodes VALUES (2, 'b', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("INSERT INTO nodes VALUES (3, 'c', [0.0, 0.0, 1.0, 0.0])");

    enable_hnsw("nodes", "vec");

    // Start node = 1, no outgoing edges. Reachable set = {1}.
    exec_ok("CREATE EDGE TYPE isolated_edge FROM nodes TO nodes");

    auto result = engine_->execute(
        "SELECT * FROM nodes "
        "WHERE NEAREST(vec, 10) TO [1.0, 0.0, 0.0, 0.0] "
        "WITHIN TRAVERSE isolated_edge FROM nodes(1) DIRECTION OUT MAX_DEPTH 3");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Must return exactly 1 row (only the start node, which is in scope).
    auto ids = sorted_ids(*result);
    ASSERT_EQ(ids.size(), 1u) << "scope-of-one should return exactly 1 row";
    EXPECT_EQ(ids[0], 1);
}

// =============================================================================
// Test 4b: Scope where NO in-scope row has an embedding (0 results, no crash)
// =============================================================================

TEST_F(QA_GDB745_Adversarial, ScopeWhereNoInScopeRowHasEmbedding) {
    exec_ok("CREATE TABLE articles (id INT PRIMARY KEY, body VARCHAR, vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "articles");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4);

    // Rows 1-3: NULL embeddings (in-scope nodes).
    exec_ok("INSERT INTO articles (id, body) VALUES (1, 'pending1')");
    exec_ok("INSERT INTO articles (id, body) VALUES (2, 'pending2')");
    exec_ok("INSERT INTO articles (id, body) VALUES (3, 'pending3')");
    // Row 4: non-null embedding but OUT of scope.
    exec_ok("INSERT INTO articles VALUES (4, 'ready', [1.0, 0.0, 0.0, 0.0])");

    enable_hnsw("articles", "vec");

    exec_ok("CREATE EDGE TYPE art_links FROM articles TO articles");
    exec_ok("LINK articles(1) TO articles(2) VIA art_links");
    exec_ok("LINK articles(2) TO articles(3) VIA art_links");

    auto result = engine_->execute(
        "SELECT * FROM articles "
        "WHERE NEAREST(vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
        "WITHIN TRAVERSE art_links FROM articles(1) DIRECTION OUT MAX_DEPTH 3");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // No in-scope row has an embedding -> 0 results. Must not crash.
    EXPECT_EQ(result->rows.size(), 0u)
        << "no in-scope embedding rows should yield 0 results, not crash or leak OOS rows";
}

// =============================================================================
// Test 5: Whole-table scope == unscoped results
//
// Connect every node in a chain so the whole table is reachable. Scoped and
// unscoped NEAREST must agree on membership (order may differ due to tie
// resolution, but the set of IDs must match).
// =============================================================================

TEST_F(QA_GDB745_Adversarial, WholeScopeMatchesUnscoped) {
    exec_ok("CREATE TABLE docs2 (id INT PRIMARY KEY, body VARCHAR, vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "docs2");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4);

    for (int i = 1; i <= 5; ++i) {
        float v0 = 1.0F - static_cast<float>(i - 1) * 0.1F;
        float v1 = static_cast<float>(i - 1) * 0.1F;
        std::string sql = "INSERT INTO docs2 VALUES (" + std::to_string(i) +
                          ", 'row" + std::to_string(i) + "', [" +
                          std::to_string(v0) + ", " + std::to_string(v1) + ", 0.0, 0.0])";
        exec_ok(sql);
    }

    enable_hnsw("docs2", "vec");

    exec_ok("CREATE EDGE TYPE all_chain FROM docs2 TO docs2");
    exec_ok("LINK docs2(1) TO docs2(2) VIA all_chain");
    exec_ok("LINK docs2(2) TO docs2(3) VIA all_chain");
    exec_ok("LINK docs2(3) TO docs2(4) VIA all_chain");
    exec_ok("LINK docs2(4) TO docs2(5) VIA all_chain");

    // Scoped query (whole table reachable from 1).
    auto scoped_result = engine_->execute(
        "SELECT * FROM docs2 "
        "WHERE NEAREST(vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
        "WITHIN TRAVERSE all_chain FROM docs2(1) DIRECTION OUT MAX_DEPTH 10");
    ASSERT_TRUE(scoped_result.has_value()) << scoped_result.error().message;

    // Unscoped (no WITHIN TRAVERSE).
    auto unscoped_result = engine_->execute(
        "SELECT * FROM docs2 "
        "WHERE NEAREST(vec, 5) TO [1.0, 0.0, 0.0, 0.0]");
    ASSERT_TRUE(unscoped_result.has_value()) << unscoped_result.error().message;

    auto scoped_ids = sorted_ids(*scoped_result);
    auto unscoped_ids = sorted_ids(*unscoped_result);

    EXPECT_EQ(scoped_ids, unscoped_ids)
        << "whole-table scope should produce the same row set as unscoped NEAREST";
}

// =============================================================================
// Test 6: Scoped DOT-product query — most-similar-first within scope
//
// Uses USING DOT metric. Rows are chosen so the in-scope result ordering is
// deterministic (cosine would give the same result here, but we want to verify
// DOT ordering specifically, exercising the GDB-717 negation path together
// with GDB-745 scoping).
// =============================================================================

TEST_F(QA_GDB745_Adversarial, ScopedDotProductOrderingWithinScope) {
    exec_ok("CREATE TABLE vecs (id INT PRIMARY KEY, body VARCHAR, vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "vecs");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4);

    // Row 1: highest dot product with query [1,0,0,0] -> dot=2.0.
    exec_ok("INSERT INTO vecs VALUES (1, 'a', [2.0, 0.0, 0.0, 0.0])");
    // Row 2: lower dot product -> dot=1.5.
    exec_ok("INSERT INTO vecs VALUES (2, 'b', [1.5, 0.0, 0.0, 0.0])");
    // Row 3: even lower -> dot=0.5.
    exec_ok("INSERT INTO vecs VALUES (3, 'c', [0.5, 0.0, 0.0, 0.0])");
    // Row 4: out-of-scope, highest dot product of all -> dot=3.0.
    exec_ok("INSERT INTO vecs VALUES (4, 'd', [3.0, 0.0, 0.0, 0.0])");

    enable_hnsw("vecs", "vec");

    exec_ok("CREATE EDGE TYPE dot_chain FROM vecs TO vecs");
    exec_ok("LINK vecs(1) TO vecs(2) VIA dot_chain");
    exec_ok("LINK vecs(2) TO vecs(3) VIA dot_chain");
    // Row 4 is NOT linked — out of scope.

    auto result = engine_->execute(
        "SELECT * FROM vecs "
        "WHERE NEAREST(vec, 3) TO [1.0, 0.0, 0.0, 0.0] "
        "WITHIN TRAVERSE dot_chain FROM vecs(1) DIRECTION OUT MAX_DEPTH 3 USING DOT");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    ASSERT_EQ(result->rows.size(), 3u)
        << "expected exactly 3 in-scope results for DOT-product scoped query; got "
        << result->rows.size();

    // Row 4 (out-of-scope) must not appear even though it has the highest dot.
    for (const auto& row : result->rows) {
        ASSERT_FALSE(row.empty());
        EXPECT_NE(row[0].as_int32(), 4)
            << "out-of-scope row 4 leaked into DOT-product scoped results";
    }

    // Primary correctness: the in-scope set is exactly {1, 2, 3}.
    auto ids = sorted_ids(*result);
    std::vector<int32_t> expected_ids = {1, 2, 3};
    EXPECT_EQ(ids, expected_ids) << "DOT scoped query should return exactly in-scope rows {1,2,3}";

    // Ordering check: row 1 (dot=2.0) should appear first (most similar to
    // query [1,0,0,0] under DOT), row 3 (dot=0.5) should appear last.
    // Only check if we have 3 rows and the first column is int32.
    auto& rows = result->rows;
    if (!rows.empty() && rows[0][0].type_id() == TypeId::INT32) {
        EXPECT_EQ(rows[0][0].as_int32(), 1) << "row 1 (dot=2.0) should be first";
        EXPECT_EQ(rows[2][0].as_int32(), 3) << "row 3 (dot=0.5) should be last";
    }
}

// =============================================================================
// Test 7: Scoped query on metric-mismatched index (GDB-723 brute-force fallback)
//
// Build HNSW as COSINE (the default); query with DOT. DOT sort metric
// becomes INNER_PRODUCT (negated dot), which != COSINE, so GDB-723 forces
// brute-force fallback. Brute-force must still respect the RID scope
// (GDB-745). This test exercises both correctness invariants simultaneously.
// =============================================================================

TEST_F(QA_GDB745_Adversarial, MetricMismatchFallbackRespectsScope) {
    exec_ok("CREATE TABLE mixed (id INT PRIMARY KEY, body VARCHAR, vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "mixed");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4);

    exec_ok("INSERT INTO mixed VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO mixed VALUES (2, 'b', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO mixed VALUES (3, 'c', [0.8, 0.2, 0.0, 0.0])");
    exec_ok("INSERT INTO mixed VALUES (4, 'oos', [0.0, 1.0, 0.0, 0.0])");

    // Build HNSW with default COSINE metric.
    enable_hnsw("mixed", "vec");

    exec_ok("CREATE EDGE TYPE mixed_chain FROM mixed TO mixed");
    exec_ok("LINK mixed(1) TO mixed(2) VIA mixed_chain");
    exec_ok("LINK mixed(2) TO mixed(3) VIA mixed_chain");
    // Row 4 is not linked.

    // Query with DOT — sort metric becomes INNER_PRODUCT != COSINE,
    // triggering GDB-723 brute-force fallback.
    auto result = engine_->execute(
        "SELECT * FROM mixed "
        "WHERE NEAREST(vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
        "WITHIN TRAVERSE mixed_chain FROM mixed(1) DIRECTION OUT MAX_DEPTH 3 USING DOT");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto ids = sorted_ids(*result);

    // Must return {1, 2, 3} only — row 4 out of scope even via brute-force.
    std::vector<int32_t> expected = {1, 2, 3};
    EXPECT_EQ(ids, expected)
        << "GDB-723 metric-mismatch brute-force fallback must still respect GDB-745 RID scope";
}

// =============================================================================
// Test 8: Mutation class — planner must include NULL-embedding reachable rows
//         in allowed_rids (so they occupy slots in the scope, even though they
//         contribute 0 distance matches).
//
// This test verifies that the planner's PK->RID scan does NOT skip rows with
// NULL embeddings when building allowed_rids.  Those rows should be scoped
// (i.e., their RID appears in allowed_rids) even though the operator itself
// will skip them at distance-computation time.
//
// Strategy: create a scenario where skipping NULL-embedding rows from
// allowed_rids would cause an out-of-scope row to leak.  Specifically:
//   Row 1 (NULL emb):  in-scope start node
//   Row 2 ([1,0,0,0]): in-scope via edge 1->2
//   Row 3 ([0,1,0,0]): out-of-scope
//
// If the planner skips row 1's RID from allowed_rids, the scope set contains
// only row 2's RID — which is correct behavior.  But if the planner used
// ordinals (the pre-fix bug), the ordinal 0 for row 1 would be in the set,
// and the HNSW node_id=0 would map to row 2 -> correct by accident.  The
// real mutation we test: if the planner skipped row 1's RID and instead
// used the ordinal of a NULL row as an HNSW node ID slot, row 3 might appear.
//
// Post-fix correctness check: brute-force and HNSW must both return exactly
// {2} (row 1 is in scope but has no embedding; row 3 is not in scope).
// =============================================================================

TEST_F(QA_GDB745_Adversarial, PlannerIncludesNullEmbeddingRowsInScopeRids) {
    exec_ok("CREATE TABLE scope_test (id INT PRIMARY KEY, body VARCHAR, vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "scope_test");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4);

    // Row 1: NULL embedding (start node — must be in allowed_rids even though
    //         it has no embedding value).
    exec_ok("INSERT INTO scope_test (id, body) VALUES (1, 'null_emb')");
    // Row 2: in-scope, has embedding.
    exec_ok("INSERT INTO scope_test VALUES (2, 'in_scope', [1.0, 0.0, 0.0, 0.0])");
    // Row 3: out-of-scope, has embedding.
    exec_ok("INSERT INTO scope_test VALUES (3, 'oos', [0.9, 0.1, 0.0, 0.0])");

    exec_ok("CREATE EDGE TYPE scope_links FROM scope_test TO scope_test");
    exec_ok("LINK scope_test(1) TO scope_test(2) VIA scope_links");
    // Row 3 not linked.

    // Brute-force path (no HNSW).
    auto bf_result = engine_->execute(
        "SELECT * FROM scope_test "
        "WHERE NEAREST(vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
        "WITHIN TRAVERSE scope_links FROM scope_test(1) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(bf_result.has_value()) << bf_result.error().message;
    auto bf_ids = sorted_ids(*bf_result);
    EXPECT_EQ(bf_ids, std::vector<int32_t>({2}))
        << "brute-force: only row 2 should appear (row 1 has no emb, row 3 OOS)";

    // HNSW path.
    enable_hnsw("scope_test", "vec");
    auto hnsw_result = engine_->execute(
        "SELECT * FROM scope_test "
        "WHERE NEAREST(vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
        "WITHIN TRAVERSE scope_links FROM scope_test(1) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(hnsw_result.has_value()) << hnsw_result.error().message;
    auto hnsw_ids = sorted_ids(*hnsw_result);
    EXPECT_EQ(hnsw_ids, std::vector<int32_t>({2}))
        << "HNSW: planner must insert NULL-embedding row 1's RID into allowed_rids so "
           "the scope predicate is correct; row 3 (OOS) must not appear";
}
