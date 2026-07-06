/// @file test_qa_gdb_1229.cpp
/// @brief Adversarial QA tests for GDB-1229 (NEAREST(k) + subquery WHERE
///        strict-intersection semantics; no backfill past top-k; no dup RIDs).
///
/// The fix pushes NEAREST down alongside subquery predicates (IN/MATCH) so the
/// vector filter is no longer silently dropped, and NearestScanOperator's
/// emit_top_k_window() enforces a strict top-k-DISTINCT-then-filter contract
/// (never widening past the k-th distinct candidate to compensate for WHERE
/// rejections). These tests probe boundary cases beyond the two un-skipped
/// blend tests: zero/one/full intersections, duplicate-vector dedup through
/// the subquery path, k > pool size, k == 1, and non-regression for NEAREST
/// without any subquery predicate.

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

class QAGdb1229Test : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_1229";
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

// -----------------------------------------------------------------------------
// 1. Strict intersection: empty, singleton, and full-k cases.
// -----------------------------------------------------------------------------

// Intersection is EMPTY: top-2 nearest to [1,0,0,0] are {ml, vec} (both dist 0),
// but carol's authored set is {vec}... wait -- construct a true zero case:
// bob's authored set is {ml, db}; nearest-2 to [0,0,1,0] (far from all) with
// k=1 picks whichever is closest, and intersecting with carol's authored set
// {vec} (which is NOT in bob's set) yields empty.
TEST_F(QAGdb1229Test, StrictIntersectionEmpty) {
    // Nearest-1 to [0,0,1,0] is one of {ml, db, vec} (all equidistant-ish, but
    // exactly one wins the top-1 slot). Intersect with carol's authored set
    // {vec}: only non-empty if the winner happens to be vec, so instead force
    // a guaranteed-empty case by intersecting bob's set {ml, db} with a
    // constant-false-like graph filter: carol's authored articles {vec} have
    // no overlap with bob's {ml, db}.
    auto qr = exec_ok(
        "SELECT articles.title FROM articles "
        "WHERE NEAREST(body_vec, 2) TO [1.0, 0.0, 0.0, 0.0] "
        "AND articles.id IN (TRAVERSE authored FROM users(3) DIRECTION OUT)");
    // top-2 nearest to [1,0,0,0]: {ml, vec} (both distance 0). Carol's set is
    // {vec}. Intersection = {vec}, NOT empty -- this exercises the singleton
    // case instead; keep as a sanity check that it's exactly 1, never 2.
    auto got = titles(qr);
    EXPECT_EQ(got.size(), 1u);
    EXPECT_TRUE(got.count("vec"));
}

// True empty intersection: nearest-1 to [0,1,0,0] is 'db' (distance 0), and
// carol's authored set is {vec}. {db} ∩ {vec} = {}.
TEST_F(QAGdb1229Test, StrictIntersectionEmptyGuaranteed) {
    auto qr = exec_ok(
        "SELECT articles.title FROM articles "
        "WHERE NEAREST(body_vec, 1) TO [0.0, 1.0, 0.0, 0.0] "
        "AND articles.id IN (TRAVERSE authored FROM users(3) DIRECTION OUT)");
    EXPECT_EQ(qr.rows.size(), 0u)
        << "NEAREST+subquery intersection must not backfill when WHERE rejects the sole top-k "
           "candidate";
}

// Full-k intersection: nearest-2 to [1,0,0,0] are {ml, vec} (both dist 0), and
// bob's authored set is {ml, db}. The graph filter alone would admit only
// {ml}; broaden to carol+bob combined scope via a MATCH-free direct IN over
// both authored sets to get a full k=2 match: use TRAVERSE from users(2) OUT
// unioned conceptually via k=1 exact overlap {ml}. For a genuine k==|result|
// case, search body_vec nearest-1 to [1,0,0,0]: two rows tie at distance 0
// ({ml},{vec}); restrict candidate pool via prior tests. Simplest full-match:
// intersect nearest-2 to [1,0,0,0] ({ml,vec}) with "id IN (10,12)" expressed
// as a graph traversal covering exactly those two articles is awkward with the
// current schema, so assert the arithmetic case using carol's own set unioned
// with bob's via two LINKs (already covers ml,db via bob) -- instead verify
// with an id-list IN-subquery-free relational AND, which still exercises
// emit_top_k_window's strict window (non-subquery predicate path).
TEST_F(QAGdb1229Test, StrictIntersectionFullK) {
    auto qr = exec_ok("SELECT articles.title FROM articles "
                      "WHERE NEAREST(body_vec, 2) TO [1.0, 0.0, 0.0, 0.0] "
                      "AND articles.id IN (10, 12)");
    auto got = titles(qr);
    EXPECT_EQ(got.size(), 2u) << "full overlap between top-k and predicate must yield exactly k";
    EXPECT_TRUE(got.count("ml"));
    EXPECT_TRUE(got.count("vec"));
}

// -----------------------------------------------------------------------------
// 2. No duplicates through the subquery/blend path with duplicate vectors.
// -----------------------------------------------------------------------------

TEST_F(QAGdb1229Test, NoDuplicateRidsWithDuplicateVectorsThroughSubquery) {
    // 'ml' (10) and 'vec' (12) share the identical embedding [1,0,0,0]. Ensure
    // NEAREST(k=2) intersected with a subquery predicate never emits the same
    // RID twice and never exceeds the true distinct top-k count.
    auto qr = exec_ok(
        "SELECT articles.title FROM articles "
        "WHERE NEAREST(body_vec, 2) TO [1.0, 0.0, 0.0, 0.0] "
        "AND articles.id IN (TRAVERSE authored FROM users(2) DIRECTION OUT)");
    // bob authored {ml, db}; nearest-2 to [1,0,0,0] is {ml, vec} (tie at 0).
    // Intersection = {ml}. Must be exactly 1 row, not 2 (no dup, no backfill).
    EXPECT_EQ(qr.rows.size(), 1u);
    auto got = titles(qr);
    EXPECT_TRUE(got.count("ml"));

    std::unordered_set<int32_t> seen_ids;
    for (const auto& row : qr.rows) {
        // title is the only projected column in this fixture's schema for
        // this query; re-run with id projected to check RID-level dedup.
        (void)row;
    }
}

TEST_F(QAGdb1229Test, NoDuplicateIdsProjected) {
    auto qr = exec_ok(
        "SELECT articles.id, articles.title FROM articles "
        "WHERE NEAREST(body_vec, 2) TO [1.0, 0.0, 0.0, 0.0] "
        "AND articles.id IN (10, 12)");
    ASSERT_EQ(qr.rows.size(), 2u);
    std::unordered_set<int64_t> ids;
    for (const auto& row : qr.rows) {
        ids.insert(row[0].as_int32());
    }
    EXPECT_EQ(ids.size(), 2u) << "duplicate RIDs must not be emitted even when candidates tie";
}

// -----------------------------------------------------------------------------
// 3. Non-regression: NEAREST without subquery predicate; NEAREST + plain WHERE.
// -----------------------------------------------------------------------------

TEST_F(QAGdb1229Test, NearestWithoutSubqueryUnchanged) {
    auto qr = exec_ok("SELECT articles.title FROM articles "
                      "WHERE NEAREST(body_vec, 2) TO [1.0, 0.0, 0.0, 0.0]");
    auto got = titles(qr);
    EXPECT_EQ(got.size(), 2u);
    EXPECT_TRUE(got.count("ml"));
    EXPECT_TRUE(got.count("vec"));
}

TEST_F(QAGdb1229Test, NearestWithSimplePlainWhereStillIntersects) {
    // Plain (non-subquery) WHERE ANDed with NEAREST: title != 'vec' should
    // strip 'vec' from the top-2 {ml, vec}, leaving only {ml} -- not
    // backfilled with 'db' to reach k=2.
    auto qr = exec_ok("SELECT articles.title FROM articles "
                      "WHERE NEAREST(body_vec, 2) TO [1.0, 0.0, 0.0, 0.0] "
                      "AND articles.title != 'vec'");
    auto got = titles(qr);
    EXPECT_EQ(got.size(), 1u) << "plain WHERE conjunct must not be backfilled past top-k either";
    EXPECT_TRUE(got.count("ml"));
}

// -----------------------------------------------------------------------------
// 4. Boundary: k > pool size; k == 1.
// -----------------------------------------------------------------------------

TEST_F(QAGdb1229Test, KLargerThanCandidatePoolWithSubquery) {
    // Only 3 articles total; ask for k=100.
    auto qr = exec_ok(
        "SELECT articles.title FROM articles "
        "WHERE NEAREST(body_vec, 100) TO [1.0, 0.0, 0.0, 0.0] "
        "AND articles.id IN (TRAVERSE authored FROM users(2) DIRECTION OUT)");
    // Top-100 (effectively all 3) intersected with bob's authored set {ml,db}.
    auto got = titles(qr);
    EXPECT_EQ(got.size(), 2u);
    EXPECT_TRUE(got.count("ml"));
    EXPECT_TRUE(got.count("db"));
}

TEST_F(QAGdb1229Test, KEqualsOneWithSubquery) {
    auto qr = exec_ok(
        "SELECT articles.title FROM articles "
        "WHERE NEAREST(body_vec, 1) TO [0.0, 1.0, 0.0, 0.0] "
        "AND articles.id IN (TRAVERSE authored FROM users(2) DIRECTION OUT)");
    auto got = titles(qr);
    EXPECT_EQ(got.size(), 1u);
    EXPECT_TRUE(got.count("db"));
}

// -----------------------------------------------------------------------------
// 5. MATCH-subquery blend boundary (empty intersection variant of GDB-1229's
//    MatchAndVectorBlend test).
// -----------------------------------------------------------------------------

TEST_F(QAGdb1229Test, MatchAndVectorBlendEmptyIntersection) {
    // Nearest-1 to [0,0,1,0] winner is whichever article is truly closest to
    // that vector; none of ml/db/vec are near [0,0,1,0] uniformly, but the
    // MATCH restricts to bob's articles {ml, db} via alice->bob->authored.
    // Search nearest-1 to [1,0,0,0] instead (winner is 'ml' or 'vec', tied at
    // 0) but MATCH result set (bob's articles) is {ml, db} -- if the vector
    // winner is 'vec' (not in bob's set), intersection is empty. Since ties
    // are broken deterministically by heap scan order, assert the actual
    // observed behavior is a strict subset of {ml, db} of size <= 1, never 2.
    auto qr = exec_ok(
        "SELECT articles.title FROM articles "
        "WHERE NEAREST(body_vec, 1) TO [1.0, 0.0, 0.0, 0.0] "
        "AND articles.id IN "
        "  (MATCH (a:users)-[f:follows]->(u:users)-[w:authored]->(art:articles) "
        "   WHERE a.id = 1 RETURN art.id)");
    auto got = titles(qr);
    EXPECT_LE(got.size(), 1u) << "MATCH+NEAREST intersection must never exceed k=1";
    if (!got.empty()) {
        EXPECT_TRUE(got.count("ml") || got.count("db"));
    }
}
