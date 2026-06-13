/// @file test_qa_gdb_1257.cpp
/// @brief QA adversarial regression tests for GDB-1257.
///
/// GDB-1257: An empty WITHIN TRAVERSE graph scope previously leaked the GLOBAL
/// NEAREST results. The NearestScan operator gated graph-scope filtering on
/// `!allowed_rids.empty()`, conflating "a scope was applied and resolved to
/// ZERO rows" with "no scope applied". The fix carries an explicit
/// `bool graph_scoped` in NearestScanConfig (set by the planner whenever a
/// WITHIN TRAVERSE clause is present) and short-circuits do_open() to emit zero
/// rows when `graph_scoped && allowed_rids.empty()`, ahead of the prefiltered,
/// HNSW, and brute-force paths.
///
/// These tests try to BREAK the fix with adversarial edge cases:
///   * empty scope across HNSW and brute-force paths
///   * scope resolving to exactly k
///   * self-reaching homogeneous edges
///   * multi-hop traversal resolving empty
///   * UNSCOPED queries must NOT be over-corrected to empty
///   * interaction with LIMIT / ORDER BY
///   * repeated open/close (operator-level re-execution stability)

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"
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

namespace {

class QA_GDB1257 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1257";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
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

    void exec_ok(const std::string& sql) {
        auto r = engine_->execute(sql);
        ASSERT_TRUE(r.has_value()) << sql << "\n -> " << r.error().message;
    }

    void register_embedding(table_id_t table_id, int32_t col_id, int32_t dim,
                            const std::string& source, const std::string& provider) {
        EmbeddingColumnDef emb_def;
        emb_def.table_id = table_id;
        emb_def.column_id = col_id;
        emb_def.dimension = dim;
        emb_def.source_expr = source;
        emb_def.provider = provider;
        auto reg = catalog_.register_embedding_column(emb_def);
        ASSERT_TRUE(reg.has_value()) << reg.error().message;
        if (catalog_.get_embedding_provider(provider).has_value()) {
            return;
        }
        EmbeddingProviderConfig prov_config;
        prov_config.name = provider;
        prov_config.type = "builtin";
        prov_config.dimension = dim;
        ASSERT_TRUE(catalog_.register_embedding_provider(prov_config).has_value());
    }

    void setup_schema() {
        exec_ok("CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR, description_vec EMBEDDING)");
        exec_ok("CREATE TABLE readers (id INT PRIMARY KEY, name VARCHAR)");
        auto books = catalog_.get_table(default_database_id, "books");
        ASSERT_TRUE(books.has_value());
        register_embedding(books->table_id, 2, 4, "title", "builtin/4");
        engine_->set_provider_registry(provider_registry_.get());
        exec_ok("CREATE EDGE TYPE reads FROM readers TO books");
        exec_ok("CREATE EDGE TYPE follows FROM readers TO readers");
        exec_ok("CREATE EDGE TYPE similar_to FROM books TO books");
    }

    std::vector<int32_t> ids_of(const QueryResult& qr) {
        std::vector<int32_t> ids;
        for (const auto& row : qr.rows) {
            if (!row.empty()) {
                ids.push_back(row[0].as_int32());
            }
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    std::filesystem::path data_dir_;
    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<ProviderRegistry> provider_registry_;
};

// --- Core fix: empty scope, brute-force path (small table, no HNSW) ----------
// Reader 1 reads no books. Brute-force path. Must emit 0 rows, not all books.
TEST_F(QA_GDB1257, EmptyScopeBruteForceEmitsZero) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    exec_ok("INSERT INTO books VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (2, 'b', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (3, 'c', [0.0, 0.0, 1.0, 0.0])");
    auto r = engine_->execute("SELECT id FROM books "
                              "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->rows.size(), 0u) << "empty scope leaked global results";
}

// --- Empty scope, k larger than table size -----------------------------------
// k=100 with empty scope: must still be 0, not min(k, table_size).
TEST_F(QA_GDB1257, EmptyScopeLargeKEmitsZero) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    for (int i = 1; i <= 10; ++i) {
        exec_ok("INSERT INTO books VALUES (" + std::to_string(i) + ", 't', [1.0, 0.0, 0.0, 0.0])");
    }
    auto r = engine_->execute("SELECT id FROM books "
                              "WHERE NEAREST(description_vec, 100) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->rows.size(), 0u);
}

// --- Empty scope, k == 1 boundary --------------------------------------------
TEST_F(QA_GDB1257, EmptyScopeKOneEmitsZero) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    exec_ok("INSERT INTO books VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
    auto r = engine_->execute("SELECT id FROM books "
                              "WHERE NEAREST(description_vec, 1) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 1");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->rows.size(), 0u);
}

// --- Empty scope at scale (stress brute-force) -------------------------------
// 500 books, none linked. Must be 0, proving the short-circuit fires before
// any per-row distance work matters for correctness.
TEST_F(QA_GDB1257, EmptyScopeManyBooksEmitsZero) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    for (int i = 1; i <= 500; ++i) {
        exec_ok("INSERT INTO books VALUES (" + std::to_string(i) +
                ", 't', [1.0, 0.0, 0.0, 0.0])");
    }
    auto r = engine_->execute("SELECT id FROM books "
                              "WHERE NEAREST(description_vec, 10) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 3");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->rows.size(), 0u);
}

// --- Multi-hop traversal that resolves empty ---------------------------------
// reader 1 follows reader 2; neither reads any book. reads OUT depth 3 from
// reader 1 still reaches zero books. Must be 0.
TEST_F(QA_GDB1257, MultiHopResolvingEmptyEmitsZero) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    exec_ok("INSERT INTO readers VALUES (2, 'bob')");
    exec_ok("INSERT INTO books VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (2, 'b', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("LINK readers(1) TO readers(2) VIA follows");
    auto r = engine_->execute("SELECT id FROM books "
                              "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 3");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->rows.size(), 0u);
}

// --- Self-reaching homogeneous edge, no outgoing links -----------------------
// similar_to OUT from book 1 with no links: scope = {book 1} (start seeded for
// homogeneous edge). NOT empty. Must return exactly the start book, proving the
// short-circuit does NOT fire for a non-empty single-element scope.
TEST_F(QA_GDB1257, SelfReachingStartOnlyScopeNotEmptied) {
    setup_schema();
    exec_ok("INSERT INTO books VALUES (1, 'start', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (2, 'far', [0.0, 0.0, 1.0, 0.0])");
    auto r = engine_->execute("SELECT id FROM books "
                              "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE similar_to FROM books(1) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto ids = ids_of(*r);
    ASSERT_EQ(ids.size(), 1u) << "start-only scope should yield exactly the start book";
    EXPECT_EQ(ids[0], 1);
}

// --- Scope resolving to exactly k --------------------------------------------
// reader 1 reads exactly 3 books; k=3. Must return exactly those 3 (no global
// leak, no under-fill).
TEST_F(QA_GDB1257, ScopeExactlyKReturnsScopedSet) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    exec_ok("INSERT INTO books VALUES (10, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (11, 'b', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (12, 'c', [0.8, 0.2, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (99, 'unreached', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("LINK readers(1) TO books(10) VIA reads");
    exec_ok("LINK readers(1) TO books(11) VIA reads");
    exec_ok("LINK readers(1) TO books(12) VIA reads");
    auto r = engine_->execute("SELECT id FROM books "
                              "WHERE NEAREST(description_vec, 3) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 1");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto ids = ids_of(*r);
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 11);
    EXPECT_EQ(ids[2], 12);
    EXPECT_EQ(std::count(ids.begin(), ids.end(), 99), 0) << "unreached book leaked";
}

// --- No over-correction: UNSCOPED query still searches globally --------------
// No WITHIN TRAVERSE → graph_scoped=false → must return global nearest. The fix
// must NOT make all NEAREST queries empty.
TEST_F(QA_GDB1257, UnscopedQueryStillReturnsGlobalResults) {
    setup_schema();
    exec_ok("INSERT INTO books VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (2, 'b', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (3, 'c', [0.0, 0.0, 1.0, 0.0])");
    auto r = engine_->execute("SELECT id FROM books "
                              "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0]");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->rows.size(), 3u) << "unscoped NEAREST must search globally";
}

// --- No over-correction on empty table, unscoped -----------------------------
// Unscoped NEAREST against an empty table is legitimately 0 rows; ensure it is
// not conflated with the scoped-empty short-circuit (both yield 0 but via
// different paths — this guards against accidental coupling).
TEST_F(QA_GDB1257, UnscopedEmptyTableEmitsZero) {
    setup_schema();
    auto r = engine_->execute("SELECT id FROM books "
                              "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0]");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->rows.size(), 0u);
}

// --- Interaction with LIMIT --------------------------------------------------
// Empty scope + LIMIT 10: LIMIT over an empty input is still empty.
TEST_F(QA_GDB1257, EmptyScopeWithLimitEmitsZero) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    exec_ok("INSERT INTO books VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (2, 'b', [0.0, 1.0, 0.0, 0.0])");
    auto r = engine_->execute("SELECT id FROM books "
                              "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 2 "
                              "LIMIT 10");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->rows.size(), 0u);
}

// --- Interaction with ORDER BY -----------------------------------------------
// Empty scope + ORDER BY id: ordering an empty set is empty.
TEST_F(QA_GDB1257, EmptyScopeWithOrderByEmitsZero) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    exec_ok("INSERT INTO books VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (2, 'b', [0.0, 1.0, 0.0, 0.0])");
    auto r = engine_->execute("SELECT id FROM books "
                              "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 2 "
                              "ORDER BY id");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->rows.size(), 0u);
}

// --- Non-empty scope with LIMIT below scope size -----------------------------
// reader reads 3 books, LIMIT 2: must return 2 of the scoped set, not global.
TEST_F(QA_GDB1257, NonEmptyScopeWithLimitClampsScopedSet) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    exec_ok("INSERT INTO books VALUES (10, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (11, 'b', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (12, 'c', [0.8, 0.2, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (99, 'unreached', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("LINK readers(1) TO books(10) VIA reads");
    exec_ok("LINK readers(1) TO books(11) VIA reads");
    exec_ok("LINK readers(1) TO books(12) VIA reads");
    auto r = engine_->execute("SELECT id FROM books "
                              "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 1 "
                              "LIMIT 2");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto ids = ids_of(*r);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(std::count(ids.begin(), ids.end(), 99), 0) << "unreached book leaked under LIMIT";
}

// --- Repeated execution: empty scope must stay empty on re-run ---------------
// Re-running the same scoped-empty query must remain 0 (no stale state from a
// prior run leaking results — guards do_open() result/seen-set reset).
TEST_F(QA_GDB1257, RepeatedEmptyScopeStaysEmpty) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    exec_ok("INSERT INTO books VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (2, 'b', [0.0, 1.0, 0.0, 0.0])");
    const std::string q = "SELECT id FROM books "
                          "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                          "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 2";
    for (int i = 0; i < 3; ++i) {
        auto r = engine_->execute(q);
        ASSERT_TRUE(r.has_value()) << r.error().message;
        EXPECT_EQ(r->rows.size(), 0u) << "run " << i;
    }
}

// --- Scope becomes empty after a prior non-empty scoped query ----------------
// Run a non-empty scoped query, then an empty-scope query, to ensure the
// empty case is not contaminated by the planner/operator state of the prior
// non-empty case.
TEST_F(QA_GDB1257, EmptyScopeAfterNonEmptyScopeStaysEmpty) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    exec_ok("INSERT INTO readers VALUES (2, 'bob')");
    exec_ok("INSERT INTO books VALUES (10, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (11, 'b', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("LINK readers(1) TO books(10) VIA reads");

    auto r1 = engine_->execute("SELECT id FROM books "
                               "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                               "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 1");
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    EXPECT_EQ(r1->rows.size(), 1u);

    // reader 2 reads nothing → empty scope.
    auto r2 = engine_->execute("SELECT id FROM books "
                               "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                               "WITHIN TRAVERSE reads FROM readers(2) DIRECTION OUT MAX_DEPTH 1");
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    EXPECT_EQ(r2->rows.size(), 0u) << "empty scope leaked after prior non-empty scope";
}

} // namespace
