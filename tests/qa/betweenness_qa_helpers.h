/// @file betweenness_qa_helpers.h
/// Shared fixture base and helpers for Betweenness Centrality QA tests.
///
/// Used by: test_qa_gdb_495.cpp (canonical; absorbed GDB-489 per GDB-958
/// consolidation).
///
/// ODR safety: all functions and methods are defined inline / in-class.

#pragma once

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/betweenness_centrality.h"
#include "sixseven/graph/graph_engine.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "test_qa_helpers.h"

namespace sixseven {
namespace betweenness_qa {

// ============================================================================
// Shared helpers
// ============================================================================

inline TableSchema make_table_schema(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", TypeId::INT64, false, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

inline Value pk(int64_t v) {
    return Value(v);
}

/// Extract (node_id, centrality) pairs from algorithm result rows.
inline std::unordered_map<int64_t, double>
to_centrality_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, double> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 2u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto centrality = std::get<double>(row.values[1].data());
        result[node_id] = centrality;
    }
    return result;
}

/// Verify that all centrality scores are non-negative.
inline void verify_scores_non_negative(const std::unordered_map<int64_t, double>& scores) {
    for (const auto& [node, score] : scores) {
        EXPECT_GE(score, 0.0) << "node " << node << " should have non-negative centrality";
    }
}

/// Verify that no score is NaN or Inf.
inline void verify_scores_finite(const std::unordered_map<int64_t, double>& scores) {
    for (const auto& [node, score] : scores) {
        EXPECT_FALSE(std::isnan(score)) << "node " << node << " has NaN centrality";
        EXPECT_FALSE(std::isinf(score)) << "node " << node << " has Inf centrality";
    }
}

/// Verify output rows are sorted by node_id.
inline void verify_sorted_by_node_id(const std::vector<AlgorithmRow>& rows) {
    for (size_t i = 1; i < rows.size(); ++i) {
        auto prev = std::get<int64_t>(rows[i - 1].values[0].data());
        auto curr = std::get<int64_t>(rows[i].values[0].data());
        EXPECT_LT(prev, curr) << "results should be sorted by node_id";
    }
}

// ============================================================================
// Shared base fixture
// ============================================================================

/// Base fixture for Betweenness Centrality QA tests.
///
/// Provides a Catalog and GraphEngine with a single "nodes" table containing
/// an INT64 PK column, plus run_betweenness helpers.
class BetweennessQaFixtureBase : public ::testing::Test {
protected:
    void SetUp() override {
        bootstrap_qa_catalog(catalog_);
        auto t = catalog_.create_table(default_database_id, make_table_schema("nodes"));
        ASSERT_TRUE(t.has_value()) << t.error().message;
        table_id_ = *t;
    }

    void build_graph(const std::string& edge_type,
                     const std::vector<std::pair<int64_t, int64_t>>& edges) {
        auto et = engine_.create_edge_type(
            default_database_id, edge_type, table_id_, table_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(et.has_value()) << et.error().message;

        for (auto [src, tgt] : edges) {
            auto link = engine_.link(default_database_id, edge_type, pk(src), pk(tgt));
            ASSERT_TRUE(link.has_value()) << link.error().message;
        }
    }

    /// Run betweenness centrality with normalized=true (default).
    Result<std::vector<AlgorithmRow>> run_betweenness(const std::string& edge_type) {
        AlgorithmContext ctx{
            engine_, default_database_id, edge_type, {{"normalized", Value(true)}}};
        return betweenness_centrality_execute(ctx);
    }

    /// Run betweenness centrality with normalized=false.
    Result<std::vector<AlgorithmRow>> run_betweenness_unnormalized(const std::string& edge_type) {
        AlgorithmContext ctx{
            engine_, default_database_id, edge_type, {{"normalized", Value(false)}}};
        return betweenness_centrality_execute(ctx);
    }

    /// Run betweenness centrality with raw named_args (for type edge cases).
    Result<std::vector<AlgorithmRow>>
    run_betweenness_raw(const std::string& edge_type, std::unordered_map<std::string, Value> args) {
        AlgorithmContext ctx{engine_, default_database_id, edge_type, std::move(args)};
        return betweenness_centrality_execute(ctx);
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

} // namespace betweenness_qa
} // namespace sixseven
