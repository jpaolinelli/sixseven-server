/// @file test_qa_gdb_1190.cpp
/// @brief Adversarial QA for GDB-1190 (merge of DerivedTableNearest + InNearest
///        into NearestPredicateReturnsTopK in test_subquery_graph_vector.cpp).
///
/// GDB-1190 is a test-only consolidation: two developer tests that executed
/// the identical single-table `WHERE NEAREST(col, k) TO [...]` query (differing
/// only in projected column: id vs docs.body) were merged into one test that
/// asserts both projections. No production code changed.
///
/// QA angle: (1) confirm the merge genuinely preserves detection power for
/// both projections by constructing a scenario where a real regression in the
/// NEAREST predicate executor would be caught by each half independently; and
/// (2) stress the underlying NEAREST(col, K) WHERE-predicate path itself with
/// boundary K values, ties, exact-vector matches, and K larger than the table,
/// since that is the actual attack surface the merged test exercises.

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

namespace {

class QaGdb1190Test : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_1190";
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

// -----------------------------------------------------------------------------
// Regression-detection-power check: reproduce the exact merged-test fixture
// data and confirm BOTH the id projection and the docs.body projection
// independently detect a wrong-column / wrong-row result. This proves the
// consolidation in GDB-1190 didn't quietly drop coverage for either half of
// the original two tests.
// -----------------------------------------------------------------------------

TEST_F(QaGdb1190Test, MergedProjectionsBothCatchWrongResults) {
    exec_ok("INSERT INTO docs VALUES (1, 'alpha', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (2, 'beta', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (3, 'gamma', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (4, 'delta', [0.0, 0.0, 1.0, 0.0])");

    auto qr_ids = exec_ok("SELECT id FROM docs WHERE NEAREST(body_vec, 2) TO [1.0, 0.0, 0.0, 0.0]");
    ASSERT_EQ(qr_ids.rows.size(), 2u);
    auto ids = collect_column_ints(qr_ids, 0);
    EXPECT_TRUE(ids.count(1));
    EXPECT_TRUE(ids.count(2));
    // The id-projection assertion is specific enough to fail if the executor
    // returned {3,4} (the FAR docs) instead of {1,2} -- i.e. it would catch a
    // reversed-distance-ordering bug.
    EXPECT_FALSE(ids.count(3));
    EXPECT_FALSE(ids.count(4));

    auto qr_bodies = exec_ok("SELECT docs.body FROM docs "
                             "WHERE NEAREST(body_vec, 2) TO [1.0, 0.0, 0.0, 0.0]");
    auto bodies = collect_column_strings(qr_bodies, 0);
    EXPECT_EQ(bodies.size(), 2u);
    EXPECT_TRUE(bodies.count("alpha"));
    EXPECT_TRUE(bodies.count("beta"));
    // Same discriminating power via the qualified-column projection path,
    // which exercises column resolution (docs.body) independently of id.
    EXPECT_FALSE(bodies.count("gamma"));
    EXPECT_FALSE(bodies.count("delta"));
}

// -----------------------------------------------------------------------------
// Boundary: K larger than the table's row count should not crash or hang, and
// should return exactly all rows (not K rows with duplicates/garbage).
// -----------------------------------------------------------------------------

TEST_F(QaGdb1190Test, KLargerThanTableReturnsAllRowsNoDuplicates) {
    exec_ok("INSERT INTO docs VALUES (1, 'alpha', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (2, 'beta', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (3, 'gamma', [0.0, 1.0, 0.0, 0.0])");

    auto qr = exec_ok("SELECT id FROM docs WHERE NEAREST(body_vec, 1000) TO [1.0, 0.0, 0.0, 0.0]");
    EXPECT_EQ(qr.rows.size(), 3u);
    auto ids = collect_column_ints(qr, 0);
    EXPECT_EQ(ids.size(), 3u);
    EXPECT_TRUE(ids.count(1));
    EXPECT_TRUE(ids.count(2));
    EXPECT_TRUE(ids.count(3));
}

// -----------------------------------------------------------------------------
// Boundary: K = 0 should return zero rows, not an error and not all rows.
// -----------------------------------------------------------------------------

TEST_F(QaGdb1190Test, KZeroReturnsNoRows) {
    exec_ok("INSERT INTO docs VALUES (1, 'alpha', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (2, 'beta', [0.9, 0.1, 0.0, 0.0])");

    auto result = engine_->execute("SELECT id FROM docs WHERE NEAREST(body_vec, 0) TO [1.0, 0.0, 0.0, 0.0]");
    // Accept either "succeeds with zero rows" or a well-formed INVALID_ARGUMENT
    // rejection -- but a crash, hang, or silently returning all rows is a bug.
    if (result.has_value()) {
        EXPECT_EQ(result->rows.size(), 0u)
            << "K=0 should return no rows, not all/garbage rows";
    } else {
        EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT)
            << "K=0 rejection should be INVALID_ARGUMENT, got: " << result.error().message;
    }
}

// -----------------------------------------------------------------------------
// Boundary: exact match -- a query vector identical to a row's embedding must
// place that row first among ties (distance 0) and be included in top-K.
// -----------------------------------------------------------------------------

TEST_F(QaGdb1190Test, ExactMatchVectorIncludedInTopK) {
    exec_ok("INSERT INTO docs VALUES (1, 'alpha', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (2, 'beta', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (3, 'gamma', [0.0, 0.0, 1.0, 0.0])");

    // Query vector is an EXACT match for doc 1's embedding.
    auto qr = exec_ok("SELECT id FROM docs WHERE NEAREST(body_vec, 1) TO [1.0, 0.0, 0.0, 0.0]");
    ASSERT_EQ(qr.rows.size(), 1u);
    auto ids = collect_column_ints(qr, 0);
    EXPECT_TRUE(ids.count(1)) << "exact-match row must be the sole top-1 result";
}

// -----------------------------------------------------------------------------
// Boundary: exact ties in distance -- two rows equidistant from the query
// vector, K=1. The result must be a *stable, valid* single row (one of the
// tied candidates), not zero rows, not both rows, not a crash.
// -----------------------------------------------------------------------------

TEST_F(QaGdb1190Test, ExactTiesInDistanceReturnExactlyKRows) {
    // Both docs are unit vectors at distance sqrt(2) from the origin query,
    // symmetric so distance-to-query is identical for both.
    exec_ok("INSERT INTO docs VALUES (1, 'alpha', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (2, 'beta', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (3, 'gamma', [0.0, 0.0, 1.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (4, 'delta', [0.0, 0.0, 0.0, 1.0])");

    // Query equidistant (by symmetry) from docs 1-4 under L2/cosine distance
    // to [0.5,0.5,0.5,0.5] is identical for all four axis-aligned unit vectors.
    auto qr = exec_ok("SELECT id FROM docs WHERE NEAREST(body_vec, 1) TO [0.5, 0.5, 0.5, 0.5]");
    // Regardless of tie-break policy, exactly 1 row must come back for K=1.
    EXPECT_EQ(qr.rows.size(), 1u) << "tie-break must still respect exact K, not return all tied rows";
    auto ids = collect_column_ints(qr, 0);
    EXPECT_EQ(ids.size(), 1u);
    EXPECT_TRUE(ids.count(1) || ids.count(2) || ids.count(3) || ids.count(4));
}

// -----------------------------------------------------------------------------
// Both projection forms (unqualified id, qualified docs.body) must agree on
// which underlying row set was selected as top-K, even though presented via
// different columns -- this directly re-validates the specific claim GDB-1190
// makes: that merging the two assertions into one test still checks both
// paths meaningfully rather than one subsuming/masking the other.
// -----------------------------------------------------------------------------

TEST_F(QaGdb1190Test, IdAndQualifiedBodyProjectionsAgreeOnTopKRowSet) {
    exec_ok("INSERT INTO docs VALUES (1, 'alpha', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (2, 'beta', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (3, 'gamma', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("INSERT INTO docs VALUES (4, 'delta', [0.0, 0.0, 1.0, 0.0])");

    auto qr_ids = exec_ok("SELECT id FROM docs WHERE NEAREST(body_vec, 3) TO [1.0, 0.0, 0.0, 0.0]");
    auto qr_bodies = exec_ok("SELECT docs.body FROM docs WHERE NEAREST(body_vec, 3) TO [1.0, 0.0, 0.0, 0.0]");

    ASSERT_EQ(qr_ids.rows.size(), 3u);
    ASSERT_EQ(qr_bodies.rows.size(), 3u);

    auto ids = collect_column_ints(qr_ids, 0);
    auto bodies = collect_column_strings(qr_bodies, 0);

    std::unordered_set<std::string> expected_bodies;
    for (auto id : ids) {
        if (id == 1) expected_bodies.insert("alpha");
        if (id == 2) expected_bodies.insert("beta");
        if (id == 3) expected_bodies.insert("gamma");
        if (id == 4) expected_bodies.insert("delta");
    }
    EXPECT_EQ(bodies, expected_bodies)
        << "id-projection and body-projection top-K queries must select the same underlying rows";
}

}  // namespace
