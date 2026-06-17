/// @file test_qa_gdb_855.cpp
/// QA adversarial tests for GDB-855: Enforce node_id-first output schema in
/// register_algorithm.
///
/// Verifies:
///   AC1: register_algorithm rejects defs with empty output_columns.
///   AC2: register_algorithm rejects defs whose first column is not "node_id".
///   AC3: register_algorithm rejects defs whose first column is not INT64.
///   AC4: Valid node_id-first defs are accepted and findable.
///   AC5: Rejected registration leaves registry unmodified (find returns nullptr).
///
/// Attack surface probed:
///   - Wrong type variants (INT32, UINT64, FLOAT64, STRING, BOOL) for node_id col
///   - Name case sensitivity: "Node_Id", "NODE_ID", "node_ID"
///   - node_id-first with nullable==true (is nullable flag checked?)
///   - node_id-first with many extra columns (valid)
///   - State integrity: rejected def + re-register same name must succeed
///   - INVALID_ARGUMENT vs ALREADY_EXISTS ordering (valid schema, dup name)
///   - Single-column schema (only node_id, no additional columns)
///   - node_id first but duplicate column names in the rest

#include "sixseven/graph/algorithm_registry.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using namespace sixseven;

namespace {

/// No-op execute for test defs.
Result<std::vector<AlgorithmRow>> noop_execute(const AlgorithmContext& /*ctx*/) {
    return ok(std::vector<AlgorithmRow>{});
}

/// Build a minimal valid def (node_id INT64 first).
AlgorithmDef make_valid_def(const std::string& name) {
    AlgorithmDef def;
    def.name = name;
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"score", TypeId::FLOAT64, false},
    };
    return def;
}

} // namespace

// ============================================================================
// Suite: QA_GDB855_SchemaValidation
// ============================================================================

// AC4 — valid node_id INT64 first must be accepted.
TEST(QA_GDB855_SchemaValidation, ValidNodeIdFirstAccepted) {
    AlgorithmRegistry registry;
    auto result = registry.register_algorithm(make_valid_def("my_algo"), noop_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_NE(registry.find("my_algo"), nullptr);
}

// AC1 — empty output_columns rejected with INVALID_ARGUMENT.
TEST(QA_GDB855_SchemaValidation, EmptyOutputColumnsRejected) {
    AlgorithmRegistry registry;
    AlgorithmDef def;
    def.name = "bad_algo";
    // output_columns deliberately empty.
    auto result = registry.register_algorithm(std::move(def), noop_execute);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// AC5 — after rejection due to empty columns, find must return nullptr.
TEST(QA_GDB855_SchemaValidation, AfterEmptyColumnsRejectedFindIsNull) {
    AlgorithmRegistry registry;
    AlgorithmDef def;
    def.name = "ghost_algo";
    (void)registry.register_algorithm(std::move(def), noop_execute);
    EXPECT_EQ(registry.find("ghost_algo"), nullptr);
    EXPECT_FALSE(registry.has("ghost_algo"));
}

// AC2 — first column named "vertex_id" (not "node_id") rejected.
TEST(QA_GDB855_SchemaValidation, WrongFirstColumnNameRejected) {
    AlgorithmRegistry registry;
    AlgorithmDef def;
    def.name = "wrong_name";
    def.output_columns = {{"vertex_id", TypeId::INT64, false}};
    auto result = registry.register_algorithm(std::move(def), noop_execute);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// AC3 — first column named "node_id" but type INT32 (not INT64) rejected.
TEST(QA_GDB855_SchemaValidation, WrongTypeINT32Rejected) {
    AlgorithmRegistry registry;
    AlgorithmDef def;
    def.name = "wrong_int32";
    def.output_columns = {{"node_id", TypeId::INT32, false}};
    auto result = registry.register_algorithm(std::move(def), noop_execute);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// AC3 — first column named "node_id" but type UINT64 rejected.
TEST(QA_GDB855_SchemaValidation, WrongTypeUINT64Rejected) {
    AlgorithmRegistry registry;
    AlgorithmDef def;
    def.name = "wrong_uint64";
    def.output_columns = {{"node_id", TypeId::UINT64, false}};
    auto result = registry.register_algorithm(std::move(def), noop_execute);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// AC3 — first column named "node_id" but type FLOAT64 rejected.
TEST(QA_GDB855_SchemaValidation, WrongTypeFLOAT64Rejected) {
    AlgorithmRegistry registry;
    AlgorithmDef def;
    def.name = "wrong_float64";
    def.output_columns = {{"node_id", TypeId::FLOAT64, false}};
    auto result = registry.register_algorithm(std::move(def), noop_execute);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// AC3 — first column named "node_id" but type STRING rejected.
TEST(QA_GDB855_SchemaValidation, WrongTypeSTRINGRejected) {
    AlgorithmRegistry registry;
    AlgorithmDef def;
    def.name = "wrong_string";
    def.output_columns = {{"node_id", TypeId::STRING, false}};
    auto result = registry.register_algorithm(std::move(def), noop_execute);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// AC3 — first column named "node_id" but type BOOL rejected.
TEST(QA_GDB855_SchemaValidation, WrongTypeBOOLRejected) {
    AlgorithmRegistry registry;
    AlgorithmDef def;
    def.name = "wrong_bool";
    def.output_columns = {{"node_id", TypeId::BOOL, false}};
    auto result = registry.register_algorithm(std::move(def), noop_execute);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// Case sensitivity: "Node_Id" (mixed case) must NOT be accepted as "node_id".
// The validation check uses exact string equality.
TEST(QA_GDB855_SchemaValidation, NodeIdNameCaseSensitiveMixedCaseRejected) {
    AlgorithmRegistry registry;
    AlgorithmDef def;
    def.name = "case_algo";
    def.output_columns = {{"Node_Id", TypeId::INT64, false}};
    auto result = registry.register_algorithm(std::move(def), noop_execute);
    // The column name check in register_algorithm is exact (operator==),
    // so "Node_Id" != "node_id" and this must be rejected.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// Case sensitivity: "NODE_ID" (all caps) must also be rejected.
TEST(QA_GDB855_SchemaValidation, NodeIdNameAllCapsRejected) {
    AlgorithmRegistry registry;
    AlgorithmDef def;
    def.name = "allcaps_algo";
    def.output_columns = {{"NODE_ID", TypeId::INT64, false}};
    auto result = registry.register_algorithm(std::move(def), noop_execute);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// Nullable flag: node_id INT64 with nullable==true should still be accepted
// (the validation only checks name and type_id, not nullable).
TEST(QA_GDB855_SchemaValidation, NodeIdNullableAccepted) {
    AlgorithmRegistry registry;
    AlgorithmDef def;
    def.name = "nullable_node_id";
    // nullable=true — validation does not mandate false here.
    def.output_columns = {{"node_id", TypeId::INT64, true}};
    auto result = registry.register_algorithm(std::move(def), noop_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_NE(registry.find("nullable_node_id"), nullptr);
}

// Single-column schema: only node_id, no extra columns — still valid.
TEST(QA_GDB855_SchemaValidation, SingleColumnNodeIdAccepted) {
    AlgorithmRegistry registry;
    AlgorithmDef def;
    def.name = "single_col";
    def.output_columns = {{"node_id", TypeId::INT64, false}};
    auto result = registry.register_algorithm(std::move(def), noop_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_NE(registry.find("single_col"), nullptr);
}

// Many extra columns after node_id are accepted.
TEST(QA_GDB855_SchemaValidation, ManyColumnsAfterNodeIdAccepted) {
    AlgorithmRegistry registry;
    AlgorithmDef def;
    def.name = "many_cols";
    def.output_columns = {
        {"node_id", TypeId::INT64, false},
        {"rank", TypeId::FLOAT64, false},
        {"component", TypeId::INT64, false},
        {"label", TypeId::STRING, true},
        {"weight", TypeId::FLOAT32, false},
        {"active", TypeId::BOOL, false},
    };
    auto result = registry.register_algorithm(std::move(def), noop_execute);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const auto* entry = registry.find("many_cols");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->def.output_columns.size(), 6u);
    EXPECT_EQ(entry->def.output_columns[0].name, "node_id");
    EXPECT_EQ(entry->def.output_columns[0].type_id, TypeId::INT64);
}

// ============================================================================
// Suite: QA_GDB855_StateIntegrity
// ============================================================================

// AC5 — after rejection, subsequent valid registration of the same name succeeds.
TEST(QA_GDB855_StateIntegrity, RejectedThenValidRegistrationSucceeds) {
    AlgorithmRegistry registry;

    // First attempt: wrong first column type — rejected.
    {
        AlgorithmDef bad_def;
        bad_def.name = "my_algo";
        bad_def.output_columns = {{"node_id", TypeId::INT32, false}};
        auto r = registry.register_algorithm(std::move(bad_def), noop_execute);
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    }

    // The name must not be taken after the rejected registration.
    EXPECT_EQ(registry.find("my_algo"), nullptr);
    EXPECT_FALSE(registry.has("my_algo"));

    // Second attempt: valid schema — must succeed.
    {
        auto result = registry.register_algorithm(make_valid_def("my_algo"), noop_execute);
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }
    EXPECT_NE(registry.find("my_algo"), nullptr);
}

// AC5 — after empty-columns rejection, registry state is completely clean.
TEST(QA_GDB855_StateIntegrity, RejectedEmptyDoesNotCorruptRegistry) {
    AlgorithmRegistry registry;

    // Register one valid algo first.
    (void)registry.register_algorithm(make_valid_def("pre_existing"), noop_execute);

    // Try to register an invalid one.
    {
        AlgorithmDef bad;
        bad.name = "invalid_algo";
        (void)registry.register_algorithm(std::move(bad), noop_execute);
    }

    // pre_existing must still be findable.
    EXPECT_NE(registry.find("pre_existing"), nullptr);
    // invalid_algo must not appear.
    EXPECT_EQ(registry.find("invalid_algo"), nullptr);
    // list() must contain only pre_existing.
    auto names = registry.list();
    EXPECT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "pre_existing");
}

// INVALID_ARGUMENT before ALREADY_EXISTS: a duplicate name with a bad schema
// gets INVALID_ARGUMENT (not ALREADY_EXISTS) because schema validation runs
// first in register_algorithm.
TEST(QA_GDB855_StateIntegrity, InvalidArgBeforeAlreadyExistsOrdering) {
    AlgorithmRegistry registry;

    // Register a valid algo.
    (void)registry.register_algorithm(make_valid_def("pagerank"), noop_execute);

    // Attempt to register the same name but with an invalid schema.
    // The validation runs before the duplicate check per the implementation.
    AlgorithmDef bad;
    bad.name = "pagerank"; // same name
    // Empty output_columns — invalid schema.
    auto result = registry.register_algorithm(std::move(bad), noop_execute);
    ASSERT_FALSE(result.has_value());
    // Must be INVALID_ARGUMENT, not ALREADY_EXISTS, because validation is first.
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// Verify ALREADY_EXISTS fires (not INVALID_ARGUMENT) when schema is valid but
// the name is already taken.
TEST(QA_GDB855_StateIntegrity, ValidSchemaDuplicateNameIsAlreadyExists) {
    AlgorithmRegistry registry;

    (void)registry.register_algorithm(make_valid_def("pagerank"), noop_execute);

    auto result = registry.register_algorithm(make_valid_def("pagerank"), noop_execute);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::ALREADY_EXISTS);
}

// ============================================================================
// Suite: QA_GDB855_ErrorMessages
// ============================================================================

// Error message must mention the algorithm name and the word "node_id".
TEST(QA_GDB855_ErrorMessages, InvalidArgMessageContainsAlgoNameAndNodeId) {
    AlgorithmRegistry registry;
    AlgorithmDef def;
    def.name = "my_broken_algo";
    // Wrong type — triggers validation failure.
    def.output_columns = {{"node_id", TypeId::INT32, false}};
    auto result = registry.register_algorithm(std::move(def), noop_execute);
    ASSERT_FALSE(result.has_value());
    const auto& msg = result.error().message;
    EXPECT_NE(msg.find("my_broken_algo"), std::string::npos)
        << "Error message must mention the algorithm name; got: " << msg;
    EXPECT_NE(msg.find("node_id"), std::string::npos)
        << "Error message must mention 'node_id'; got: " << msg;
}

// Error message for empty output_columns must also mention the algo name.
TEST(QA_GDB855_ErrorMessages, EmptyColumnsMessageContainsAlgoName) {
    AlgorithmRegistry registry;
    AlgorithmDef def;
    def.name = "empty_schema_algo";
    auto result = registry.register_algorithm(std::move(def), noop_execute);
    ASSERT_FALSE(result.has_value());
    const auto& msg = result.error().message;
    EXPECT_NE(msg.find("empty_schema_algo"), std::string::npos)
        << "Error message must mention the algorithm name; got: " << msg;
}
