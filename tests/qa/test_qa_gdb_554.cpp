/// @file test_qa_gdb_554.cpp
/// QA adversarial tests for GDB-554: pk_to_int64 returns 0 for non-int32/int64
/// PK types in variable-length match.
///
/// Verifies the fix: visited set now uses Value directly with ValueHash/ValueEqual,
/// supporting all PK types correctly.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/coercion.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/variable_length_match.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Fixture: Variable-length match with configurable PK types
// ============================================================================

class QA_GDB554 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb554";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    table_id_t create_table_with_pk(const std::string& name, TypeId pk_type) {
        TableSchema ts;
        ts.name = name;
        CatalogColumnDef pk_col;
        pk_col.ordinal = 0;
        pk_col.name = "id";
        pk_col.type_id = pk_type;
        pk_col.nullable = false;
        ts.columns.push_back(pk_col);
        ts.pk_columns = "id";

        auto tid = catalog_->create_table(default_database_id, std::move(ts));
        EXPECT_TRUE(tid.has_value()) << tid.error().message;

        auto schema = catalog_->get_table(default_database_id, name);
        EXPECT_TRUE(schema.has_value());
        auto sr = storage_->create_table_storage(default_database_id, *tid, *schema);
        EXPECT_TRUE(sr.has_value()) << sr.error().message;

        return *tid;
    }

    void insert_row(table_id_t tid, const std::string& table_name, const Value& pk_val) {
        auto ts = storage_->get_table_storage(tid);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table(default_database_id, table_name);
        ASSERT_TRUE(schema.has_value());

        std::vector<Value> vals = {pk_val};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    void create_edge_and_link(const std::string& edge_name,
                              table_id_t tid,
                              TypeId pk_type,
                              const Value& from,
                              const Value& to) {
        // Create edge type if not already created.
        auto existing = graph_->get_edge_table(default_database_id, edge_name);
        if (!existing.has_value()) {
            auto eid = graph_->create_edge_type(
                default_database_id, edge_name, tid, tid, pk_type, pk_type, {});
            ASSERT_TRUE(eid.has_value()) << eid.error().message;
        }
        auto r = graph_->link(default_database_id, edge_name, from, to);
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    std::vector<Tuple> run_vl_match(const std::string& node_label,
                                    const std::string& edge_type,
                                    table_id_t tid,
                                    TypeId pk_type,
                                    std::optional<int32_t> min_h,
                                    std::optional<int32_t> max_h) {
        MatchConfig config;
        config.nodes.push_back({"a", node_label});
        config.nodes.push_back({"b", node_label});
        config.edges.push_back(MatchEdgeDef("r", edge_type, TraverseDirection::OUT, min_h, max_h));

        std::vector<OutputColumn> out_cols;
        out_cols.push_back({"a", "id", pk_type, false, tid});
        out_cols.push_back({"b", "id", pk_type, false, tid});
        OutputSchema schema(std::move(out_cols));

        BoundStatement bound;
        VariableLengthMatchOperator op(*graph_,
                                       *catalog_,
                                       *storage_,
                                       default_database_id,
                                       std::move(config),
                                       std::move(schema),
                                       nullptr,
                                       bound,
                                       100'000);
        auto open_result = op.open();
        EXPECT_TRUE(open_result.has_value()) << open_result.error().message;
        if (!open_result.has_value())
            return {};

        std::vector<Tuple> results;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row.has_value() || !row->has_value())
                break;
            results.push_back(std::move(**row));
        }
        op.close();
        return results;
    }

    static constexpr database_id_t default_database_id = 1;
    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
};

// ============================================================================
// Core fix: INT64 PK type still works (regression check)
// ============================================================================

TEST_F(QA_GDB554, Int64PK_Works) {
    auto tid = create_table_with_pk("int64_nodes", TypeId::INT64);
    insert_row(tid, "int64_nodes", Value(int64_t{1}));
    insert_row(tid, "int64_nodes", Value(int64_t{2}));
    insert_row(tid, "int64_nodes", Value(int64_t{3}));

    create_edge_and_link("knows64", tid, TypeId::INT64, Value(int64_t{1}), Value(int64_t{2}));
    create_edge_and_link("knows64", tid, TypeId::INT64, Value(int64_t{2}), Value(int64_t{3}));

    auto results = run_vl_match("int64_nodes", "knows64", tid, TypeId::INT64, 1, 3);
    // From 1: reach 2 (1 hop), 3 (2 hops). From 2: reach 3 (1 hop).
    EXPECT_GE(results.size(), 3u);
}

// ============================================================================
// Core fix: INT32 PK type still works
// ============================================================================

TEST_F(QA_GDB554, Int32PK_Works) {
    auto tid = create_table_with_pk("int32_nodes", TypeId::INT32);
    insert_row(tid, "int32_nodes", Value(int32_t{10}));
    insert_row(tid, "int32_nodes", Value(int32_t{20}));
    insert_row(tid, "int32_nodes", Value(int32_t{30}));

    create_edge_and_link("knows32", tid, TypeId::INT32, Value(int32_t{10}), Value(int32_t{20}));
    create_edge_and_link("knows32", tid, TypeId::INT32, Value(int32_t{20}), Value(int32_t{30}));

    auto results = run_vl_match("int32_nodes", "knows32", tid, TypeId::INT32, 1, 3);
    EXPECT_GE(results.size(), 3u);
}

// ============================================================================
// Previously broken: non-int32/int64 PK types
// ============================================================================

TEST_F(QA_GDB554, StringPK_VisitedSetWorks) {
    auto tid = create_table_with_pk("str_nodes", TypeId::STRING);
    insert_row(tid, "str_nodes", Value(std::string("alice")));
    insert_row(tid, "str_nodes", Value(std::string("bob")));
    insert_row(tid, "str_nodes", Value(std::string("charlie")));

    create_edge_and_link(
        "knows_str", tid, TypeId::STRING, Value(std::string("alice")), Value(std::string("bob")));
    create_edge_and_link(
        "knows_str", tid, TypeId::STRING, Value(std::string("bob")), Value(std::string("charlie")));

    auto results = run_vl_match("str_nodes", "knows_str", tid, TypeId::STRING, 1, 3);
    // From alice: bob (1 hop), charlie (2 hops).
    // From bob: charlie (1 hop).
    // Total: at least 3 results — previously all PKs collapsed to 0.
    EXPECT_GE(results.size(), 3u) << "STRING PKs should work correctly in visited set";
}

TEST_F(QA_GDB554, Int16PK_Works) {
    auto tid = create_table_with_pk("int16_nodes", TypeId::INT16);
    insert_row(tid, "int16_nodes", Value(int16_t{1}));
    insert_row(tid, "int16_nodes", Value(int16_t{2}));
    insert_row(tid, "int16_nodes", Value(int16_t{3}));

    create_edge_and_link("knows16", tid, TypeId::INT16, Value(int16_t{1}), Value(int16_t{2}));
    create_edge_and_link("knows16", tid, TypeId::INT16, Value(int16_t{2}), Value(int16_t{3}));

    auto results = run_vl_match("int16_nodes", "knows16", tid, TypeId::INT16, 1, 3);
    EXPECT_GE(results.size(), 3u) << "INT16 PKs should work correctly in visited set";
}

TEST_F(QA_GDB554, Int8PK_Works) {
    auto tid = create_table_with_pk("int8_nodes", TypeId::INT8);
    insert_row(tid, "int8_nodes", Value(int8_t{1}));
    insert_row(tid, "int8_nodes", Value(int8_t{2}));
    insert_row(tid, "int8_nodes", Value(int8_t{3}));

    create_edge_and_link("knows8", tid, TypeId::INT8, Value(int8_t{1}), Value(int8_t{2}));
    create_edge_and_link("knows8", tid, TypeId::INT8, Value(int8_t{2}), Value(int8_t{3}));

    auto results = run_vl_match("int8_nodes", "knows8", tid, TypeId::INT8, 1, 3);
    EXPECT_GE(results.size(), 3u) << "INT8 PKs should work correctly in visited set";
}

// ============================================================================
// Adversarial: cycle prevention still works with non-int64 PKs
// ============================================================================

TEST_F(QA_GDB554, StringPK_CyclePreventionWorks) {
    auto tid = create_table_with_pk("cyc_nodes", TypeId::STRING);
    insert_row(tid, "cyc_nodes", Value(std::string("a")));
    insert_row(tid, "cyc_nodes", Value(std::string("b")));
    insert_row(tid, "cyc_nodes", Value(std::string("c")));

    // Triangle cycle: a->b->c->a
    create_edge_and_link(
        "cyc_edge", tid, TypeId::STRING, Value(std::string("a")), Value(std::string("b")));
    create_edge_and_link(
        "cyc_edge", tid, TypeId::STRING, Value(std::string("b")), Value(std::string("c")));
    create_edge_and_link(
        "cyc_edge", tid, TypeId::STRING, Value(std::string("c")), Value(std::string("a")));

    auto results = run_vl_match("cyc_nodes", "cyc_edge", tid, TypeId::STRING, 1, 10);
    // With cycle prevention, BFS should not loop forever.
    // From each node, can reach 2 others. 3 nodes * 2 = 6 max results.
    EXPECT_LE(results.size(), 6u) << "Cycle prevention should bound results with STRING PKs";
    EXPECT_GE(results.size(), 6u) << "Should reach all nodes in the cycle";
}

// ============================================================================
// Adversarial: distinct PK values with same hash should still deduplicate
// ============================================================================

TEST_F(QA_GDB554, StringPK_DistinctNodes_AllReachable) {
    // Create a longer chain with string PKs to stress the visited set.
    auto tid = create_table_with_pk("long_nodes", TypeId::STRING);
    std::vector<std::string> names = {"node_a", "node_b", "node_c", "node_d", "node_e"};
    for (const auto& name : names) {
        insert_row(tid, "long_nodes", Value(name));
    }

    for (size_t i = 0; i + 1 < names.size(); ++i) {
        create_edge_and_link(
            "chain_str", tid, TypeId::STRING, Value(names[i]), Value(names[i + 1]));
    }

    auto results = run_vl_match("long_nodes", "chain_str", tid, TypeId::STRING, 1, 10);
    // From node_a: can reach 4 nodes. From node_b: 3. From node_c: 2. From node_d: 1.
    // Total: 4+3+2+1 = 10.
    EXPECT_EQ(results.size(), 10u) << "All nodes in chain should be reachable with STRING PKs";
}

} // namespace
} // namespace sixseven
