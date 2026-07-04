/// @file test_subquery_graph_vector.cpp
/// @brief Tests for TRAVERSE (graph) queries used inside subqueries — derived
///        tables, IN, EXISTS, and scalar positions — plus coverage of the
///        NEAREST vector-search WHERE predicate. NEAREST is no longer a
///        standalone statement or subquery form (see the *Rejected tests
///        below); NearestPredicateReturnsTopK covers the single-table
///        WHERE-predicate form that replaced it.
///
/// This is the core "blend" feature: graph traversal composes with relational
/// SQL via subqueries. Phase 1 covers non-correlated nesting.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// Test fixture: a docs table with an EMBEDDING column and a "refs" edge type.
// =============================================================================

class SubqueryGraphVectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_subquery_gv";
        std::error_code ec;
        std::filesystem::remove_all(data_dir_, ec);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());
        provider_registry_ = std::make_unique<ProviderRegistry>(catalog_);

        exec_ok("CREATE TABLE docs (id INT PRIMARY KEY, body VARCHAR, body_vec EMBEDDING)");

        auto schema = catalog_.get_table(default_database_id, "docs");
        ASSERT_TRUE(schema.has_value());
        register_embedding(schema->table_id, 2, 4, "body", "builtin/4");
        engine_->set_provider_registry(provider_registry_.get());

        // Vectors chosen so doc 1 and doc 2 are close to [1,0,0,0]; 3 and 4 far.
        exec_ok("INSERT INTO docs VALUES (1, 'alpha', [1.0, 0.0, 0.0, 0.0])");
        exec_ok("INSERT INTO docs VALUES (2, 'beta', [0.9, 0.1, 0.0, 0.0])");
        exec_ok("INSERT INTO docs VALUES (3, 'gamma', [0.0, 1.0, 0.0, 0.0])");
        exec_ok("INSERT INTO docs VALUES (4, 'delta', [0.0, 0.0, 1.0, 0.0])");

        // Edges: 1 -> 2 -> 3 (4 is unreachable from 1 via "refs").
        exec_ok("CREATE EDGE TYPE refs FROM docs TO docs");
        exec_ok("LINK docs(1) TO docs(2) VIA refs");
        exec_ok("LINK docs(2) TO docs(3) VIA refs");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        provider_registry_.reset();
        std::error_code ec;
        std::filesystem::remove_all(data_dir_, ec);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << "SQL: " << sql << "\nError: " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "SQL should have failed: " << sql;
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected)
                << "Expected " << static_cast<int>(expected) << " but got "
                << static_cast<int>(result.error().code) << ": " << result.error().message;
        }
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

    std::unordered_set<int64_t> collect_column_ints(const QueryResult& qr, size_t col) {
        std::unordered_set<int64_t> result;
        for (const auto& row : qr.rows) {
            if (row[col].type_id() == TypeId::INT32) {
                result.insert(row[col].as_int32());
            } else {
                result.insert(row[col].as_int64());
            }
        }
        return result;
    }

    std::unordered_set<std::string> collect_column_strings(const QueryResult& qr, size_t col) {
        std::unordered_set<std::string> result;
        for (const auto& row : qr.rows) {
            result.insert(row[col].as_string());
        }
        return result;
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
// NEAREST vector-search WHERE predicate, and derived tables: FROM (TRAVERSE ...)
// =============================================================================

TEST_F(SubqueryGraphVectorTest, NearestPredicateReturnsTopK) {
    // The two nearest docs to [1,0,0,0] are docs 1 (alpha) and 2 (beta). Vector
    // search is a single-table WHERE predicate (the NEAREST derived-table and
    // IN-subquery forms were both removed; this test covers that one path via
    // both an id projection and a qualified-column projection).
    auto qr_ids = exec_ok("SELECT id FROM docs WHERE NEAREST(body_vec, 2) TO [1.0, 0.0, 0.0, 0.0]");

    ASSERT_EQ(qr_ids.rows.size(), 2u);
    auto ids = collect_column_ints(qr_ids, 0);
    EXPECT_TRUE(ids.count(1));
    EXPECT_TRUE(ids.count(2));

    auto qr_bodies = exec_ok("SELECT docs.body FROM docs "
                             "WHERE NEAREST(body_vec, 2) TO [1.0, 0.0, 0.0, 0.0]");

    auto bodies = collect_column_strings(qr_bodies, 0);
    EXPECT_EQ(bodies.size(), 2u);
    EXPECT_TRUE(bodies.count("alpha"));
    EXPECT_TRUE(bodies.count("beta"));
}

TEST_F(SubqueryGraphVectorTest, DerivedTableTraverse) {
    // Traversing refs OUT from doc 1 (depth 2) reaches nodes {2, 3} — the start
    // node itself is not emitted by a standalone TRAVERSE.
    auto qr =
        exec_ok("SELECT t.* FROM (TRAVERSE refs FROM docs(1) DIRECTION OUT MAX_DEPTH 2) AS t");

    EXPECT_EQ(qr.rows.size(), 2u);
}

// =============================================================================
// IN (subquery): WHERE x IN (TRAVERSE ...)
// =============================================================================

TEST_F(SubqueryGraphVectorTest, InTraverse) {
    // Nodes reached from doc 1 via refs OUT (depth 2): {2, 3} = beta, gamma.
    auto qr = exec_ok("SELECT docs.body FROM docs "
                      "WHERE docs.id IN (TRAVERSE refs FROM docs(1) DIRECTION OUT MAX_DEPTH 2)");

    auto bodies = collect_column_strings(qr, 0);
    EXPECT_EQ(bodies.size(), 2u);
    EXPECT_TRUE(bodies.count("beta"));
    EXPECT_TRUE(bodies.count("gamma"));
    EXPECT_FALSE(bodies.count("alpha"));
    EXPECT_FALSE(bodies.count("delta"));
}

TEST_F(SubqueryGraphVectorTest, NotInTraverse) {
    // Docs NOT reached from doc 1: doc 1 (start, not emitted) and doc 4 = alpha, delta.
    auto qr =
        exec_ok("SELECT docs.body FROM docs "
                "WHERE docs.id NOT IN (TRAVERSE refs FROM docs(1) DIRECTION OUT MAX_DEPTH 2)");

    auto bodies = collect_column_strings(qr, 0);
    EXPECT_EQ(bodies.size(), 2u);
    EXPECT_TRUE(bodies.count("alpha"));
    EXPECT_TRUE(bodies.count("delta"));
}

// =============================================================================
// EXISTS (subquery): non-decorrelated, evaluated per outer row.
// =============================================================================

TEST_F(SubqueryGraphVectorTest, ExistsTraverse) {
    // The traversal yields >= 1 row regardless of the outer row, so every doc
    // qualifies. Exercises the filter-time eval_exists path with a graph engine.
    auto qr = exec_ok("SELECT docs.body FROM docs "
                      "WHERE EXISTS (TRAVERSE refs FROM docs(1) DIRECTION OUT MAX_DEPTH 2)");

    EXPECT_EQ(qr.rows.size(), 4u);
}

TEST_F(SubqueryGraphVectorTest, ExistsNearestRejected) {
    // Vector search is no longer a standalone statement/subquery, so an EXISTS
    // subquery wrapping NEAREST no longer parses. (Pure subquery-composition
    // mechanics with no single-table predicate equivalent.)
    exec_error("SELECT docs.body FROM docs "
               "WHERE EXISTS (NEAREST 1 FROM docs.body_vec TO [1.0, 0.0, 0.0, 0.0])",
               StatusCode::PARSE_ERROR);
}

// =============================================================================
// Scalar subqueries: a NEAREST scalar subquery is rejected.
// =============================================================================

TEST_F(SubqueryGraphVectorTest, ScalarNearestRejected) {
    // Vector search is no longer a standalone statement/subquery, so a NEAREST
    // scalar subquery no longer parses.
    exec_error("SELECT (NEAREST 1 FROM docs.body_vec TO [1.0, 0.0, 0.0, 0.0]) FROM docs",
               StatusCode::PARSE_ERROR);
}

// =============================================================================
// Correlated subqueries — inner TRAVERSE/NEAREST references the outer row.
// =============================================================================

TEST_F(SubqueryGraphVectorTest, CorrelatedExistsTraverse) {
    // For each doc, does it have an outgoing refs edge? The start node is the
    // OUTER doc's id. Only docs 1 (->2) and 2 (->3) have outgoing edges.
    auto qr = exec_ok("SELECT docs.body FROM docs "
                      "WHERE EXISTS (TRAVERSE refs FROM docs(docs.id) DIRECTION OUT MAX_DEPTH 1)");

    auto bodies = collect_column_strings(qr, 0);
    EXPECT_EQ(bodies.size(), 2u);
    EXPECT_TRUE(bodies.count("alpha"));
    EXPECT_TRUE(bodies.count("beta"));
    EXPECT_FALSE(bodies.count("gamma"));
    EXPECT_FALSE(bodies.count("delta"));
}

TEST_F(SubqueryGraphVectorTest, CorrelatedInTraverse) {
    // For each doc, is node 3 reachable from it (OUT, depth 2)? doc 1 reaches
    // {2,3}; doc 2 reaches {3}; docs 3 and 4 reach nothing.
    auto qr = exec_ok("SELECT docs.body FROM docs "
                      "WHERE 3 IN (TRAVERSE refs FROM docs(docs.id) DIRECTION OUT MAX_DEPTH 2)");

    auto bodies = collect_column_strings(qr, 0);
    EXPECT_EQ(bodies.size(), 2u);
    EXPECT_TRUE(bodies.count("alpha"));
    EXPECT_TRUE(bodies.count("beta"));
}

TEST_F(SubqueryGraphVectorTest, CorrelatedNearestRejected) {
    // A correlated vector subquery — for each query row, search docs using THAT
    // row's vector (TO queries.qvec) — composed only the searched-table differs
    // from the outer FROM table, so it has no single-table WHERE-predicate
    // equivalent. Since vector search is no longer a subquery, this no longer
    // parses.
    exec_ok("CREATE TABLE queries (qid INT PRIMARY KEY, qvec EMBEDDING)");
    auto qschema = catalog_.get_table(default_database_id, "queries");
    ASSERT_TRUE(qschema.has_value());
    register_embedding(qschema->table_id, 1, 4, "qid", "builtin/4");

    exec_ok("INSERT INTO queries VALUES (10, [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO queries VALUES (20, [0.0, 1.0, 0.0, 0.0])");

    exec_error("SELECT queries.qid FROM queries "
               "WHERE 1 IN (NEAREST 1 FROM docs.body_vec TO queries.qvec)",
               StatusCode::PARSE_ERROR);
}

// =============================================================================
// MATCH (graph pattern) inside subqueries.
// =============================================================================

TEST_F(SubqueryGraphVectorTest, DerivedTableMatch) {
    // Pattern matches the refs edges: (1->2) and (2->3); the source ids are {1, 2}.
    auto qr = exec_ok("SELECT m.id FROM (MATCH (a:docs)-[r:refs]->(b:docs) RETURN a.id) AS m");

    auto ids = collect_column_ints(qr, 0);
    EXPECT_EQ(ids.size(), 2u);
    EXPECT_TRUE(ids.count(1));
    EXPECT_TRUE(ids.count(2));
}

TEST_F(SubqueryGraphVectorTest, InMatch) {
    // Target ends of refs edges are docs {2, 3} = beta, gamma.
    auto qr = exec_ok("SELECT docs.body FROM docs "
                      "WHERE docs.id IN (MATCH (a:docs)-[r:refs]->(b:docs) RETURN b.id)");

    auto bodies = collect_column_strings(qr, 0);
    EXPECT_EQ(bodies.size(), 2u);
    EXPECT_TRUE(bodies.count("beta"));
    EXPECT_TRUE(bodies.count("gamma"));
}

TEST_F(SubqueryGraphVectorTest, CorrelatedExistsMatch) {
    // For each doc, does an outgoing refs edge exist whose source is THIS doc?
    // The inner WHERE references the outer docs.id. Only docs 1 and 2 qualify.
    auto qr = exec_ok(
        "SELECT docs.body FROM docs "
        "WHERE EXISTS (MATCH (a:docs)-[r:refs]->(b:docs) WHERE a.id = docs.id RETURN b.id)");

    auto bodies = collect_column_strings(qr, 0);
    EXPECT_EQ(bodies.size(), 2u);
    EXPECT_TRUE(bodies.count("alpha"));
    EXPECT_TRUE(bodies.count("beta"));
    EXPECT_FALSE(bodies.count("gamma"));
    EXPECT_FALSE(bodies.count("delta"));
}
