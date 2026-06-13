// GDB-791: Fix wrong neighbor binding for undirected edges in multi-hop MATCH.
//
// In execute_multi_hop, when direction is BOTH, edges from get_edges_to() have
// target_pk == current node.  The original code derived neighbor as:
//   tgt_pk = (direction == IN) ? source_pk : target_pk
// For BOTH this always picks target_pk even for reverse edges, binding the
// current node to itself.  The fix carries neighbor_pk at fetch time, exactly
// like execute_single_hop.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/pattern_match.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

/// Fixture: one node table "nodes" with edges B->A and B->C.
///
///  A <-- B --> C
///
/// Querying MATCH (a:nodes)-[r1:e]-(b:nodes)-[r2:e]->(c:nodes) starting from A
/// (undirected first segment, directed second segment) must bind b=B and c=C.
/// Before the fix the BOTH path in execute_multi_hop binds b=A (self) so
/// nothing matches the directed second segment.
class QA_GDB791Test : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb791";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        // Create node table "nodes" with columns (id INT64, name STRING).
        {
            TableSchema ts;
            ts.name = "nodes";
            CatalogColumnDef id_col;
            id_col.ordinal = 0;
            id_col.name = "id";
            id_col.type_id = TypeId::INT64;
            id_col.nullable = false;
            ts.columns.push_back(id_col);
            CatalogColumnDef name_col;
            name_col.ordinal = 1;
            name_col.name = "name";
            name_col.type_id = TypeId::STRING;
            name_col.nullable = false;
            ts.columns.push_back(name_col);
            ts.pk_columns = "id";
            auto tid = catalog_->create_table(default_database_id, std::move(ts));
            ASSERT_TRUE(tid.has_value()) << tid.error().message;
            nodes_id_ = *tid;

            auto schema = catalog_->get_table(default_database_id, "nodes");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, nodes_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        // Insert: id=1 "A", id=2 "B", id=3 "C"
        insert_node(1, "A");
        insert_node(2, "B");
        insert_node(3, "C");

        // Create edge type "e": nodes -> nodes.
        auto eid = graph_->create_edge_type(
            default_database_id, "e", nodes_id_, nodes_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Edges: B->A (id 2 -> id 1) and B->C (id 2 -> id 3).
        link("e", 2, 1);
        link("e", 2, 3);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void insert_node(int64_t id, const std::string& name) {
        auto ts = storage_->get_table_storage(nodes_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        std::vector<Value> vals = {Value(id), Value(name)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    void link(const std::string& edge_type, int64_t from, int64_t to) {
        auto r = graph_->link(default_database_id, edge_type, Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    // Helper: collect all (a.name, b.name, c.name) tuples from a 3-node MATCH.
    std::vector<std::tuple<std::string, std::string, std::string>>
    run_match(TraverseDirection dir1, TraverseDirection dir2) {
        MatchConfig config;
        config.nodes.push_back({"a", "nodes"});
        config.nodes.push_back({"b", "nodes"});
        config.nodes.push_back({"c", "nodes"});
        config.edges.push_back({"r1", "e", dir1});
        config.edges.push_back({"r2", "e", dir2});

        std::vector<OutputColumn> out_cols;
        out_cols.push_back({"a", "name", TypeId::STRING, false, nodes_id_});
        out_cols.push_back({"b", "name", TypeId::STRING, false, nodes_id_});
        out_cols.push_back({"c", "name", TypeId::STRING, false, nodes_id_});
        OutputSchema schema(std::move(out_cols));

        BoundStatement bound;
        PatternMatchOperator op(*graph_,
                                *catalog_,
                                *storage_,
                                default_database_id,
                                std::move(config),
                                std::move(schema),
                                nullptr,
                                bound);

        auto open_result = op.open();
        EXPECT_TRUE(open_result.has_value()) << open_result.error().message;

        std::vector<std::tuple<std::string, std::string, std::string>> results;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value()) {
                break;
            }
            auto& vals = row->value().values;
            EXPECT_EQ(vals.size(), 3u);
            if (vals.size() == 3u) {
                results.emplace_back(vals[0].as_string(), vals[1].as_string(), vals[2].as_string());
            }
        }
        op.close();
        return results;
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t nodes_id_ = 0;
};

// Primary regression: undirected first segment + directed second segment.
// Graph: B->A, B->C.
// MATCH (a:nodes)-[r1:e]-(b:nodes)-[r2:e]->(c:nodes)
// Starting from a=A: undirected r1 finds B (via reverse edge B->A).
// Then directed r2 from B finds C.
// Expected: (A, B, C).
// Before the fix, BOTH path bound b=A (self), no forward edges from A, result empty.
TEST_F(QA_GDB791Test, UndirectedFirstSegmentReachesReverseNeighbor) {
    auto results = run_match(TraverseDirection::BOTH, TraverseDirection::OUT);

    bool found_abc = false;
    for (auto& [a, b, c] : results) {
        if (a == "A" && b == "B" && c == "C") {
            found_abc = true;
        }
    }
    EXPECT_TRUE(found_abc) << "Expected binding (A, B, C) not found; result count="
                           << results.size();

    // No spurious self-binding where b == a.
    for (auto& [a, b, c] : results) {
        EXPECT_NE(b, a) << "Spurious self-binding: b==" << b << " equals a==" << a;
    }
}

// Undirected both segments.
// MATCH (a:nodes)-[r1:e]-(b:nodes)-[r2:e]-(c:nodes)
// From A: undirected r1 finds B (B->A reverse); undirected r2 from B finds A and C.
// When a=="A", b must be "B" (not "A").
TEST_F(QA_GDB791Test, UndirectedBothSegmentsFindReverseNeighbors) {
    auto results = run_match(TraverseDirection::BOTH, TraverseDirection::BOTH);

    for (auto& [a, b, c] : results) {
        if (a == "A") {
            EXPECT_EQ(b, "B") << "From A, undirected neighbor should be B, got b=" << b;
        }
    }

    bool abc_found = false;
    for (auto& [a, b, c] : results) {
        if (a == "A" && b == "B" && c == "C") {
            abc_found = true;
        }
    }
    EXPECT_TRUE(abc_found) << "Expected (A,B,C) not found in results";
}

// Control: pure OUT multi-hop must continue to work.
// MATCH (a:nodes)-[r1:e]->(b:nodes)-[r2:e]->(c:nodes)
// Graph: B->A, B->C. A and C have no outgoing edges, so zero results.
TEST_F(QA_GDB791Test, PureOutMultiHopControlCase) {
    auto results = run_match(TraverseDirection::OUT, TraverseDirection::OUT);
    EXPECT_EQ(results.size(), 0u) << "Pure OUT 2-hop: expected 0 results, got " << results.size();
}

// Mixed: IN then OUT.
// MATCH (a:nodes)<-[r1:e]-(b:nodes)-[r2:e]->(c:nodes)
// Graph: B->A, B->C.
// a=A: get_edges_to(A) -> B->A, so b=B. Then get_edges_from(B) -> B->A, B->C, so c=A and c=C.
//   Results: (A,B,A), (A,B,C).
// a=B: nobody points to B, no results.
// a=C: get_edges_to(C) -> B->C, so b=B. Then get_edges_from(B) -> B->A, B->C, so c=A and c=C.
//   Results: (C,B,A), (C,B,C).
// Total: 4 results.
TEST_F(QA_GDB791Test, InThenOutMultiHopMixedDirection) {
    auto results = run_match(TraverseDirection::IN, TraverseDirection::OUT);
    std::sort(results.begin(), results.end());

    ASSERT_EQ(results.size(), 4u) << "Expected 4 results for IN->OUT pattern, got "
                                  << results.size();

    // All b values must be "B".
    for (auto& [a, b, c] : results) {
        EXPECT_EQ(b, "B") << "b should always be B for IN->OUT, got b=" << b;
    }

    // (A,B,A) and (A,B,C) must both appear.
    bool found_aba = false;
    bool found_abc = false;
    bool found_cba = false;
    bool found_cbc = false;
    for (auto& [a, b, c] : results) {
        if (a == "A" && b == "B" && c == "A") {
            found_aba = true;
        }
        if (a == "A" && b == "B" && c == "C") {
            found_abc = true;
        }
        if (a == "C" && b == "B" && c == "A") {
            found_cba = true;
        }
        if (a == "C" && b == "B" && c == "C") {
            found_cbc = true;
        }
    }
    EXPECT_TRUE(found_aba) << "(A,B,A) not found";
    EXPECT_TRUE(found_abc) << "(A,B,C) not found";
    EXPECT_TRUE(found_cba) << "(C,B,A) not found";
    EXPECT_TRUE(found_cbc) << "(C,B,C) not found";
}

} // namespace
} // namespace sixseven