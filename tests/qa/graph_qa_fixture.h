/// @file graph_qa_fixture.h
/// Shared fixture base and helpers for graph-algorithm QA tests.
///
/// Used by: test_qa_gdb_519.cpp, test_qa_gdb_520.cpp, test_qa_gdb_521.cpp,
///          test_qa_gdb_522.cpp, test_qa_gdb_523.cpp, test_qa_gdb_524.cpp,
///          test_qa_gdb_551.cpp
///
/// ODR safety: all functions and methods are defined inline / in-class.

#pragma once

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/graph/graph_engine.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace sixseven {
namespace graph_qa {

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

// ============================================================================
// Shared base fixture
// ============================================================================

/// Base fixture for graph-algorithm QA tests.
///
/// Provides a Catalog and GraphEngine with a single "nodes" table containing
/// an INT64 PK column.  Concrete per-ticket fixtures derive from this class,
/// keep their own name, and add algorithm-specific run() helpers.
class GraphQaFixtureBase : public ::testing::Test {
protected:
    void SetUp() override {
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

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t table_id_ = 0;
};

} // namespace graph_qa
} // namespace sixseven
