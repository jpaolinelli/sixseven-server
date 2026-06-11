/// @file test_qa_gdb_745.cpp
/// @brief QA regression tests for GDB-745: graph-scoped NEAREST via HNSW
/// filtered on the wrong node-id space (heap ordinals vs HNSW node ids).
///
/// Pre-fix, the planner populated the scope set with heap ordinals (counting
/// every row, including NULL embeddings) while the HNSW filter predicate
/// compared them against HNSW node ids (assigned only to rows with non-null
/// embeddings). With a NULL-embedding row before an in-scope row, the spaces
/// shift: the filter silently returns out-of-scope rows and drops in-scope
/// rows. The fix scopes by heap RID — the one canonical id space — on every
/// NEAREST execution path.

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
// Fixture: full SQL pipeline with graph engine + HNSW index manager
// =============================================================================

class QA_GDB745 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb745";
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

    /// Register embedding column metadata (dimension) so the HNSW rebuild
    /// can size the index.
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

    /// Create an HNSW IndexDef on the column and rebuild from table data.
    /// After this, graph-scoped NEAREST exercises the HNSW fast path
    /// (filter predicate + node_id → RID map).
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
// Core repro: NULL-embedding row before in-scope rows shifts the id spaces
// =============================================================================

TEST_F(QA_GDB745, HnswScopeNotShiftedByNullEmbeddingRow) {
    exec_ok("CREATE TABLE notes (id INT PRIMARY KEY, body VARCHAR, body_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "notes");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4);

    // Heap ordinals:           0 (NULL emb), 1, 2, 3
    // HNSW node ids (non-null):              0, 1, 2
    exec_ok("INSERT INTO notes (id, body) VALUES (1, 'pending')");
    exec_ok("INSERT INTO notes VALUES (2, 'alpha', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO notes VALUES (3, 'beta', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO notes VALUES (4, 'gamma', [0.0, 1.0, 0.0, 0.0])");

    enable_hnsw("notes", "body_vec");

    // Graph: 2 -> 3. Reachable set from 2 is exactly {2, 3}.
    exec_ok("CREATE EDGE TYPE links FROM notes TO notes");
    exec_ok("LINK notes(2) TO notes(3) VIA links");

    auto result = engine_->execute("SELECT * FROM notes "
                                   "WHERE NEAREST(body_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                                   "WITHIN TRAVERSE links FROM notes(2) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Pre-fix: ordinal scope {1, 2} interpreted as HNSW node ids selects
    // rows 3 and 4 — out-of-scope row 4 returned, in-scope row 2 dropped.
    auto ids = sorted_ids(*result);
    ASSERT_EQ(ids.size(), 2u) << "graph scope selected the wrong number of rows";
    EXPECT_EQ(ids[0], 2) << "in-scope row 2 was dropped by the scope filter";
    EXPECT_EQ(ids[1], 3);
}

// =============================================================================
// Brute-force path must agree with the HNSW path (same canonical space)
// =============================================================================

TEST_F(QA_GDB745, BruteForceAndHnswAgreeWithNullEmbeddingRow) {
    exec_ok("CREATE TABLE docs (id INT PRIMARY KEY, body VARCHAR, body_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "docs");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4);

    exec_ok("INSERT INTO docs (id, body) VALUES (1, 'pending')");
    exec_ok("INSERT INTO docs VALUES (2, 'alpha', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (3, 'beta', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (4, 'gamma', [0.0, 1.0, 0.0, 0.0])");

    exec_ok("CREATE EDGE TYPE refs FROM docs TO docs");
    exec_ok("LINK docs(2) TO docs(3) VIA refs");

    const std::string query = "SELECT * FROM docs "
                              "WHERE NEAREST(body_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE refs FROM docs(2) DIRECTION OUT MAX_DEPTH 2";

    // Brute-force (no HNSW index yet).
    auto bf = engine_->execute(query);
    ASSERT_TRUE(bf.has_value()) << bf.error().message;
    auto bf_ids = sorted_ids(*bf);

    // HNSW path.
    enable_hnsw("docs", "body_vec");
    auto hnsw = engine_->execute(query);
    ASSERT_TRUE(hnsw.has_value()) << hnsw.error().message;
    auto hnsw_ids = sorted_ids(*hnsw);

    std::vector<int32_t> expected = {2, 3};
    EXPECT_EQ(bf_ids, expected);
    EXPECT_EQ(hnsw_ids, expected) << "HNSW path disagrees with brute-force scope";
}

// =============================================================================
// Deleted row after index build: stale HNSW node must not leak or shift scope
// =============================================================================

TEST_F(QA_GDB745, HnswScopeWithRowDeletedAfterIndexBuild) {
    exec_ok("CREATE TABLE pages (id INT PRIMARY KEY, body VARCHAR, body_vec EMBEDDING)");
    auto schema = catalog_.get_table(default_database_id, "pages");
    ASSERT_TRUE(schema.has_value());
    register_embedding(schema->table_id, 2, 4);

    exec_ok("INSERT INTO pages VALUES (1, 'alpha', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO pages VALUES (2, 'beta', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO pages VALUES (3, 'gamma', [0.8, 0.2, 0.0, 0.0])");
    exec_ok("INSERT INTO pages VALUES (4, 'delta', [0.0, 1.0, 0.0, 0.0])");

    exec_ok("CREATE EDGE TYPE chain FROM pages TO pages");
    exec_ok("LINK pages(1) TO pages(2) VIA chain");
    exec_ok("LINK pages(2) TO pages(3) VIA chain");

    // Build the HNSW index with all four rows, THEN delete row 2. The HNSW
    // node for row 2 is now stale; scoping must not shift onto other rows.
    enable_hnsw("pages", "body_vec");
    exec_ok("DELETE FROM pages WHERE id = 2");

    auto result = engine_->execute("SELECT * FROM pages "
                                   "WHERE NEAREST(body_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                                   "WITHIN TRAVERSE chain FROM pages(1) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Reachable {1, 2, 3}; row 2 deleted; row 4 out of scope.
    auto ids = sorted_ids(*result);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 1);
    EXPECT_EQ(ids[1], 3);
}
