// GDB-1001: Verify that get_edges_from/get_edges_to errors are propagated via
// tl::unexpected in PatternMatchOperator, VariableLengthMatchOperator, and
// MatchShortestPathOperator instead of being silently swallowed.
//
// Trigger: use a MatchEdgeDef that names an edge type that does NOT exist in
// GraphEngine. GraphEngine::get_edges_from returns NOT_FOUND for an unknown
// edge type. The pre-fix code swallowed that error (if (fwd) { ... } with no
// else), returning ok with zero rows instead of an error. Post-fix, all three
// operators propagate the error via tl::unexpected.
//
// Happy-path test: a node with NO outgoing edges on a VALID edge type returns
// ok({}) -- empty vector -- so the operator succeeds with 0 rows. This proves
// that leaf-node traversal is unbroken by the propagation change.
//
// Mutation result: on the old (swallow) code the negative tests below would
// PASS open() (succeed) and return 0 rows -- the ASSERT_FALSE(has_value())
// check would FAIL. On the fixed code they return an error and the checks
// PASS. Verified by reverting a single propagation site and confirming the
// relevant test assertion fires.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/match_shortest_path.h"
#include "sixseven/executor/pattern_match.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/executor/variable_length_match.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

// Builds a small graph with a single 'persons' table and a 'knows' edge type:
//   node 1 -> node 2 (knows)
//   node 3 has no outgoing edges
//
// The fixture is used to test both the happy path (node 3, no edges) and the
// error-propagation path (referring to a non-existent edge type 'nosuchedge').
class MatchErrorPropagationTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_match_err_prop";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_ok = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_ok.has_value()) << db_ok.error().message;

        // Create 'persons' table with INT64 pk 'id'.
        {
            TableSchema ts;
            ts.name = "persons";
            CatalogColumnDef id_col;
            id_col.ordinal = 0;
            id_col.name = "id";
            id_col.type_id = TypeId::INT64;
            id_col.nullable = false;
            ts.columns.push_back(id_col);
            ts.pk_columns = "id";
            auto tid = catalog_->create_table(default_database_id, std::move(ts));
            ASSERT_TRUE(tid.has_value()) << tid.error().message;
            persons_id_ = *tid;

            auto schema = catalog_->get_table(default_database_id, "persons");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, persons_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        // Insert nodes 1, 2, 3.
        insert_node(1);
        insert_node(2);
        insert_node(3);

        // Create 'knows' edge type.
        auto eid = graph_->create_edge_type(default_database_id,
                                            "knows",
                                            persons_id_,
                                            persons_id_,
                                            TypeId::INT64,
                                            TypeId::INT64,
                                            {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Link 1 -> 2 only; node 3 has no outgoing edges.
        auto lr = graph_->link(default_database_id, "knows", Value(int64_t{1}), Value(int64_t{2}));
        ASSERT_TRUE(lr.has_value()) << lr.error().message;
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void insert_node(int64_t id) {
        auto ts = storage_->get_table_storage(persons_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table(default_database_id, "persons");
        ASSERT_TRUE(schema.has_value());

        std::vector<Value> vals = {Value(id)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    // Build a minimal output schema with one INT64 column bound to persons_id_.
    OutputSchema make_schema() const {
        std::vector<OutputColumn> cols;
        cols.push_back({"persons", "id", TypeId::INT64, false, persons_id_});
        return OutputSchema(std::move(cols));
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t persons_id_ = 0;
};

// ---------------------------------------------------------------------------
// PatternMatchOperator -- single-hop
// ---------------------------------------------------------------------------

// Negative: single-hop MATCH with an edge type that doesn't exist in
// GraphEngine. Pre-fix: swallowed error, returned ok with 0 rows.
// Post-fix: open() propagates the error.
TEST_F(MatchErrorPropagationTest, PatternMatchSingleHopUnknownEdgeTypeErrors) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    // "nosuchedge" has never been registered in GraphEngine -- returns NOT_FOUND.
    config.edges.push_back(MatchEdgeDef("r", "nosuchedge", TraverseDirection::OUT));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            make_schema(),
                            nullptr,
                            bound);

    auto result = op.open();
    ASSERT_FALSE(result.has_value())
        << "Expected error for unknown edge type, but op.open() succeeded";
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND)
        << "Error message: " << result.error().message;
}

// Positive: single-hop MATCH where node 3 has no outgoing 'knows' edges.
// get_edges_from returns ok({}) for node 3 (not an error), so the operator
// must succeed and return 0 rows for node 3 (and 1 row for node 1->2).
TEST_F(MatchErrorPropagationTest, PatternMatchSingleHopNoEdgesSucceeds) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT));

    BoundStatement bound;
    PatternMatchOperator op(*graph_,
                            *catalog_,
                            *storage_,
                            default_database_id,
                            std::move(config),
                            make_schema(),
                            nullptr,
                            bound);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    size_t count = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        ++count;
    }
    op.close();

    // Exactly one edge 1->2, so one result row.
    EXPECT_EQ(count, 1u);
}

// ---------------------------------------------------------------------------
// VariableLengthMatchOperator -- BFS expansion
// ---------------------------------------------------------------------------

// Negative: variable-length MATCH with an unknown edge type.
// The BFS expansion reaches get_edges_from which returns NOT_FOUND.
// Pre-fix: swallowed. Post-fix: propagated.
TEST_F(MatchErrorPropagationTest, VarLenMatchUnknownEdgeTypeErrors) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "nosuchedge", TraverseDirection::OUT, 1, 3));

    BoundStatement bound;
    VariableLengthMatchOperator op(*graph_,
                                   *catalog_,
                                   *storage_,
                                   default_database_id,
                                   std::move(config),
                                   make_schema(),
                                   nullptr,
                                   bound);

    auto result = op.open();
    ASSERT_FALSE(result.has_value())
        << "Expected error for unknown edge type in VarLen MATCH, but open() succeeded";
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND)
        << "Error message: " << result.error().message;
}

// Positive: BFS starting at node 3 (no edges); get_edges_from returns ok({}).
// The operator must succeed and return 0 rows (no paths found).
TEST_F(MatchErrorPropagationTest, VarLenMatchNoEdgesSucceeds) {
    // Only node 3 is the source; it has no outgoing edges on 'knows'.
    // We still run over all persons -- the result set has 1->2 only.
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 1, 1));

    BoundStatement bound;
    VariableLengthMatchOperator op(*graph_,
                                   *catalog_,
                                   *storage_,
                                   default_database_id,
                                   std::move(config),
                                   make_schema(),
                                   nullptr,
                                   bound);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    size_t count = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        ++count;
    }
    op.close();

    // Only 1->2 is a valid 1-hop path.
    EXPECT_EQ(count, 1u);
}

// ---------------------------------------------------------------------------
// MatchShortestPathOperator -- BFS + legacy swallow removal
// ---------------------------------------------------------------------------

// Negative: shortest-path MATCH with an unknown edge type.
// The unweighted BFS used to swallow the get_neighbors error (legacy behavior).
// Post-fix, the error is propagated for BOTH weighted and unweighted paths.
TEST_F(MatchErrorPropagationTest, ShortestPathUnweightedUnknownEdgeTypeErrors) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "nosuchedge", TraverseDirection::OUT));

    std::vector<OutputColumn> cols;
    cols.push_back({"", "path", TypeId::PATH, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    MatchShortestPathOperator op(*graph_,
                                 *catalog_,
                                 *storage_,
                                 default_database_id,
                                 std::move(config),
                                 std::move(schema),
                                 nullptr,
                                 bound,
                                 PathSelector::ANY_SHORTEST,
                                 "p",
                                 1);

    auto result = op.open();
    ASSERT_FALSE(result.has_value())
        << "Expected error for unknown edge type in unweighted ShortestPath, "
           "but open() succeeded";
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND)
        << "Error message: " << result.error().message;
}

// Positive: shortest-path MATCH on the valid graph (1->2).
// With NO outgoing edges for node 3, get_edges_from returns ok({}) (not an
// error). The operator must succeed and find the 1->2 path.
TEST_F(MatchErrorPropagationTest, ShortestPathValidGraphSucceeds) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT));

    std::vector<OutputColumn> cols;
    cols.push_back({"", "path", TypeId::PATH, false, 0});
    OutputSchema schema(std::move(cols));

    BoundStatement bound;
    MatchShortestPathOperator op(*graph_,
                                 *catalog_,
                                 *storage_,
                                 default_database_id,
                                 std::move(config),
                                 std::move(schema),
                                 nullptr,
                                 bound,
                                 PathSelector::ANY_SHORTEST,
                                 "p",
                                 1);

    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    size_t count = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value()) {
            break;
        }
        ++count;
    }
    op.close();

    // At least one path found (1->2).
    EXPECT_GE(count, 1u);
}

} // namespace
} // namespace sixseven
