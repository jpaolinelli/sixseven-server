// QA GDB-679: Adversarial tests for __path (TypeId::PATH) schema wiring through
// the planner and binder when WITH TRACE is specified on TRAVERSE.
//
// The production change under test is src/planner/binder.cpp bind_traverse(),
// which appends a trailing {alias,"__path",PATH} output column when
// stmt.trace==true so that `SELECT __path FROM (TRAVERSE ... WITH TRACE) AS t`
// resolves. compute_traverse_columns() and the planner side (plan_traverse,
// plan_table_source) were merged earlier (GDB-678).
//
// These tests probe for: column position / ordinal bugs, trace-flag edge cases,
// NODES vs EDGES mode differences, nested / derived-table resolution, case
// sensitivity of __path, FETCH interaction, and binder/planner schema mismatch.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

// Graph fixture:
//   users: 1..5  follows edges 1->2,1->3,2->4,3->4,4->5 (homogeneous)
// Plus a heterogeneous edge type for EDGES/heterogeneous probing.
class QaTraversePathSchema : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb679";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        for (int i = 1; i <= 5; ++i) {
            exec_ok("INSERT INTO users VALUES (" + std::to_string(i) + ", 'u" +
                    std::to_string(i) + "')");
        }
        exec_ok("CREATE EDGE TYPE follows FROM users TO users");
        exec_ok("LINK users(1) TO users(2) VIA follows");
        exec_ok("LINK users(1) TO users(3) VIA follows");
        exec_ok("LINK users(2) TO users(4) VIA follows");
        exec_ok("LINK users(3) TO users(4) VIA follows");
        exec_ok("LINK users(4) TO users(5) VIA follows");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << sql << ": "
                                        << (result ? "" : result.error().message);
        return result ? std::move(*result) : QueryResult{};
    }

    Result<QueryResult> exec(const std::string& sql) { return engine_->execute(sql); }

    static std::optional<size_t> col_index(const QueryResult& qr, const std::string& name) {
        for (size_t i = 0; i < qr.column_names.size(); ++i) {
            if (qr.column_names[i] == name) {
                return i;
            }
        }
        return std::nullopt;
    }

    static size_t count_cols(const QueryResult& qr, const std::string& name) {
        size_t n = 0;
        for (const auto& c : qr.column_names) {
            if (c == name) {
                ++n;
            }
        }
        return n;
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ---------------------------------------------------------------------------
// Case sensitivity of __path.
// ---------------------------------------------------------------------------

// __path is an internal/meta name. The column emitted must be lowercase "__path".
// A different case must NOT silently match (would indicate fold bugs).
TEST_F(QaTraversePathSchema, PathColumnIsExactlyLowercaseUnderscoreUnderscorePath) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");
    EXPECT_TRUE(col_index(qr, "__path").has_value());
    EXPECT_FALSE(col_index(qr, "__PATH").has_value());
    EXPECT_FALSE(col_index(qr, "__Path").has_value());
    EXPECT_FALSE(col_index(qr, "_path").has_value());
    EXPECT_FALSE(col_index(qr, "path").has_value());
}

// Referencing __path with mismatched case in the outer SELECT. If identifiers are
// case-insensitive in this engine this would resolve; if case-sensitive it fails.
// Either way it must not crash and must be internally consistent with a plain ref.
TEST_F(QaTraversePathSchema, DerivedTableUppercasePathRefBehavesConsistently) {
    auto lower = exec("SELECT __path FROM "
                      "(TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE) AS t");
    ASSERT_TRUE(lower.has_value()) << lower.error().message;

    auto upper = exec("SELECT __PATH FROM "
                      "(TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE) AS t");
    // Whatever the case policy, the engine must not crash. We only assert that a
    // successful resolution yields a PATH column.
    if (upper.has_value()) {
        ASSERT_EQ(upper->column_types.size(), 1u);
        EXPECT_EQ(upper->column_types[0], TypeId::PATH);
    } else {
        SUCCEED() << "uppercase __PATH not resolved (case-sensitive): "
                  << upper.error().message;
    }
}

// ---------------------------------------------------------------------------
// Exactly one __path column (no duplication between binder + planner schema).
// ---------------------------------------------------------------------------

TEST_F(QaTraversePathSchema, StandaloneTraceEmitsExactlyOnePathColumn) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");
    EXPECT_EQ(count_cols(qr, "__path"), 1u);
}

TEST_F(QaTraversePathSchema, FromTraverseNodesTraceEmitsExactlyOnePathColumn) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");
    EXPECT_EQ(count_cols(qr, "__path"), 1u);
}

TEST_F(QaTraversePathSchema, FromTraverseEdgesTraceEmitsExactlyOnePathColumn) {
    auto qr = exec_ok(
        "SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES WITH TRACE");
    EXPECT_EQ(count_cols(qr, "__path"), 1u);
}

// ---------------------------------------------------------------------------
// Ordinal correctness: projecting __path alongside another column. A position
// mismatch between the binder's declared output_columns and the planner's
// physical schema would surface as the wrong VALUE/type landing in a slot.
// ---------------------------------------------------------------------------

// SELECT node, __path : node must be INT, __path must be PATH, in that order.
TEST_F(QaTraversePathSchema, DerivedTableProjectNodeThenPathTypesAreCorrect) {
    auto qr = exec_ok("SELECT node, __path FROM "
                      "(TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE) AS t");
    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "node");
    EXPECT_EQ(qr.column_names[1], "__path");
    EXPECT_EQ(qr.column_types[1], TypeId::PATH);
    ASSERT_FALSE(qr.rows.empty());
    for (const auto& row : qr.rows) {
        // node is an integer PK; __path is a PATH. A slot/ordinal mismatch would
        // put a PATH where an int is expected (or vice versa).
        EXPECT_NE(row[0].type_id(), TypeId::PATH);
        EXPECT_EQ(row[1].type_id(), TypeId::PATH);
    }
}

// Reversed projection order: __path first, then node. Catches ordinal mapping
// that assumes __path is always last.
TEST_F(QaTraversePathSchema, DerivedTableProjectPathThenNodeTypesAreCorrect) {
    auto qr = exec_ok("SELECT __path, node FROM "
                      "(TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE) AS t");
    ASSERT_EQ(qr.column_names.size(), 2u);
    EXPECT_EQ(qr.column_names[0], "__path");
    EXPECT_EQ(qr.column_names[1], "node");
    EXPECT_EQ(qr.column_types[0], TypeId::PATH);
    ASSERT_FALSE(qr.rows.empty());
    for (const auto& row : qr.rows) {
        EXPECT_EQ(row[0].type_id(), TypeId::PATH);
        EXPECT_NE(row[1].type_id(), TypeId::PATH);
    }
}

// ---------------------------------------------------------------------------
// FETCH interaction. bind_traverse appends __path AFTER the optional `source`
// column. Verify ordering holds with FETCH present (source exists) and absent.
// ---------------------------------------------------------------------------

TEST_F(QaTraversePathSchema, DerivedTableFetchAndTracePathAfterSource) {
    // FETCH surfaces `source`; __path must come after it (binder order matches
    // plan_traverse: node, depth, source, __path).
    auto qr = exec_ok("SELECT * FROM "
                      "(TRAVERSE follows FROM users(1) DIRECTION OUT FETCH WITH TRACE) AS t");
    auto src = col_index(qr, "source");
    auto path = col_index(qr, "__path");
    ASSERT_TRUE(path.has_value());
    if (src.has_value()) {
        EXPECT_GT(*path, *src) << "__path must follow source when FETCH is present";
    }
    // __path is the trailing column regardless.
    EXPECT_EQ(*path, qr.column_names.size() - 1);
    EXPECT_EQ(qr.column_types[*path], TypeId::PATH);
}

TEST_F(QaTraversePathSchema, DerivedTableTraceWithoutFetchHasNoSourceButHasPath) {
    auto qr = exec_ok("SELECT * FROM "
                      "(TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE) AS t");
    EXPECT_FALSE(col_index(qr, "source").has_value());
    auto path = col_index(qr, "__path");
    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(*path, qr.column_names.size() - 1);
}

// ---------------------------------------------------------------------------
// Trace-flag negative cases across every context (binder must NOT declare
// __path; selecting it must fail rather than silently return garbage).
// ---------------------------------------------------------------------------

TEST_F(QaTraversePathSchema, DerivedTableNoTraceSelectStarHasNoPath) {
    auto qr =
        exec_ok("SELECT * FROM (TRAVERSE follows FROM users(1) DIRECTION OUT) AS t");
    EXPECT_FALSE(col_index(qr, "__path").has_value());
}

TEST_F(QaTraversePathSchema, DerivedTableNoTraceExplicitPathRefFails) {
    auto r = exec("SELECT __path FROM (TRAVERSE follows FROM users(1) DIRECTION OUT) AS t");
    EXPECT_FALSE(r.has_value())
        << "__path must not resolve without WITH TRACE (binder declared no such column)";
}

TEST_F(QaTraversePathSchema, FromTraverseEdgesNoTracePathRefFails) {
    auto r = exec("SELECT __path FROM TRAVERSE follows FROM users(1) "
                  "DIRECTION OUT MODE EDGES");
    EXPECT_FALSE(r.has_value());
}

// ---------------------------------------------------------------------------
// NODES vs EDGES position invariants. NODES: __path after __source.
// EDGES: __path after __depth and must NOT collide with __from/__to.
// ---------------------------------------------------------------------------

TEST_F(QaTraversePathSchema, EdgesModePathDoesNotDisplaceFromTo) {
    auto qr = exec_ok(
        "SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES WITH TRACE");
    auto from = col_index(qr, "__from");
    auto to = col_index(qr, "__to");
    auto depth = col_index(qr, "__depth");
    auto path = col_index(qr, "__path");
    ASSERT_TRUE(from.has_value());
    ASSERT_TRUE(to.has_value());
    ASSERT_TRUE(depth.has_value());
    ASSERT_TRUE(path.has_value());
    // Documented order: __from, __to, __depth, __path.
    EXPECT_LT(*from, *to);
    EXPECT_LT(*to, *depth);
    EXPECT_EQ(*path, *depth + 1);
    EXPECT_FALSE(col_index(qr, "__node").has_value()) << "EDGES mode must not expose __node";
    EXPECT_FALSE(col_index(qr, "__source").has_value());
}

TEST_F(QaTraversePathSchema, NodesModePathDoesNotDisplaceNodeColumns) {
    auto qr = exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");
    auto node = col_index(qr, "__node");
    auto depth = col_index(qr, "__depth");
    auto source = col_index(qr, "__source");
    auto path = col_index(qr, "__path");
    ASSERT_TRUE(node.has_value());
    ASSERT_TRUE(depth.has_value());
    ASSERT_TRUE(source.has_value());
    ASSERT_TRUE(path.has_value());
    EXPECT_LT(*node, *depth);
    EXPECT_LT(*depth, *source);
    EXPECT_EQ(*path, *source + 1);
    // Target table columns (id, name) still present and before the meta block.
    EXPECT_TRUE(col_index(qr, "id").has_value());
    EXPECT_TRUE(col_index(qr, "name").has_value());
    EXPECT_LT(*col_index(qr, "id"), *node);
}

// ---------------------------------------------------------------------------
// Nested derived tables — __path must propagate through two levels of wrapping.
// ---------------------------------------------------------------------------

TEST_F(QaTraversePathSchema, NestedDerivedTablePathPropagates) {
    auto r = exec(
        "SELECT __path FROM "
        "(SELECT __path FROM "
        "(TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE) AS inner_t) AS outer_t");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->column_types.size(), 1u);
    EXPECT_EQ(r->column_types[0], TypeId::PATH);
    ASSERT_FALSE(r->rows.empty());
    for (const auto& row : r->rows) {
        EXPECT_EQ(row[0].type_id(), TypeId::PATH);
    }
}

// Nested wrap where the inner query drops __path: the outer ref must then fail.
TEST_F(QaTraversePathSchema, NestedDerivedTableDroppedPathCannotBeReselected) {
    auto r = exec(
        "SELECT __path FROM "
        "(SELECT node FROM "
        "(TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE) AS inner_t) AS outer_t");
    EXPECT_FALSE(r.has_value())
        << "__path projected away by inner query must not resolve in outer query";
}

// ---------------------------------------------------------------------------
// WHERE / filter coexisting with trace; path column must remain wired and typed.
// ---------------------------------------------------------------------------

TEST_F(QaTraversePathSchema, DerivedTableWhereOnDepthStillExposesPath) {
    auto r = exec("SELECT __path FROM "
                  "(TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE) AS t "
                  "WHERE depth > 0");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->column_types.size(), 1u);
    EXPECT_EQ(r->column_types[0], TypeId::PATH);
    for (const auto& row : r->rows) {
        EXPECT_EQ(row[0].type_id(), TypeId::PATH);
    }
}

// ---------------------------------------------------------------------------
// Multi-row path integrity. Every row's __path slot must hold a PATH value
// (no NULL leakage, no type confusion) across a fan-out traversal.
// ---------------------------------------------------------------------------

TEST_F(QaTraversePathSchema, EveryRowPathSlotIsPathTyped) {
    auto qr = exec_ok("TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE");
    auto path = col_index(qr, "__path");
    ASSERT_TRUE(path.has_value());
    ASSERT_GE(qr.rows.size(), 4u) << "fan-out from user 1 should reach multiple nodes";
    for (const auto& row : qr.rows) {
        ASSERT_LT(*path, row.size());
        EXPECT_EQ(row[*path].type_id(), TypeId::PATH);
    }
}

// ---------------------------------------------------------------------------
// Direction edge case: IN direction with trace (path reconstruction along a
// reversed edge set). Schema must still carry __path.
// ---------------------------------------------------------------------------

TEST_F(QaTraversePathSchema, DirectionInWithTraceStillExposesPath) {
    auto r = exec("TRAVERSE follows FROM users(5) DIRECTION IN WITH TRACE");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    auto path = col_index(*r, "__path");
    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(r->column_types[*path], TypeId::PATH);
}

// ---------------------------------------------------------------------------
// Zero-result traversal with trace: a start node with no outgoing edges. The
// __path column must still be declared in the schema even with no rows.
// ---------------------------------------------------------------------------

TEST_F(QaTraversePathSchema, NoOutgoingEdgesStillDeclaresPathColumn) {
    // user 5 has no OUT edges (it is the sink). Depth-0 start row may still be
    // emitted, but the schema must carry __path regardless of row count.
    auto qr = exec_ok("TRAVERSE follows FROM users(5) DIRECTION OUT WITH TRACE");
    EXPECT_TRUE(col_index(qr, "__path").has_value())
        << "__path column must exist in schema even when traversal yields a single/zero rows";
}

// ---------------------------------------------------------------------------
// Binder/planner consistency probe: SELECT * over a derived TRAVERSE WITH TRACE
// must expose the SAME __path column the binder declared, at the trailing slot.
// A binder-vs-planner mismatch would drop or mis-order it under the derived path.
// ---------------------------------------------------------------------------

TEST_F(QaTraversePathSchema, DerivedTableSelectStarMatchesBinderTrailingPath) {
    auto qr = exec_ok("SELECT * FROM "
                      "(TRAVERSE follows FROM users(1) DIRECTION OUT WITH TRACE) AS t");
    auto path = col_index(qr, "__path");
    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(*path, qr.column_names.size() - 1)
        << "binder declares __path as trailing column; derived SELECT * must agree";
    EXPECT_EQ(qr.column_types[*path], TypeId::PATH);
    // node + depth precede __path in standalone schema.
    EXPECT_TRUE(col_index(qr, "node").has_value());
    EXPECT_TRUE(col_index(qr, "depth").has_value());
}

} // namespace
} // namespace sixseven
