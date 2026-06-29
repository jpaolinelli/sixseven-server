#pragma once

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/value.h"
#include "sixseven/graph/algorithm_registry.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sixseven {

/// Build a single-column (INT64 "id" primary-key) TableSchema.
inline TableSchema make_table_schema(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", TypeId::INT64, false, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

/// Wrap an int64_t as an INT64 Value (used to specify node primary keys).
inline Value pk(int64_t v) {
    return Value(v);
}

/// Result row for closeness-centrality algorithms: (closeness, sum_farness,
/// reachable_count, component_size, normalized_closeness).
struct ClosenessResult {
    double closeness;
    int64_t sum_farness;
    int64_t reachable_count;
    int64_t component_size;
    double normalized_closeness;
};

/// Decode a vector of AlgorithmRow into a node_id -> ClosenessResult map.
/// Expects each row to carry exactly 6 values in the column order produced
/// by closeness_centrality_execute: node_id, closeness, sum_farness,
/// reachable_count, component_size, normalized_closeness.
inline std::unordered_map<int64_t, ClosenessResult>
to_closeness_map(const std::vector<AlgorithmRow>& rows) {
    std::unordered_map<int64_t, ClosenessResult> result;
    for (const auto& row : rows) {
        EXPECT_EQ(row.values.size(), 6u);
        auto node_id = std::get<int64_t>(row.values[0].data());
        auto closeness = std::get<double>(row.values[1].data());
        auto sum_farness = std::get<int64_t>(row.values[2].data());
        auto reachable_count = std::get<int64_t>(row.values[3].data());
        auto component_size = std::get<int64_t>(row.values[4].data());
        auto normalized_closeness = std::get<double>(row.values[5].data());
        result[node_id] = {
            closeness, sum_farness, reachable_count, component_size, normalized_closeness};
    }
    return result;
}

} // namespace sixseven
