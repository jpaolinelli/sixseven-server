/// QA adversarial tests for GDB-27: Edge Storage (Graph Engine).
/// Tests edge table operations, adjacency indexes, LINK/UNLINK, and edge properties.

#include "sixseven/catalog/catalog.h"
#include "sixseven/graph/edge_table.h"
#include "sixseven/graph/graph_engine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <set>
#include <vector>

using namespace sixseven;

// -- Helpers ------------------------------------------------------------------

static EdgeTableConfig make_config(const std::string& name, bool prevent_duplicates = false) {
    EdgeTableConfig config;
    config.edge_id = 1;
    config.name = name;
    config.source_table_id = 100;
    config.target_table_id = 200;
    config.source_pk_type = TypeId::INT64;
    config.target_pk_type = TypeId::INT64;
    config.prevent_duplicates = prevent_duplicates;
    return config;
}

static EdgeTableConfig make_config_with_properties(const std::string& name,
                                                   bool prevent_duplicates = false) {
    auto config = make_config(name, prevent_duplicates);
    config.property_columns = {
        {"weight", TypeId::FLOAT64},
        {"label", TypeId::STRING},
    };
    return config;
}

static Value pk(int64_t v) {
    return Value(v);
}

// =============================================================================
// EdgeTable boundary and adversarial tests
// =============================================================================

// -- Boundary PKs -------------------------------------------------------------

TEST(QA_EdgeTable, ZeroPrimaryKeys) {
    EdgeTable table(make_config("e"));

    auto r = table.insert_edge(pk(0), pk(0), {});
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto from = table.get_edges_from(pk(0));
    ASSERT_TRUE(from.has_value());
    ASSERT_EQ(from->size(), 1u);
    EXPECT_EQ((*from)[0].source_pk.as_int64(), 0);
    EXPECT_EQ((*from)[0].target_pk.as_int64(), 0);

    auto to = table.get_edges_to(pk(0));
    ASSERT_TRUE(to.has_value());
    ASSERT_EQ(to->size(), 1u);
}

TEST(QA_EdgeTable, NegativePrimaryKeys) {
    EdgeTable table(make_config("e"));

    auto r1 = table.insert_edge(pk(-1), pk(-2), {});
    ASSERT_TRUE(r1.has_value()) << r1.error().message;
    auto r2 = table.insert_edge(pk(-100), pk(-200), {});
    ASSERT_TRUE(r2.has_value()) << r2.error().message;

    auto from = table.get_edges_from(pk(-1));
    ASSERT_TRUE(from.has_value());
    ASSERT_EQ(from->size(), 1u);
    EXPECT_EQ((*from)[0].target_pk.as_int64(), -2);

    auto to = table.get_edges_to(pk(-200));
    ASSERT_TRUE(to.has_value());
    ASSERT_EQ(to->size(), 1u);
    EXPECT_EQ((*to)[0].source_pk.as_int64(), -100);
}

TEST(QA_EdgeTable, MaxInt64PrimaryKeys) {
    EdgeTable table(make_config("e"));

    int64_t max_val = std::numeric_limits<int64_t>::max();
    int64_t min_val = std::numeric_limits<int64_t>::min();

    auto r1 = table.insert_edge(pk(max_val), pk(min_val), {});
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    auto from = table.get_edges_from(pk(max_val));
    ASSERT_TRUE(from.has_value());
    ASSERT_EQ(from->size(), 1u);
    EXPECT_EQ((*from)[0].target_pk.as_int64(), min_val);

    auto to = table.get_edges_to(pk(min_val));
    ASSERT_TRUE(to.has_value());
    ASSERT_EQ(to->size(), 1u);
    EXPECT_EQ((*to)[0].source_pk.as_int64(), max_val);
}

TEST(QA_EdgeTable, MixedPositiveAndNegativePKs) {
    EdgeTable table(make_config("e"));

    ASSERT_TRUE(table.insert_edge(pk(-5), pk(5), {}).has_value());
    ASSERT_TRUE(table.insert_edge(pk(-5), pk(-10), {}).has_value());
    ASSERT_TRUE(table.insert_edge(pk(5), pk(-5), {}).has_value());

    // Forward: -5 should have two outgoing edges.
    auto from_neg5 = table.get_edges_from(pk(-5));
    ASSERT_TRUE(from_neg5.has_value());
    EXPECT_EQ(from_neg5->size(), 2u);

    // Reverse: -5 should have one incoming edge (from pk(5)).
    auto to_neg5 = table.get_edges_to(pk(-5));
    ASSERT_TRUE(to_neg5.has_value());
    EXPECT_EQ(to_neg5->size(), 1u);
    EXPECT_EQ((*to_neg5)[0].source_pk.as_int64(), 5);
}

// -- Double delete ------------------------------------------------------------

TEST(QA_EdgeTable, DoubleDeleteFails) {
    EdgeTable table(make_config("e"));

    auto r = table.insert_edge(pk(1), pk(2), {});
    ASSERT_TRUE(r.has_value());

    auto d1 = table.delete_edge(*r);
    ASSERT_TRUE(d1.has_value()) << d1.error().message;

    auto d2 = table.delete_edge(*r);
    ASSERT_FALSE(d2.has_value());
    EXPECT_EQ(d2.error().code, StatusCode::NOT_FOUND);
}

// -- Delete row ID 0 (invalid, never assigned) --------------------------------

TEST(QA_EdgeTable, DeleteRowIdZeroFails) {
    EdgeTable table(make_config("e"));

    auto d = table.delete_edge(0);
    ASSERT_FALSE(d.has_value());
    EXPECT_EQ(d.error().code, StatusCode::NOT_FOUND);
}

// -- Get edge row ID 0 (invalid) ----------------------------------------------

TEST(QA_EdgeTable, GetEdgeRowIdZeroFails) {
    EdgeTable table(make_config("e"));

    auto e = table.get_edge(0);
    ASSERT_FALSE(e.has_value());
    EXPECT_EQ(e.error().code, StatusCode::NOT_FOUND);
}

// -- Empty table lookups ------------------------------------------------------

TEST(QA_EdgeTable, ForwardLookupEmptyTable) {
    EdgeTable table(make_config("e"));

    auto from = table.get_edges_from(pk(1));
    ASSERT_TRUE(from.has_value());
    EXPECT_TRUE(from->empty());
}

TEST(QA_EdgeTable, ReverseLookupEmptyTable) {
    EdgeTable table(make_config("e"));

    auto to = table.get_edges_to(pk(1));
    ASSERT_TRUE(to.has_value());
    EXPECT_TRUE(to->empty());
}

TEST(QA_EdgeTable, FindEdgeEmptyTable) {
    EdgeTable table(make_config("e", true));

    auto found = table.find_edge(pk(1), pk(2));
    ASSERT_TRUE(found.has_value());
    EXPECT_FALSE(found->has_value());
}

TEST(QA_EdgeTable, FindEdgeEmptyTableNoUniqueIndex) {
    EdgeTable table(make_config("e", false));

    auto found = table.find_edge(pk(1), pk(2));
    ASSERT_TRUE(found.has_value());
    EXPECT_FALSE(found->has_value());
}

// -- Find edge after deletion returns nullopt ---------------------------------

TEST(QA_EdgeTable, FindEdgeAfterDeletion) {
    EdgeTable table(make_config("e", true));

    auto r = table.insert_edge(pk(1), pk(2), {});
    ASSERT_TRUE(r.has_value());

    ASSERT_TRUE(table.delete_edge(*r).has_value());

    auto found = table.find_edge(pk(1), pk(2));
    ASSERT_TRUE(found.has_value());
    EXPECT_FALSE(found->has_value());
}

TEST(QA_EdgeTable, FindEdgeAfterDeletionNoUniqueIndex) {
    EdgeTable table(make_config("e", false));

    auto r = table.insert_edge(pk(1), pk(2), {});
    ASSERT_TRUE(r.has_value());

    ASSERT_TRUE(table.delete_edge(*r).has_value());

    auto found = table.find_edge(pk(1), pk(2));
    ASSERT_TRUE(found.has_value());
    EXPECT_FALSE(found->has_value());
}

// -- Insert/delete/re-insert with unique constraint ---------------------------

TEST(QA_EdgeTable, InsertDeleteReinsertCycleWithUniqueConstraint) {
    EdgeTable table(make_config("e", true));

    for (int cycle = 0; cycle < 10; ++cycle) {
        auto r = table.insert_edge(pk(1), pk(2), {});
        ASSERT_TRUE(r.has_value()) << "cycle " << cycle << ": " << r.error().message;
        EXPECT_EQ(table.size(), 1u);

        // Duplicate should be blocked.
        auto dup = table.insert_edge(pk(1), pk(2), {});
        EXPECT_FALSE(dup.has_value());
        EXPECT_EQ(table.size(), 1u);

        ASSERT_TRUE(table.delete_edge(*r).has_value());
        EXPECT_EQ(table.size(), 0u);
    }
}

// -- Self-edge with duplicate prevention and deletion -------------------------

TEST(QA_EdgeTable, SelfEdgeDuplicatePreventionAndDeletion) {
    EdgeTable table(make_config("e", true));

    auto r = table.insert_edge(pk(42), pk(42), {});
    ASSERT_TRUE(r.has_value()) << r.error().message;

    // Duplicate self-edge should be blocked.
    auto dup = table.insert_edge(pk(42), pk(42), {});
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::CONSTRAINT_VIOLATION);

    // Delete self-edge.
    ASSERT_TRUE(table.delete_edge(*r).has_value());

    // Both indexes should be clean.
    auto from = table.get_edges_from(pk(42));
    ASSERT_TRUE(from.has_value());
    EXPECT_TRUE(from->empty());

    auto to = table.get_edges_to(pk(42));
    ASSERT_TRUE(to.has_value());
    EXPECT_TRUE(to->empty());

    // Re-insert should work.
    auto r2 = table.insert_edge(pk(42), pk(42), {});
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
}

// -- Properties via forward and reverse lookups -------------------------------

TEST(QA_EdgeTable, PropertiesRetrievableViaForwardLookup) {
    EdgeTable table(make_config_with_properties("e"));

    std::vector<Value> props = {Value(3.14), Value(std::string("pi"))};
    auto r = table.insert_edge(pk(10), pk(20), props);
    ASSERT_TRUE(r.has_value());

    auto edges = table.get_edges_from(pk(10));
    ASSERT_TRUE(edges.has_value());
    ASSERT_EQ(edges->size(), 1u);
    ASSERT_EQ((*edges)[0].properties.size(), 2u);
    EXPECT_DOUBLE_EQ((*edges)[0].properties[0].as_float64(), 3.14);
    EXPECT_EQ((*edges)[0].properties[1].as_string(), "pi");
}

TEST(QA_EdgeTable, PropertiesRetrievableViaReverseLookup) {
    EdgeTable table(make_config_with_properties("e"));

    std::vector<Value> props = {Value(2.718), Value(std::string("euler"))};
    auto r = table.insert_edge(pk(10), pk(20), props);
    ASSERT_TRUE(r.has_value());

    auto edges = table.get_edges_to(pk(20));
    ASSERT_TRUE(edges.has_value());
    ASSERT_EQ(edges->size(), 1u);
    ASSERT_EQ((*edges)[0].properties.size(), 2u);
    EXPECT_DOUBLE_EQ((*edges)[0].properties[0].as_float64(), 2.718);
    EXPECT_EQ((*edges)[0].properties[1].as_string(), "euler");
}

TEST(QA_EdgeTable, PropertiesRetrievableViaFindEdge) {
    EdgeTable table(make_config_with_properties("e", true));

    std::vector<Value> props = {Value(1.0), Value(std::string("one"))};
    ASSERT_TRUE(table.insert_edge(pk(5), pk(6), props).has_value());

    auto found = table.find_edge(pk(5), pk(6));
    ASSERT_TRUE(found.has_value());
    ASSERT_TRUE(found->has_value());
    ASSERT_EQ((*found)->properties.size(), 2u);
    EXPECT_DOUBLE_EQ((*found)->properties[0].as_float64(), 1.0);
    EXPECT_EQ((*found)->properties[1].as_string(), "one");
}

// -- No properties when schema has none ---------------------------------------

TEST(QA_EdgeTable, InsertWithEmptyPropertiesWhenSchemaHasNone) {
    EdgeTable table(make_config("e"));

    auto r = table.insert_edge(pk(1), pk(2), {});
    ASSERT_TRUE(r.has_value());

    auto edge = table.get_edge(*r);
    ASSERT_TRUE(edge.has_value());
    EXPECT_TRUE(edge->properties.empty());
}

TEST(QA_EdgeTable, InsertWithPropertiesWhenSchemaExpectsNoneFails) {
    EdgeTable table(make_config("e")); // No property columns.

    auto r = table.insert_edge(pk(1), pk(2), {Value(42)});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// -- Row ID monotonicity ------------------------------------------------------

TEST(QA_EdgeTable, RowIdsMonotonicallyIncreasing) {
    EdgeTable table(make_config("e"));

    uint64_t prev = 0;
    for (int i = 0; i < 100; ++i) {
        auto r = table.insert_edge(pk(i), pk(i + 1000), {});
        ASSERT_TRUE(r.has_value()) << r.error().message;
        EXPECT_GT(*r, prev);
        prev = *r;
    }
}

TEST(QA_EdgeTable, RowIdsUniqueEvenAfterDeletions) {
    EdgeTable table(make_config("e"));

    auto r1 = table.insert_edge(pk(1), pk(2), {});
    ASSERT_TRUE(r1.has_value());
    auto r2 = table.insert_edge(pk(3), pk(4), {});
    ASSERT_TRUE(r2.has_value());

    ASSERT_TRUE(table.delete_edge(*r1).has_value());

    // New insert should get a new row ID, not reuse the deleted one.
    auto r3 = table.insert_edge(pk(5), pk(6), {});
    ASSERT_TRUE(r3.has_value());
    EXPECT_NE(*r3, *r1);
    EXPECT_NE(*r3, *r2);
    EXPECT_GT(*r3, *r2);
}

// -- Insert many, delete all, verify clean state ------------------------------

TEST(QA_EdgeTable, InsertManyDeleteAllVerifyClean) {
    EdgeTable table(make_config("e"));

    std::vector<uint64_t> row_ids;
    constexpr int count = 200;
    for (int i = 0; i < count; ++i) {
        auto r = table.insert_edge(pk(i), pk(i + 1000), {});
        ASSERT_TRUE(r.has_value()) << r.error().message;
        row_ids.push_back(*r);
    }
    ASSERT_EQ(table.size(), static_cast<uint64_t>(count));

    for (uint64_t id : row_ids) {
        ASSERT_TRUE(table.delete_edge(id).has_value());
    }
    EXPECT_EQ(table.size(), 0u);
    EXPECT_TRUE(table.empty());

    // All lookups should return empty.
    for (int i = 0; i < count; ++i) {
        auto from = table.get_edges_from(pk(i));
        ASSERT_TRUE(from.has_value());
        EXPECT_TRUE(from->empty()) << "source pk " << i << " still has edges";
    }
}

// -- Duplicate edges without prevention: verify correct counts ----------------

TEST(QA_EdgeTable, DuplicateEdgesWithoutPreventionCorrectCounts) {
    EdgeTable table(make_config("e", false));

    // Insert same edge 5 times.
    for (int i = 0; i < 5; ++i) {
        auto r = table.insert_edge(pk(1), pk(2), {});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    EXPECT_EQ(table.size(), 5u);

    auto from = table.get_edges_from(pk(1));
    ASSERT_TRUE(from.has_value());
    EXPECT_EQ(from->size(), 5u);

    auto to = table.get_edges_to(pk(2));
    ASSERT_TRUE(to.has_value());
    EXPECT_EQ(to->size(), 5u);
}

TEST(QA_EdgeTable, DuplicateEdgesDeleteOneLeaveRest) {
    EdgeTable table(make_config("e", false));

    std::vector<uint64_t> ids;
    for (int i = 0; i < 3; ++i) {
        auto r = table.insert_edge(pk(1), pk(2), {});
        ASSERT_TRUE(r.has_value());
        ids.push_back(*r);
    }

    // Delete the middle one.
    ASSERT_TRUE(table.delete_edge(ids[1]).has_value());

    EXPECT_EQ(table.size(), 2u);
    auto from = table.get_edges_from(pk(1));
    ASSERT_TRUE(from.has_value());
    EXPECT_EQ(from->size(), 2u);
}

// -- Large scale stress -------------------------------------------------------

TEST(QA_EdgeTable, StressManySourcesToOneTarget) {
    EdgeTable table(make_config("e"));

    constexpr int count = 500;
    for (int i = 0; i < count; ++i) {
        auto r = table.insert_edge(pk(i), pk(9999), {});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    auto to = table.get_edges_to(pk(9999));
    ASSERT_TRUE(to.has_value());
    EXPECT_EQ(to->size(), static_cast<size_t>(count));

    // Spot-check a few sources.
    std::set<int64_t> sources;
    for (const auto& e : *to) {
        sources.insert(e.source_pk.as_int64());
    }
    EXPECT_EQ(sources.size(), static_cast<size_t>(count));
}

TEST(QA_EdgeTable, StressOneSourceToManyTargets) {
    EdgeTable table(make_config("e"));

    constexpr int count = 500;
    for (int i = 0; i < count; ++i) {
        auto r = table.insert_edge(pk(9999), pk(i), {});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    auto from = table.get_edges_from(pk(9999));
    ASSERT_TRUE(from.has_value());
    EXPECT_EQ(from->size(), static_cast<size_t>(count));
}

TEST(QA_EdgeTable, StressInsertDeleteInterleaved) {
    EdgeTable table(make_config("e"));

    // Insert 100 edges, delete odd-numbered ones, verify even-numbered remain.
    std::vector<uint64_t> ids;
    for (int i = 0; i < 100; ++i) {
        auto r = table.insert_edge(pk(i), pk(i + 1000), {});
        ASSERT_TRUE(r.has_value());
        ids.push_back(*r);
    }

    for (int i = 1; i < 100; i += 2) {
        ASSERT_TRUE(table.delete_edge(ids[static_cast<size_t>(i)]).has_value());
    }

    EXPECT_EQ(table.size(), 50u);

    // Each even-index source should have one edge.
    for (int i = 0; i < 100; i += 2) {
        auto from = table.get_edges_from(pk(i));
        ASSERT_TRUE(from.has_value());
        EXPECT_EQ(from->size(), 1u) << "pk " << i << " should have 1 edge";
    }

    // Each odd-index source should have zero edges.
    for (int i = 1; i < 100; i += 2) {
        auto from = table.get_edges_from(pk(i));
        ASSERT_TRUE(from.has_value());
        EXPECT_TRUE(from->empty()) << "pk " << i << " should have 0 edges";
    }
}

// -- String PK adversarial cases ----------------------------------------------

TEST(QA_EdgeTable, EmptyStringPrimaryKeys) {
    EdgeTableConfig config;
    config.edge_id = 1;
    config.name = "e";
    config.source_table_id = 100;
    config.target_table_id = 200;
    config.source_pk_type = TypeId::STRING;
    config.target_pk_type = TypeId::STRING;

    EdgeTable table(config);

    auto r = table.insert_edge(Value(std::string("")), Value(std::string("")), {});
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto from = table.get_edges_from(Value(std::string("")));
    ASSERT_TRUE(from.has_value());
    ASSERT_EQ(from->size(), 1u);
    EXPECT_EQ((*from)[0].target_pk.as_string(), "");
}

TEST(QA_EdgeTable, LongStringPrimaryKeys) {
    EdgeTableConfig config;
    config.edge_id = 1;
    config.name = "e";
    config.source_table_id = 100;
    config.target_table_id = 200;
    config.source_pk_type = TypeId::STRING;
    config.target_pk_type = TypeId::STRING;

    EdgeTable table(config);

    std::string long_str(1000, 'x');
    auto r = table.insert_edge(Value(long_str), Value(std::string("short")), {});
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto from = table.get_edges_from(Value(long_str));
    ASSERT_TRUE(from.has_value());
    ASSERT_EQ(from->size(), 1u);
    EXPECT_EQ((*from)[0].source_pk.as_string(), long_str);
}

// -- Size/empty consistency ---------------------------------------------------

TEST(QA_EdgeTable, SizeEmptyConsistency) {
    EdgeTable table(make_config("e"));

    EXPECT_EQ(table.size(), 0u);
    EXPECT_TRUE(table.empty());

    auto r1 = table.insert_edge(pk(1), pk(2), {});
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(table.size(), 1u);
    EXPECT_FALSE(table.empty());

    auto r2 = table.insert_edge(pk(3), pk(4), {});
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(table.size(), 2u);
    EXPECT_FALSE(table.empty());

    ASSERT_TRUE(table.delete_edge(*r1).has_value());
    EXPECT_EQ(table.size(), 1u);
    EXPECT_FALSE(table.empty());

    ASSERT_TRUE(table.delete_edge(*r2).has_value());
    EXPECT_EQ(table.size(), 0u);
    EXPECT_TRUE(table.empty());
}

// -- Multiple edges: delete in reverse order ----------------------------------

TEST(QA_EdgeTable, DeleteInReverseOrder) {
    EdgeTable table(make_config("e"));

    std::vector<uint64_t> ids;
    for (int i = 0; i < 10; ++i) {
        auto r = table.insert_edge(pk(1), pk(i + 100), {});
        ASSERT_TRUE(r.has_value());
        ids.push_back(*r);
    }

    // Delete in reverse order.
    for (int i = 9; i >= 0; --i) {
        ASSERT_TRUE(table.delete_edge(ids[static_cast<size_t>(i)]).has_value());
    }

    EXPECT_TRUE(table.empty());

    auto from = table.get_edges_from(pk(1));
    ASSERT_TRUE(from.has_value());
    EXPECT_TRUE(from->empty());
}

// -- Properties with many columns ---------------------------------------------

TEST(QA_EdgeTable, ManyPropertyColumns) {
    EdgeTableConfig config = make_config("e");
    constexpr int num_props = 20;
    for (int i = 0; i < num_props; ++i) {
        config.property_columns.push_back({"prop_" + std::to_string(i), TypeId::INT64});
    }

    EdgeTable table(config);

    std::vector<Value> props;
    props.reserve(num_props);
    for (int i = 0; i < num_props; ++i) {
        props.emplace_back(static_cast<int64_t>(i * 100));
    }

    auto r = table.insert_edge(pk(1), pk(2), props);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto edge = table.get_edge(*r);
    ASSERT_TRUE(edge.has_value());
    ASSERT_EQ(edge->properties.size(), static_cast<size_t>(num_props));
    for (int i = 0; i < num_props; ++i) {
        EXPECT_EQ(edge->properties[static_cast<size_t>(i)].as_int64(), i * 100);
    }
}

// -- find_edge returns the first match without unique index --------------------

TEST(QA_EdgeTable, FindEdgeWithoutUniqueIndexReturnsSomeMatch) {
    EdgeTable table(make_config("e", false));

    // Insert 3 duplicate edges.
    auto r1 = table.insert_edge(pk(1), pk(2), {});
    auto r2 = table.insert_edge(pk(1), pk(2), {});
    auto r3 = table.insert_edge(pk(1), pk(2), {});
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r3.has_value());

    // find_edge should return one of them.
    auto found = table.find_edge(pk(1), pk(2));
    ASSERT_TRUE(found.has_value());
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ((*found)->source_pk.as_int64(), 1);
    EXPECT_EQ((*found)->target_pk.as_int64(), 2);
}

// =============================================================================
// GraphEngine adversarial tests
// =============================================================================

static TableSchema make_table_schema(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", TypeId::INT64, false, ""},
        {1, "name", TypeId::STRING, true, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

class QA_GraphEngine : public ::testing::Test {
protected:
    void SetUp() override {
        auto t1 = catalog_.create_table(default_database_id, make_table_schema("users"));
        ASSERT_TRUE(t1.has_value()) << t1.error().message;
        users_id_ = *t1;

        auto t2 = catalog_.create_table(default_database_id, make_table_schema("posts"));
        ASSERT_TRUE(t2.has_value()) << t2.error().message;
        posts_id_ = *t2;
    }

    Catalog catalog_;
    GraphEngine engine_{catalog_};
    table_id_t users_id_ = 0;
    table_id_t posts_id_ = 0;
};

// -- Create edge type with empty name -----------------------------------------

TEST_F(QA_GraphEngine, CreateEdgeTypeEmptyName) {
    // Empty name might be unusual but shouldn't crash.
    auto result = engine_.create_edge_type(
        default_database_id, "", users_id_, posts_id_, TypeId::INT64, TypeId::INT64, {});
    // Whether it succeeds or fails, it should not crash.
    if (result.has_value()) {
        // If it succeeds, we should be able to link via empty name.
        auto link = engine_.link(default_database_id, "", pk(1), pk(2));
        EXPECT_TRUE(link.has_value());
    }
}

// -- Drop edge type removes all edges -----------------------------------------

TEST_F(QA_GraphEngine, DropEdgeTypeRemovesEdges) {
    ASSERT_TRUE(engine_
                    .create_edge_type(default_database_id,
                                      "follows",
                                      users_id_,
                                      users_id_,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      {})
                    .has_value());

    ASSERT_TRUE(engine_.link(default_database_id, "follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_.link(default_database_id, "follows", pk(3), pk(4)).has_value());

    ASSERT_TRUE(engine_.drop_edge_type(default_database_id, "follows").has_value());

    // Trying to query the dropped type should fail.
    auto from = engine_.get_edges_from(default_database_id, "follows", pk(1));
    ASSERT_FALSE(from.has_value());
    EXPECT_EQ(from.error().code, StatusCode::NOT_FOUND);
}

// -- Recreate dropped edge type works -----------------------------------------

TEST_F(QA_GraphEngine, RecreateDroppedEdgeType) {
    ASSERT_TRUE(engine_
                    .create_edge_type(default_database_id,
                                      "follows",
                                      users_id_,
                                      users_id_,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      {})
                    .has_value());
    ASSERT_TRUE(engine_.link(default_database_id, "follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_.drop_edge_type(default_database_id, "follows").has_value());

    // Recreate.
    ASSERT_TRUE(engine_
                    .create_edge_type(default_database_id,
                                      "follows",
                                      users_id_,
                                      users_id_,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      {})
                    .has_value());

    // New type should be empty (no carry-over from dropped type).
    auto from = engine_.get_edges_from(default_database_id, "follows", pk(1));
    ASSERT_TRUE(from.has_value());
    EXPECT_TRUE(from->empty());

    // Can link again.
    auto link = engine_.link(default_database_id, "follows", pk(10), pk(20));
    ASSERT_TRUE(link.has_value()) << link.error().message;
}

// -- Link after drop fails ----------------------------------------------------

TEST_F(QA_GraphEngine, LinkAfterDropFails) {
    ASSERT_TRUE(engine_
                    .create_edge_type(default_database_id,
                                      "follows",
                                      users_id_,
                                      users_id_,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      {})
                    .has_value());
    ASSERT_TRUE(engine_.drop_edge_type(default_database_id, "follows").has_value());

    auto link = engine_.link(default_database_id, "follows", pk(1), pk(2));
    ASSERT_FALSE(link.has_value());
    EXPECT_EQ(link.error().code, StatusCode::NOT_FOUND);
}

// -- Unlink_where with edges to multiple targets ------------------------------

TEST_F(QA_GraphEngine, UnlinkWhereOnlyDeletesMatchingTarget) {
    ASSERT_TRUE(engine_
                    .create_edge_type(default_database_id,
                                      "follows",
                                      users_id_,
                                      users_id_,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      {})
                    .has_value());

    // User 1 follows users 2, 3, 4.
    ASSERT_TRUE(engine_.link(default_database_id, "follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_.link(default_database_id, "follows", pk(1), pk(3)).has_value());
    ASSERT_TRUE(engine_.link(default_database_id, "follows", pk(1), pk(4)).has_value());

    // Unlink_where only for target=3 with always-true predicate.
    auto result = engine_.unlink_where(
        default_database_id, "follows", pk(1), pk(3), [](const EdgeRow&) { return true; });
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, 1u);

    // User 1 should still follow users 2 and 4.
    auto from = engine_.get_edges_from(default_database_id, "follows", pk(1));
    ASSERT_TRUE(from.has_value());
    EXPECT_EQ(from->size(), 2u);

    bool found_2 = false;
    bool found_4 = false;
    for (const auto& e : *from) {
        if (e.target_pk.as_int64() == 2)
            found_2 = true;
        if (e.target_pk.as_int64() == 4)
            found_4 = true;
    }
    EXPECT_TRUE(found_2);
    EXPECT_TRUE(found_4);
}

// -- Unlink_where on empty source returns 0 -----------------------------------

TEST_F(QA_GraphEngine, UnlinkWhereEmptySourceReturnsZero) {
    ASSERT_TRUE(engine_
                    .create_edge_type(default_database_id,
                                      "follows",
                                      users_id_,
                                      users_id_,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      {})
                    .has_value());

    auto result = engine_.unlink_where(
        default_database_id, "follows", pk(999), pk(888), [](const EdgeRow&) { return true; });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0u);
}

// -- List edge types after create and drop ------------------------------------

TEST_F(QA_GraphEngine, ListEdgeTypesAfterCreateAndDrop) {
    EXPECT_EQ(engine_.list_edge_types(default_database_id).size(), 0u);

    ASSERT_TRUE(engine_
                    .create_edge_type(default_database_id,
                                      "follows",
                                      users_id_,
                                      users_id_,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      {})
                    .has_value());
    ASSERT_TRUE(engine_
                    .create_edge_type(default_database_id,
                                      "authored",
                                      users_id_,
                                      posts_id_,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      {})
                    .has_value());
    EXPECT_EQ(engine_.list_edge_types(default_database_id).size(), 2u);

    ASSERT_TRUE(engine_.drop_edge_type(default_database_id, "follows").has_value());
    auto types = engine_.list_edge_types(default_database_id);
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0], "authored");
}

// -- GetEdgeTable for nonexistent type ----------------------------------------

TEST_F(QA_GraphEngine, GetEdgeTableNonexistent) {
    auto result = engine_.get_edge_table(default_database_id, "nope");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// -- Multiple independent edge types ------------------------------------------

TEST_F(QA_GraphEngine, MultipleTypesFullyIndependent) {
    ASSERT_TRUE(engine_
                    .create_edge_type(default_database_id,
                                      "follows",
                                      users_id_,
                                      users_id_,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      {})
                    .has_value());
    ASSERT_TRUE(engine_
                    .create_edge_type(default_database_id,
                                      "authored",
                                      users_id_,
                                      posts_id_,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      {})
                    .has_value());

    // Link in both.
    ASSERT_TRUE(engine_.link(default_database_id, "follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_.link(default_database_id, "follows", pk(1), pk(3)).has_value());
    ASSERT_TRUE(engine_.link(default_database_id, "authored", pk(1), pk(100)).has_value());

    // Unlink from "follows" shouldn't affect "authored".
    ASSERT_TRUE(engine_.unlink(default_database_id, "follows", pk(1), pk(2)).has_value());

    auto follows = engine_.get_edges_from(default_database_id, "follows", pk(1));
    ASSERT_TRUE(follows.has_value());
    EXPECT_EQ(follows->size(), 1u);

    auto authored = engine_.get_edges_from(default_database_id, "authored", pk(1));
    ASSERT_TRUE(authored.has_value());
    EXPECT_EQ(authored->size(), 1u);
    EXPECT_EQ((*authored)[0].target_pk.as_int64(), 100);
}

// -- Large link/unlink cycle --------------------------------------------------

TEST_F(QA_GraphEngine, LargeLinkUnlinkCycle) {
    ASSERT_TRUE(engine_
                    .create_edge_type(default_database_id,
                                      "follows",
                                      users_id_,
                                      users_id_,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      {})
                    .has_value());

    constexpr int count = 200;
    for (int i = 0; i < count; ++i) {
        auto r = engine_.link(default_database_id, "follows", pk(1), pk(i + 100));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    auto from = engine_.get_edges_from(default_database_id, "follows", pk(1));
    ASSERT_TRUE(from.has_value());
    EXPECT_EQ(from->size(), static_cast<size_t>(count));

    // Unlink all.
    for (int i = 0; i < count; ++i) {
        auto r = engine_.unlink(default_database_id, "follows", pk(1), pk(i + 100));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    from = engine_.get_edges_from(default_database_id, "follows", pk(1));
    ASSERT_TRUE(from.has_value());
    EXPECT_TRUE(from->empty());
}

// -- Unlink_where with predicate filtering on properties ----------------------

TEST_F(QA_GraphEngine, UnlinkWherePredicateFiltersCorrectly) {
    std::vector<ColumnDef> props = {{"score", TypeId::INT64}};
    ASSERT_TRUE(engine_
                    .create_edge_type(default_database_id,
                                      "rated",
                                      users_id_,
                                      posts_id_,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      props)
                    .has_value());

    // Insert edges with different scores.
    ASSERT_TRUE(engine_.link(default_database_id, "rated", pk(1), pk(10), {Value(int64_t(100))})
                    .has_value());
    ASSERT_TRUE(engine_.link(default_database_id, "rated", pk(1), pk(10), {Value(int64_t(50))})
                    .has_value());
    ASSERT_TRUE(engine_.link(default_database_id, "rated", pk(1), pk(10), {Value(int64_t(200))})
                    .has_value());
    ASSERT_TRUE(engine_.link(default_database_id, "rated", pk(1), pk(10), {Value(int64_t(30))})
                    .has_value());

    // Delete edges where score < 100.
    auto result =
        engine_.unlink_where(default_database_id, "rated", pk(1), pk(10), [](const EdgeRow& e) {
            return e.properties[0].as_int64() < 100;
        });
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, 2u); // score 50 and 30.

    auto remaining = engine_.get_edges_from(default_database_id, "rated", pk(1));
    ASSERT_TRUE(remaining.has_value());
    ASSERT_EQ(remaining->size(), 2u);

    // Remaining scores should be 100 and 200.
    std::set<int64_t> scores;
    for (const auto& e : *remaining) {
        scores.insert(e.properties[0].as_int64());
    }
    EXPECT_TRUE(scores.count(100));
    EXPECT_TRUE(scores.count(200));
}

// -- Unlink_where with false predicate deletes nothing ------------------------

TEST_F(QA_GraphEngine, UnlinkWhereFalsePredicateDeletesNothing) {
    ASSERT_TRUE(engine_
                    .create_edge_type(default_database_id,
                                      "follows",
                                      users_id_,
                                      users_id_,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      {})
                    .has_value());

    ASSERT_TRUE(engine_.link(default_database_id, "follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_.link(default_database_id, "follows", pk(1), pk(2)).has_value());
    ASSERT_TRUE(engine_.link(default_database_id, "follows", pk(1), pk(2)).has_value());

    auto result = engine_.unlink_where(
        default_database_id, "follows", pk(1), pk(2), [](const EdgeRow&) { return false; });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0u);

    auto from = engine_.get_edges_from(default_database_id, "follows", pk(1));
    ASSERT_TRUE(from.has_value());
    EXPECT_EQ(from->size(), 3u);
}

// -- Unlink non-existent edge -------------------------------------------------

TEST_F(QA_GraphEngine, UnlinkNonexistentEdgeFromExistingType) {
    ASSERT_TRUE(engine_
                    .create_edge_type(default_database_id,
                                      "follows",
                                      users_id_,
                                      users_id_,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      {})
                    .has_value());

    // Edge type exists but no edges yet.
    auto result = engine_.unlink(default_database_id, "follows", pk(1), pk(2));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// -- Drop nonexistent edge type -----------------------------------------------

TEST_F(QA_GraphEngine, DropNonexistentEdgeType) {
    auto result = engine_.drop_edge_type(default_database_id, "imaginary");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

// -- Edge properties with edge type that has no property columns ---------------

TEST_F(QA_GraphEngine, LinkWithUnexpectedPropertiesFails) {
    ASSERT_TRUE(engine_
                    .create_edge_type(default_database_id,
                                      "follows",
                                      users_id_,
                                      users_id_,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      {})
                    .has_value());

    auto result = engine_.link(default_database_id, "follows", pk(1), pk(2), {Value(int64_t(42))});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// -- Reverse index verification after link+unlink -----------------------------

TEST_F(QA_GraphEngine, ReverseIndexCleanAfterUnlink) {
    ASSERT_TRUE(engine_
                    .create_edge_type(default_database_id,
                                      "follows",
                                      users_id_,
                                      users_id_,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      {})
                    .has_value());

    // Multiple users follow user 5.
    ASSERT_TRUE(engine_.link(default_database_id, "follows", pk(1), pk(5)).has_value());
    ASSERT_TRUE(engine_.link(default_database_id, "follows", pk(2), pk(5)).has_value());
    ASSERT_TRUE(engine_.link(default_database_id, "follows", pk(3), pk(5)).has_value());

    // Unlink user 2 -> 5.
    ASSERT_TRUE(engine_.unlink(default_database_id, "follows", pk(2), pk(5)).has_value());

    // Reverse: user 5 should have 2 incoming edges (from 1 and 3).
    auto to = engine_.get_edges_to(default_database_id, "follows", pk(5));
    ASSERT_TRUE(to.has_value());
    ASSERT_EQ(to->size(), 2u);

    std::set<int64_t> sources;
    for (const auto& e : *to) {
        sources.insert(e.source_pk.as_int64());
    }
    EXPECT_TRUE(sources.count(1));
    EXPECT_TRUE(sources.count(3));
    EXPECT_FALSE(sources.count(2));
}

// -- Self-referencing edge via GraphEngine -------------------------------------

TEST_F(QA_GraphEngine, SelfEdgeViaGraphEngine) {
    ASSERT_TRUE(engine_
                    .create_edge_type(default_database_id,
                                      "follows",
                                      users_id_,
                                      users_id_,
                                      TypeId::INT64,
                                      TypeId::INT64,
                                      {},
                                      true)
                    .has_value());

    ASSERT_TRUE(engine_.link(default_database_id, "follows", pk(1), pk(1)).has_value());

    auto from = engine_.get_edges_from(default_database_id, "follows", pk(1));
    ASSERT_TRUE(from.has_value());
    EXPECT_EQ(from->size(), 1u);

    auto to = engine_.get_edges_to(default_database_id, "follows", pk(1));
    ASSERT_TRUE(to.has_value());
    EXPECT_EQ(to->size(), 1u);

    // Duplicate self-edge blocked.
    auto dup = engine_.link(default_database_id, "follows", pk(1), pk(1));
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, StatusCode::CONSTRAINT_VIOLATION);

    // Unlink self-edge.
    ASSERT_TRUE(engine_.unlink(default_database_id, "follows", pk(1), pk(1)).has_value());

    from = engine_.get_edges_from(default_database_id, "follows", pk(1));
    ASSERT_TRUE(from.has_value());
    EXPECT_TRUE(from->empty());

    to = engine_.get_edges_to(default_database_id, "follows", pk(1));
    ASSERT_TRUE(to.has_value());
    EXPECT_TRUE(to->empty());
}
