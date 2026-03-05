#include "sixseven/graph/edge_table.h"

#include <gtest/gtest.h>

using namespace sixseven;

// -- Helper: build an EdgeTableConfig -----------------------------------------

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

// == Create edge table ========================================================

TEST(EdgeTable, CreateEmpty) {
    EdgeTable table(make_config("follows"));
    EXPECT_TRUE(table.empty());
    EXPECT_EQ(table.size(), 0u);
    EXPECT_EQ(table.config().name, "follows");
}

// == Insert edges =============================================================

TEST(EdgeTable, InsertSingleEdge) {
    EdgeTable table(make_config("follows"));

    auto result = table.insert_edge(pk(1), pk(2), {});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_GE(*result, 1u);
    EXPECT_EQ(table.size(), 1u);
}

TEST(EdgeTable, InsertMultipleEdges) {
    EdgeTable table(make_config("follows"));

    auto r1 = table.insert_edge(pk(1), pk(2), {});
    auto r2 = table.insert_edge(pk(1), pk(3), {});
    auto r3 = table.insert_edge(pk(2), pk(3), {});
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(table.size(), 3u);

    // Row IDs should be unique.
    EXPECT_NE(*r1, *r2);
    EXPECT_NE(*r2, *r3);
}

TEST(EdgeTable, InsertWithProperties) {
    EdgeTable table(make_config_with_properties("follows"));

    std::vector<Value> props = {Value(0.75), Value(std::string("friend"))};
    auto result = table.insert_edge(pk(1), pk(2), props);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto edge = table.get_edge(*result);
    ASSERT_TRUE(edge.has_value()) << edge.error().message;
    ASSERT_EQ(edge->properties.size(), 2u);
    EXPECT_DOUBLE_EQ(edge->properties[0].as_float64(), 0.75);
    EXPECT_EQ(edge->properties[1].as_string(), "friend");
}

TEST(EdgeTable, InsertWrongPropertyCountFails) {
    EdgeTable table(make_config_with_properties("follows"));

    // Too few properties.
    auto r1 = table.insert_edge(pk(1), pk(2), {Value(0.5)});
    EXPECT_FALSE(r1.has_value());
    EXPECT_EQ(r1.error().code, StatusCode::INVALID_ARGUMENT);

    // Too many properties.
    auto r2 =
        table.insert_edge(pk(1), pk(2), {Value(0.5), Value(std::string("a")), Value(int32_t(1))});
    EXPECT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, StatusCode::INVALID_ARGUMENT);
}

// == Duplicate prevention =====================================================

TEST(EdgeTable, DuplicatePreventionBlocks) {
    EdgeTable table(make_config("follows", true));

    auto r1 = table.insert_edge(pk(1), pk(2), {});
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    auto r2 = table.insert_edge(pk(1), pk(2), {});
    EXPECT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, StatusCode::CONSTRAINT_VIOLATION);
    EXPECT_EQ(table.size(), 1u);
}

TEST(EdgeTable, DuplicatePreventionAllowsDifferentPairs) {
    EdgeTable table(make_config("follows", true));

    auto r1 = table.insert_edge(pk(1), pk(2), {});
    auto r2 = table.insert_edge(pk(1), pk(3), {});
    auto r3 = table.insert_edge(pk(2), pk(1), {}); // Reverse direction is different.
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(table.size(), 3u);
}

TEST(EdgeTable, NoDuplicatePreventionAllowsDuplicates) {
    EdgeTable table(make_config("follows", false));

    auto r1 = table.insert_edge(pk(1), pk(2), {});
    auto r2 = table.insert_edge(pk(1), pk(2), {});
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(table.size(), 2u);
    EXPECT_NE(*r1, *r2);
}

// == Forward lookup (get_edges_from) ==========================================

TEST(EdgeTable, ForwardLookupSingleEdge) {
    EdgeTable table(make_config("follows"));

    ASSERT_TRUE(table.insert_edge(pk(1), pk(2), {}).has_value());

    auto edges = table.get_edges_from(pk(1));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    ASSERT_EQ(edges->size(), 1u);
    EXPECT_EQ((*edges)[0].source_pk.as_int64(), 1);
    EXPECT_EQ((*edges)[0].target_pk.as_int64(), 2);
}

TEST(EdgeTable, ForwardLookupMultipleEdges) {
    EdgeTable table(make_config("follows"));

    ASSERT_TRUE(table.insert_edge(pk(1), pk(2), {}).has_value());
    ASSERT_TRUE(table.insert_edge(pk(1), pk(3), {}).has_value());
    ASSERT_TRUE(table.insert_edge(pk(1), pk(4), {}).has_value());
    ASSERT_TRUE(table.insert_edge(pk(2), pk(5), {}).has_value()); // Different source.

    auto edges = table.get_edges_from(pk(1));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    EXPECT_EQ(edges->size(), 3u);

    // All edges should have source_pk == 1.
    for (const auto& e : *edges) {
        EXPECT_EQ(e.source_pk.as_int64(), 1);
    }
}

TEST(EdgeTable, ForwardLookupNoEdges) {
    EdgeTable table(make_config("follows"));

    ASSERT_TRUE(table.insert_edge(pk(1), pk(2), {}).has_value());

    auto edges = table.get_edges_from(pk(99));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    EXPECT_TRUE(edges->empty());
}

// == Reverse lookup (get_edges_to) ============================================

TEST(EdgeTable, ReverseLookupSingleEdge) {
    EdgeTable table(make_config("follows"));

    ASSERT_TRUE(table.insert_edge(pk(1), pk(2), {}).has_value());

    auto edges = table.get_edges_to(pk(2));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    ASSERT_EQ(edges->size(), 1u);
    EXPECT_EQ((*edges)[0].source_pk.as_int64(), 1);
    EXPECT_EQ((*edges)[0].target_pk.as_int64(), 2);
}

TEST(EdgeTable, ReverseLookupMultipleEdges) {
    EdgeTable table(make_config("follows"));

    ASSERT_TRUE(table.insert_edge(pk(1), pk(5), {}).has_value());
    ASSERT_TRUE(table.insert_edge(pk(2), pk(5), {}).has_value());
    ASSERT_TRUE(table.insert_edge(pk(3), pk(5), {}).has_value());
    ASSERT_TRUE(table.insert_edge(pk(4), pk(6), {}).has_value()); // Different target.

    auto edges = table.get_edges_to(pk(5));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    EXPECT_EQ(edges->size(), 3u);

    // All edges should have target_pk == 5.
    for (const auto& e : *edges) {
        EXPECT_EQ(e.target_pk.as_int64(), 5);
    }
}

TEST(EdgeTable, ReverseLookupNoEdges) {
    EdgeTable table(make_config("follows"));

    ASSERT_TRUE(table.insert_edge(pk(1), pk(2), {}).has_value());

    auto edges = table.get_edges_to(pk(99));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    EXPECT_TRUE(edges->empty());
}

// == Get edge by row ID =======================================================

TEST(EdgeTable, GetEdgeById) {
    EdgeTable table(make_config("follows"));

    auto row_id = table.insert_edge(pk(1), pk(2), {});
    ASSERT_TRUE(row_id.has_value());

    auto edge = table.get_edge(*row_id);
    ASSERT_TRUE(edge.has_value()) << edge.error().message;
    EXPECT_EQ(edge->edge_row_id, *row_id);
    EXPECT_EQ(edge->source_pk.as_int64(), 1);
    EXPECT_EQ(edge->target_pk.as_int64(), 2);
}

TEST(EdgeTable, GetEdgeNotFound) {
    EdgeTable table(make_config("follows"));

    auto edge = table.get_edge(999);
    EXPECT_FALSE(edge.has_value());
    EXPECT_EQ(edge.error().code, StatusCode::NOT_FOUND);
}

// == Find edge by source/target ===============================================

TEST(EdgeTable, FindEdgeWithUniqueIndex) {
    EdgeTable table(make_config("follows", true));

    ASSERT_TRUE(table.insert_edge(pk(1), pk(2), {}).has_value());
    ASSERT_TRUE(table.insert_edge(pk(1), pk(3), {}).has_value());

    auto found = table.find_edge(pk(1), pk(2));
    ASSERT_TRUE(found.has_value()) << found.error().message;
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ((*found)->source_pk.as_int64(), 1);
    EXPECT_EQ((*found)->target_pk.as_int64(), 2);
}

TEST(EdgeTable, FindEdgeWithoutUniqueIndex) {
    EdgeTable table(make_config("follows", false));

    ASSERT_TRUE(table.insert_edge(pk(1), pk(2), {}).has_value());
    ASSERT_TRUE(table.insert_edge(pk(1), pk(3), {}).has_value());

    auto found = table.find_edge(pk(1), pk(2));
    ASSERT_TRUE(found.has_value()) << found.error().message;
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ((*found)->source_pk.as_int64(), 1);
    EXPECT_EQ((*found)->target_pk.as_int64(), 2);
}

TEST(EdgeTable, FindEdgeNotFound) {
    EdgeTable table(make_config("follows", true));

    ASSERT_TRUE(table.insert_edge(pk(1), pk(2), {}).has_value());

    auto found = table.find_edge(pk(1), pk(99));
    ASSERT_TRUE(found.has_value()) << found.error().message;
    EXPECT_FALSE(found->has_value());
}

// == Delete edges =============================================================

TEST(EdgeTable, DeleteEdge) {
    EdgeTable table(make_config("follows"));

    auto row_id = table.insert_edge(pk(1), pk(2), {});
    ASSERT_TRUE(row_id.has_value());
    EXPECT_EQ(table.size(), 1u);

    auto del = table.delete_edge(*row_id);
    ASSERT_TRUE(del.has_value()) << del.error().message;
    EXPECT_EQ(table.size(), 0u);

    // Edge no longer findable.
    auto edge = table.get_edge(*row_id);
    EXPECT_FALSE(edge.has_value());
}

TEST(EdgeTable, DeleteEdgeUpdatesForwardIndex) {
    EdgeTable table(make_config("follows"));

    auto r1 = table.insert_edge(pk(1), pk(2), {});
    auto r2 = table.insert_edge(pk(1), pk(3), {});
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());

    ASSERT_TRUE(table.delete_edge(*r1).has_value());

    auto edges = table.get_edges_from(pk(1));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    ASSERT_EQ(edges->size(), 1u);
    EXPECT_EQ((*edges)[0].target_pk.as_int64(), 3);
}

TEST(EdgeTable, DeleteEdgeUpdatesReverseIndex) {
    EdgeTable table(make_config("follows"));

    auto r1 = table.insert_edge(pk(1), pk(5), {});
    auto r2 = table.insert_edge(pk(2), pk(5), {});
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());

    ASSERT_TRUE(table.delete_edge(*r1).has_value());

    auto edges = table.get_edges_to(pk(5));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    ASSERT_EQ(edges->size(), 1u);
    EXPECT_EQ((*edges)[0].source_pk.as_int64(), 2);
}

TEST(EdgeTable, DeleteEdgeUpdatesUniqueIndex) {
    EdgeTable table(make_config("follows", true));

    auto r1 = table.insert_edge(pk(1), pk(2), {});
    ASSERT_TRUE(r1.has_value());

    // Duplicate initially blocked.
    auto dup = table.insert_edge(pk(1), pk(2), {});
    EXPECT_FALSE(dup.has_value());

    // Delete the edge.
    ASSERT_TRUE(table.delete_edge(*r1).has_value());

    // Now the same edge can be re-inserted.
    auto r2 = table.insert_edge(pk(1), pk(2), {});
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    EXPECT_EQ(table.size(), 1u);
}

TEST(EdgeTable, DeleteNonexistentEdgeFails) {
    EdgeTable table(make_config("follows"));

    auto del = table.delete_edge(999);
    EXPECT_FALSE(del.has_value());
    EXPECT_EQ(del.error().code, StatusCode::NOT_FOUND);
}

// == Property storage =========================================================

TEST(EdgeTable, PropertyStorageAndRetrieval) {
    EdgeTable table(make_config_with_properties("rated"));

    std::vector<Value> props1 = {Value(4.5), Value(std::string("great"))};
    auto r1 = table.insert_edge(pk(1), pk(10), props1);
    ASSERT_TRUE(r1.has_value());

    std::vector<Value> props2 = {Value(2.0), Value(std::string("ok"))};
    auto r2 = table.insert_edge(pk(1), pk(20), props2);
    ASSERT_TRUE(r2.has_value());

    // Retrieve via get_edge.
    auto e1 = table.get_edge(*r1);
    ASSERT_TRUE(e1.has_value());
    ASSERT_EQ(e1->properties.size(), 2u);
    EXPECT_DOUBLE_EQ(e1->properties[0].as_float64(), 4.5);
    EXPECT_EQ(e1->properties[1].as_string(), "great");

    // Retrieve via forward lookup.
    auto edges = table.get_edges_from(pk(1));
    ASSERT_TRUE(edges.has_value());
    EXPECT_EQ(edges->size(), 2u);
    for (const auto& e : *edges) {
        EXPECT_EQ(e.properties.size(), 2u);
    }
}

// == Edge with string PKs =====================================================

TEST(EdgeTable, StringPrimaryKeys) {
    EdgeTableConfig config;
    config.edge_id = 1;
    config.name = "links";
    config.source_table_id = 100;
    config.target_table_id = 200;
    config.source_pk_type = TypeId::STRING;
    config.target_pk_type = TypeId::STRING;
    config.prevent_duplicates = true;

    EdgeTable table(config);

    auto r1 = table.insert_edge(Value(std::string("page_a")), Value(std::string("page_b")), {});
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    auto edges = table.get_edges_from(Value(std::string("page_a")));
    ASSERT_TRUE(edges.has_value());
    ASSERT_EQ(edges->size(), 1u);
    EXPECT_EQ((*edges)[0].target_pk.as_string(), "page_b");

    auto rev = table.get_edges_to(Value(std::string("page_b")));
    ASSERT_TRUE(rev.has_value());
    ASSERT_EQ(rev->size(), 1u);
    EXPECT_EQ((*rev)[0].source_pk.as_string(), "page_a");
}

// == Self-referencing edges ===================================================

TEST(EdgeTable, SelfReferenceEdge) {
    EdgeTable table(make_config("follows"));

    auto r = table.insert_edge(pk(1), pk(1), {});
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto from = table.get_edges_from(pk(1));
    ASSERT_TRUE(from.has_value());
    EXPECT_EQ(from->size(), 1u);

    auto to = table.get_edges_to(pk(1));
    ASSERT_TRUE(to.has_value());
    EXPECT_EQ(to->size(), 1u);
}

// == Large-scale insert and lookup ============================================

TEST(EdgeTable, ManyEdgesFromSameSource) {
    EdgeTable table(make_config("follows"));

    constexpr int count = 100;
    for (int i = 0; i < count; ++i) {
        auto r = table.insert_edge(pk(1), pk(1000 + i), {});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    EXPECT_EQ(table.size(), static_cast<uint64_t>(count));

    auto edges = table.get_edges_from(pk(1));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    EXPECT_EQ(edges->size(), static_cast<size_t>(count));
}

TEST(EdgeTable, ManyEdgesToSameTarget) {
    EdgeTable table(make_config("follows"));

    constexpr int count = 100;
    for (int i = 0; i < count; ++i) {
        auto r = table.insert_edge(pk(1000 + i), pk(1), {});
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    auto edges = table.get_edges_to(pk(1));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    EXPECT_EQ(edges->size(), static_cast<size_t>(count));
}

// == Delete with multiple edges ===============================================

TEST(EdgeTable, DeleteOneOfManyFromSameSource) {
    EdgeTable table(make_config("follows"));

    auto r1 = table.insert_edge(pk(1), pk(2), {});
    auto r2 = table.insert_edge(pk(1), pk(3), {});
    auto r3 = table.insert_edge(pk(1), pk(4), {});
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r3.has_value());

    // Delete the middle edge.
    ASSERT_TRUE(table.delete_edge(*r2).has_value());

    auto edges = table.get_edges_from(pk(1));
    ASSERT_TRUE(edges.has_value()) << edges.error().message;
    EXPECT_EQ(edges->size(), 2u);

    // Verify remaining edges.
    bool found_2 = false;
    bool found_4 = false;
    for (const auto& e : *edges) {
        if (e.target_pk.as_int64() == 2)
            found_2 = true;
        if (e.target_pk.as_int64() == 4)
            found_4 = true;
    }
    EXPECT_TRUE(found_2);
    EXPECT_TRUE(found_4);
}

TEST(EdgeTable, DeleteAllEdgesFromSource) {
    EdgeTable table(make_config("follows"));

    auto r1 = table.insert_edge(pk(1), pk(2), {});
    auto r2 = table.insert_edge(pk(1), pk(3), {});
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());

    ASSERT_TRUE(table.delete_edge(*r1).has_value());
    ASSERT_TRUE(table.delete_edge(*r2).has_value());

    auto edges = table.get_edges_from(pk(1));
    ASSERT_TRUE(edges.has_value());
    EXPECT_TRUE(edges->empty());
    EXPECT_TRUE(table.empty());
}

// == Config access ============================================================

TEST(EdgeTable, ConfigAccess) {
    auto config = make_config_with_properties("authored", true);
    config.edge_id = 42;
    config.source_table_id = 10;
    config.target_table_id = 20;

    EdgeTable table(config);

    EXPECT_EQ(table.config().edge_id, 42);
    EXPECT_EQ(table.config().name, "authored");
    EXPECT_EQ(table.config().source_table_id, 10);
    EXPECT_EQ(table.config().target_table_id, 20);
    EXPECT_TRUE(table.config().prevent_duplicates);
    EXPECT_EQ(table.config().property_columns.size(), 2u);
}
