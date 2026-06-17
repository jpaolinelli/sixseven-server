// QA adversarial tests for GDB-850: AlgorithmRegistry execute-function hardening.
//
// These tests independently derive expected values and are written to catch:
//   - execute function cross-wiring between entries
//   - dangling lambda captures (lifetime safety)
//   - execute returning errors (not silent empty / crash)
//   - exact metadata independence per entry
//   - duplicate rejection / overwrite semantics
//   - case sensitivity / empty-name edge cases
//   - zero-row and many-row exact counts

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/graph/algorithm_registry.h"
#include "sixseven/graph/graph_engine.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

AlgorithmContext make_ctx(GraphEngine& ge) {
    return AlgorithmContext{ge, default_database_id, /*edge_type=*/"", /*named_args=*/{}};
}

AlgorithmDef make_def(const std::string& name, std::vector<AlgorithmOutputColumn> cols = {}) {
    AlgorithmDef def;
    def.name = name;
    if (cols.empty()) {
        def.output_columns = {{"node_id", TypeId::INT64, false}, {"score", TypeId::FLOAT64, false}};
    } else {
        def.output_columns = std::move(cols);
    }
    return def;
}

} // namespace

// ---------------------------------------------------------------------------
// Fixture: shared catalog + graph engine (no storage, minimal context)
// ---------------------------------------------------------------------------

class QA_GDB850 : public ::testing::Test {
protected:
    Catalog catalog_;
    // Constructed lazily so tests that don't need it don't pay the cost.
    std::unique_ptr<GraphEngine> ge_;

    void SetUp() override { ge_ = std::make_unique<GraphEngine>(catalog_); }

    AlgorithmContext ctx() { return make_ctx(*ge_); }
};

// ---------------------------------------------------------------------------
// AC: execute function is stored and invocable (non-null guard)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB850, ExecuteFunctionNotNullAfterRegister) {
    AlgorithmRegistry reg;
    auto fn = [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
        return ok(std::vector<AlgorithmRow>{});
    };
    ASSERT_TRUE(reg.register_algorithm(make_def("a"), fn).has_value());
    const auto* entry = reg.find("a");
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(static_cast<bool>(entry->execute))
        << "execute std::function must not be null after registration";
}

// ---------------------------------------------------------------------------
// AC: multiple algorithms — find returns the RIGHT entry, no cross-wiring
// ---------------------------------------------------------------------------

TEST_F(QA_GDB850, MultiplAlgosNoCrossWiring) {
    AlgorithmRegistry reg;

    // algo_a returns sentinel rows with node_id=10, score=1.0
    auto fn_a = [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
        return ok(std::vector<AlgorithmRow>{
            {std::vector<Value>{Value(static_cast<int64_t>(10)), Value(1.0)}}});
    };

    // algo_b returns sentinel rows with node_id=20, score=2.0
    auto fn_b = [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
        return ok(std::vector<AlgorithmRow>{
            {std::vector<Value>{Value(static_cast<int64_t>(20)), Value(2.0)}}});
    };

    ASSERT_TRUE(reg.register_algorithm(make_def("algo_a"), fn_a).has_value());
    ASSERT_TRUE(reg.register_algorithm(make_def("algo_b"), fn_b).has_value());

    // Verify algo_a returns 10 / 1.0
    const auto* ea = reg.find("algo_a");
    ASSERT_NE(ea, nullptr);
    auto ra = ea->execute(ctx());
    ASSERT_TRUE(ra.has_value());
    ASSERT_EQ(ra->size(), 1u);
    EXPECT_EQ(std::get<int64_t>((*ra)[0].values[0].data()), static_cast<int64_t>(10));
    EXPECT_DOUBLE_EQ(std::get<double>((*ra)[0].values[1].data()), 1.0);

    // Verify algo_b returns 20 / 2.0 — NOT algo_a's values
    const auto* eb = reg.find("algo_b");
    ASSERT_NE(eb, nullptr);
    auto rb = eb->execute(ctx());
    ASSERT_TRUE(rb.has_value());
    ASSERT_EQ(rb->size(), 1u);
    EXPECT_EQ(std::get<int64_t>((*rb)[0].values[0].data()), static_cast<int64_t>(20));
    EXPECT_DOUBLE_EQ(std::get<double>((*rb)[0].values[1].data()), 2.0);

    // Cross-check: algo_a's result must NOT equal algo_b's sentinel
    EXPECT_NE(std::get<int64_t>((*ra)[0].values[0].data()),
              std::get<int64_t>((*rb)[0].values[0].data()))
        << "execute functions must not be cross-wired between entries";
}

// ---------------------------------------------------------------------------
// AC: find unregistered name returns nullptr — no crash
// ---------------------------------------------------------------------------

TEST_F(QA_GDB850, FindUnregisteredReturnsNullptr) {
    AlgorithmRegistry reg;
    EXPECT_EQ(reg.find("never_registered"), nullptr);
    EXPECT_EQ(reg.find(""), nullptr);
    EXPECT_EQ(reg.find("ALSO_MISSING"), nullptr);
}

// ---------------------------------------------------------------------------
// AC: duplicate registration rejects — second execute is NOT stored
// ---------------------------------------------------------------------------

TEST_F(QA_GDB850, DuplicateRegistrationRejectsAndPreservesOriginal) {
    AlgorithmRegistry reg;

    // First: fn that returns node_id=99
    auto fn_first = [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
        return ok(std::vector<AlgorithmRow>{
            {std::vector<Value>{Value(static_cast<int64_t>(99)), Value(0.9)}}});
    };
    // Second: fn that returns node_id=77 — should be rejected
    auto fn_second = [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
        return ok(std::vector<AlgorithmRow>{
            {std::vector<Value>{Value(static_cast<int64_t>(77)), Value(0.7)}}});
    };

    ASSERT_TRUE(reg.register_algorithm(make_def("dup_algo"), fn_first).has_value());

    auto second_result = reg.register_algorithm(make_def("DUP_ALGO"), fn_second);
    ASSERT_FALSE(second_result.has_value()) << "re-registration must return an error";
    EXPECT_EQ(second_result.error().code, StatusCode::ALREADY_EXISTS);

    // Original entry must still exist and execute the FIRST fn (node_id=99)
    const auto* entry = reg.find("dup_algo");
    ASSERT_NE(entry, nullptr);
    auto result = entry->execute(ctx());
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ(std::get<int64_t>((*result)[0].values[0].data()), static_cast<int64_t>(99))
        << "duplicate rejection must not overwrite the original execute fn";
}

// ---------------------------------------------------------------------------
// AC: execute returning ERROR surfaces the error, not crash or silent empty
// ---------------------------------------------------------------------------

TEST_F(QA_GDB850, ExecuteReturningErrorSurfacesError) {
    AlgorithmRegistry reg;

    auto error_fn = [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
        return make_error(StatusCode::INTERNAL_ERROR, "algorithm internal failure");
    };

    ASSERT_TRUE(reg.register_algorithm(make_def("error_algo"), error_fn).has_value());
    const auto* entry = reg.find("error_algo");
    ASSERT_NE(entry, nullptr);

    auto result = entry->execute(ctx());
    ASSERT_FALSE(result.has_value()) << "error from execute must propagate as an error Result";
    EXPECT_EQ(result.error().code, StatusCode::INTERNAL_ERROR);
    EXPECT_FALSE(result.error().message.empty()) << "error message must not be empty";
}

// ---------------------------------------------------------------------------
// AC: execute returning 0 rows — exact count
// ---------------------------------------------------------------------------

TEST_F(QA_GDB850, ExecuteReturnsZeroRows) {
    AlgorithmRegistry reg;

    auto zero_fn = [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
        return ok(std::vector<AlgorithmRow>{});
    };

    ASSERT_TRUE(reg.register_algorithm(make_def("zero_algo"), zero_fn).has_value());
    const auto* entry = reg.find("zero_algo");
    ASSERT_NE(entry, nullptr);

    auto result = entry->execute(ctx());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 0u) << "zero-row execute must return exactly 0 rows";
}

// ---------------------------------------------------------------------------
// AC: execute returning many rows — exact count and exact values
// ---------------------------------------------------------------------------

TEST_F(QA_GDB850, ExecuteReturnsManyRowsExactValues) {
    AlgorithmRegistry reg;

    // 5 rows with deterministic sentinel values
    auto many_fn = [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
        std::vector<AlgorithmRow> rows;
        for (int64_t i = 0; i < 5; ++i) {
            rows.push_back({std::vector<Value>{Value(i), Value(static_cast<double>(i) * 0.1)}});
        }
        return ok(std::move(rows));
    };

    ASSERT_TRUE(reg.register_algorithm(make_def("many_algo"), many_fn).has_value());
    const auto* entry = reg.find("many_algo");
    ASSERT_NE(entry, nullptr);

    auto result = entry->execute(ctx());
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 5u);

    for (int64_t i = 0; i < 5; ++i) {
        ASSERT_EQ((*result)[static_cast<size_t>(i)].values.size(), 2u);
        EXPECT_EQ(std::get<int64_t>((*result)[static_cast<size_t>(i)].values[0].data()), i)
            << "row " << i << " node_id mismatch";
        EXPECT_DOUBLE_EQ(std::get<double>((*result)[static_cast<size_t>(i)].values[1].data()),
                         static_cast<double>(i) * 0.1)
            << "row " << i << " score mismatch";
    }
}

// ---------------------------------------------------------------------------
// AC: metadata independence — multiple algos have distinct output_columns
// ---------------------------------------------------------------------------

TEST_F(QA_GDB850, MetadataIndependenceAcrossEntries) {
    AlgorithmRegistry reg;

    AlgorithmDef def_a =
        make_def("meta_a", {{"node_id", TypeId::INT64, false}, {"rank", TypeId::FLOAT64, false}});
    AlgorithmDef def_b = make_def("meta_b",
                                  {{"source", TypeId::INT64, false},
                                   {"target", TypeId::INT64, false},
                                   {"weight", TypeId::FLOAT64, true}});

    auto noop = [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
        return ok(std::vector<AlgorithmRow>{});
    };

    ASSERT_TRUE(reg.register_algorithm(std::move(def_a), noop).has_value());
    ASSERT_TRUE(reg.register_algorithm(std::move(def_b), noop).has_value());

    const auto* ea = reg.find("meta_a");
    const auto* eb = reg.find("meta_b");
    ASSERT_NE(ea, nullptr);
    ASSERT_NE(eb, nullptr);

    // meta_a: 2 columns
    ASSERT_EQ(ea->def.output_columns.size(), 2u);
    EXPECT_EQ(ea->def.output_columns[0].name, "node_id");
    EXPECT_EQ(ea->def.output_columns[1].name, "rank");

    // meta_b: 3 columns, third is nullable
    ASSERT_EQ(eb->def.output_columns.size(), 3u);
    EXPECT_EQ(eb->def.output_columns[0].name, "source");
    EXPECT_EQ(eb->def.output_columns[1].name, "target");
    EXPECT_EQ(eb->def.output_columns[2].name, "weight");
    EXPECT_TRUE(eb->def.output_columns[2].nullable);

    // Metadata must NOT bleed between entries
    EXPECT_NE(ea->def.output_columns.size(), eb->def.output_columns.size())
        << "metadata columns must be stored independently per entry";
}

// ---------------------------------------------------------------------------
// AC: lambda capture lifetime safety — register lambda capturing a local,
//     let that scope end, then invoke. No UAF / dangling capture.
// ---------------------------------------------------------------------------

TEST_F(QA_GDB850, LambdaCaptureLifetimeSafety) {
    AlgorithmRegistry reg;

    // The lambda captures `sentinel` by value. After registration the local
    // `sentinel` variable goes out of scope; the std::function must keep its
    // own copy alive (std::function stores by value).
    {
        int64_t sentinel = 42;
        auto fn = [sentinel](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
            return ok(std::vector<AlgorithmRow>{{std::vector<Value>{Value(sentinel), Value(0.0)}}});
        };
        ASSERT_TRUE(reg.register_algorithm(make_def("lifetime_algo"), fn).has_value());
        // `fn`, `sentinel` go out of scope here
    }

    // Invoke after the capture's original scope is gone
    const auto* entry = reg.find("lifetime_algo");
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(static_cast<bool>(entry->execute));

    auto result = entry->execute(ctx());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ(std::get<int64_t>((*result)[0].values[0].data()), static_cast<int64_t>(42))
        << "std::function must keep its captured value alive after the capture scope ends";
}

// ---------------------------------------------------------------------------
// AC: case sensitivity — find uses case-insensitive matching
// ---------------------------------------------------------------------------

TEST_F(QA_GDB850, FindCaseInsensitiveAllVariants) {
    AlgorithmRegistry reg;

    auto fn = [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
        return ok(std::vector<AlgorithmRow>{
            {std::vector<Value>{Value(static_cast<int64_t>(5)), Value(0.5)}}});
    };

    ASSERT_TRUE(reg.register_algorithm(make_def("CaSeAlGo"), fn).has_value());

    for (const char* variant : {"CaSeAlGo", "casealgo", "CASEALGO", "cAsEaLgO"}) {
        const auto* entry = reg.find(variant);
        ASSERT_NE(entry, nullptr) << "find(\"" << variant << "\") must succeed";
        auto result = entry->execute(ctx());
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->size(), 1u);
        EXPECT_EQ(std::get<int64_t>((*result)[0].values[0].data()), static_cast<int64_t>(5))
            << "execute must return the same fn regardless of lookup case";
    }
}

// ---------------------------------------------------------------------------
// AC: empty name registration — production must handle consistently
//     (either reject or allow; we assert whatever it does, consistently)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB850, EmptyNameRegistrationConsistency) {
    AlgorithmRegistry reg;
    auto fn = [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
        return ok(std::vector<AlgorithmRow>{});
    };

    auto first = reg.register_algorithm(make_def(""), fn);
    auto second = reg.register_algorithm(make_def(""), fn);

    if (first.has_value()) {
        // Empty name allowed: duplicate must be rejected (consistent with named algos)
        ASSERT_FALSE(second.has_value())
            << "re-registering empty name must fail with ALREADY_EXISTS";
        EXPECT_EQ(second.error().code, StatusCode::ALREADY_EXISTS);

        // find("") must succeed and execute must be callable
        const auto* entry = reg.find("");
        ASSERT_NE(entry, nullptr);
        ASSERT_TRUE(static_cast<bool>(entry->execute));
        auto result = entry->execute(ctx());
        EXPECT_TRUE(result.has_value());
    } else {
        // Empty name rejected: second must also be rejected
        ASSERT_FALSE(second.has_value())
            << "if empty name is rejected, repeated registration must also be rejected";
    }
}

// ---------------------------------------------------------------------------
// AC: re-registering different algos under distinct names after a collision
//     does not corrupt the registry state
// ---------------------------------------------------------------------------

TEST_F(QA_GDB850, RegistryStateIntactAfterDuplicateRejection) {
    AlgorithmRegistry reg;

    auto fn_x = [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
        return ok(std::vector<AlgorithmRow>{
            {std::vector<Value>{Value(static_cast<int64_t>(1000)), Value(1.0)}}});
    };
    auto fn_y = [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
        return ok(std::vector<AlgorithmRow>{
            {std::vector<Value>{Value(static_cast<int64_t>(2000)), Value(2.0)}}});
    };
    auto fn_dup = [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
        return ok(std::vector<AlgorithmRow>{
            {std::vector<Value>{Value(static_cast<int64_t>(9999)), Value(9.9)}}});
    };

    ASSERT_TRUE(reg.register_algorithm(make_def("algo_x"), fn_x).has_value());
    ASSERT_TRUE(reg.register_algorithm(make_def("algo_y"), fn_y).has_value());

    // Duplicate of algo_x
    auto dup = reg.register_algorithm(make_def("ALGO_X"), fn_dup);
    ASSERT_FALSE(dup.has_value());

    // Both algo_x and algo_y must still be callable with original semantics
    {
        const auto* ex = reg.find("algo_x");
        ASSERT_NE(ex, nullptr);
        auto r = ex->execute(ctx());
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(r->size(), 1u);
        EXPECT_EQ(std::get<int64_t>((*r)[0].values[0].data()), static_cast<int64_t>(1000));
    }
    {
        const auto* ey = reg.find("algo_y");
        ASSERT_NE(ey, nullptr);
        auto r = ey->execute(ctx());
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(r->size(), 1u);
        EXPECT_EQ(std::get<int64_t>((*r)[0].values[0].data()), static_cast<int64_t>(2000));
    }

    // list() must still return exactly 2 entries
    auto names = reg.list();
    EXPECT_EQ(names.size(), 2u)
        << "registry must contain exactly 2 entries after one rejected duplicate";
}

// ---------------------------------------------------------------------------
// AC: exact 2-row execute (the strengthened AC from GDB-850 itself)
// ---------------------------------------------------------------------------

TEST_F(QA_GDB850, ExecuteExactTwoRowsHardcodedValues) {
    AlgorithmRegistry reg;

    AlgorithmDef def;
    def.name = "two_row_algo";
    def.output_columns = {{"node_id", TypeId::INT64, false}, {"score", TypeId::FLOAT64, false}};

    auto execute_fn = [](const AlgorithmContext&) -> Result<std::vector<AlgorithmRow>> {
        std::vector<AlgorithmRow> rows;
        rows.push_back({std::vector<Value>{Value(static_cast<int64_t>(1)), Value(0.5)}});
        rows.push_back({std::vector<Value>{Value(static_cast<int64_t>(2)), Value(0.3)}});
        return ok(std::move(rows));
    };

    ASSERT_TRUE(reg.register_algorithm(std::move(def), execute_fn).has_value());
    ASSERT_TRUE(static_cast<bool>(reg.find("two_row_algo")->execute))
        << "execute must not be null after registration";

    auto result = reg.find("two_row_algo")->execute(ctx());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);

    // Row 0: node_id=1, score=0.5
    ASSERT_EQ((*result)[0].values.size(), 2u);
    EXPECT_EQ(std::get<int64_t>((*result)[0].values[0].data()), static_cast<int64_t>(1));
    EXPECT_DOUBLE_EQ(std::get<double>((*result)[0].values[1].data()), 0.5);

    // Row 1: node_id=2, score=0.3
    ASSERT_EQ((*result)[1].values.size(), 2u);
    EXPECT_EQ(std::get<int64_t>((*result)[1].values[0].data()), static_cast<int64_t>(2));
    EXPECT_DOUBLE_EQ(std::get<double>((*result)[1].values[1].data()), 0.3);
}
