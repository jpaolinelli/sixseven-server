#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
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

#include <filesystem>
#include <optional>
#include <vector>

using namespace sixseven;

// ===========================================================================
// Parser tests for path selectors
// ===========================================================================

namespace {

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

} // namespace

TEST(PathSelectorParser, AnyShortestParsed) {
    auto stmt =
        parse_one("MATCH p = ANY SHORTEST (a:persons)-[:knows]->{1,10}(b:persons) RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::ANY_SHORTEST);
    EXPECT_EQ(m->path_variable, "p");
    EXPECT_EQ(m->shortest_k, 0);
    ASSERT_EQ(m->pattern.size(), 2u);
    EXPECT_EQ(m->pattern[0].node.variable, "a");
    EXPECT_EQ(m->pattern[0].node.label, "persons");
}

TEST(PathSelectorParser, AllShortestParsed) {
    auto stmt =
        parse_one("MATCH p = ALL SHORTEST (a:persons)-[:knows]->{1,10}(b:persons) RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::ALL_SHORTEST);
    EXPECT_EQ(m->path_variable, "p");
    EXPECT_EQ(m->shortest_k, 0);
}

TEST(PathSelectorParser, ShortestKParsed) {
    auto stmt =
        parse_one("MATCH p = SHORTEST 3 (a:persons)-[:knows]->{1,10}(b:persons) RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::SHORTEST_K);
    EXPECT_EQ(m->path_variable, "p");
    EXPECT_EQ(m->shortest_k, 3);
}

TEST(PathSelectorParser, NoPathSelector) {
    auto stmt = parse_one("MATCH (a:persons)-[:knows]->(b:persons) RETURN a, b");
    auto* m = dynamic_cast<MatchStmt*>(stmt.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::NONE);
    EXPECT_TRUE(m->path_variable.empty());
}

TEST(PathSelectorParser, SelectFromMatchAnyShortest) {
    auto stmt = parse_one("SELECT a.name, b.name "
                          "FROM MATCH p = ANY SHORTEST (a:persons)-[:knows]->{1,10}(b:persons) "
                          "WHERE a.id = 1");
    auto* sel = dynamic_cast<SelectStmt*>(stmt.get());
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->from.size(), 1u);
    ASSERT_NE(sel->from[0].match_source, nullptr);

    auto* m = dynamic_cast<MatchStmt*>(sel->from[0].match_source.get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->path_selector, PathSelector::ANY_SHORTEST);
    EXPECT_EQ(m->path_variable, "p");
}

// ===========================================================================
// Executor tests for shortest path in MATCH
// ===========================================================================

/// Test fixture with a graph and storage for shortest path testing.
///
/// Graph topology:
///   1 → 2 → 3 → 6
///   1 → 4 → 5 → 6
///   (Two paths from 1 to 6, both length 3)
class MatchShortestPathTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_shortest_path_match";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        // Create 'persons' table with id column.
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

        // Insert persons: 1, 2, 3, 4, 5, 6.
        for (int64_t id : {1, 2, 3, 4, 5, 6}) {
            insert_person(id);
        }

        auto eid = graph_->create_edge_type(
            "knows", persons_id_, persons_id_, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        // Two equal-length paths: 1→2→3→6 and 1→4→5→6.
        link(1, 2);
        link(2, 3);
        link(3, 6);
        link(1, 4);
        link(4, 5);
        link(5, 6);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void link(int64_t from, int64_t to) {
        auto r = graph_->link("knows", Value(from), Value(to));
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
    std::vector<Tuple> run_shortest_match(MatchConfig config,
                                          OutputSchema schema,
                                          PathSelector selector,
                                          const std::string& path_var = "p",
                                          int32_t k = 0) {
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
                                     k);
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

    static constexpr database_id_t default_database_id = 1;
    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t persons_id_ = 0;
};

TEST_F(MatchShortestPathTest, AnyShortestFindsOnePath) {
    // ANY SHORTEST from 1 to 6 should return exactly one path of length 3.
    // There are two equal-length paths (1→2→3→6 and 1→4→5→6), but ANY returns just one.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);

    // Filter to only paths from 1 to 6.
    std::vector<Tuple> from_1_to_6;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 6) {
            from_1_to_6.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_6.size(), 1u);
    EXPECT_EQ(from_1_to_6[0].values[2].as_path().length(), 3);
}

TEST_F(MatchShortestPathTest, AllShortestFindsBothPaths) {
    // ALL SHORTEST from 1 to 6 should return both paths of length 3.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ALL_SHORTEST);

    std::vector<Tuple> from_1_to_6;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 6) {
            from_1_to_6.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_6.size(), 2u);
    EXPECT_EQ(from_1_to_6[0].values[2].as_path().length(), 3);
    EXPECT_EQ(from_1_to_6[1].values[2].as_path().length(), 3);
}

TEST_F(MatchShortestPathTest, ShortestKReturnsKPaths) {
    // SHORTEST 1 from 1 to 6 should return only 1 path.
    auto results =
        run_shortest_match(make_config(), make_schema(), PathSelector::SHORTEST_K, "p", 1);

    std::vector<Tuple> from_1_to_6;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 6) {
            from_1_to_6.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_6.size(), 1u);
    EXPECT_EQ(from_1_to_6[0].values[2].as_path().length(), 3);
}

TEST_F(MatchShortestPathTest, SameNodeReturnsZeroLengthPath) {
    // ANY SHORTEST from 1 to 1 should return a path of length 0.
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);

    std::vector<Tuple> from_1_to_1;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 1) {
            from_1_to_1.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_1.size(), 1u);
    EXPECT_EQ(from_1_to_1[0].values[2].as_path().length(), 0);
}

TEST_F(MatchShortestPathTest, PathContainsCorrectNodes) {
    // ANY SHORTEST from 1 to 3: shortest path is 1→2→3 (length 2).
    auto results = run_shortest_match(make_config(), make_schema(), PathSelector::ANY_SHORTEST);

    std::vector<Tuple> from_1_to_3;
    for (auto& t : results) {
        if (t.values[0].as_int64() == 1 && t.values[1].as_int64() == 3) {
            from_1_to_3.push_back(std::move(t));
        }
    }

    ASSERT_EQ(from_1_to_3.size(), 1u);
    const auto& path = from_1_to_3[0].values[2].as_path();
    EXPECT_EQ(path.length(), 2);
    ASSERT_EQ(path.steps.size(), 3u);
    EXPECT_EQ(path.steps[0].node_pk, 1);
    EXPECT_EQ(path.steps[1].node_pk, 2);
    EXPECT_EQ(path.steps[2].node_pk, 3);
}

// ===========================================================================
// Path function tests
// ===========================================================================

TEST(PathFunctions, PathLengthReturnsHopCount) {
    Path p;
    p.steps.push_back({1, 100});
    p.steps.push_back({2, 101});
    p.steps.push_back({3, -1});
    EXPECT_EQ(p.length(), 2);
}

TEST(PathFunctions, PathLengthEmptyPath) {
    Path p;
    EXPECT_EQ(p.length(), 0);
}

TEST(PathFunctions, PathLengthSingleNode) {
    Path p;
    p.steps.push_back({1, -1});
    EXPECT_EQ(p.length(), 0);
}

TEST(PathFunctions, NodesReturnsCorrectNodeList) {
    Path p;
    p.steps.push_back({1, 100});
    p.steps.push_back({2, 101});
    p.steps.push_back({3, -1});

    // Verify node PKs.
    ASSERT_EQ(p.steps.size(), 3u);
    EXPECT_EQ(p.steps[0].node_pk, 1);
    EXPECT_EQ(p.steps[1].node_pk, 2);
    EXPECT_EQ(p.steps[2].node_pk, 3);
}

TEST(PathFunctions, EdgesReturnsCorrectEdgeList) {
    Path p;
    p.steps.push_back({1, 100});
    p.steps.push_back({2, 101});
    p.steps.push_back({3, -1});

    // Verify edge IDs: edges are in steps with edge_id >= 0.
    std::vector<int64_t> edge_ids;
    for (const auto& step : p.steps) {
        if (step.edge_id >= 0) {
            edge_ids.push_back(step.edge_id);
        }
    }
    ASSERT_EQ(edge_ids.size(), 2u);
    EXPECT_EQ(edge_ids[0], 100);
    EXPECT_EQ(edge_ids[1], 101);
}

TEST(PathFunctions, PathValueInVariant) {
    Path p;
    p.steps.push_back({1, 100});
    p.steps.push_back({2, -1});

    Value v(std::move(p));
    EXPECT_EQ(v.type_id(), TypeId::PATH);
    EXPECT_EQ(v.as_path().length(), 1);
    EXPECT_EQ(v.as_path().steps.size(), 2u);
}

// ===========================================================================
// PathSelector enum and AST tests
// ===========================================================================

TEST(PathSelectorEnum, DefaultIsNone) {
    MatchStmt stmt;
    EXPECT_EQ(stmt.path_selector, PathSelector::NONE);
    EXPECT_TRUE(stmt.path_variable.empty());
    EXPECT_EQ(stmt.shortest_k, 0);
}

TEST(PathSelectorEnum, AllValues) {
    EXPECT_NE(PathSelector::NONE, PathSelector::ANY_SHORTEST);
    EXPECT_NE(PathSelector::ANY_SHORTEST, PathSelector::ALL_SHORTEST);
    EXPECT_NE(PathSelector::ALL_SHORTEST, PathSelector::SHORTEST_K);
}

// ===========================================================================
// Backward compatibility tests — SHORTEST PATH ... VIA still works
// ===========================================================================

TEST(BackwardCompat, ShortestPathViaStillParses) {
    auto stmt = parse_one("SHORTEST PATH FROM persons(1) TO persons(99) VIA knows");
    auto* sp = dynamic_cast<ShortestPathStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->from_table, "persons");
    EXPECT_EQ(sp->to_table, "persons");
    EXPECT_EQ(sp->edge_type, "knows");
}

TEST(BackwardCompat, ShortestPathViaWithDirection) {
    auto stmt = parse_one("SHORTEST PATH FROM persons(1) TO persons(99) VIA knows DIRECTION IN");
    auto* sp = dynamic_cast<ShortestPathStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(sp->direction, TraverseDirection::IN);
}

TEST(BackwardCompat, ShortestPathViaWithMaxDepth) {
    auto stmt = parse_one("SHORTEST PATH FROM persons(1) TO persons(99) VIA knows MAX_DEPTH 5");
    auto* sp = dynamic_cast<ShortestPathStmt*>(stmt.get());
    ASSERT_NE(sp, nullptr);
    ASSERT_TRUE(sp->max_depth.has_value());
    EXPECT_EQ(*sp->max_depth, 5);
}

/// Verify the old ShortestPathOperator still works with the existing test graph.
class ShortestPathBackwardCompat : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = std::make_unique<Catalog>();
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        TableSchema ts;
        ts.name = "nodes";
        CatalogColumnDef pk_col;
        pk_col.ordinal = 0;
        pk_col.name = "id";
        pk_col.type_id = TypeId::INT64;
        pk_col.nullable = false;
        ts.columns.push_back(pk_col);
        ts.pk_columns = "id";
        auto tid = catalog_->create_table(1, std::move(ts));
        ASSERT_TRUE(tid.has_value()) << tid.error().message;

        auto eid = graph_->create_edge_type("knows", *tid, *tid, TypeId::INT64, TypeId::INT64, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        auto r1 = graph_->link("knows", Value(int64_t{1}), Value(int64_t{2}));
        ASSERT_TRUE(r1.has_value());
        auto r2 = graph_->link("knows", Value(int64_t{2}), Value(int64_t{3}));
        ASSERT_TRUE(r2.has_value());
    }

    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<GraphEngine> graph_;
};

TEST_F(ShortestPathBackwardCompat, OldOperatorStillWorks) {
    ShortestPathConfig config;
    config.edge_type = "knows";
    config.from_key = Value(int64_t{1});
    config.to_key = Value(int64_t{3});
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
        if (!row->has_value()) {
            break;
        }
        path.push_back(row->value().values[0].as_int64());
    }
    op.close();

    ASSERT_EQ(path.size(), 3u);
    EXPECT_EQ(path[0], 1);
    EXPECT_EQ(path[1], 2);
    EXPECT_EQ(path[2], 3);
}
