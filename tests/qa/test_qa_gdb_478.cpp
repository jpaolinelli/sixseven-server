/// @file test_qa_gdb_478.cpp
/// QA adversarial tests for GDB-478: Algorithm Framework & Table-Valued Function
/// Infrastructure.
///
/// Verifies:
///   AC1: Algorithm registration framework works.
///   AC2: Named parameter parsing works.
///   AC3: Planner resolves algorithm functions.
///   AC4: Output schema standardised (node_id first, algorithm-specific columns).
///   AC5: Unit tests passing.
///
/// Adversarial categories: boundary values, error paths, edge cases, stress,
/// lifecycle, thread safety, parser edge cases.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/algorithm_scan.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace sixseven {
namespace {

// ============================================================================
// Helpers
// ============================================================================

/// Build a simple algorithm def with two output columns: node_id + score.
AlgorithmDef make_simple_algo(const std::string& name) {
    AlgorithmDef def;
    def.name = name;
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"score", TypeId::FLOAT64, false},
    };
    return def;
}

/// Execute function that returns fixed rows.
Result<std::vector<AlgorithmRow>> fixed_rows_execute(const AlgorithmContext& /*ctx*/) {
    std::vector<AlgorithmRow> rows;
    rows.push_back({std::vector<Value>{Value(int64_t{1}), Value(0.85)}});
    rows.push_back({std::vector<Value>{Value(int64_t{2}), Value(0.42)}});
    rows.push_back({std::vector<Value>{Value(int64_t{3}), Value(0.13)}});
    return ok(std::move(rows));
}

/// Execute function that returns an error.
Result<std::vector<AlgorithmRow>> error_execute(const AlgorithmContext& /*ctx*/) {
    return make_error(StatusCode::INTERNAL_ERROR, "algorithm execution failed");
}

/// Execute function that returns zero rows.
Result<std::vector<AlgorithmRow>> empty_execute(const AlgorithmContext& /*ctx*/) {
    return ok(std::vector<AlgorithmRow>{});
}

/// No-op execute.
Result<std::vector<AlgorithmRow>> noop_execute(const AlgorithmContext& /*ctx*/) {
    return ok(std::vector<AlgorithmRow>{});
}

// ============================================================================
// Part 1: AlgorithmRegistry adversarial tests
// ============================================================================

// --- Registration edge cases ---

TEST(QA_GDB478_Registry, RegisterEmptyName) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_simple_algo(""), noop_execute);
    // Empty name should succeed (framework doesn't prohibit it).
    EXPECT_TRUE(result.has_value());
    // But we should still find it.
    EXPECT_NE(registry.find(""), nullptr);
}

TEST(QA_GDB478_Registry, RegisterVeryLongName) {
    AlgorithmRegistry registry;
    std::string long_name(10000, 'a');
    auto result = registry.register_algorithm(make_simple_algo(long_name), noop_execute);
    EXPECT_TRUE(result.has_value());
    auto* entry = registry.find(long_name);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.name, long_name);
}

TEST(QA_GDB478_Registry, RegisterNameWithSpecialChars) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_simple_algo("my-algo_v2.0!@#$"), noop_execute);
    EXPECT_TRUE(result.has_value());
    EXPECT_NE(registry.find("my-algo_v2.0!@#$"), nullptr);
}

TEST(QA_GDB478_Registry, DuplicateCaseVariants) {
    AlgorithmRegistry registry;
    (void)registry.register_algorithm(make_simple_algo("PageRank"), noop_execute);

    // All case variants should be treated as duplicates.
    auto r1 = registry.register_algorithm(make_simple_algo("pagerank"), noop_execute);
    EXPECT_FALSE(r1.has_value());
    EXPECT_EQ(r1.error().code, StatusCode::ALREADY_EXISTS);

    auto r2 = registry.register_algorithm(make_simple_algo("PAGERANK"), noop_execute);
    EXPECT_FALSE(r2.has_value());

    auto r3 = registry.register_algorithm(make_simple_algo("pAgErAnK"), noop_execute);
    EXPECT_FALSE(r3.has_value());
}

TEST(QA_GDB478_Registry, RegisterAlgoWithNoOutputColumns) {
    AlgorithmRegistry registry;
    AlgorithmDef def;
    def.name = "no_output";
    // Zero output columns.
    auto result = registry.register_algorithm(std::move(def), noop_execute);
    EXPECT_TRUE(result.has_value());
    auto* entry = registry.find("no_output");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.output_columns.size(), 0);
}

TEST(QA_GDB478_Registry, RegisterAlgoWithNoParams) {
    AlgorithmRegistry registry;
    AlgorithmDef def;
    def.name = "no_params";
    def.output_columns = {{"node_id", TypeId::INT64, false}};
    // No params at all.
    auto result = registry.register_algorithm(std::move(def), noop_execute);
    EXPECT_TRUE(result.has_value());
}

TEST(QA_GDB478_Registry, RegisterManyAlgorithms) {
    AlgorithmRegistry registry;
    constexpr int N = 500;
    for (int i = 0; i < N; ++i) {
        auto r = registry.register_algorithm(make_simple_algo("algo_" + std::to_string(i)),
                                             noop_execute);
        ASSERT_TRUE(r.has_value()) << "Failed at i=" << i;
    }
    auto names = registry.list();
    EXPECT_EQ(names.size(), N);

    // Spot-check lookup.
    EXPECT_NE(registry.find("algo_0"), nullptr);
    EXPECT_NE(registry.find("algo_499"), nullptr);
    EXPECT_EQ(registry.find("algo_500"), nullptr);
}

// --- Find edge cases ---

TEST(QA_GDB478_Registry, FindEmptyStringOnEmptyRegistry) {
    AlgorithmRegistry registry;
    EXPECT_EQ(registry.find(""), nullptr);
}

TEST(QA_GDB478_Registry, HasEmptyStringOnEmptyRegistry) {
    AlgorithmRegistry registry;
    EXPECT_FALSE(registry.has(""));
}

TEST(QA_GDB478_Registry, ListOnEmptyRegistry) {
    AlgorithmRegistry registry;
    auto names = registry.list();
    EXPECT_TRUE(names.empty());
}

// --- resolve_params edge cases ---

TEST(QA_GDB478_Params, EmptyDefEmptyProvided) {
    AlgorithmDef def;
    def.name = "empty";
    // No params defined, none provided.
    auto result = AlgorithmRegistry::resolve_params(def, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

TEST(QA_GDB478_Params, UnknownParamOnEmptyDef) {
    AlgorithmDef def;
    def.name = "empty";
    std::unordered_map<std::string, Value> provided;
    provided.emplace("rogue", Value(42.0));

    auto result = AlgorithmRegistry::resolve_params(def, provided);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB478_Params, MultipleUnknownParams) {
    AlgorithmDef def;
    def.name = "test";
    def.params = {{"damping", TypeId::FLOAT64, false, Value(0.85)}};

    std::unordered_map<std::string, Value> provided;
    provided.emplace("foo", Value(1.0));
    provided.emplace("bar", Value(2.0));

    auto result = AlgorithmRegistry::resolve_params(def, provided);
    EXPECT_FALSE(result.has_value());
}

TEST(QA_GDB478_Params, RequiredParamWithDefault) {
    // A param that is both required=true and has a default.
    // The default should satisfy the requirement.
    AlgorithmDef def;
    def.name = "test";
    def.params = {{"threshold", TypeId::FLOAT64, true, Value(0.5)}};

    auto result = AlgorithmRegistry::resolve_params(def, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(result->at("threshold").data()), 0.5);
}

TEST(QA_GDB478_Params, AllParamsRequired) {
    AlgorithmDef def;
    def.name = "strict";
    def.params = {
        {"a", TypeId::INT64, true, std::nullopt},
        {"b", TypeId::INT64, true, std::nullopt},
        {"c", TypeId::INT64, true, std::nullopt},
    };

    // Missing all → error.
    auto r1 = AlgorithmRegistry::resolve_params(def, {});
    EXPECT_FALSE(r1.has_value());

    // Provide only some → error.
    std::unordered_map<std::string, Value> partial;
    partial.emplace("a", Value(int64_t{1}));
    auto r2 = AlgorithmRegistry::resolve_params(def, partial);
    EXPECT_FALSE(r2.has_value());

    // Provide all → success.
    partial.emplace("b", Value(int64_t{2}));
    partial.emplace("c", Value(int64_t{3}));
    auto r3 = AlgorithmRegistry::resolve_params(def, partial);
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r3->size(), 3);
}

TEST(QA_GDB478_Params, CaseInsensitiveLookupPreservesDefCase) {
    AlgorithmDef def;
    def.name = "test";
    def.params = {{"MyParam", TypeId::FLOAT64, false, Value(1.0)}};

    std::unordered_map<std::string, Value> provided;
    provided.emplace("myparam", Value(2.0));

    auto result = AlgorithmRegistry::resolve_params(def, provided);
    ASSERT_TRUE(result.has_value());
    // Key should be "MyParam" (definition case), not "myparam".
    EXPECT_TRUE(result->count("MyParam") > 0);
    EXPECT_DOUBLE_EQ(std::get<double>(result->at("MyParam").data()), 2.0);
}

TEST(QA_GDB478_Params, OptionalParamNotRequired) {
    AlgorithmDef def;
    def.name = "test";
    def.params = {
        {"optional_no_default", TypeId::STRING, false, std::nullopt},
    };

    // Not provided, not required, no default → should NOT be in resolved map.
    auto result = AlgorithmRegistry::resolve_params(def, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->count("optional_no_default"), 0);
}

// --- Thread safety ---

TEST(QA_GDB478_Registry, ConcurrentRegistration) {
    AlgorithmRegistry registry;
    constexpr int THREADS = 8;
    constexpr int PER_THREAD = 50;
    std::atomic<int> success_count{0};

    auto worker = [&](int thread_id) {
        for (int i = 0; i < PER_THREAD; ++i) {
            auto name = "algo_" + std::to_string(thread_id) + "_" + std::to_string(i);
            auto r = registry.register_algorithm(make_simple_algo(name), noop_execute);
            if (r.has_value()) {
                success_count.fetch_add(1);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back(worker, t);
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(success_count.load(), THREADS * PER_THREAD);
    EXPECT_EQ(registry.list().size(), THREADS * PER_THREAD);
}

TEST(QA_GDB478_Registry, ConcurrentFindAndRegister) {
    AlgorithmRegistry registry;
    // Pre-register one algorithm.
    (void)registry.register_algorithm(make_simple_algo("existing"), noop_execute);

    std::atomic<bool> stop{false};
    std::atomic<int> find_count{0};

    // Reader thread: continuously finds existing algorithm.
    auto reader = [&]() {
        while (!stop.load()) {
            auto* entry = registry.find("existing");
            if (entry != nullptr) {
                find_count.fetch_add(1);
            }
        }
    };

    // Writer thread: registers more algorithms.
    auto writer = [&]() {
        for (int i = 0; i < 100; ++i) {
            (void)registry.register_algorithm(make_simple_algo("new_" + std::to_string(i)),
                                              noop_execute);
        }
        stop.store(true);
    };

    std::thread r(reader);
    std::thread w(writer);
    w.join();
    r.join();

    EXPECT_GT(find_count.load(), 0);
    EXPECT_NE(registry.find("existing"), nullptr);
}

// ============================================================================
// Part 2: AlgorithmScanOperator lifecycle tests
// ============================================================================

class QA_GDB478_ScanOperator : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = std::make_unique<AlgorithmRegistry>();

        AlgorithmDef def;
        def.name = "test_algo";
        def.output_columns = {
            {"node_id", TypeId::INT64, false},
            {"score", TypeId::FLOAT64, false},
        };
        (void)registry_->register_algorithm(std::move(def), fixed_rows_execute);

        AlgorithmDef err_def;
        err_def.name = "error_algo";
        err_def.output_columns = {{"node_id", TypeId::INT64, false}};
        (void)registry_->register_algorithm(std::move(err_def), error_execute);

        AlgorithmDef empty_def;
        empty_def.name = "empty_algo";
        empty_def.output_columns = {
            {"node_id", TypeId::INT64, false},
            {"score", TypeId::FLOAT64, false},
        };
        (void)registry_->register_algorithm(std::move(empty_def), empty_execute);
    }

    std::unique_ptr<AlgorithmRegistry> registry_;
};

TEST_F(QA_GDB478_ScanOperator, OpenNextCloseNormalLifecycle) {
    auto* entry = registry_->find("test_algo");
    ASSERT_NE(entry, nullptr);

    // We need a GraphEngine for the context, but since fixed_rows_execute
    // doesn't use it, we construct a minimal one.
    Catalog catalog;
    GraphEngine ge(catalog);
    AlgorithmContext ctx{ge, default_database_id, "test_edge", {}};
    OutputSchema schema({
        {"test_algo", "node_id", TypeId::INT64, false, 0},
        {"test_algo", "score", TypeId::FLOAT64, false, 0},
    });

    AlgorithmScanOperator op(*entry, std::move(ctx), std::move(schema));

    auto open_r = op.open();
    ASSERT_TRUE(open_r.has_value());

    // Read all 3 rows.
    int count = 0;
    while (true) {
        auto next_r = op.next();
        ASSERT_TRUE(next_r.has_value());
        if (!next_r->has_value()) {
            break;
        }
        ++count;
    }
    EXPECT_EQ(count, 3);

    op.close();
}

TEST_F(QA_GDB478_ScanOperator, NextAfterExhausted) {
    auto* entry = registry_->find("test_algo");
    ASSERT_NE(entry, nullptr);

    Catalog catalog;
    GraphEngine ge(catalog);
    AlgorithmContext ctx{ge, default_database_id, "edge", {}};
    OutputSchema schema({
        {"", "node_id", TypeId::INT64, false, 0},
        {"", "score", TypeId::FLOAT64, false, 0},
    });

    AlgorithmScanOperator op(*entry, std::move(ctx), std::move(schema));
    ASSERT_TRUE(op.open().has_value());

    // Drain all rows.
    while (true) {
        auto r = op.next();
        ASSERT_TRUE(r.has_value());
        if (!r->has_value())
            break;
    }

    // Call next again — should return nullopt, not crash.
    auto r = op.next();
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
    op.close();
}

TEST_F(QA_GDB478_ScanOperator, EmptyResultSet) {
    auto* entry = registry_->find("empty_algo");
    ASSERT_NE(entry, nullptr);

    Catalog catalog;
    GraphEngine ge(catalog);
    AlgorithmContext ctx{ge, default_database_id, "edge", {}};
    OutputSchema schema({
        {"", "node_id", TypeId::INT64, false, 0},
        {"", "score", TypeId::FLOAT64, false, 0},
    });

    AlgorithmScanOperator op(*entry, std::move(ctx), std::move(schema));
    ASSERT_TRUE(op.open().has_value());

    // Immediately returns nullopt.
    auto r = op.next();
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
    op.close();
}

TEST_F(QA_GDB478_ScanOperator, ExecuteErrorPropagated) {
    auto* entry = registry_->find("error_algo");
    ASSERT_NE(entry, nullptr);

    Catalog catalog;
    GraphEngine ge(catalog);
    AlgorithmContext ctx{ge, default_database_id, "edge", {}};
    OutputSchema schema({{"", "node_id", TypeId::INT64, false, 0}});

    AlgorithmScanOperator op(*entry, std::move(ctx), std::move(schema));

    // open() should fail since the execute function returns an error.
    auto open_r = op.open();
    EXPECT_FALSE(open_r.has_value());
    EXPECT_EQ(open_r.error().code, StatusCode::INTERNAL_ERROR);
}

TEST_F(QA_GDB478_ScanOperator, ReOpen) {
    auto* entry = registry_->find("test_algo");
    ASSERT_NE(entry, nullptr);

    Catalog catalog;
    GraphEngine ge(catalog);
    AlgorithmContext ctx{ge, default_database_id, "edge", {}};
    OutputSchema schema({
        {"", "node_id", TypeId::INT64, false, 0},
        {"", "score", TypeId::FLOAT64, false, 0},
    });

    AlgorithmScanOperator op(*entry, std::move(ctx), std::move(schema));

    // First open/drain/close.
    ASSERT_TRUE(op.open().has_value());
    int count1 = 0;
    while (true) {
        auto r = op.next();
        ASSERT_TRUE(r.has_value());
        if (!r->has_value())
            break;
        ++count1;
    }
    op.close();
    EXPECT_EQ(count1, 3);

    // Re-open — should reset cursor and re-execute.
    ASSERT_TRUE(op.open().has_value());
    int count2 = 0;
    while (true) {
        auto r = op.next();
        ASSERT_TRUE(r.has_value());
        if (!r->has_value())
            break;
        ++count2;
    }
    op.close();
    EXPECT_EQ(count2, 3);
}

TEST_F(QA_GDB478_ScanOperator, CloseWithoutNext) {
    auto* entry = registry_->find("test_algo");
    ASSERT_NE(entry, nullptr);

    Catalog catalog;
    GraphEngine ge(catalog);
    AlgorithmContext ctx{ge, default_database_id, "edge", {}};
    OutputSchema schema({
        {"", "node_id", TypeId::INT64, false, 0},
        {"", "score", TypeId::FLOAT64, false, 0},
    });

    AlgorithmScanOperator op(*entry, std::move(ctx), std::move(schema));
    ASSERT_TRUE(op.open().has_value());
    // Close immediately without calling next — should not crash.
    op.close();
}

TEST_F(QA_GDB478_ScanOperator, PlanNodeDetail) {
    auto* entry = registry_->find("test_algo");
    ASSERT_NE(entry, nullptr);

    Catalog catalog;
    GraphEngine ge(catalog);
    AlgorithmContext ctx{ge, default_database_id, "knows", {}};
    OutputSchema schema;

    AlgorithmScanOperator op(*entry, std::move(ctx), std::move(schema));
    EXPECT_EQ(op.plan_node_name(), "Algorithm Scan");
    EXPECT_EQ(op.plan_node_detail(), "test_algo('knows')");
}

// ============================================================================
// Part 3: End-to-end planner/parser integration tests
// ============================================================================

class QA_GDB478_E2E : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb478";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        // Register a test algorithm.
        AlgorithmDef def;
        def.name = "pagerank";
        def.params = {
            {"damping", TypeId::FLOAT64, false, Value(0.85)},
            {"iterations", TypeId::INT64, false, Value(int64_t{20})},
        };
        def.output_columns = {
            {"node_id", TypeId::INT64, false},
            {"rank", TypeId::FLOAT64, false},
        };

        auto execute = [](const AlgorithmContext& ctx) -> Result<std::vector<AlgorithmRow>> {
            // Verify that the edge_type is passed correctly.
            std::vector<AlgorithmRow> rows;
            // Return some rows with the named args reflected.
            auto it = ctx.named_args.find("damping");
            double damping = 0.85;
            if (it != ctx.named_args.end()) {
                damping = std::get<double>(it->second.data());
            }
            rows.push_back({std::vector<Value>{Value(int64_t{1}), Value(damping)}});
            rows.push_back({std::vector<Value>{Value(int64_t{2}), Value(damping * 0.5)}});
            return ok(std::move(rows));
        };

        (void)algo_registry_.register_algorithm(std::move(def), execute);

        // Also register one with only positional (no named params).
        AlgorithmDef simple;
        simple.name = "connected_components";
        simple.output_columns = {
            {"node_id", TypeId::INT64, false},
            {"component_id", TypeId::INT64, false},
        };
        auto cc_exec = [](const AlgorithmContext& /*ctx*/) -> Result<std::vector<AlgorithmRow>> {
            std::vector<AlgorithmRow> rows;
            rows.push_back({std::vector<Value>{Value(int64_t{1}), Value(int64_t{0})}});
            rows.push_back({std::vector<Value>{Value(int64_t{2}), Value(int64_t{0})}});
            rows.push_back({std::vector<Value>{Value(int64_t{3}), Value(int64_t{1})}});
            return ok(std::move(rows));
        };
        (void)algo_registry_.register_algorithm(std::move(simple), cc_exec);

        engine_->set_algorithm_registry(&algo_registry_);

        // Create a table + edge type for the graph.
        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");
        exec_ok("INSERT INTO users VALUES (3, 'Charlie')");
        exec_ok("CREATE EDGE TYPE knows FROM users TO users");
        exec_ok("LINK users(1) TO users(2) VIA knows");
        exec_ok("LINK users(2) TO users(3) VIA knows");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": " << result.error().message;
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected_code) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected_code);
        }
    }

    void exec_error(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
    AlgorithmRegistry algo_registry_;
};

// =========================================================================
// FIXED (GDB-548): SELECT * from algorithm TVF now correctly returns
// columns because the binder exposes algorithm TVF output columns to scope.
// =========================================================================

TEST_F(QA_GDB478_E2E, SelectStarReturnsColumns) {
    auto result = exec_ok("SELECT * FROM pagerank('knows')");
    EXPECT_EQ(result.column_names.size(), 2);
}

TEST_F(QA_GDB478_E2E, SelectNamedColumnSucceeds) {
    auto result = exec_ok("SELECT node_id FROM pagerank('knows')");
    EXPECT_EQ(result.column_names.size(), 1);
}

TEST_F(QA_GDB478_E2E, SelectQualifiedColumnSucceeds) {
    auto result = exec_ok("SELECT pagerank.node_id FROM pagerank('knows')");
    EXPECT_EQ(result.column_names.size(), 1);
}

TEST_F(QA_GDB478_E2E, WhereOnAlgorithmColumnSucceeds) {
    auto result = exec_ok("SELECT * FROM pagerank('knows') WHERE node_id = 1");
    EXPECT_EQ(result.column_names.size(), 2);
}

TEST_F(QA_GDB478_E2E, OrderByOnAlgorithmColumnSucceeds) {
    auto result = exec_ok("SELECT * FROM pagerank('knows') ORDER BY rank DESC");
    EXPECT_EQ(result.column_names.size(), 2);
}

// =========================================================================
// WORKING: Error paths that are correctly handled
// =========================================================================

TEST_F(QA_GDB478_E2E, UnknownAlgorithmFails) {
    exec_error("SELECT * FROM nonexistent_algo('knows')", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB478_E2E, MissingEdgeTypeArgFails) {
    exec_error("SELECT * FROM pagerank()", StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB478_E2E, NonStringEdgeTypeFails) {
    exec_error("SELECT * FROM pagerank(42)", StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB478_E2E, UnknownNamedParamFails) {
    exec_error("SELECT * FROM pagerank('knows', bogus := 42)", StatusCode::INVALID_ARGUMENT);
}

TEST_F(QA_GDB478_E2E, PositionalAfterNamedFails) {
    exec_error("SELECT * FROM pagerank('knows', damping := 0.5, 'extra')");
}

TEST_F(QA_GDB478_E2E, OnlyNamedParamsNoPositional) {
    // Edge type is positional — this is the first arg, so we still need it.
    // Only named params without edge type should fail.
    exec_error("SELECT * FROM pagerank(damping := 0.5)");
}

// =========================================================================
// WORKING: Algorithm call parses, executes, and returns correct columns.
// =========================================================================

TEST_F(QA_GDB478_E2E, AlgorithmCallExecutes) {
    auto result = exec_ok("SELECT * FROM pagerank('knows')");
    EXPECT_EQ(result.column_names.size(), 2);
}

TEST_F(QA_GDB478_E2E, AlgorithmFunctionCaseInsensitive) {
    auto r1 = exec_ok("SELECT * FROM pagerank('knows')");
    auto r2 = exec_ok("SELECT * FROM PAGERANK('knows')");
    auto r3 = exec_ok("SELECT * FROM PageRank('knows')");
    EXPECT_EQ(r1.column_names.size(), 2);
    EXPECT_EQ(r2.column_names.size(), 2);
    EXPECT_EQ(r3.column_names.size(), 2);
}

TEST_F(QA_GDB478_E2E, NamedParamsAccepted) {
    auto result = exec_ok("SELECT * FROM pagerank('knows', damping := 0.5)");
    EXPECT_EQ(result.column_names.size(), 2);
}

TEST_F(QA_GDB478_E2E, NamedParamsMultipleAccepted) {
    auto result = exec_ok("SELECT * FROM pagerank('knows', damping := 0.9, iterations := 10)");
    EXPECT_EQ(result.column_names.size(), 2);
}

TEST_F(QA_GDB478_E2E, NamedParamsCaseInsensitive) {
    auto result = exec_ok("SELECT * FROM pagerank('knows', DAMPING := 0.3)");
    EXPECT_EQ(result.column_names.size(), 2);
}

TEST_F(QA_GDB478_E2E, NamedParamWithExpression) {
    auto result = exec_ok("SELECT * FROM pagerank('knows', damping := 1.0 - 0.15)");
    EXPECT_EQ(result.column_names.size(), 2);
}

TEST_F(QA_GDB478_E2E, NonexistentEdgeTypeAccepted) {
    auto result = exec_ok("SELECT * FROM pagerank('nonexistent_edge')");
    EXPECT_EQ(result.column_names.size(), 2);
}

TEST_F(QA_GDB478_E2E, ConnectedComponentsNoParams) {
    auto result = exec_ok("SELECT * FROM connected_components('knows')");
    EXPECT_EQ(result.column_names.size(), 2);
}

} // namespace
} // namespace sixseven
