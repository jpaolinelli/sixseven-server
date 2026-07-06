/// @file test_qa_subquery_blend.cpp
/// @brief QA regression tests for blending relational, graph, and vector queries
///        through subqueries (TRAVERSE / NEAREST / MATCH used inside SELECT).
///
/// These exercise the project's core "one SQL-like system" promise: a single
/// statement that filters relational rows by both a graph traversal and a vector
/// similarity search nested as subqueries, plus correlated graph/vector subqueries.

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

#include "test_catalog_helpers.h"

using namespace sixseven;

class QASubqueryBlendTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_subquery_blend";
        std::error_code ec;
        std::filesystem::remove_all(data_dir_, ec);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());
        provider_registry_ = std::make_unique<ProviderRegistry>(catalog_);

        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR, interest_vec EMBEDDING)");
        exec_ok("CREATE TABLE articles (id INT PRIMARY KEY, title VARCHAR, body_vec EMBEDDING)");

        auto users = catalog_.get_table(default_database_id, "users");
        auto articles = catalog_.get_table(default_database_id, "articles");
        ASSERT_TRUE(users.has_value());
        ASSERT_TRUE(articles.has_value());
        register_embedding(users->table_id, 2, 4, "name", "builtin/4");
        register_embedding(articles->table_id, 2, 4, "title", "builtin/4");
        engine_->set_provider_registry(provider_registry_.get());

        exec_ok("INSERT INTO users VALUES (1, 'alice', [1.0, 0.0, 0.0, 0.0])");
        exec_ok("INSERT INTO users VALUES (2, 'bob', [0.0, 1.0, 0.0, 0.0])");
        exec_ok("INSERT INTO users VALUES (3, 'carol', [0.0, 0.0, 1.0, 0.0])");

        exec_ok("INSERT INTO articles VALUES (10, 'ml', [1.0, 0.0, 0.0, 0.0])");
        exec_ok("INSERT INTO articles VALUES (11, 'db', [0.0, 1.0, 0.0, 0.0])");
        exec_ok("INSERT INTO articles VALUES (12, 'vec', [1.0, 0.0, 0.0, 0.0])");

        exec_ok("CREATE EDGE TYPE follows FROM users TO users");
        exec_ok("CREATE EDGE TYPE authored FROM users TO articles");
        exec_ok("LINK users(1) TO users(2) VIA follows"); // alice -> bob
        exec_ok("LINK users(2) TO articles(10) VIA authored");
        exec_ok("LINK users(2) TO articles(11) VIA authored");
        exec_ok("LINK users(3) TO articles(12) VIA authored");
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

    void exec_error(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "SQL should have failed: " << sql;
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
        ASSERT_TRUE(catalog_.register_embedding_column(emb_def).has_value());
        if (catalog_.get_embedding_provider(provider).has_value()) {
            return;
        }
        EmbeddingProviderConfig prov;
        prov.name = provider;
        prov.type = "builtin";
        prov.dimension = dim;
        ASSERT_TRUE(catalog_.register_embedding_provider(prov).has_value());
    }

    std::unordered_set<std::string> titles(const QueryResult& qr) {
        std::unordered_set<std::string> out;
        for (const auto& row : qr.rows) {
            out.insert(row[0].as_string());
        }
        return out;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<ProviderRegistry> provider_registry_;
};

// A single statement filtering relational rows by BOTH a graph traversal and a
// vector similarity search, each nested as a subquery.
TEST_F(QASubqueryBlendTest, RelationalGraphVectorBlendInOneQuery) {
    // Articles authored by bob (user 2) AND close to [1,0,0,0]:
    //   authored OUT from bob -> {10, 11}; the 2 nearest to [1,0,0,0] are the
    //   distance-0 articles {10, 12} (11 is far); ∩ -> {10}.
    // Vector search is now a WHERE predicate combined with the graph IN-subquery
    // over the same articles scan; both filter the relational rows in one query.
    auto qr = exec_ok("SELECT articles.title FROM articles "
                      "WHERE NEAREST(body_vec, 2) TO [1.0, 0.0, 0.0, 0.0] "
                      "AND articles.id IN (TRAVERSE authored FROM users(2) DIRECTION OUT)");

    auto got = titles(qr);
    EXPECT_EQ(got.size(), 1u);
    EXPECT_TRUE(got.count("ml"));
}

// Correlated graph + vector: a correlated NEAREST subquery whose searched table
// (articles) differs from the outer FROM table (users) and whose query vector is
// the outer row's interest_vec. This is a pure vector-subquery composition with
// no single-table WHERE-predicate equivalent; since vector search is no longer a
// subquery, it no longer parses.
TEST_F(QASubqueryBlendTest, CorrelatedVectorPerUserRejected) {
    exec_error("SELECT users.name FROM users "
               "WHERE 11 IN (NEAREST 1 FROM articles.body_vec TO users.interest_vec)");
}

// Graph pattern (MATCH) nested as an IN subquery, intersected with a vector match.
TEST_F(QASubqueryBlendTest, MatchAndVectorBlend) {
    // Articles authored by a user that alice follows, near [0,1,0,0]:
    //   MATCH alice-follows->u-authored->art  => bob's articles {10,11};
    //   the single nearest to [0,1,0,0] => {11};  ∩ => {11} = 'db'.
    // Vector search is now a WHERE predicate combined with the MATCH IN-subquery.
    auto qr = exec_ok("SELECT articles.title FROM articles "
                      "WHERE NEAREST(body_vec, 1) TO [0.0, 1.0, 0.0, 0.0] "
                      "AND articles.id IN "
                      "  (MATCH (a:users)-[f:follows]->(u:users)-[w:authored]->(art:articles) "
                      "   WHERE a.id = 1 RETURN art.id)");

    auto got = titles(qr);
    EXPECT_EQ(got.size(), 1u);
    EXPECT_TRUE(got.count("db"));
}
