// QA regression tests for GDB-1213.
//
// GDB-1213 fixes variable-length (and fixed-length single-hop) MATCH path
// construction so that a non-integer (STRING/UUID) primary key encountered
// while building a Path yields a clean INVALID_ARGUMENT error instead of
// silently dropping/misaligning path steps.
//
// Adversarial focus:
//   1. Non-integer PK correctness across {min,max} ranges, single-hop,
//      BFS multi-hop, and all directions (OUT/IN/BOTH).
//   2. No regression for integer-PK paths: full path content (node sequence +
//      edge_id alignment) must be identical to pre-fix behavior, not just
//      row counts.
//   3. Mixed graphs: does the operator still succeed for traversals that only
//      ever touch integer-PK nodes even if the table itself is int-keyed vs.
//      erroring for any string-keyed node reached along the path.
//   4. Consistency: same shape MATCH via MatchShortestPathOperator gives the
//      same INVALID_ARGUMENT behavior for non-integer PKs.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/match_shortest_path.h"
#include "sixseven/executor/pattern_match.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/variable_length_match.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

// ============================================================================
// Fixture: STRING-PK linear chain a->b->c->d->e ("persons" / "knows"), same
// shape as the sibling dev-test fixture, reused here for adversarial coverage
// across direction/quantifier combinations not covered by the dev tests.
// ============================================================================
class QaGdb1213StringPkTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1213_strpk";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        {
            TableSchema ts;
            ts.name = "persons";
            CatalogColumnDef id_col;
            id_col.ordinal = 0;
            id_col.name = "id";
            id_col.type_id = TypeId::STRING;
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
            persons_id_ = *tid;

            auto schema = catalog_->get_table(default_database_id, "persons");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, persons_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        insert_person("a", "Alice");
        insert_person("b", "Bob");
        insert_person("c", "Charlie");
        insert_person("d", "Diana");
        insert_person("e", "Eve");

        auto eid = graph_->create_edge_type(default_database_id,
                                            "knows",
                                            persons_id_,
                                            persons_id_,
                                            TypeId::STRING,
                                            TypeId::STRING,
                                            {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        link("knows", "a", "b");
        link("knows", "b", "c");
        link("knows", "c", "d");
        link("knows", "d", "e");
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void link(const std::string& edge_type, const std::string& from, const std::string& to) {
        auto r = graph_->link(default_database_id, edge_type, Value(from), Value(to));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    void insert_person(const std::string& id, const std::string& name) {
        auto ts = storage_->get_table_storage(persons_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table(default_database_id, "persons");
        ASSERT_TRUE(schema.has_value());

        std::vector<Value> vals = {Value(id), Value(name)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    OutputSchema make_schema() {
        std::vector<OutputColumn> out_cols;
        out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
        out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
        return OutputSchema(std::move(out_cols));
    }

    Result<void> run_open(TraverseDirection dir,
                          std::optional<int32_t> min_hops,
                          std::optional<int32_t> max_hops) {
        MatchConfig config;
        config.nodes.push_back({"a", "persons"});
        config.nodes.push_back({"b", "persons"});
        config.edges.push_back(MatchEdgeDef("r", "knows", dir, min_hops, max_hops));

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
        op.close();
        return result;
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t persons_id_ = 0;
};

// -- Direction coverage: OUT / IN / BOTH must all surface the clean error ----

TEST_F(QaGdb1213StringPkTest, VarLenOutDirectionErrorsCleanly) {
    auto result = run_open(TraverseDirection::OUT, 1, 3);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(result.error().message.find("integer primary key"), std::string::npos);
}

TEST_F(QaGdb1213StringPkTest, VarLenInDirectionErrorsCleanly) {
    auto result = run_open(TraverseDirection::IN, 1, 3);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(result.error().message.find("integer primary key"), std::string::npos);
}

TEST_F(QaGdb1213StringPkTest, VarLenBothDirectionErrorsCleanly) {
    auto result = run_open(TraverseDirection::BOTH, 1, 3);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(result.error().message.find("integer primary key"), std::string::npos);
}

// -- Quantifier boundary coverage --------------------------------------------

TEST_F(QaGdb1213StringPkTest, VarLenMinHopsZeroErrorsCleanly) {
    // min_hops = 0 emits the start node itself at depth 0 -- this still goes
    // through pk_to_int64 in the BFS start-node init block, so it must still
    // error rather than silently emit a bogus/zeroed PK.
    auto result = run_open(TraverseDirection::OUT, 0, 2);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QaGdb1213StringPkTest, VarLenSingleHopMinEqualsMaxErrorsCleanly) {
    // {1,1} -- exactly one hop; exercises the BFS path (is_variable_length()
    // is true because min_hops has a value) rather than the fixed-length path.
    auto result = run_open(TraverseDirection::OUT, 1, 1);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST_F(QaGdb1213StringPkTest, VarLenLargeMaxHopsErrorsCleanly) {
    // Large max_hops forces deeper BFS expansion before the chain is
    // exhausted; error must still surface immediately at the first
    // non-integer PK, not be swallowed by continued expansion.
    auto result = run_open(TraverseDirection::OUT, 1, 20);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// -- Fixed-length single-hop (non-quantified edge) ---------------------------

TEST_F(QaGdb1213StringPkTest, FixedLengthInDirectionErrorsCleanly) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::IN));

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
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    op.close();
}

TEST_F(QaGdb1213StringPkTest, FixedLengthBothDirectionErrorsCleanly) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::BOTH));

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
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    op.close();
}

// -- No crash / no partial garbage results -----------------------------------

TEST_F(QaGdb1213StringPkTest, ErrorLeavesNoPartialResults) {
    // Ensure that after an error, next() is not silently callable to drain
    // "partial" results -- open() failing must mean the operator produced no
    // usable output. We don't call next() after a failed open() (per Iterator
    // contract), but we do confirm open() doesn't crash and returns cleanly
    // on repeated invocation (no corrupted internal state from a half-built
    // BFS queue/path).
    auto r1 = run_open(TraverseDirection::OUT, 1, 3);
    ASSERT_FALSE(r1.has_value());
    auto r2 = run_open(TraverseDirection::OUT, 1, 3);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r1.error().code, r2.error().code);
}

// ============================================================================
// UUID primary key coverage.
// ============================================================================

class QaGdb1213UuidPkTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1213_uuidpk";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        {
            TableSchema ts;
            ts.name = "nodes_uuid";
            CatalogColumnDef id_col;
            id_col.ordinal = 0;
            id_col.name = "id";
            id_col.type_id = TypeId::UUID;
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

            auto schema = catalog_->get_table(default_database_id, "nodes_uuid");
            ASSERT_TRUE(schema.has_value());
            auto sr = storage_->create_table_storage(default_database_id, nodes_id_, *schema);
            ASSERT_TRUE(sr.has_value()) << sr.error().message;
        }

        uuid1_ = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                  0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
        uuid2_ = {0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
                  0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22};

        insert_node(uuid1_, "N1");
        insert_node(uuid2_, "N2");

        auto eid = graph_->create_edge_type(
            default_database_id, "linked", nodes_id_, nodes_id_, TypeId::UUID, TypeId::UUID, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        auto r = graph_->link(default_database_id, "linked", Value(uuid1_), Value(uuid2_));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void insert_node(const Uuid& id, const std::string& name) {
        auto ts = storage_->get_table_storage(nodes_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        auto schema = catalog_->get_table(default_database_id, "nodes_uuid");
        ASSERT_TRUE(schema.has_value());

        std::vector<Value> vals = {Value(id), Value(name)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t nodes_id_ = 0;
    Uuid uuid1_;
    Uuid uuid2_;
};

TEST_F(QaGdb1213UuidPkTest, VariableLengthMatchOverUuidPkErrorsCleanly) {
    MatchConfig config;
    config.nodes.push_back({"a", "nodes_uuid"});
    config.nodes.push_back({"b", "nodes_uuid"});
    config.edges.push_back(MatchEdgeDef("r", "linked", TraverseDirection::OUT, 1, 3));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, nodes_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, nodes_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    VariableLengthMatchOperator op(*graph_,
                                   *catalog_,
                                   *storage_,
                                   default_database_id,
                                   std::move(config),
                                   std::move(schema),
                                   nullptr,
                                   bound);
    auto result = op.open();
    ASSERT_FALSE(result.has_value())
        << "expected variable-length MATCH over UUID-PK table to fail cleanly";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(result.error().message.find("integer primary key"), std::string::npos)
        << "unexpected error message: " << result.error().message;
    op.close();
}

TEST_F(QaGdb1213UuidPkTest, FixedLengthMatchOverUuidPkErrorsCleanly) {
    MatchConfig config;
    config.nodes.push_back({"a", "nodes_uuid"});
    config.nodes.push_back({"b", "nodes_uuid"});
    config.edges.push_back(MatchEdgeDef("r", "linked", TraverseDirection::OUT));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"a", "name", TypeId::STRING, false, nodes_id_});
    out_cols.push_back({"b", "name", TypeId::STRING, false, nodes_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    VariableLengthMatchOperator op(*graph_,
                                   *catalog_,
                                   *storage_,
                                   default_database_id,
                                   std::move(config),
                                   std::move(schema),
                                   nullptr,
                                   bound);
    auto result = op.open();
    ASSERT_FALSE(result.has_value())
        << "expected fixed-length MATCH over UUID-PK table to fail cleanly";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    op.close();
}

// ============================================================================
// Consistency: MatchShortestPathOperator (sibling operator) over the same
// STRING-PK graph shape must exhibit the same INVALID_ARGUMENT behavior.
// ============================================================================

TEST_F(QaGdb1213StringPkTest, ShortestPathOverSamePatternAlsoErrorsCleanly) {
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 1, 3));

    BoundStatement bound;
    MatchShortestPathOperator op(*graph_,
                                 *catalog_,
                                 *storage_,
                                 default_database_id,
                                 std::move(config),
                                 make_schema(),
                                 nullptr,
                                 bound,
                                 PathSelector::ANY_SHORTEST,
                                 "p",
                                 0);
    auto result = op.open();
    ASSERT_FALSE(result.has_value())
        << "expected MatchShortestPathOperator over STRING-PK table to fail cleanly, "
           "consistent with VariableLengthMatchOperator";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(result.error().message.find("integer primary key"), std::string::npos)
        << "unexpected error message: " << result.error().message;
    op.close();
}

// ============================================================================
// Integer-PK regression: full path content verification (node sequence +
// edge_id alignment), not just row counts. Uses a fixture with distinguishable
// edge ids so misalignment would be detectable.
// ============================================================================

class QaGdb1213IntPkPathContentTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1213_intpk_path";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

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

        for (int64_t i = 1; i <= 5; ++i) {
            insert_person(i);
        }

        auto eid = graph_->create_edge_type(default_database_id,
                                            "knows",
                                            persons_id_,
                                            persons_id_,
                                            TypeId::INT64,
                                            TypeId::INT64,
                                            {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;

        link("knows", 1, 2);
        link("knows", 2, 3);
        link("knows", 3, 4);
        link("knows", 4, 5);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void link(const std::string& edge_type, int64_t from, int64_t to) {
        auto r = graph_->link(default_database_id, edge_type, Value(from), Value(to));
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

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t persons_id_ = 0;
};

TEST_F(QaGdb1213IntPkPathContentTest, VariableLengthPathColumnHasCompleteAlignedSteps) {
    // (a:persons)-[r:knows]->{2,2}(b:persons) with a "path" output column.
    // Verify the actual Path content: exactly 3 steps (2 hops), each
    // intermediate step's edge_id != -1 (an edge was actually traversed),
    // and the final step's edge_id == -1 (terminal node, no outgoing edge
    // recorded on this path). This directly exercises the regression concern:
    // "no dropped step; edge_id attached to the right node."
    MatchConfig config;
    config.nodes.push_back({"a", "persons"});
    config.nodes.push_back({"b", "persons"});
    config.edges.push_back(MatchEdgeDef("r", "knows", TraverseDirection::OUT, 2, 2));

    std::vector<OutputColumn> out_cols;
    out_cols.push_back({"path", "path", TypeId::PATH, false, persons_id_});
    OutputSchema schema(std::move(out_cols));

    BoundStatement bound;
    VariableLengthMatchOperator op(*graph_,
                                   *catalog_,
                                   *storage_,
                                   default_database_id,
                                   std::move(config),
                                   std::move(schema),
                                   nullptr,
                                   bound);
    auto open_result = op.open();
    ASSERT_TRUE(open_result.has_value()) << open_result.error().message;

    bool found_1_to_3 = false;
    while (true) {
        auto row = op.next();
        ASSERT_TRUE(row.has_value()) << row.error().message;
        if (!row->has_value())
            break;

        ASSERT_EQ((*row)->values.size(), 1u);
        const Value& path_val = (*row)->values[0];
        ASSERT_TRUE(std::holds_alternative<Path>(path_val.data()))
            << "path column did not carry a Path value";
        const Path& path = std::get<Path>(path_val.data());

        // {2,2} means exactly 2 hops -> 3 steps (start, mid, end).
        ASSERT_EQ(path.steps.size(), 3u) << "path must have exactly 3 steps for a 2-hop match "
                                             "-- a dropped step would shrink this";

        // Every step except the last must have a real edge_id (>= 0);
        // the last step is terminal (edge_id == -1).
        EXPECT_GE(path.steps[0].edge_id, 0)
            << "first step's edge_id must be attached (not misaligned/dropped)";
        EXPECT_GE(path.steps[1].edge_id, 0)
            << "second step's edge_id must be attached (not misaligned/dropped)";
        EXPECT_EQ(path.steps[2].edge_id, -1) << "terminal step must have no outgoing edge_id";

        if (path.steps[0].node_pk == 1 && path.steps[2].node_pk == 3) {
            found_1_to_3 = true;
            // Middle node on the chain 1->2->3 must be 2.
            EXPECT_EQ(path.steps[1].node_pk, 2)
                << "middle path step misaligned: expected node 2 between 1 and 3";
        }
    }
    op.close();

    EXPECT_TRUE(found_1_to_3) << "expected to find the 1->2->3 path with correct alignment";
}

} // namespace
} // namespace sixseven
