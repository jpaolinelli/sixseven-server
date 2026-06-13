/// @file test_qa_gdb_1251.cpp
/// @brief QA adversarial regression tests for GDB-1251.
///
/// GDB-1251: `NEAREST(...) WITHIN TRAVERSE ...` must validate that the
/// traversal's REACHED node table is the table that owns the EMBEDDING column.
/// When the edge reaches a different table the planner previously intersected
/// reachable PKs with the vector table's PKs by raw numeric equality, producing
/// a silent garbage scope. The fix (src/executor/planner.cpp plan_nearest_impl):
///   * OUT  → target table must equal vector table
///   * IN   → source table must equal vector table
///   * BOTH → both endpoints must equal vector table
/// plus: start-key coercion uses the START table's PK type, and the start node
/// is only seeded into scope when the start table IS the vector table.
///
/// These tests try to BREAK the fix with edge cases beyond the dev unit suite:
/// error-message exactness, self-loop edges, MAX_DEPTH boundaries, multi-hop
/// mixed reachability, non-existent edge types, and start-seed collisions under
/// IN/BOTH directions.

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

class QA_NearestTraverseScope : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1251";
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

    /// books(id INT PK, title, description_vec EMBEDDING),
    /// readers(id INT PK, name), edges: reads(readers→books),
    /// follows(readers→readers), similar_to(books→books).
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

// --- Error-message exactness for the canonical repro -------------------------

// The error must name the edge, the reached table, and the embedding column's
// owning table. Adversarial: assert all three substrings AND the INVALID_ARGUMENT
// code, not just that it failed.
TEST_F(QA_NearestTraverseScope, GDB1251_ReproErrorNamesEdgeReachedAndVectorTable) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    auto r = engine_->execute("SELECT title FROM books "
                              "WHERE NEAREST(description_vec, 3) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE follows FROM readers(1) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    const std::string& m = r.error().message;
    EXPECT_NE(m.find("follows"), std::string::npos) << m;
    EXPECT_NE(m.find("readers"), std::string::npos) << m;
    EXPECT_NE(m.find("books"), std::string::npos) << m;
}

// --- Non-existent edge type --------------------------------------------------

// Adversarial: an undefined edge type in WITHIN TRAVERSE must error cleanly,
// not crash or fall through to a garbage scope.
TEST_F(QA_NearestTraverseScope, GDB1251_NonexistentEdgeTypeErrors) {
    setup_schema();
    exec_ok("INSERT INTO books VALUES (1, 'b', [1.0, 0.0, 0.0, 0.0])");
    auto r = engine_->execute("SELECT title FROM books "
                              "WHERE NEAREST(description_vec, 3) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE no_such_edge FROM books(1) DIRECTION OUT MAX_DEPTH 1");
    ASSERT_FALSE(r.has_value());
    // Must not be a silent success / garbage scope.
    EXPECT_NE(r.error().code, StatusCode::OK);
}

// --- Start-seed collision regression under DIRECTION IN ----------------------

// reads DIRECTION IN starts from a BOOK and reaches readers → must error even
// though a reader id may numerically collide with the start book's id. The fix
// must reject BEFORE seeding the start node.
TEST_F(QA_NearestTraverseScope, GDB1251_ReadsDirectionInRejectedNoStartLeak) {
    setup_schema();
    exec_ok("INSERT INTO books VALUES (1, 'book_one', [1.0, 0.0, 0.0, 0.0])");
    auto r = engine_->execute("SELECT id FROM books "
                              "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE reads FROM books(1) DIRECTION IN MAX_DEPTH 1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(r.error().message.find("readers"), std::string::npos) << r.error().message;
}

// --- MAX_DEPTH boundary: depth 1 reaches direct neighbors only ---------------

// Adversarial multi-hop: reader 1 reads book 10; book 10 similar_to book 20.
// Traversing `reads` OUT MAX_DEPTH 1 from reader 1 reaches ONLY book 10 (reads
// is a single hop; it does not chain into similar_to). Confirm scope is exactly
// {10}, proving no over-broad reachability.
TEST_F(QA_NearestTraverseScope, GDB1251_ReadsDepthOneReachesDirectNeighborOnly) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    exec_ok("INSERT INTO books VALUES (10, 'reached', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (20, 'two_hops', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("LINK readers(1) TO books(10) VIA reads");
    exec_ok("LINK books(10) TO books(20) VIA similar_to");

    auto r = engine_->execute("SELECT id FROM books "
                              "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 1");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto ids = ids_of(*r);
    ASSERT_EQ(ids.size(), 1u) << "expected only directly-read book 10";
    EXPECT_EQ(ids[0], 10);
}

// --- Empty reachable set: valid edge, no links ------------------------------
//
// FIXED (GDB-1257, in src/executor/nearest_scan.cpp): previously an empty graph
// scope was indistinguishable from "no scope applied". The NearestScan operator
// gated on `!allowed_rids.empty()`, so when a valid WITHIN TRAVERSE resolved to
// ZERO reachable rows, the filter was bypassed and NEAREST returned the GLOBAL
// nearest neighbors instead of nothing.
//
// GDB-1251 makes heterogeneous `reads` traversals legal, which is exactly how a
// legitimately-empty scope arises (a reader who reads no books). GDB-1257 fixes
// the root cause so an empty-but-applied scope now correctly emits zero rows.
//
// This test pins the FIXED behavior: an empty graph scope emits exactly 0 rows.
TEST_F(QA_NearestTraverseScope, GDB1257_EmptyScopeEmitsZeroRows) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    exec_ok("INSERT INTO books VALUES (1, 'collide', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (2, 'other', [0.0, 1.0, 0.0, 0.0])");
    // No LINK at all → reader 1 reaches zero books. Correct answer is 0 rows.
    auto r = engine_->execute("SELECT id FROM books "
                              "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    // An applied-but-empty graph scope must emit zero rows (no global leak).
    EXPECT_EQ(r->rows.size(), 0u)
        << "GDB-1257: empty graph scope must emit 0 rows, not leak global NEAREST results.";
}

// --- Homogeneous self-reaching edge: similar_to OUT keeps start in scope -----

// For a homogeneous edge (books→books) the start node IS the vector table, so
// the start book must remain in scope (regression that the start-seed guard did
// not over-restrict homogeneous traversals).
TEST_F(QA_NearestTraverseScope, GDB1251_HomogeneousStartNodeStaysInScope) {
    setup_schema();
    exec_ok("INSERT INTO books VALUES (1, 'start', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (2, 'neighbor', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (3, 'far', [0.0, 0.0, 1.0, 0.0])");
    exec_ok("LINK books(1) TO books(2) VIA similar_to");

    auto r = engine_->execute("SELECT id FROM books "
                              "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE similar_to FROM books(1) DIRECTION OUT MAX_DEPTH 1");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto ids = ids_of(*r);
    // Scope must contain the start node (1) and reached neighbor (2), exclude 3.
    EXPECT_NE(std::find(ids.begin(), ids.end(), 1), ids.end()) << "start node dropped from scope";
    EXPECT_NE(std::find(ids.begin(), ids.end(), 2), ids.end());
    EXPECT_EQ(std::find(ids.begin(), ids.end(), 3), ids.end());
}

// --- Self-loop heterogeneous edge under BOTH: reads BOTH must error ----------

// DIRECTION BOTH on reads (readers→books) mixes two PK id-spaces; must error
// regardless of start side.
TEST_F(QA_NearestTraverseScope, GDB1251_ReadsBothFromReaderErrors) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    auto r = engine_->execute("SELECT title FROM books "
                              "WHERE NEAREST(description_vec, 3) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE reads FROM readers(1) DIRECTION BOTH MAX_DEPTH 2");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// --- Heterogeneous STRING-PK start key coercion regression -------------------

// Start key 'alice' must coerce against readers' STRING PK, not books' INT PK.
// A non-coercible mismatch would otherwise fail or build a garbage scope.
TEST_F(QA_NearestTraverseScope, GDB1251_StringStartKeyCoercedAgainstStartTable) {
    exec_ok("CREATE TABLE qbooks (id INT PRIMARY KEY, title VARCHAR, description_vec EMBEDDING)");
    exec_ok("CREATE TABLE qreaders (id VARCHAR PRIMARY KEY, name VARCHAR)");
    auto books = catalog_.get_table(default_database_id, "qbooks");
    ASSERT_TRUE(books.has_value());
    register_embedding(books->table_id, 2, 4, "title", "builtin/4");
    engine_->set_provider_registry(provider_registry_.get());
    exec_ok("CREATE EDGE TYPE qreads FROM qreaders TO qbooks");
    exec_ok("INSERT INTO qreaders VALUES ('alice', 'Alice')");
    exec_ok("INSERT INTO qbooks VALUES (10, 'reached', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO qbooks VALUES (11, 'unreached', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("LINK qreaders('alice') TO qbooks(10) VIA qreads");

    auto r = engine_->execute("SELECT id FROM qbooks "
                              "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                              "WITHIN TRAVERSE qreads FROM qreaders('alice') DIRECTION OUT MAX_DEPTH 1");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto ids = ids_of(*r);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], 10);
}

// =============================================================================
// AC-mapped regression suite (QA_GDB1251): one test per acceptance criterion,
// preserved alongside the adversarial suite above so each AC has a named,
// 1:1 traceable test.
// =============================================================================

class QA_GDB1251 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1251";
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
        auto result = engine_->execute(sql);
        ASSERT_TRUE(result.has_value()) << "SQL failed: " << sql << "\n" << result.error().message;
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

        if (catalog_.get_embedding_provider(provider).has_value()) {
            return;
        }
        EmbeddingProviderConfig prov_config;
        prov_config.name = provider;
        prov_config.type = "builtin";
        prov_config.dimension = dim;
        auto prov_reg = catalog_.register_embedding_provider(prov_config);
        ASSERT_TRUE(prov_reg.has_value()) << prov_reg.error().message;
    }

    /// books(id INT PK, title, description_vec EMBEDDING),
    /// readers(id INT PK, name),
    /// edges: reads(readers->books), follows(readers->readers),
    /// similar_to(books->books).
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

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<ProviderRegistry> provider_registry_;
};


TEST_F(QA_GDB1251, AC1_FollowsReachesReadersButNearestOnBooks_Errors) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");

    auto result =
        engine_->execute("SELECT title FROM books "
                         "WHERE NEAREST(description_vec, 3) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE follows FROM readers(1) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_FALSE(result.has_value()) << "follows reaches readers, NEAREST is on books";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    const std::string& msg = result.error().message;
    EXPECT_NE(msg.find("follows"), std::string::npos) << msg;
    EXPECT_NE(msg.find("readers"), std::string::npos) << msg;
    EXPECT_NE(msg.find("books"), std::string::npos) << msg;
}

// =============================================================================
// AC2: heterogeneous edge ENDING at the vector table is allowed; the start-node
// PK collision must not leak into scope.
// =============================================================================

TEST_F(QA_GDB1251, AC2_ReadsEndingAtBooks_AllowedAndStartCollisionExcluded) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    // Book id 1 numerically collides with reader 1's id but is NOT reached.
    exec_ok("INSERT INTO books VALUES (1, 'collision', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (10, 'reached_a', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (11, 'reached_b', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (12, 'unreached', [0.0, 0.0, 1.0, 0.0])");

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
    EXPECT_EQ(std::count(ids.begin(), ids.end(), 1), 0)
        << "start-node PK collision (book id 1) leaked into scope";
    EXPECT_EQ(std::count(ids.begin(), ids.end(), 12), 0) << "unreached book leaked into scope";
}

// =============================================================================
// AC3: direction matrix.
// =============================================================================

// reads DIRECTION IN from a book reaches readers -> error.
TEST_F(QA_GDB1251, AC3_ReadsDirectionIn_ReachesReaders_Errors) {
    setup_schema();
    exec_ok("INSERT INTO books VALUES (1, 'b', [1.0, 0.0, 0.0, 0.0])");

    auto result = engine_->execute("SELECT title FROM books "
                                   "WHERE NEAREST(description_vec, 3) TO [1.0, 0.0, 0.0, 0.0] "
                                   "WITHIN TRAVERSE reads FROM books(1) DIRECTION IN MAX_DEPTH 2");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(result.error().message.find("readers"), std::string::npos) << result.error().message;
}

// similar_to DIRECTION IN (books->books) reaches books -> allowed.
TEST_F(QA_GDB1251, AC3_SimilarToDirectionIn_ReachesBooks_Allowed) {
    setup_schema();
    exec_ok("INSERT INTO books VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (2, 'b', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("LINK books(1) TO books(2) VIA similar_to");

    auto result =
        engine_->execute("SELECT id FROM books "
                         "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE similar_to FROM books(2) DIRECTION IN MAX_DEPTH 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_GE(result->rows.size(), 1u);
}

// reads DIRECTION BOTH (heterogeneous) -> error.
TEST_F(QA_GDB1251, AC3_ReadsDirectionBoth_Heterogeneous_Errors) {
    setup_schema();
    exec_ok("INSERT INTO books VALUES (1, 'b', [1.0, 0.0, 0.0, 0.0])");

    auto result =
        engine_->execute("SELECT title FROM books "
                         "WHERE NEAREST(description_vec, 3) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE reads FROM books(1) DIRECTION BOTH MAX_DEPTH 2");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// similar_to DIRECTION BOTH (homogeneous) -> allowed.
TEST_F(QA_GDB1251, AC3_SimilarToDirectionBoth_Homogeneous_Allowed) {
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
// AC4: start key is coerced against the START table's PK type (STRING), not the
// vector table's PK type (INT).
// =============================================================================

TEST_F(QA_GDB1251, AC4_StringPkStartKey_CoercedAgainstStartTable) {
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

} // namespace
