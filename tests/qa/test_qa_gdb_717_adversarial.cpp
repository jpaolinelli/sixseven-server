/// @file test_qa_gdb_717_adversarial.cpp
/// @brief Adversarial QA tests for GDB-717: NEAREST ... USING DOT inversion fix.
///
/// GDB-717 maps USING DOT to the negated INNER_PRODUCT sort key and negates
/// back when emitting the user-visible _distance column (raw dot product,
/// higher = more similar). These tests attack that sign round-trip:
///
///  - Ordering attacks: tied dots, all-negative dots, mixed signs, k
///    boundaries (0, 1, exact row count, k > rows), single-row and empty
///    tables, float overflow to +/-Inf in the negation round-trip, underflow
///    to zero, and a NaN sort key (Inf + -Inf) from finite stored data.
///  - _distance semantics: ORDER BY _distance ASC/DESC over the raw dot,
///    _distance in projection expressions and aliases, _distance referenced
///    in the residual WHERE (must error, never silently mis-filter), LIMIT
///    smaller than k, derived-table (subquery) wrapping.
///  - Path consistency: brute-force vs btree-prefiltered must produce
///    identical ordering AND identical _distance values; prefiltered RIDs
///    pointing at nonexistent rows; dimension-mismatched query vectors.
///  - HNSW interaction: a populated HNSW index hijacks the DOT query and
///    returns L2-ordered raw index distances (known-wrong, GDB-723). Pinned
///    here as stable behavior so a change in either direction is visible.
///  - Metric guards: the default (no USING) metric remains COSINE ordering.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/nearest_scan.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/table_heap.h"
#include "sixseven/table/tuple.h"
#include "sixseven/vector/distance.h"
#include "sixseven/vector/hnsw_index.h"
#include "sixseven/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// End-to-end SQL pipeline fixture (parser -> binder -> planner -> executor)
// =============================================================================

class QA717AdvDotEndToEndTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb717_adv";
        std::error_code ec;
        std::filesystem::remove_all(data_dir_, ec);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());
        provider_registry_ = std::make_unique<ProviderRegistry>(catalog_);

        exec_ok("CREATE TABLE adv (id INT PRIMARY KEY, name VARCHAR, vec EMBEDDING)");

        auto adv = catalog_.get_table(default_database_id, "adv");
        ASSERT_TRUE(adv.has_value());
        register_embedding(adv->table_id, 2, 4, "name", "builtin/4");
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

    void insert(int id, const std::string& name, const std::string& vec_literal) {
        exec_ok("INSERT INTO adv VALUES (" + std::to_string(id) + ", '" + name + "', " +
                vec_literal + ")");
    }

    std::vector<std::string> names_in_order(const QueryResult& qr) {
        std::vector<std::string> out;
        out.reserve(qr.rows.size());
        for (const auto& row : qr.rows) {
            out.push_back(row[0].as_string());
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
// Ordering attacks
// -----------------------------------------------------------------------------

// Tied dot products: three distinct vectors all with dot = 1.0 against the
// query. All must surface with _distance exactly 1.0, ranked between the
// dot=2 row and the dot=0.5 row. Order among ties is unspecified.
TEST_F(QA717AdvDotEndToEndTest, TiedDotValuesAllSurfaceWithEqualDistance) {
    insert(1, "tie_a", "[1.0, 0.0, 0.0, 0.0]");  // dot = 1.0
    insert(2, "tie_b", "[0.0, 1.0, 0.0, 0.0]");  // dot = 1.0
    insert(3, "tie_c", "[2.0, -1.0, 0.0, 0.0]"); // dot = 1.0
    insert(4, "best", "[1.0, 1.0, 0.0, 0.0]");   // dot = 2.0
    insert(5, "worst", "[0.5, 0.0, 0.0, 0.0]");  // dot = 0.5

    auto qr = exec_ok("SELECT name, _distance FROM adv "
                      "WHERE NEAREST(vec, 5) TO [1.0, 1.0, 0.0, 0.0] USING DOT");

    ASSERT_EQ(qr.rows.size(), 5u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "best");
    EXPECT_NEAR(qr.rows[0][1].as_float64(), 2.0, 1e-5);
    EXPECT_EQ(qr.rows[4][0].as_string(), "worst");
    EXPECT_NEAR(qr.rows[4][1].as_float64(), 0.5, 1e-5);

    // Ranks 1..3 are the tied rows, in any order, all with _distance 1.0.
    std::unordered_set<std::string> mid;
    for (size_t i = 1; i <= 3; ++i) {
        mid.insert(qr.rows[i][0].as_string());
        EXPECT_NEAR(qr.rows[i][1].as_float64(), 1.0, 1e-5)
            << "tied row at rank " << i << " has wrong _distance";
    }
    EXPECT_EQ(mid.size(), 3u);
    EXPECT_TRUE(mid.count("tie_a") == 1 && mid.count("tie_b") == 1 && mid.count("tie_c") == 1);
}

// All dot products negative: the LEAST negative (closest to zero) is the most
// similar and must come first. A sign slip anywhere in the round-trip would
// invert this ordering.
TEST_F(QA717AdvDotEndToEndTest, AllNegativeDotsLeastNegativeFirst) {
    insert(1, "slight", "[-0.1, 0.0, 0.0, 0.0]"); // dot = -0.1
    insert(2, "medium", "[-0.5, 0.0, 0.0, 0.0]"); // dot = -0.5
    insert(3, "strong", "[-2.0, 0.0, 0.0, 0.0]"); // dot = -2.0

    auto qr = exec_ok("SELECT name, _distance FROM adv "
                      "WHERE NEAREST(vec, 2) TO [1.0, 0.0, 0.0, 0.0] USING DOT");

    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "slight");
    EXPECT_NEAR(qr.rows[0][1].as_float64(), -0.1, 1e-5);
    EXPECT_EQ(qr.rows[1][0].as_string(), "medium");
    EXPECT_NEAR(qr.rows[1][1].as_float64(), -0.5, 1e-5);
}

// Mixed-sign dots spanning zero: strict descending raw-dot order.
TEST_F(QA717AdvDotEndToEndTest, MixedSignDotsStrictDescendingOrder) {
    insert(1, "neg", "[-3.0, 0.0, 0.0, 0.0]");  // dot = -3.0
    insert(2, "pos", "[3.0, 0.0, 0.0, 0.0]");   // dot = 3.0
    insert(3, "zero", "[0.0, 5.0, 0.0, 0.0]");  // dot = 0.0
    insert(4, "small", "[0.5, 0.0, 0.0, 0.0]"); // dot = 0.5

    auto qr = exec_ok("SELECT name, _distance FROM adv "
                      "WHERE NEAREST(vec, 4) TO [1.0, 0.0, 0.0, 0.0] USING DOT");

    auto got = names_in_order(qr);
    ASSERT_EQ(got.size(), 4u);
    EXPECT_EQ(got[0], "pos");
    EXPECT_EQ(got[1], "small");
    EXPECT_EQ(got[2], "zero");
    EXPECT_EQ(got[3], "neg");
    // _distance strictly decreasing.
    for (size_t i = 1; i < qr.rows.size(); ++i) {
        EXPECT_GT(qr.rows[i - 1][1].as_float64(), qr.rows[i][1].as_float64())
            << "raw dot _distance not strictly descending at rank " << i;
    }
}

// k = 1 returns exactly the single highest-dot row.
TEST_F(QA717AdvDotEndToEndTest, KOneReturnsSingleBestRow) {
    insert(1, "low", "[0.1, 0.0, 0.0, 0.0]");
    insert(2, "high", "[9.0, 0.0, 0.0, 0.0]");
    insert(3, "mid", "[1.0, 0.0, 0.0, 0.0]");

    auto qr = exec_ok("SELECT name, _distance FROM adv "
                      "WHERE NEAREST(vec, 1) TO [1.0, 0.0, 0.0, 0.0] USING DOT");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "high");
    EXPECT_NEAR(qr.rows[0][1].as_float64(), 9.0, 1e-4);
}

// k = 0 parses and returns zero rows (no crash, no error).
TEST_F(QA717AdvDotEndToEndTest, KZeroReturnsNoRows) {
    insert(1, "a", "[1.0, 0.0, 0.0, 0.0]");

    auto qr = exec_ok("SELECT name FROM adv "
                      "WHERE NEAREST(vec, 0) TO [1.0, 0.0, 0.0, 0.0] USING DOT");
    EXPECT_TRUE(qr.rows.empty());
}

// k exactly equal to the row count returns every row, ordered.
TEST_F(QA717AdvDotEndToEndTest, KEqualsRowCountReturnsAllOrdered) {
    insert(1, "c", "[0.3, 0.0, 0.0, 0.0]");
    insert(2, "a", "[0.9, 0.0, 0.0, 0.0]");
    insert(3, "b", "[0.6, 0.0, 0.0, 0.0]");

    auto qr = exec_ok("SELECT name FROM adv "
                      "WHERE NEAREST(vec, 3) TO [1.0, 0.0, 0.0, 0.0] USING DOT");

    auto got = names_in_order(qr);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], "a");
    EXPECT_EQ(got[1], "b");
    EXPECT_EQ(got[2], "c");
}

// k far larger than the row count returns all rows without error.
TEST_F(QA717AdvDotEndToEndTest, KLargerThanRowCountReturnsAll) {
    insert(1, "x", "[1.0, 0.0, 0.0, 0.0]");
    insert(2, "y", "[-1.0, 0.0, 0.0, 0.0]");

    auto qr = exec_ok("SELECT name FROM adv "
                      "WHERE NEAREST(vec, 1000) TO [1.0, 0.0, 0.0, 0.0] USING DOT");

    auto got = names_in_order(qr);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], "x");
    EXPECT_EQ(got[1], "y");
}

// Single-row table: the lone row surfaces with the correct raw dot product.
TEST_F(QA717AdvDotEndToEndTest, SingleRowTableReturnsThatRow) {
    insert(1, "only", "[-0.25, 0.0, 0.0, 0.0]");

    auto qr = exec_ok("SELECT name, _distance FROM adv "
                      "WHERE NEAREST(vec, 5) TO [1.0, 0.0, 0.0, 0.0] USING DOT");

    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "only");
    EXPECT_NEAR(qr.rows[0][1].as_float64(), -0.25, 1e-5);
}

// Empty table: zero rows, no error.
TEST_F(QA717AdvDotEndToEndTest, EmptyTableReturnsNoRows) {
    auto qr = exec_ok("SELECT name FROM adv "
                      "WHERE NEAREST(vec, 5) TO [1.0, 0.0, 0.0, 0.0] USING DOT");
    EXPECT_TRUE(qr.rows.empty());
}

// Float overflow in the negation round-trip: dot(1e30 * 1e30) = 1e60
// overflows float32 to +Inf; the sort key becomes -Inf (ranks first) and the
// displayed _distance must come back as +Inf, not -Inf. The mirror-image row
// produces -Inf and must rank last.
TEST_F(QA717AdvDotEndToEndTest, ExtremeMagnitudeOverflowInfinityRoundTrip) {
    insert(1, "huge_pos", "[1e30, 0.0, 0.0, 0.0]");  // dot = +1e60 -> +Inf
    insert(2, "normal", "[1.0, 0.0, 0.0, 0.0]");     // dot = 1e30 (finite)
    insert(3, "huge_neg", "[-1e30, 0.0, 0.0, 0.0]"); // dot = -1e60 -> -Inf

    auto qr = exec_ok("SELECT name, _distance FROM adv "
                      "WHERE NEAREST(vec, 3) TO [1e30, 0.0, 0.0, 0.0] USING DOT");

    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "huge_pos");
    EXPECT_TRUE(std::isinf(qr.rows[0][1].as_float64()));
    EXPECT_GT(qr.rows[0][1].as_float64(), 0.0); // +Inf, not -Inf.

    EXPECT_EQ(qr.rows[1][0].as_string(), "normal");
    EXPECT_NEAR(qr.rows[1][1].as_float64(), 1e30, 1e25);

    EXPECT_EQ(qr.rows[2][0].as_string(), "huge_neg");
    EXPECT_TRUE(std::isinf(qr.rows[2][1].as_float64()));
    EXPECT_LT(qr.rows[2][1].as_float64(), 0.0); // -Inf.
}

// Float underflow: 1e-30 * 1e-30 = 1e-60 flushes to (sub)zero in float32.
// The tiny row must rank between a positive and a negative dot, with
// _distance ~ 0 and the sign of zero not flipping the order.
TEST_F(QA717AdvDotEndToEndTest, TinyMagnitudeUnderflowRanksAtZero) {
    insert(1, "pos", "[1.0, 0.0, 0.0, 0.0]");    // dot = 1e-30 vs tiny query... see below
    insert(2, "tiny", "[1e-30, 0.0, 0.0, 0.0]"); // dot = 1e-60 -> 0
    insert(3, "neg", "[-1.0, 0.0, 0.0, 0.0]");   // dot = -1e-30

    // Query magnitude 1e-30: dots are 1e-30 (pos), ~0 (tiny), -1e-30 (neg).
    auto qr = exec_ok("SELECT name, _distance FROM adv "
                      "WHERE NEAREST(vec, 3) TO [1e-30, 0.0, 0.0, 0.0] USING DOT");

    auto got = names_in_order(qr);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], "pos");
    EXPECT_EQ(got[1], "tiny");
    EXPECT_EQ(got[2], "neg");
    EXPECT_NEAR(qr.rows[1][1].as_float64(), 0.0, 1e-45);
}

// NaN sort key from finite stored data: +1e60 overflows to +Inf, -1e60 to
// -Inf, and their sum is NaN. The query must not crash or error; the NaN row
// must report a NaN _distance, and the remaining (orderable) rows must still
// be ranked correctly relative to each other.
TEST_F(QA717AdvDotEndToEndTest, NanSortKeyFromInfMinusInfDoesNotCrash) {
    insert(1, "nan_row", "[1e30, -1e30, 0.0, 0.0]"); // +Inf + -Inf = NaN
    insert(2, "pos", "[1.0, 0.0, 0.0, 0.0]");        // dot = 1e30
    insert(3, "neg", "[-1.0, 0.0, 0.0, 0.0]");       // dot = -1e30

    auto result = engine_->execute("SELECT name, _distance FROM adv "
                                   "WHERE NEAREST(vec, 3) TO [1e30, 1e30, 0.0, 0.0] USING DOT");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 3u);

    // Exactly one row reports NaN; 'pos' must still rank above 'neg'.
    size_t nan_count = 0;
    int pos_rank = -1;
    int neg_rank = -1;
    for (size_t i = 0; i < result->rows.size(); ++i) {
        const auto name = result->rows[i][0].as_string();
        const double d = result->rows[i][1].as_float64();
        if (name == "nan_row") {
            EXPECT_TRUE(std::isnan(d)) << "nan_row _distance = " << d;
            ++nan_count;
        } else if (name == "pos") {
            pos_rank = static_cast<int>(i);
        } else if (name == "neg") {
            neg_rank = static_cast<int>(i);
        }
    }
    EXPECT_EQ(nan_count, 1u);
    ASSERT_NE(pos_rank, -1);
    ASSERT_NE(neg_rank, -1);
    EXPECT_LT(pos_rank, neg_rank) << "finite rows misordered in presence of NaN";
}

// -----------------------------------------------------------------------------
// _distance semantics attacks
// -----------------------------------------------------------------------------

// ORDER BY _distance DESC must agree with NEAREST's own most-similar-first
// emission order; ORDER BY _distance ASC must be the exact reverse. If the
// emitted _distance carried the internal negated sort key, these would flip.
TEST_F(QA717AdvDotEndToEndTest, OrderByDistanceDescAndAscConsistent) {
    insert(1, "r1", "[3.0, 0.0, 0.0, 0.0]");  // dot = 3
    insert(2, "r2", "[1.0, 0.0, 0.0, 0.0]");  // dot = 1
    insert(3, "r3", "[-2.0, 0.0, 0.0, 0.0]"); // dot = -2

    auto desc = exec_ok("SELECT name, _distance FROM adv "
                        "WHERE NEAREST(vec, 3) TO [1.0, 0.0, 0.0, 0.0] USING DOT "
                        "ORDER BY _distance DESC");
    auto got_desc = names_in_order(desc);
    ASSERT_EQ(got_desc.size(), 3u);
    EXPECT_EQ(got_desc[0], "r1");
    EXPECT_EQ(got_desc[1], "r2");
    EXPECT_EQ(got_desc[2], "r3");

    auto asc = exec_ok("SELECT name, _distance FROM adv "
                       "WHERE NEAREST(vec, 3) TO [1.0, 0.0, 0.0, 0.0] USING DOT "
                       "ORDER BY _distance ASC");
    auto got_asc = names_in_order(asc);
    ASSERT_EQ(got_asc.size(), 3u);
    EXPECT_EQ(got_asc[0], "r3");
    EXPECT_EQ(got_asc[1], "r2");
    EXPECT_EQ(got_asc[2], "r1");
}

// _distance used inside a projection expression and under an alias: the
// arithmetic must see the raw dot product (negating it yields the internal
// sort key, exposing any double-negation).
TEST_F(QA717AdvDotEndToEndTest, DistanceInProjectionExpressionAndAlias) {
    insert(1, "a", "[2.0, 0.0, 0.0, 0.0]");  // dot = 2
    insert(2, "b", "[-0.5, 0.0, 0.0, 0.0]"); // dot = -0.5

    auto qr = exec_ok("SELECT name, _distance * -1 AS neg_dot FROM adv "
                      "WHERE NEAREST(vec, 2) TO [1.0, 0.0, 0.0, 0.0] USING DOT");

    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "a");
    EXPECT_NEAR(qr.rows[0][1].as_float64(), -2.0, 1e-5);
    EXPECT_EQ(qr.rows[1][0].as_string(), "b");
    EXPECT_NEAR(qr.rows[1][1].as_float64(), 0.5, 1e-5);
}

// _distance referenced in the residual WHERE is not visible to the post-
// filter schema. It must produce a clean error — never silently mis-filter
// on the wrong (negated) value.
TEST_F(QA717AdvDotEndToEndTest, WhereOnDistanceErrorsCleanlyNotSilentlyWrong) {
    insert(1, "a", "[2.0, 0.0, 0.0, 0.0]");
    insert(2, "b", "[-0.5, 0.0, 0.0, 0.0]");

    auto result = engine_->execute("SELECT name FROM adv "
                                   "WHERE NEAREST(vec, 2) TO [1.0, 0.0, 0.0, 0.0] USING DOT "
                                   "AND _distance > 0.0");
    // Current contract: the residual WHERE schema strips _distance, so the
    // reference fails with NOT_FOUND. If this ever starts succeeding it must
    // filter on the RAW dot (both rows vs row 'a' only) — revisit then.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// LIMIT smaller than k truncates after the most-similar-first ordering.
TEST_F(QA717AdvDotEndToEndTest, LimitSmallerThanKKeepsTopRows) {
    insert(1, "third", "[0.3, 0.0, 0.0, 0.0]");
    insert(2, "first", "[5.0, 0.0, 0.0, 0.0]");
    insert(3, "second", "[1.0, 0.0, 0.0, 0.0]");

    auto qr = exec_ok("SELECT name FROM adv "
                      "WHERE NEAREST(vec, 3) TO [1.0, 0.0, 0.0, 0.0] USING DOT "
                      "LIMIT 2");

    auto got = names_in_order(qr);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], "first");
    EXPECT_EQ(got[1], "second");
}

// Derived-table wrapping: ordering and raw-dot _distance survive through a
// subquery in FROM.
TEST_F(QA717AdvDotEndToEndTest, SubqueryInFromPreservesOrderingAndDistance) {
    insert(1, "low", "[0.2, 0.0, 0.0, 0.0]");
    insert(2, "high", "[4.0, 0.0, 0.0, 0.0]");
    insert(3, "neg", "[-1.0, 0.0, 0.0, 0.0]");

    auto qr = exec_ok("SELECT * FROM (SELECT name, _distance FROM adv "
                      "WHERE NEAREST(vec, 3) TO [1.0, 0.0, 0.0, 0.0] USING DOT) AS sub");

    ASSERT_EQ(qr.rows.size(), 3u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "high");
    EXPECT_NEAR(qr.rows[0][1].as_float64(), 4.0, 1e-5);
    EXPECT_EQ(qr.rows[1][0].as_string(), "low");
    EXPECT_NEAR(qr.rows[1][1].as_float64(), 0.2, 1e-5);
    EXPECT_EQ(qr.rows[2][0].as_string(), "neg");
    EXPECT_NEAR(qr.rows[2][1].as_float64(), -1.0, 1e-5);
}

// Two NEAREST conjuncts in one WHERE: the planner drives the scan from the
// first and the second residual-evaluates to TRUE (silently ignored). Pinned
// so a behavior change (error or real conjunction) is caught.
TEST_F(QA717AdvDotEndToEndTest, TwoNearestConjunctsFirstOneWins) {
    insert(1, "dot_best", "[5.0, 0.0, 0.0, 0.0]"); // dot = 5, L2 far
    insert(2, "l2_best", "[1.0, 0.0, 0.0, 0.0]");  // dot = 1, L2 = 0
    insert(3, "neither", "[-1.0, 0.0, 0.0, 0.0]");

    auto qr = exec_ok("SELECT name FROM adv "
                      "WHERE NEAREST(vec, 2) TO [1.0, 0.0, 0.0, 0.0] USING DOT "
                      "AND NEAREST(vec, 1) TO [1.0, 0.0, 0.0, 0.0] USING L2");

    // First (DOT, k=2) drives: dot_best then l2_best. The L2 conjunct does
    // not further constrain the result.
    auto got = names_in_order(qr);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], "dot_best");
    EXPECT_EQ(got[1], "l2_best");
}

// Metric guard: with no USING clause the default metric is COSINE and its
// ordering must be unaffected by the DOT remap. Vector [9,0,0,0] has the
// biggest dot but a perfect cosine; [0,1,0,0] is orthogonal (cosine dist 1);
// [-1,0,0,0] is opposite (cosine dist 2).
TEST_F(QA717AdvDotEndToEndTest, DefaultMetricRemainsCosineOrdering) {
    insert(1, "parallel", "[9.0, 0.0, 0.0, 0.0]");
    insert(2, "ortho", "[0.0, 1.0, 0.0, 0.0]");
    insert(3, "anti", "[-1.0, 0.0, 0.0, 0.0]");

    auto qr = exec_ok("SELECT name, _distance FROM adv "
                      "WHERE NEAREST(vec, 3) TO [1.0, 0.0, 0.0, 0.0]");

    auto got = names_in_order(qr);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], "parallel");
    EXPECT_EQ(got[1], "ortho");
    EXPECT_EQ(got[2], "anti");
    // COSINE _distance is emitted unchanged (no negation round-trip).
    EXPECT_NEAR(qr.rows[0][1].as_float64(), 0.0, 1e-5);
    EXPECT_NEAR(qr.rows[1][1].as_float64(), 1.0, 1e-5);
    EXPECT_NEAR(qr.rows[2][1].as_float64(), 2.0, 1e-5);
}

// =============================================================================
// Operator-level fixture (path consistency + HNSW interaction)
// =============================================================================

class QA717AdvNearestScanOperatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        table_path_ = std::filesystem::temp_directory_path() / "sixseven_qa717adv_table.db";
        std::filesystem::remove(table_path_);
        auto fid = dm_.create_file(table_path_, false, true);
        ASSERT_TRUE(fid.has_value()) << fid.error().message;
        table_fid_ = *fid;
        table_bpm_ = std::make_unique<BufferPoolManager>(dm_, table_fid_, 64);

        hnsw_path_ = std::filesystem::temp_directory_path() / "sixseven_qa717adv_hnsw.db";
        std::filesystem::remove(hnsw_path_);
        auto hfid = dm_.create_file(hnsw_path_, false, true);
        ASSERT_TRUE(hfid.has_value()) << hfid.error().message;
        hnsw_fid_ = *hfid;
        hnsw_bpm_ = std::make_unique<BufferPoolManager>(dm_, hnsw_fid_, 256);

        storage_schema_ = Schema({
            {"id", TypeId::INT32},
            {"name", TypeId::STRING},
            {"embedding", TypeId::EMBEDDING},
        });

        output_cols_ = {
            {"", "id", TypeId::INT32, false, 0},
            {"", "name", TypeId::STRING, true, 0},
            {"", "embedding", TypeId::EMBEDDING, true, 0},
            {"", "_distance", TypeId::FLOAT64, false, 0},
        };
    }

    void TearDown() override {
        hnsw_bpm_.reset();
        table_bpm_.reset();
        (void)dm_.close_file(hnsw_fid_);
        (void)dm_.close_file(table_fid_);
        std::filesystem::remove(hnsw_path_);
        std::filesystem::remove(table_path_);
    }

    RID
    insert_row(TableHeap& heap, int32_t id, const std::string& name, const Embedding& embedding) {
        std::vector<Value> vals = {Value(id), Value(name), Value(embedding)};
        auto bytes = TupleSerializer::serialize(vals, storage_schema_);
        EXPECT_TRUE(bytes.has_value()) << bytes.error().message;
        if (!bytes.has_value()) {
            return RID::invalid();
        }
        auto rid = heap.insert_tuple(*bytes);
        EXPECT_TRUE(rid.has_value()) << rid.error().message;
        if (!rid.has_value()) {
            return RID::invalid();
        }
        return *rid;
    }

    std::vector<Tuple> drain(Iterator& op) {
        std::vector<Tuple> results;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value()) {
                break;
            }
            results.push_back(std::move(row->value()));
        }
        return results;
    }

    std::filesystem::path table_path_;
    std::filesystem::path hnsw_path_;
    DiskManager dm_;
    FileId table_fid_ = 0;
    FileId hnsw_fid_ = 0;
    std::unique_ptr<BufferPoolManager> table_bpm_;
    std::unique_ptr<BufferPoolManager> hnsw_bpm_;
    Schema storage_schema_;
    std::vector<OutputColumn> output_cols_;
};

// Path consistency: brute-force and btree-prefiltered (all RIDs) must produce
// IDENTICAL ordering and IDENTICAL _distance values for the same dot-product
// query. Distinct dot values avoid tie-order flakiness.
TEST_F(QA717AdvNearestScanOperatorTest, BruteForceAndPrefilteredIdenticalResults) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    std::vector<RID> rids;
    rids.push_back(insert_row(heap, 1, "a", {0.7F, 0.0F, 0.0F}));  // dot 0.7
    rids.push_back(insert_row(heap, 2, "b", {-0.4F, 0.0F, 0.0F})); // dot -0.4
    rids.push_back(insert_row(heap, 3, "c", {2.5F, 0.0F, 0.0F}));  // dot 2.5
    rids.push_back(insert_row(heap, 4, "d", {0.1F, 0.0F, 0.0F}));  // dot 0.1

    auto run = [&](bool prefiltered) {
        NearestScanConfig config;
        config.k = 4;
        config.query_vector = {1.0F, 0.0F, 0.0F};
        config.metric = DistanceMetric::INNER_PRODUCT;
        config.embedding_column_index = 2;
        if (prefiltered) {
            config.prefiltered_rids = rids;
        }
        OutputSchema schema(output_cols_);
        BoundStatement bound;
        NearestScanOperator op(
            heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);
        EXPECT_TRUE(op.open().has_value());
        auto results = drain(op);
        op.close();
        return results;
    };

    auto brute = run(false);
    auto prefil = run(true);

    ASSERT_EQ(brute.size(), 4u);
    ASSERT_EQ(prefil.size(), 4u);
    // Expected order: c (2.5), a (0.7), d (0.1), b (-0.4).
    EXPECT_EQ(brute[0].values[0].as_int32(), 3);
    for (size_t i = 0; i < brute.size(); ++i) {
        EXPECT_EQ(brute[i].values[0].as_int32(), prefil[i].values[0].as_int32())
            << "path ordering diverges at rank " << i;
        EXPECT_DOUBLE_EQ(brute[i].values[3].as_float64(), prefil[i].values[3].as_float64())
            << "path _distance diverges at rank " << i;
    }
}

// Prefiltered RIDs that all point at nonexistent rows: skipped gracefully,
// zero results, no error.
TEST_F(QA717AdvNearestScanOperatorTest, PrefilteredAllRidsInvalidReturnsEmpty) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    insert_row(heap, 1, "real", {1.0F, 0.0F, 0.0F});

    NearestScanConfig config;
    config.k = 5;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::INNER_PRODUCT;
    config.embedding_column_index = 2;
    config.prefiltered_rids = {RID{9999, 0}, RID{9999, 7}};

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    EXPECT_TRUE(results.empty());
    op.close();
}

// HNSW interaction pin (GDB-723, pre-existing, NOT this ticket's inversion):
// with a populated HNSW index a dot-product query is served by the index,
// which computes L2 regardless of the metric, and distances pass through
// without the dot-product display negation. The L2-nearest row (id=1, dot
// 0.9) therefore beats the dot-best row (id=2, dot 5.0). This test pins that
// known-wrong-but-stable behavior; it must FLIP when GDB-723 is fixed.
TEST_F(QA717AdvNearestScanOperatorTest, HnswPathStillL2OrderedForDotQueries) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    HnswIndex hnsw(*hnsw_bpm_, nullptr);

    HnswIndexConfig hnsw_config;
    hnsw_config.dimension = 3;
    hnsw_config.m = 8;
    hnsw_config.ef_construction = 50;
    hnsw_config.ef_search = 50;
    ASSERT_TRUE(hnsw.create(hnsw_config).has_value());

    // id=1: L2^2 = 0.01 (nearest), dot = 0.9. id=2: L2^2 = 16, dot = 5 (best).
    Embedding embs[] = {
        {0.9F, 0.0F, 0.0F},
        {5.0F, 0.0F, 0.0F},
        {0.0F, 1.0F, 0.0F},
    };
    for (int i = 0; i < 3; ++i) {
        insert_row(heap, i + 1, "r" + std::to_string(i + 1), embs[i]);
        ASSERT_TRUE(hnsw.insert(embs[i]).has_value());
    }

    NearestScanConfig config;
    config.k = 3;
    config.query_vector = {1.0F, 0.0F, 0.0F};
    config.metric = DistanceMetric::INNER_PRODUCT; // what the planner emits for USING DOT
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound, &hnsw);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 3u);

    // L2 ordering (0.01, 2.0, 16.0), not dot ordering (5.0, 0.9, 0.0): the
    // dot-best row id=2 lands LAST because its L2 distance is largest.
    EXPECT_EQ(results[0].values[0].as_int32(), 1);
    EXPECT_EQ(results[1].values[0].as_int32(), 3);
    EXPECT_EQ(results[2].values[0].as_int32(), 2);

    // Distances are raw index (L2-squared) values, ascending and >= 0 — the
    // dot-product display negation must NOT be applied on this path.
    for (size_t i = 0; i < results.size(); ++i) {
        EXPECT_GE(results[i].values[3].as_float64(), 0.0)
            << "HNSW distance unexpectedly negated at rank " << i;
        if (i > 0) {
            EXPECT_LE(results[i - 1].values[3].as_float64(), results[i].values[3].as_float64());
        }
    }
    EXPECT_NEAR(results[0].values[3].as_float64(), 0.01, 1e-4);
    op.close();
}

// Dimension mismatch: a 2-dim query against 3-dim embeddings computes over
// the common prefix (min-dim truncation, pre-existing contract). Pinned: no
// crash, ordering follows the truncated dot, third component ignored.
TEST_F(QA717AdvNearestScanOperatorTest, DimensionMismatchTruncatesToCommonPrefix) {
    TableHeap heap(*table_bpm_, dm_, table_fid_);
    // Truncated dots vs query [1, 1]: id=1 -> 0.2, id=2 -> 2.0, id=3 -> -1.0.
    // id=1's huge third component must NOT contribute.
    insert_row(heap, 1, "small", {0.1F, 0.1F, 100.0F});
    insert_row(heap, 2, "big", {1.0F, 1.0F, 0.0F});
    insert_row(heap, 3, "neg", {-1.0F, 0.0F, 100.0F});

    NearestScanConfig config;
    config.k = 3;
    config.query_vector = {1.0F, 1.0F}; // 2-dim vs 3-dim stored
    config.metric = DistanceMetric::INNER_PRODUCT;
    config.embedding_column_index = 2;

    OutputSchema schema(output_cols_);
    BoundStatement bound;
    NearestScanOperator op(
        heap, storage_schema_, std::move(config), std::move(schema), nullptr, bound);

    ASSERT_TRUE(op.open().has_value());
    auto results = drain(op);
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].values[0].as_int32(), 2);
    EXPECT_EQ(results[1].values[0].as_int32(), 1);
    EXPECT_EQ(results[2].values[0].as_int32(), 3);
    EXPECT_NEAR(results[0].values[3].as_float64(), 2.0, 1e-5);
    EXPECT_NEAR(results[1].values[3].as_float64(), 0.2, 1e-5);
    EXPECT_NEAR(results[2].values[3].as_float64(), -1.0, 1e-5);
    op.close();
}
