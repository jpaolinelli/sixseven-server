/// @file shortest_path_qa_fixture.h
/// Shared fixture for weighted shortest-path QA tests.
///
/// Used by: test_qa_gdb_555.cpp, test_qa_gdb_559.cpp
///
/// ODR safety: all functions and methods are defined inline / in-class.

#pragma once

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/match_shortest_path.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sixseven {
namespace shortest_path_qa {

/// Base fixture for MatchShortestPathOperator QA tests.
///
/// Concrete per-ticket fixtures derive from this class and supply the
/// data_dir suffix string (e.g. "sixseven_qa_gdb555").  Every member
/// function is defined inline so all 2 translation units share one
/// definition without ODR violations.
class ShortestPathQaFixtureBase : public ::testing::Test {
protected:
    /// Derived class must set data_dir_suffix_ before SetUp() runs.
    /// The simplest pattern is to do so in the derived constructor.
    explicit ShortestPathQaFixtureBase(std::string data_dir_suffix)
        : data_dir_suffix_(std::move(data_dir_suffix)) {}

    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / data_dir_suffix_;
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        // Create nodes table.
        {
            TableSchema ts;
            ts.name = "nodes";
            CatalogColumnDef pk_col;
            pk_col.ordinal = 0;
            pk_col.name = "id";
            pk_col.type_id = TypeId::INT64;
            pk_col.nullable = false;
            ts.columns.push_back(pk_col);
            ts.pk_columns = "id";
            auto tid = catalog_->create_table(default_database_id, std::move(ts));
            ASSERT_TRUE(tid.has_value()) << tid.error().message;
            nodes_id_ = *tid;

            auto schema = catalog_->get_table(default_database_id, "nodes");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, nodes_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void create_edge_type(const std::string& name) {
        ColumnDef w_col{"weight", TypeId::FLOAT64};
        auto eid = graph_->create_edge_type(
            default_database_id, name, nodes_id_, nodes_id_, TypeId::INT64, TypeId::INT64, {w_col});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;
    }

    void insert_node(int64_t id) {
        auto ts = storage_->get_table_storage(nodes_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table(default_database_id, "nodes");
        ASSERT_TRUE(schema.has_value());
        std::vector<Value> vals = {Value(id)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    void link(int64_t from, int64_t to, double w, const std::string& edge = "road") {
        auto r = graph_->link(default_database_id, edge, Value(from), Value(to), {Value(w)});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    std::unique_ptr<ColumnRefExpr> make_weight_expr() {
        auto expr = std::make_unique<ColumnRefExpr>();
        expr->table = "r";
        expr->column = "weight";
        return expr;
    }

    MatchConfig make_config(const std::string& edge = "road", int32_t max_hops = 100) {
        MatchConfig config;
        config.nodes.push_back({"a", "nodes"});
        config.nodes.push_back({"b", "nodes"});
        config.edges.push_back(MatchEdgeDef("r", edge, TraverseDirection::OUT, 1, max_hops));
        return config;
    }

    OutputSchema make_schema() {
        std::vector<OutputColumn> cols;
        cols.push_back({"a", "id", TypeId::INT64, false, nodes_id_});
        cols.push_back({"b", "id", TypeId::INT64, false, nodes_id_});
        cols.push_back({"p", "path", TypeId::PATH, false, 0});
        return OutputSchema(std::move(cols));
    }

    std::vector<Tuple> run(PathSelector sel,
                           const Expr* weight,
                           const std::string& edge = "road",
                           int32_t k = 0,
                           int32_t max_hops = 100) {
        BoundStatement bound;
        MatchShortestPathOperator op(*graph_,
                                     *catalog_,
                                     *storage_,
                                     default_database_id,
                                     make_config(edge, max_hops),
                                     make_schema(),
                                     nullptr,
                                     bound,
                                     sel,
                                     "p",
                                     k,
                                     MatchShortestPathOperator::DEFAULT_MAX_VISITED,
                                     weight);
        auto open_result = op.open();
        EXPECT_TRUE(open_result.has_value()) << open_result.error().message;
        if (!open_result.has_value())
            return {};

        std::vector<Tuple> results;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row || !row->has_value())
                break;
            results.push_back(std::move(**row));
        }
        op.close();
        return results;
    }

    static std::vector<const Tuple*>
    filter_pair(const std::vector<Tuple>& results, int64_t src, int64_t tgt) {
        std::vector<const Tuple*> filtered;
        for (const auto& t : results) {
            if (t.values.size() >= 2 && !t.values[0].is_null() && !t.values[1].is_null() &&
                t.values[0].as_int64() == src && t.values[1].as_int64() == tgt) {
                filtered.push_back(&t);
            }
        }
        return filtered;
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t nodes_id_ = 0;

private:
    std::string data_dir_suffix_;
};

} // namespace shortest_path_qa
} // namespace sixseven
