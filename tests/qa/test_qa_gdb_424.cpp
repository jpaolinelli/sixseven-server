/// @file test_qa_gdb_424.cpp
/// QA adversarial tests for GDB-424: Path Selectors & Shortest Path in MATCH.
///
/// Verifies:
///   AC1: ANY SHORTEST returns one shortest path
///   AC2: ALL SHORTEST returns all paths tied for shortest
///   AC3: SHORTEST 3 returns exactly 3 shortest paths
///   AC4: path_length(p) returns hop count
///   AC5: nodes(p) returns list of node PKs
///   AC6: edges(p) returns list of edge IDs
///   AC7: Existing SHORTEST PATH ... VIA still works
///   AC8: Unit tests written and passing
///   AC9: No regressions
///
/// Adversarial categories: diamond graphs, disconnected nodes, self-loops,
/// cycles, SHORTEST K > available paths, K=0 parse error, deeply nested paths,
/// path functions on null/empty, same-node queries, direction reversal,
/// operator reopen, stress tests.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/expr_evaluator.h"
#include "sixseven/executor/match_shortest_path.h"
#include "sixseven/executor/shortest_path.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/ast.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Parser helpers
// ============================================================================

std::vector<StmtPtr> parse_ok(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    EXPECT_TRUE(tokens.has_value()) << tokens.error().message;
    if (!tokens)
        return {};
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_TRUE(stmts.has_value()) << stmts.error().message;
    return stmts ? std::move(*stmts) : std::vector<StmtPtr>{};
}

StmtPtr parse_one(std::string_view sql) {
    auto stmts = parse_ok(sql);
    EXPECT_EQ(stmts.size(), 1u);
    if (stmts.size() != 1)
        return nullptr;
    return std::move(stmts[0]);
}

bool parse_fails(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens)
        return true;
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    return !stmts.has_value();
}

// ============================================================================
// Test fixture — richer graph for adversarial shortest path testing
// ============================================================================

/// Graph topology:
///   Diamond: 1→2→4, 1→3→4 (two paths from 1 to 4, length 2)
///   Extension: 4→5→6 (path from 1 to 6, length 4)
///   Bypass:   1→6 via alternate edge with 3 hops: 1→7→8→6
///   Disconnected: node 9 has no edges
///   Self-loop edge: 10→10
class QA_GDB424 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb424";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        // Create persons table.
        {
            TableSchema ts;
            ts.name = "persons";
            CatalogColumnDef pk_col;
            pk_col.ordinal = 0;
            pk_col.name = "id";
            pk_col.type_id = TypeId::INT64;
            pk_col.nullable = false;
            ts.columns.push_back(pk_col);
            ts.pk_columns = "id";
            auto tid = catalog_->create_table(default_database_id, std::move(ts));
            ASSERT_TRUE(tid.has_value()) << tid.error().message;
            persons_id_ = *tid;

            auto schema = catalog_->get_table(default_database_id, "persons");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, persons_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        // Insert nodes: 1-10.
        for (int64_t id = 1; id <= 10; ++id) {
            insert_person(id);
        }

        auto eid = graph_->create_edge_type(default_database_id,
                                            "knows",
                                            persons_id_,
                                            persons_id_,
                                            TypeId::INT64,
                                            TypeId::INT64,
                                            {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Diamond: 1→2→4, 1→3→4
        link(1, 2);
        link(1, 3);
        link(2, 4);
        link(3, 4);

        // Extension: 4→5→6
        link(4, 5);
        link(5, 6);

        // Alternate path to 6: 1→7→8→6
        link(1, 7);
        link(7, 8);
        link(8, 6);

        // Self-loop: 10→10
        link(10, 10);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void link(int64_t from, int64_t to) {
        auto r = graph_->link(default_database_id, "knows", Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    void insert_person(int64_t id) {
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

    /// Run a shortest-path MATCH operator and collect output tuples.
    std::vector<Tuple>
    run_shortest_match(MatchConfig config,
                       OutputSchema schema,
                       PathSelector selector,
                       const std::string& path_var = "p",
                       int32_t k = 0,
                       size_t max_visited = MatchShortestPathOperator::DEFAULT_MAX_VISITED) {
        BoundStatement bound;
        MatchShortestPathOperator op(*graph_,
                                     *catalog_,
                                     *storage_,
                                     default_database_id,
                                     std::move(config),
                                     std::move(schema),
                                     nullptr,
                                     bound,
                                     selector,
                                     path_var,
                                     k,
                                     max_visited);
        auto open_result = op.open();
        EXPECT_TRUE(open_result.has_value()) << open_result.error().message;

        std::vector<Tuple> results;
        while (true) {
            auto row = op.next();
            EXPECT_TRUE(row.has_value()) << row.error().message;
            if (!row->has_value())
                break;
            results.push_back(std::move(**row));
        }
        op.close();
        return results;
    }

    /// Build default config for persons→persons via knows.
    MatchConfig make_config() {
        MatchConfig config;
        config.nodes.push_back({"a", "persons"});
        config.nodes.push_back({"b", "persons"});
        config.edges.push_back(MatchEdgeDef("", "knows", TraverseDirection::OUT, 1, 10));
        return config;
    }

    /// Build output schema with source id, target id, and path.
    OutputSchema make_schema() {
        std::vector<OutputColumn> cols;
        cols.push_back({"a", "id", TypeId::INT64, false, persons_id_});
        cols.push_back({"b", "id", TypeId::INT64, false, persons_id_});
        cols.push_back({"p", "path", TypeId::PATH, false, 0});
        return OutputSchema(std::move(cols));
    }

    /// Filter results to a specific source→target pair.
    std::vector<Tuple> filter_pair(std::vector<Tuple>& results, int64_t from, int64_t to) {
        std::vector<Tuple> filtered;
        for (auto& t : results) {
            if (t.values[0].as_int64() == from && t.values[1].as_int64() == to) {
                filtered.push_back(std::move(t));
            }
        }
        return filtered;
    }

    /// Bind a SQL statement.
    Result<BoundStatement> bind_sql(const std::string& sql) {
        Lexer lexer(sql);
        auto tokens = lexer.tokenize();
        if (!tokens)
            return tl::unexpected(tokens.error());
        Parser parser(std::move(*tokens));
        auto stmts = parser.parse_all();
        if (!stmts)
            return tl::unexpected(stmts.error());
        if (stmts->empty())
            return make_error(StatusCode::PARSE_ERROR, "no statements parsed");
        Binder binder(*catalog_, default_database_id);
        return binder.bind(*stmts->front());
    }

    static constexpr database_id_t default_database_id = 1;
    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t persons_id_ = 0;
};

// ============================================================================
// AC1: ANY SHORTEST returns one shortest path
// ============================================================================

TEST_F(QA_GDB424, AC1_AnyShortestReturnsExactlyOnePath_Diamond) {
    // Diamond: 1→2→4 and 1→3→4, both length 2.
    // ANY SHORTEST should return exactly 1 path.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 1, 4);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].values[2].as_path().length(), 2);
}

TEST_F(QA_GDB424, AC1_AnyShortestPrefersShorterPath) {
    // 1→6: shortest via 1→7→8→6 (length 3) vs 1→2→4→5→6 or 1→3→4→5→6 (length 4).
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 1, 6);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].values[2].as_path().length(), 3);
}

TEST_F(QA_GDB424, AC1_AnyShortestDisconnectedPairReturnsNoPaths) {
    // Node 9 is disconnected — no path from 1 to 9.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 1, 9);
    EXPECT_EQ(filtered.size(), 0u);
}

// ============================================================================
// AC2: ALL SHORTEST returns all paths tied for shortest
// ============================================================================

TEST_F(QA_GDB424, AC2_AllShortestReturnsBothDiamondPaths) {
    // 1→4: two paths of length 2 (via 2 and via 3).
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ALL_SHORTEST);
    auto filtered = filter_pair(results, 1, 4);
    ASSERT_EQ(filtered.size(), 2u);
    EXPECT_EQ(filtered[0].values[2].as_path().length(), 2);
    EXPECT_EQ(filtered[1].values[2].as_path().length(), 2);

    // Verify the paths go through different intermediate nodes (2 vs 3).
    auto& path0 = filtered[0].values[2].as_path();
    auto& path1 = filtered[1].values[2].as_path();
    std::set<int64_t> intermediates;
    intermediates.insert(path0.steps[1].node_pk);
    intermediates.insert(path1.steps[1].node_pk);
    EXPECT_EQ(intermediates.size(), 2u) << "Paths should go through different intermediates";
    EXPECT_TRUE(intermediates.count(2) > 0);
    EXPECT_TRUE(intermediates.count(3) > 0);
}

TEST_F(QA_GDB424, AC2_AllShortestDoesNotReturnLongerPaths) {
    // 1→6: shortest path is length 3 (1→7→8→6).
    // There are also longer paths of length 4 (via diamond then 4→5→6).
    // ALL SHORTEST should only return the length-3 path(s).
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ALL_SHORTEST);
    auto filtered = filter_pair(results, 1, 6);
    ASSERT_GE(filtered.size(), 1u);
    for (const auto& t : filtered) {
        EXPECT_EQ(t.values[2].as_path().length(), 3)
            << "ALL SHORTEST should not return paths longer than shortest";
    }
}

// ============================================================================
// AC3: SHORTEST K returns exactly K shortest paths
// ============================================================================

TEST_F(QA_GDB424, AC3_ShortestK_ReturnsExactlyK) {
    // 1→4: 2 paths of length 2.
    // SHORTEST 1 should return exactly 1.
    auto results =
        run_shortest_match(make_config(), make_schema(), PathSelector::SHORTEST_K, "p", 1);
    auto filtered = filter_pair(results, 1, 4);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].values[2].as_path().length(), 2);
}

TEST_F(QA_GDB424, AC3_ShortestK_ReturnsLessThanKWhenNotEnoughPaths) {
    // 1→2: only 1 path of length 1 (direct edge).
    // SHORTEST 5 should return only the 1 available path, not error.
    auto results =
        run_shortest_match(make_config(), make_schema(), PathSelector::SHORTEST_K, "p", 5);
    auto filtered = filter_pair(results, 1, 2);
    ASSERT_GE(filtered.size(), 1u);
    // There should be at most 1 shortest path from 1→2.
    EXPECT_LE(filtered.size(), 1u);
}

TEST_F(QA_GDB424, AC3_ShortestK_K2ReturnsBothDiamondPaths) {
    // 1→4: 2 paths of length 2.
    // SHORTEST 2 should return both.
    auto results =
        run_shortest_match(make_config(), make_schema(), PathSelector::SHORTEST_K, "p", 2);
    auto filtered = filter_pair(results, 1, 4);
    ASSERT_EQ(filtered.size(), 2u);
}

// ============================================================================
// AC4: path_length(p) returns hop count
// ============================================================================

TEST_F(QA_GDB424, AC4_PathLengthZeroForSameNode) {
    // Path from 1 to 1 should have length 0.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 1, 1);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].values[2].as_path().length(), 0);
}

TEST_F(QA_GDB424, AC4_PathLengthDirectEdge) {
    // Path from 1 to 2: 1 hop.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 1, 2);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].values[2].as_path().length(), 1);
}

TEST_F(QA_GDB424, AC4_PathLengthMultiHop) {
    // Path from 1 to 6: 3 hops (1→7→8→6).
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 1, 6);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].values[2].as_path().length(), 3);
}

// ============================================================================
// AC5: nodes(p) returns list of node PKs
// ============================================================================

TEST_F(QA_GDB424, AC5_NodesContainsCorrectPKs) {
    // Path from 1 to 4 (via 2 or 3): 3 nodes.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 1, 4);
    ASSERT_EQ(filtered.size(), 1u);
    const auto& path = filtered[0].values[2].as_path();
    ASSERT_EQ(path.steps.size(), 3u);
    EXPECT_EQ(path.steps[0].node_pk, 1);
    // Middle is either 2 or 3.
    EXPECT_TRUE(path.steps[1].node_pk == 2 || path.steps[1].node_pk == 3);
    EXPECT_EQ(path.steps[2].node_pk, 4);
}

TEST_F(QA_GDB424, AC5_NodesSingleNodePath) {
    // Path from 1 to 1: 1 node.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 1, 1);
    ASSERT_EQ(filtered.size(), 1u);
    const auto& path = filtered[0].values[2].as_path();
    ASSERT_EQ(path.steps.size(), 1u);
    EXPECT_EQ(path.steps[0].node_pk, 1);
}

// ============================================================================
// AC6: edges(p) returns list of edge IDs
// ============================================================================

TEST_F(QA_GDB424, AC6_EdgesCountMatchesPathLength) {
    // Path from 1 to 4 (length 2) should have 2 edges.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 1, 4);
    ASSERT_EQ(filtered.size(), 1u);
    const auto& path = filtered[0].values[2].as_path();
    int64_t edge_count = 0;
    for (const auto& step : path.steps) {
        if (step.edge_id >= 0) {
            ++edge_count;
        }
    }
    EXPECT_EQ(edge_count, path.length());
}

TEST_F(QA_GDB424, AC6_EdgesEmptyForSameNodePath) {
    // Path from 1 to 1: no edges.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 1, 1);
    ASSERT_EQ(filtered.size(), 1u);
    const auto& path = filtered[0].values[2].as_path();
    for (const auto& step : path.steps) {
        EXPECT_EQ(step.edge_id, -1) << "Same-node path should have no edges";
    }
}

TEST_F(QA_GDB424, AC6_EdgeIdsAreNonNegativeForNonTerminal) {
    // Path from 1 to 6 (length 3): first 3 steps should have non-negative edge IDs,
    // last step should have -1.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 1, 6);
    ASSERT_EQ(filtered.size(), 1u);
    const auto& path = filtered[0].values[2].as_path();
    ASSERT_EQ(path.steps.size(), 4u);
    for (size_t i = 0; i < path.steps.size() - 1; ++i) {
        EXPECT_GE(path.steps[i].edge_id, 0)
            << "Non-terminal step " << i << " should have valid edge ID";
    }
    EXPECT_EQ(path.steps.back().edge_id, -1) << "Terminal step should have edge_id -1";
}

// ============================================================================
// AC7: Existing SHORTEST PATH ... VIA backward compatibility
// ============================================================================

TEST_F(QA_GDB424, AC7_ShortestPathViaStillParsesCorrectly) {
    auto stmt = parse_one("SHORTEST PATH FROM persons(1) TO persons(99) VIA knows");
    auto* sp = dynamic_cast<ShortestPathStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->from_table, "persons");
    EXPECT_EQ(sp->to_table, "persons");
    EXPECT_EQ(sp->edge_type, "knows");
}

TEST_F(QA_GDB424, AC7_OldOperatorStillFindsPaths) {
    ShortestPathConfig config;
    config.edge_type = "knows";
    config.from_key = Value(int64_t{1});
    config.to_key = Value(int64_t{4});
    config.direction = TraverseDirection::OUT;

    std::vector<OutputColumn> cols;
    cols.push_back({"", "node", TypeId::INT64, false, 0});
    cols.push_back({"", "hop", TypeId::INT64, false, 0});
    OutputSchema schema(std::move(cols));

    ShortestPathOperator op(*graph_, std::move(config), std::move(schema));
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    std::vector<int64_t> path;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value())
            break;
        path.push_back(row->value().values[0].as_int64());
    }
    op.close();

    ASSERT_EQ(path.size(), 3u);
    EXPECT_EQ(path[0], 1);
    EXPECT_EQ(path[2], 4);
}

// ============================================================================
// Adversarial: Parser edge cases
// ============================================================================

TEST_F(QA_GDB424, Parser_ShortestK_ZeroIsRejected) {
    // SHORTEST 0 should be rejected (K >= 1 required).
    EXPECT_TRUE(
        parse_fails("MATCH p = SHORTEST 0 (a:persons)-[:knows]->{1,10}(b:persons) RETURN a, b"));
}

TEST_F(QA_GDB424, Parser_ShortestK_NegativeIsRejected) {
    // Negative K should be rejected.
    EXPECT_TRUE(
        parse_fails("MATCH p = SHORTEST -1 (a:persons)-[:knows]->{1,10}(b:persons) RETURN a, b"));
}

TEST_F(QA_GDB424, Parser_ShortestK_LargeK) {
    // Very large K should parse successfully.
    auto stmt =
        parse_one("MATCH p = SHORTEST 1000 (a:persons)-[:knows]->{1,10}(b:persons) RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::SHORTEST_K);
    EXPECT_EQ(m->shortest_k, 1000);
}

TEST_F(QA_GDB424, Parser_PathVariablePreserved) {
    // Path variable name should be preserved correctly.
    auto stmt = parse_one(
        "MATCH mypath = ANY SHORTEST (a:persons)-[:knows]->{1,10}(b:persons) RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_variable, "mypath");
}

TEST_F(QA_GDB424, Parser_MissingPathSelectorKeyword) {
    // "p = FOOBAR" should fail — invalid selector keyword.
    EXPECT_TRUE(
        parse_fails("MATCH p = FOOBAR (a:persons)-[:knows]->{1,10}(b:persons) RETURN a, b"));
}

TEST_F(QA_GDB424, Parser_SelectFromMatchAllShortest) {
    auto stmt = parse_one("SELECT a.id, b.id "
                          "FROM MATCH p = ALL SHORTEST (a:persons)-[:knows]->{1,10}(b:persons) "
                          "WHERE a.id = 1");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    ASSERT_NE(sel->from[0].match_source, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::ALL_SHORTEST);
}

TEST_F(QA_GDB424, Parser_SelectFromMatchShortestK) {
    auto stmt = parse_one("SELECT a.id, b.id, path_length(p) AS hops "
                          "FROM MATCH p = SHORTEST 5 (a:persons)-[:knows]->{1,10}(b:persons) "
                          "WHERE a.id = 1");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_NE(sel->from[0].match_source, nullptr);
    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::SHORTEST_K);
    EXPECT_EQ(m->shortest_k, 5);
}

// ============================================================================
// Adversarial: Same-node path
// ============================================================================

TEST_F(QA_GDB424, SameNodePath_AnyShortestReturnsZeroLength) {
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 5, 5);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].values[2].as_path().length(), 0);
    EXPECT_EQ(filtered[0].values[2].as_path().steps.size(), 1u);
}

TEST_F(QA_GDB424, SameNodePath_AllShortestReturnsOneZeroLength) {
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ALL_SHORTEST);
    auto filtered = filter_pair(results, 5, 5);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].values[2].as_path().length(), 0);
}

// ============================================================================
// Adversarial: Disconnected nodes
// ============================================================================

TEST_F(QA_GDB424, DisconnectedNodeReturnsEmptyResult_AnyShort) {
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 9, 1);
    EXPECT_EQ(filtered.size(), 0u);
}

TEST_F(QA_GDB424, DisconnectedNodeReturnsEmptyResult_AllShort) {
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ALL_SHORTEST);
    auto filtered = filter_pair(results, 9, 1);
    EXPECT_EQ(filtered.size(), 0u);
}

TEST_F(QA_GDB424, DisconnectedNodeReturnsEmptyResult_ShortestK) {
    auto results =
        run_shortest_match(make_config(), make_schema(), PathSelector::SHORTEST_K, "p", 3);
    auto filtered = filter_pair(results, 9, 1);
    EXPECT_EQ(filtered.size(), 0u);
}

// ============================================================================
// Adversarial: Self-loop
// ============================================================================

TEST_F(QA_GDB424, SelfLoopNodeSameNodePath) {
    // Node 10 has a self-loop. Path from 10 to 10 should be zero-length
    // (same node trivial case), not a 1-hop self-loop path.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 10, 10);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].values[2].as_path().length(), 0)
        << "Same-node should short-circuit to zero-length, ignoring self-loop";
}

// ============================================================================
// Adversarial: Cycle detection
// ============================================================================

TEST_F(QA_GDB424, CycleDoesNotCauseInfiniteLoop) {
    // Add cycle: 6→1.
    link(6, 1);

    // ANY SHORTEST from 1 to 5 should still find 1→2→4→5 or 1→3→4→5 (length 3).
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 1, 5);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].values[2].as_path().length(), 3);
}

TEST_F(QA_GDB424, CycleAllShortest) {
    // Add cycle: 4→1 (creates 1→2→4→1 and 1→3→4→1 cycles).
    link(4, 1);

    // ALL SHORTEST from 1 to 4 should still return the two length-2 paths,
    // not infinite loop through cycles.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ALL_SHORTEST);
    auto filtered = filter_pair(results, 1, 4);
    ASSERT_EQ(filtered.size(), 2u);
    for (const auto& t : filtered) {
        EXPECT_EQ(t.values[2].as_path().length(), 2);
    }
}

// ============================================================================
// Adversarial: No path exists (wrong direction)
// ============================================================================

TEST_F(QA_GDB424, ReverseDirectionNoPathForward) {
    // All edges are OUT. Using OUT direction, 6→1 has no path.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 6, 1);
    EXPECT_EQ(filtered.size(), 0u);
}

// ============================================================================
// Adversarial: max_visited limit
// ============================================================================

TEST_F(QA_GDB424, MaxVisitedLimitTruncatesResults) {
    // With very small max_visited, results are truncated (not error).
    auto results =
        run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST, "p", 0, 2);
    // Should not crash, may return fewer results.
    // Just verify it terminates and produces valid tuples.
    for (const auto& t : results) {
        EXPECT_GE(t.values.size(), 3u);
    }
}

// ============================================================================
// Adversarial: Operator reopen produces same results
// ============================================================================

TEST_F(QA_GDB424, OperatorReopenProducesSameResults) {
    BoundStatement bound;
    MatchShortestPathOperator op(*graph_,
                                 *catalog_,
                                 *storage_,
                                 default_database_id,
                                 make_config(),
                                 make_schema(),
                                 nullptr,
                                 bound,
                                 PathSelector::ALL_SHORTEST,
                                 "p",
                                 0);

    // First open/drain/close.
    auto r1 = op.open();
    ASSERT_TRUE(r1.has_value());
    size_t count1 = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value());
        if (!row->has_value())
            break;
        ++count1;
    }
    op.close();

    // Second open/drain/close — should produce same count.
    auto r2 = op.open();
    ASSERT_TRUE(r2.has_value());
    size_t count2 = 0;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value());
        if (!row->has_value())
            break;
        ++count2;
    }
    op.close();

    EXPECT_EQ(count1, count2);
    EXPECT_GT(count1, 0u);
}

// GDB-977: the pure Path-struct edge-case tests that previously lived here
// (PathLengthEmptyPath, PathLengthSingleStep, PathLengthMultiStep,
// PathValueVariantRoundTrip) were exact duplicates of the fixture-free
// QA_GDB422_Path.{EmptyPathLength,SingleNodePathLength,LongPathLength,
// PathValueRoundTrip} cases in test_qa_gdb_422.cpp. They were removed: they
// touched no QA_GDB424 fixture state yet paid the full heavyweight SetUp.
// Path-struct coverage is retained by those QA_GDB422_Path tests.

// ============================================================================
// Adversarial: Path functions on non-path values
// ============================================================================

TEST_F(QA_GDB424, PathFunctions_NullArgReturnsNull) {
    // Verify that path_length propagates a NULL argument to a NULL result.
    // Build a tuple whose single column is a NULL value declared as PATH, then
    // evaluate path_length() over it and assert the evaluator succeeds and
    // returns a NULL Value (not an error, not a crash).
    Tuple tuple;
    tuple.values.push_back(Value::make_null());

    std::vector<OutputColumn> cols;
    cols.push_back({"", "p", TypeId::PATH, true, 0});
    OutputSchema schema(std::move(cols));

    auto col_ref = std::make_unique<ColumnRefExpr>();
    col_ref->column = "p";

    FunctionCallExpr fn_expr;
    fn_expr.name = "path_length";
    fn_expr.args.push_back(std::move(col_ref));

    BoundStatement bound;
    auto result = evaluate_expr(fn_expr, tuple, schema, bound);
    ASSERT_TRUE(result.has_value()) << "path_length(NULL) should succeed";
    EXPECT_TRUE(result->is_null()) << "path_length(NULL) should return a NULL Value";
}

// ============================================================================
// Adversarial: ALL SHORTEST with no tied paths (unique shortest)
// ============================================================================

TEST_F(QA_GDB424, AllShortestUniqueShortestReturnsOnePath) {
    // 1→2: only one direct path of length 1.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ALL_SHORTEST);
    auto filtered = filter_pair(results, 1, 2);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].values[2].as_path().length(), 1);
}

// ============================================================================
// Adversarial: SHORTEST K with K > total number of paths
// ============================================================================

TEST_F(QA_GDB424, ShortestK_KExceedsTotalPaths) {
    // 1→2: only 1 path. SHORTEST 100 should return just that 1 path.
    auto results =
        run_shortest_match(make_config(), make_schema(), PathSelector::SHORTEST_K, "p", 100);
    auto filtered = filter_pair(results, 1, 2);
    ASSERT_GE(filtered.size(), 1u);
    EXPECT_LE(filtered.size(), 1u);
}

// ============================================================================
// Adversarial: Verify path correctness for longer paths
// ============================================================================

TEST_F(QA_GDB424, PathNodesAreInOrder) {
    // 1→7→8→6 is the shortest path from 1 to 6 (length 3).
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 1, 6);
    ASSERT_EQ(filtered.size(), 1u);
    const auto& path = filtered[0].values[2].as_path();
    ASSERT_EQ(path.steps.size(), 4u);
    EXPECT_EQ(path.steps[0].node_pk, 1);
    EXPECT_EQ(path.steps[3].node_pk, 6);
    // The path should be 1→7→8→6.
    EXPECT_EQ(path.steps[1].node_pk, 7);
    EXPECT_EQ(path.steps[2].node_pk, 8);
}

// ============================================================================
// Adversarial: Multiple selectors produce consistent results
// ============================================================================

TEST_F(QA_GDB424, AnyShortestPathLengthMatchesAllShortestPathLength) {
    // For any source-target pair, the path length from ANY SHORTEST should
    // match the path length from ALL SHORTEST.
    auto any_results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto all_results = run_shortest_match(make_config(), make_schema(), PathSelector::ALL_SHORTEST);

    // Check 1→4 specifically.
    auto any_1_4 = filter_pair(any_results, 1, 4);
    auto all_1_4 = filter_pair(all_results, 1, 4);

    ASSERT_EQ(any_1_4.size(), 1u);
    ASSERT_GE(all_1_4.size(), 1u);
    EXPECT_EQ(any_1_4[0].values[2].as_path().length(), all_1_4[0].values[2].as_path().length());
}

// ============================================================================
// Adversarial: Stress test — many nodes
// ============================================================================

TEST_F(QA_GDB424, StressTest_LinearChain20Nodes) {
    // Add nodes 11-20 and chain 10→11→12→...→20.
    for (int64_t id = 11; id <= 20; ++id) {
        insert_person(id);
    }
    // Note: 10 has a self-loop from SetUp. Add chain from 10.
    for (int64_t id = 10; id < 20; ++id) {
        link(id, id + 1);
    }

    // ANY SHORTEST from 10 to 20 should find the 10-hop chain.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);
    auto filtered = filter_pair(results, 10, 20);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].values[2].as_path().length(), 10);

    // Verify path is sequential 10→11→12→...→20.
    const auto& path = filtered[0].values[2].as_path();
    ASSERT_EQ(path.steps.size(), 11u);
    for (int64_t i = 0; i <= 10; ++i) {
        EXPECT_EQ(path.steps[static_cast<size_t>(i)].node_pk, 10 + i);
    }
}

} // namespace
} // namespace sixseven
