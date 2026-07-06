/// @file test_qa_gdb_1241.cpp
/// @brief QA regression tests for GDB-1241: the NEAREST(...) parser must accept
/// WITHIN TRAVERSE and USING clauses in either order without silently dropping
/// the graph scope. Before this fix, `USING <metric> WITHIN TRAVERSE ...`
/// (USING first) caused the WITHIN TRAVERSE clause to never be parsed at all,
/// so the query silently ran unscoped and returned out-of-scope rows.
///
/// These tests exercise the FULL end-to-end pipeline (parse -> bind -> plan ->
/// execute) against a real graph + vector dataset where the traverse scope
/// genuinely restricts the candidate set, not just the parse tree.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
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

#include "test_qa_helpers.h"

using namespace sixseven;

namespace {

// Dataset: readers --reads--> books.
// Reader 1 reads books 10 and 11 only. Books 1 (PK-collides with reader 1),
// and 12 are NOT reached by the traverse and must never appear in a scoped
// result set.
//
// Cosine distance of query [1,0,0,0] against:
//   book 1  [1.0,0.0,0.0,0.0]  -> 0.0     (unreached, PK collision trap)
//   book 10 [0.9,0.1,0.0,0.0]  -> ~0.006  (reached)
//   book 11 [0.0,1.0,0.0,0.0]  -> 1.0     (reached)
//   book 12 [0.0,0.0,1.0,0.0]  -> 1.0     (unreached)
//
// With k=5 (larger than the whole table), an UNSCOPED query returns all 4
// books. A correctly SCOPED query (regardless of clause order) must return
// exactly {10, 11}.
class QA_GDB1241 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1241";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        bootstrap_qa_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());
        provider_registry_ = std::make_unique<ProviderRegistry>(catalog_);
        engine_->set_provider_registry(provider_registry_.get());

        seed();
    }

    void TearDown() override {
        engine_.reset();
        provider_registry_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    Result<QueryResult> run(const std::string& sql) { return engine_->execute(sql); }

    void register_embedding(table_id_t table_id, int32_t col_id, int32_t dim) {
        EmbeddingColumnDef emb;
        emb.table_id = table_id;
        emb.column_id = col_id;
        emb.dimension = dim;
        emb.source_expr = "title";
        emb.provider = "builtin/4";
        auto reg = catalog_.register_embedding_column(emb);
        ASSERT_TRUE(reg.has_value()) << reg.error().message;

        auto existing = catalog_.get_embedding_provider("builtin/4");
        if (existing.has_value())
            return;
        EmbeddingProviderConfig prov_config;
        prov_config.name = "builtin/4";
        prov_config.type = "builtin";
        prov_config.dimension = dim;
        auto prov_reg = catalog_.register_embedding_provider(prov_config);
        ASSERT_TRUE(prov_reg.has_value()) << prov_reg.error().message;
    }

    void seed() {
        exec_ok(
            "CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR, description_vec EMBEDDING)");
        exec_ok("CREATE TABLE readers (id INT PRIMARY KEY, name VARCHAR)");

        auto books = catalog_.get_table(default_database_id, "books");
        ASSERT_TRUE(books.has_value());
        register_embedding(books->table_id, 2, 4);

        exec_ok("CREATE EDGE TYPE reads FROM readers TO books");

        exec_ok("INSERT INTO readers VALUES (1, 'alice')");

        // Book id 1 numerically collides with reader 1's PK but is NOT reached.
        exec_ok("INSERT INTO books VALUES (1, 'collision', [1.0, 0.0, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (10, 'reached_a', [0.9, 0.1, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (11, 'reached_b', [0.0, 1.0, 0.0, 0.0])");
        exec_ok("INSERT INTO books VALUES (12, 'unreached', [0.0, 0.0, 1.0, 0.0])");

        exec_ok("LINK readers(1) TO books(10) VIA reads");
        exec_ok("LINK readers(1) TO books(11) VIA reads");
    }

    static std::vector<int32_t> sorted_ids(const QueryResult& qr) {
        std::vector<int32_t> ids;
        for (const auto& row : qr.rows) {
            ids.push_back(row[0].as_int32());
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    std::filesystem::path data_dir_;
    Catalog catalog_;
    DiskManager dm_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<ProviderRegistry> provider_registry_;
};

// =============================================================================
// AC (core): correct order (WITHIN before USING) scopes the result set.
// =============================================================================
TEST_F(QA_GDB1241, WithinBeforeUsingScopesResultSet) {
    auto result =
        run("SELECT id FROM books "
            "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
            "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 1 "
            "USING DOT");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto ids = sorted_ids(*result);
    ASSERT_EQ(ids.size(), 2u) << "expected exactly the reached books {10, 11}";
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 11);
}

// =============================================================================
// AC (core, the actual bug): reversed order (USING before WITHIN) must scope
// IDENTICALLY. Before the fix, USING-first silently dropped WITHIN TRAVERSE,
// so this query would return all 4 books (unscoped) instead of {10, 11}.
// =============================================================================
TEST_F(QA_GDB1241, UsingBeforeWithinScopesIdenticallyToWithinBeforeUsing) {
    auto correct_order =
        run("SELECT id FROM books "
            "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
            "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 1 "
            "USING DOT");
    auto reversed_order =
        run("SELECT id FROM books "
            "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
            "USING DOT "
            "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 1");

    ASSERT_TRUE(correct_order.has_value()) << correct_order.error().message;
    ASSERT_TRUE(reversed_order.has_value()) << reversed_order.error().message;

    auto correct_ids = sorted_ids(*correct_order);
    auto reversed_ids = sorted_ids(*reversed_order);

    // The core regression assertion: identical scoped sets in both orders.
    EXPECT_EQ(correct_ids, reversed_ids);

    ASSERT_EQ(reversed_ids.size(), 2u)
        << "reversed clause order (USING before WITHIN) leaked out-of-scope "
           "rows -- the graph scope was silently dropped (GDB-1241 regression)";
    EXPECT_EQ(reversed_ids[0], 10);
    EXPECT_EQ(reversed_ids[1], 11);
    EXPECT_EQ(std::count(reversed_ids.begin(), reversed_ids.end(), 1), 0)
        << "unreached PK-collision book (id 1) leaked into scope";
    EXPECT_EQ(std::count(reversed_ids.begin(), reversed_ids.end(), 12), 0)
        << "unreached book (id 12) leaked into scope";
}

// Same check but with the default (COSINE) metric spelled out reversed, to
// make sure the bug isn't metric-specific.
TEST_F(QA_GDB1241, UsingCosineBeforeWithinScopesCorrectly) {
    auto result =
        run("SELECT id FROM books "
            "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
            "USING COSINE "
            "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto ids = sorted_ids(*result);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 11);
}

// =============================================================================
// AC: duplicate clauses are a clean parse error, not silently ignored/last-
// wins, in either order.
// =============================================================================
TEST_F(QA_GDB1241, DuplicateWithinClauseIsParseError) {
    auto result = run("SELECT id FROM books "
                      "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                      "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 1 "
                      "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(QA_GDB1241, DuplicateUsingClauseIsParseError) {
    auto result = run("SELECT id FROM books "
                      "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                      "USING DOT USING COSINE");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(QA_GDB1241, DuplicateUsingClauseReversedOrderStillErrors) {
    // USING first, then a second USING after WITHIN -- must still be rejected.
    auto result =
        run("SELECT id FROM books "
            "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
            "USING DOT "
            "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 1 "
            "USING COSINE");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// =============================================================================
// AC: only WITHIN (no USING) still works -- default metric applies.
// =============================================================================
TEST_F(QA_GDB1241, OnlyWithinNoUsingDefaultsMetricAndScopes) {
    auto result =
        run("SELECT id FROM books "
            "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
            "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto ids = sorted_ids(*result);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 11);
}

// =============================================================================
// AC: only USING (no WITHIN) still works -- unscoped, all rows visible.
// =============================================================================
TEST_F(QA_GDB1241, OnlyUsingNoWithinIsUnscoped) {
    auto result = run("SELECT id FROM books "
                      "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                      "USING DOT");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto ids = sorted_ids(*result);
    // Unscoped: all 4 books visible (1, 10, 11, 12).
    ASSERT_EQ(ids.size(), 4u);
    EXPECT_EQ(std::count(ids.begin(), ids.end(), 1), 1);
    EXPECT_EQ(std::count(ids.begin(), ids.end(), 12), 1);
}

// =============================================================================
// AC: neither clause present -- unscoped, default metric.
// =============================================================================
TEST_F(QA_GDB1241, NeitherClausePresentIsUnscopedDefaultMetric) {
    auto result = run("SELECT id FROM books "
                      "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0]");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto ids = sorted_ids(*result);
    ASSERT_EQ(ids.size(), 4u);
}

} // namespace
