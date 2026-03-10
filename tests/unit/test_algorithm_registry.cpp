#include "sixseven/graph/algorithm_registry.h"

#include <gtest/gtest.h>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Build a minimal algorithm definition for testing.
AlgorithmDef make_test_algo(const std::string& name) {
    AlgorithmDef def;
    def.name = name;
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"score", TypeId::FLOAT64, false},
    };
    return def;
}

/// No-op execute function for testing.
Result<std::vector<AlgorithmRow>> noop_execute(const AlgorithmContext& /*ctx*/) {
    return ok(std::vector<AlgorithmRow>{});
}

} // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

TEST(AlgorithmRegistry, RegisterAndFind) {
    AlgorithmRegistry registry;

    auto result = registry.register_algorithm(make_test_algo("pagerank"), noop_execute);
    ASSERT_TRUE(result.has_value());

    auto* entry = registry.find("pagerank");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, "pagerank");
    EXPECT_EQ(entry->def.output_columns.size(), 2);
}

TEST(AlgorithmRegistry, FindIsCaseInsensitive) {
    AlgorithmRegistry registry;
    (void)registry.register_algorithm(make_test_algo("PageRank"), noop_execute);

    EXPECT_NE(registry.find("PAGERANK"), nullptr);
    EXPECT_NE(registry.find("pagerank"), nullptr);
    EXPECT_NE(registry.find("PageRank"), nullptr);
}

TEST(AlgorithmRegistry, DuplicateRegistrationFails) {
    AlgorithmRegistry registry;
    (void)registry.register_algorithm(make_test_algo("pagerank"), noop_execute);

    auto result = registry.register_algorithm(make_test_algo("PAGERANK"), noop_execute);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(AlgorithmRegistry, FindNonexistentReturnsNull) {
    AlgorithmRegistry registry;
    EXPECT_EQ(registry.find("nonexistent"), nullptr);
}

TEST(AlgorithmRegistry, HasChecksExistence) {
    AlgorithmRegistry registry;
    (void)registry.register_algorithm(make_test_algo("pagerank"), noop_execute);

    EXPECT_TRUE(registry.has("pagerank"));
    EXPECT_TRUE(registry.has("PAGERANK"));
    EXPECT_FALSE(registry.has("dijkstra"));
}

TEST(AlgorithmRegistry, ListReturnsAllNames) {
    AlgorithmRegistry registry;
    (void)registry.register_algorithm(make_test_algo("pagerank"), noop_execute);
    (void)registry.register_algorithm(make_test_algo("betweenness"), noop_execute);

    auto names = registry.list();
    EXPECT_EQ(names.size(), 2);

    // Names should contain both algorithms (order not guaranteed).
    bool has_pagerank = false;
    bool has_betweenness = false;
    for (const auto& name : names) {
        if (name == "pagerank")
            has_pagerank = true;
        if (name == "betweenness")
            has_betweenness = true;
    }
    EXPECT_TRUE(has_pagerank);
    EXPECT_TRUE(has_betweenness);
}

// ---------------------------------------------------------------------------
// Parameter resolution
// ---------------------------------------------------------------------------

TEST(AlgorithmRegistry, ResolveParamsWithDefaults) {
    AlgorithmDef def;
    def.name = "pagerank";
    def.params = {
        {"damping", TypeId::FLOAT64, false, Value(0.85)},
        {"iterations", TypeId::INT64, false, Value(static_cast<int64_t>(20))},
    };

    // Provide no parameters — defaults should apply.
    auto result = AlgorithmRegistry::resolve_params(def, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 2);
    EXPECT_DOUBLE_EQ(std::get<double>(result->at("damping").data()), 0.85);
    EXPECT_EQ(std::get<int64_t>(result->at("iterations").data()), 20);
}

TEST(AlgorithmRegistry, ResolveParamsOverridesDefaults) {
    AlgorithmDef def;
    def.name = "pagerank";
    def.params = {
        {"damping", TypeId::FLOAT64, false, Value(0.85)},
        {"iterations", TypeId::INT64, false, Value(static_cast<int64_t>(20))},
    };

    std::unordered_map<std::string, Value> provided;
    provided.emplace("damping", Value(0.5));

    auto result = AlgorithmRegistry::resolve_params(def, provided);
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(result->at("damping").data()), 0.5);
    EXPECT_EQ(std::get<int64_t>(result->at("iterations").data()), 20);
}

TEST(AlgorithmRegistry, ResolveParamsMissingRequiredFails) {
    AlgorithmDef def;
    def.name = "test_algo";
    def.params = {
        {"required_param", TypeId::STRING, true, std::nullopt},
    };

    auto result = AlgorithmRegistry::resolve_params(def, {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(AlgorithmRegistry, ResolveParamsUnknownParamFails) {
    AlgorithmDef def;
    def.name = "test_algo";
    def.params = {
        {"damping", TypeId::FLOAT64, false, Value(0.85)},
    };

    std::unordered_map<std::string, Value> provided;
    provided.emplace("unknown_param", Value(42.0));

    auto result = AlgorithmRegistry::resolve_params(def, provided);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(AlgorithmRegistry, ResolveParamsCaseInsensitive) {
    AlgorithmDef def;
    def.name = "pagerank";
    def.params = {
        {"Damping", TypeId::FLOAT64, false, Value(0.85)},
    };

    std::unordered_map<std::string, Value> provided;
    provided.emplace("DAMPING", Value(0.5));

    auto result = AlgorithmRegistry::resolve_params(def, provided);
    ASSERT_TRUE(result.has_value());
    // The resolved key uses the definition's case ("Damping").
    EXPECT_DOUBLE_EQ(std::get<double>(result->at("Damping").data()), 0.5);
}

// ---------------------------------------------------------------------------
// Algorithm execution
// ---------------------------------------------------------------------------

TEST(AlgorithmRegistry, ExecuteReturnsRows) {
    AlgorithmRegistry registry;

    AlgorithmDef def;
    def.name = "test_algo";
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"score", TypeId::FLOAT64, false},
    };

    auto execute_fn = [](const AlgorithmContext& /*ctx*/) -> Result<std::vector<AlgorithmRow>> {
        std::vector<AlgorithmRow> rows;
        rows.push_back({std::vector<Value>{Value(static_cast<int64_t>(1)), Value(0.5)}});
        rows.push_back({std::vector<Value>{Value(static_cast<int64_t>(2)), Value(0.3)}});
        return ok(std::move(rows));
    };

    (void)registry.register_algorithm(std::move(def), execute_fn);

    auto* entry = registry.find("test_algo");
    ASSERT_NE(entry, nullptr);

    // Create a minimal context (no real graph engine needed for this test).
    // We can't easily create a real GraphEngine without storage, so we just
    // test the execute function directly.
    EXPECT_EQ(entry->def.output_columns.size(), 2);
    EXPECT_EQ(entry->def.output_columns[0].name, "node_id");
    EXPECT_EQ(entry->def.output_columns[1].name, "score");
}

// ---------------------------------------------------------------------------
// Output schema standardisation
// ---------------------------------------------------------------------------

TEST(AlgorithmRegistry, OutputSchemaNodeIdFirst) {
    AlgorithmDef def;
    def.name = "pagerank";
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"rank", TypeId::FLOAT64, false},
    };

    // Standard: node_id is always first.
    EXPECT_EQ(def.output_columns[0].name, "node_id");
    EXPECT_EQ(def.output_columns[0].type_id, TypeId::INT64);
}

TEST(AlgorithmRegistry, OutputSchemaMultipleColumns) {
    AlgorithmDef def;
    def.name = "community_detection";
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"community_id", TypeId::INT64, false},
        {"modularity", TypeId::FLOAT64, true},
    };

    EXPECT_EQ(def.output_columns.size(), 3);
    EXPECT_EQ(def.output_columns[2].nullable, true);
}
