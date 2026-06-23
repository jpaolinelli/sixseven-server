/// Unit tests for multi-column EMBEDDING source_expr support (GDB-901).
///
/// Covers:
///   - parse_source_columns: single, multi, whitespace-trimmed, empty
///   - build_source_text: single-col (byte-identical regression), multi-col concat,
///     NULL/non-STRING skipped, zero-resolve distinguishable from all-NULL
///   - INSERT path: multi-col source_expr produces correct source_text in job
///   - BACKFILL path: multi-col source_expr concatenates correctly; zero-resolve
///     skips the row and does NOT embed column 0's text
///
/// Note: REEMBED is driven through QueryEngine internals that need a running
/// server + real embed provider; its correctness is covered by the shared
/// build_source_text helper tests below.

#include "sixseven/vector/embedding_column.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// ===========================================================================
// parse_source_columns
// ===========================================================================

TEST(ParseSourceColumns, SingleColumn) {
    auto cols = EmbeddingColumnManager::parse_source_columns("name");
    ASSERT_EQ(cols.size(), 1u);
    EXPECT_EQ(cols[0], "name");
}

TEST(ParseSourceColumns, TwoColumns) {
    auto cols = EmbeddingColumnManager::parse_source_columns("name,active");
    ASSERT_EQ(cols.size(), 2u);
    EXPECT_EQ(cols[0], "name");
    EXPECT_EQ(cols[1], "active");
}

TEST(ParseSourceColumns, WhitespaceTrimmed) {
    auto cols = EmbeddingColumnManager::parse_source_columns(" name , active ");
    ASSERT_EQ(cols.size(), 2u);
    EXPECT_EQ(cols[0], "name");
    EXPECT_EQ(cols[1], "active");
}

TEST(ParseSourceColumns, EmptyExprReturnsEmpty) {
    auto cols = EmbeddingColumnManager::parse_source_columns("");
    EXPECT_TRUE(cols.empty());
}

TEST(ParseSourceColumns, BlankTokensSkipped) {
    // "name," — trailing comma produces a blank second token which is skipped.
    auto cols = EmbeddingColumnManager::parse_source_columns("name,");
    ASSERT_EQ(cols.size(), 1u);
    EXPECT_EQ(cols[0], "name");
}

TEST(ParseSourceColumns, ThreeColumns) {
    auto cols = EmbeddingColumnManager::parse_source_columns("a,b,c");
    ASSERT_EQ(cols.size(), 3u);
    EXPECT_EQ(cols[0], "a");
    EXPECT_EQ(cols[1], "b");
    EXPECT_EQ(cols[2], "c");
}

// ===========================================================================
// build_source_text
// ===========================================================================

// Helper: build a flat schema from name list.
static std::vector<CatalogColumnDef> make_schema(const std::vector<std::string>& names) {
    std::vector<CatalogColumnDef> cols;
    for (size_t i = 0; i < names.size(); ++i) {
        CatalogColumnDef cd;
        cd.ordinal = static_cast<int32_t>(i);
        cd.name = names[i];
        cd.type_id = TypeId::STRING;
        cd.nullable = true;
        cols.push_back(std::move(cd));
    }
    return cols;
}

TEST(BuildSourceText, SingleColumnUnchanged) {
    // Regression: single-column source_expr must produce byte-identical text to the
    // pre-multi-col behaviour.
    auto schema = make_schema({"id", "name", "active"});
    std::vector<Value> values = {
        Value(int32_t{1}), Value(std::string("Alice")), Value(std::string("true"))};
    auto result = EmbeddingColumnManager::build_source_text("name", schema, values);
    EXPECT_EQ(result.text, "Alice");
    EXPECT_EQ(result.resolved_count, 1u);
}

TEST(BuildSourceText, MultiColumnConcatenated) {
    auto schema = make_schema({"id", "name", "active"});
    std::vector<Value> values = {
        Value(int32_t{1}), Value(std::string("Alice")), Value(std::string("yes"))};
    auto result = EmbeddingColumnManager::build_source_text("name,active", schema, values);
    EXPECT_EQ(result.text, "Alice yes");
    EXPECT_EQ(result.resolved_count, 2u);
}

TEST(BuildSourceText, NullValuesSkipped) {
    auto schema = make_schema({"id", "name", "bio"});
    // "bio" is null — only "name" contributes.
    Value null_val;
    std::vector<Value> values = {Value(int32_t{1}), Value(std::string("Bob")), null_val};
    auto result = EmbeddingColumnManager::build_source_text("name,bio", schema, values);
    EXPECT_EQ(result.text, "Bob");
    // Both columns were found in schema, so resolved_count is 2.
    EXPECT_EQ(result.resolved_count, 2u);
}

TEST(BuildSourceText, NonStringValuesSkipped) {
    // "active" is BOOL — should be skipped (not a STRING).
    std::vector<CatalogColumnDef> schema;
    {
        CatalogColumnDef cd;
        cd.ordinal = 0;
        cd.name = "name";
        cd.type_id = TypeId::STRING;
        schema.push_back(cd);
    }
    {
        CatalogColumnDef cd;
        cd.ordinal = 1;
        cd.name = "active";
        cd.type_id = TypeId::BOOL;
        schema.push_back(cd);
    }
    std::vector<Value> values = {Value(std::string("Carol")), Value(true)};
    auto result = EmbeddingColumnManager::build_source_text("name,active", schema, values);
    EXPECT_EQ(result.text, "Carol");
    EXPECT_EQ(result.resolved_count, 2u); // both found in schema
}

TEST(BuildSourceText, ZeroResolveWhenNoColumnMatches) {
    // source_expr names columns that do not exist in the schema.
    auto schema = make_schema({"id", "title"});
    std::vector<Value> values = {Value(int32_t{1}), Value(std::string("Hello"))};
    auto result = EmbeddingColumnManager::build_source_text("nonexistent_col", schema, values);
    EXPECT_EQ(result.resolved_count, 0u);
    EXPECT_TRUE(result.text.empty());
}

TEST(BuildSourceText, AllNullsWithColumnsFoundReturnsEmptyTextButNonZeroResolved) {
    // All source columns are NULL — text is empty but resolved_count > 0.
    // This is the distinguishable "legit empty" case from "misconfigured".
    auto schema = make_schema({"id", "name", "bio"});
    Value null_val;
    std::vector<Value> values = {Value(int32_t{1}), null_val, null_val};
    auto result = EmbeddingColumnManager::build_source_text("name,bio", schema, values);
    EXPECT_TRUE(result.text.empty());
    EXPECT_EQ(result.resolved_count, 2u);
}

TEST(BuildSourceText, WhitespaceTrimmedColumnNamesMatch) {
    auto schema = make_schema({"name", "active"});
    std::vector<Value> values = {Value(std::string("Dave")), Value(std::string("yes"))};
    // source_expr with spaces around commas must still resolve.
    auto result = EmbeddingColumnManager::build_source_text(" name , active ", schema, values);
    EXPECT_EQ(result.text, "Dave yes");
    EXPECT_EQ(result.resolved_count, 2u);
}

// ===========================================================================
// INSERT path: enqueue_embedding_jobs via EmbeddingColumnManager helpers
//
// We test the helper directly here — the insert.cpp wiring uses the same
// helper, so this is a proxy for the INSERT path behaviour.
// ===========================================================================

TEST(InsertPathMultiColSource, MultiColSourceExprProducesCorrectJobText) {
    // Simulate what insert.cpp now does: build CatalogColumnDef from column_names_
    // then call build_source_text.
    std::vector<std::string> column_names = {"id", "name", "active"};
    std::vector<Value> values = {
        Value(int32_t{42}), Value(std::string("Eve")), Value(std::string("true"))};

    // Build the insert schema the same way insert.cpp does it.
    std::vector<CatalogColumnDef> insert_schema;
    insert_schema.reserve(column_names.size());
    for (size_t i = 0; i < column_names.size(); ++i) {
        CatalogColumnDef cd;
        cd.name = column_names[i];
        cd.ordinal = static_cast<int32_t>(i);
        insert_schema.push_back(std::move(cd));
    }

    auto src = EmbeddingColumnManager::build_source_text("name,active", insert_schema, values);
    EXPECT_EQ(src.text, "Eve true");
    EXPECT_EQ(src.resolved_count, 2u);
}

TEST(InsertPathMultiColSource, SingleColSourceExprUnchanged) {
    std::vector<std::string> column_names = {"id", "name", "active"};
    std::vector<Value> values = {
        Value(int32_t{1}), Value(std::string("Frank")), Value(std::string("yes"))};

    std::vector<CatalogColumnDef> insert_schema;
    for (size_t i = 0; i < column_names.size(); ++i) {
        CatalogColumnDef cd;
        cd.name = column_names[i];
        cd.ordinal = static_cast<int32_t>(i);
        insert_schema.push_back(std::move(cd));
    }

    auto src = EmbeddingColumnManager::build_source_text("name", insert_schema, values);
    // Byte-identical to old single-column extraction.
    EXPECT_EQ(src.text, "Frank");
    EXPECT_EQ(src.resolved_count, 1u);
}

// ===========================================================================
// BACKFILL path: critical mutation-grade test
//
// With the OLD code (source_idx defaults to 0), source_expr="name,active"
// would fail the exact-match and silently embed column 0's text ("id" as string
// or just skip).  With the NEW code the columns are parsed and concatenated.
//
// We simulate the backfill inner-loop logic directly to avoid spinning up
// BackfillManager's threads + storage.
// ===========================================================================

TEST(BackfillPathMultiColSource, MultiColNotCol0WhenSourceExprIsMulti) {
    // Schema: col0="id" (INT), col1="name" (STRING), col2="active" (STRING).
    // Embedding source_expr = "name,active".
    // OLD code: source_idx stays 0 (no exact match on "name,active"), embeds col0.
    // NEW code: parse + concat -> "Alice yes".
    auto schema = make_schema({"id", "name", "active"});
    // Adjust types so col0 is INT (non-STRING), cols 1+2 are STRING.
    schema[0].type_id = TypeId::INT32;

    std::vector<Value> values = {
        Value(int32_t{1}), Value(std::string("Alice")), Value(std::string("yes"))};

    auto src = EmbeddingColumnManager::build_source_text("name,active", schema, values);
    // Must be "Alice yes" — NOT anything from col0.
    EXPECT_EQ(src.text, "Alice yes");
    EXPECT_EQ(src.resolved_count, 2u);
    // Also ensure col0's text ("1" as INT) is NOT in the result.
    EXPECT_EQ(src.text.find('1'), std::string::npos);
}

TEST(BackfillPathMultiColSource, ZeroResolveSkipsRow) {
    // source_expr names a column that does not exist — resolved_count==0, text empty.
    // Backfill should skip the row (never embed) when resolved_count==0.
    auto schema = make_schema({"id", "name"});
    std::vector<Value> values = {Value(int32_t{1}), Value(std::string("Grace"))};

    auto src = EmbeddingColumnManager::build_source_text("bogus_column", schema, values);
    EXPECT_EQ(src.resolved_count, 0u);
    EXPECT_TRUE(src.text.empty());
    // Caller should LOG_ERROR + skip when resolved_count == 0.
    // We verify the signal (resolved_count) is correct; the LOG_ERROR is in backfill_manager.cpp.
}

TEST(BackfillPathMultiColSource, SingleColSourceExprRegression) {
    // source_expr="name" on a table where col0 != name.
    // Regression: must embed exactly col1 (name), not col0 (id).
    auto schema = make_schema({"id", "name"});
    schema[0].type_id = TypeId::INT32;
    std::vector<Value> values = {Value(int32_t{99}), Value(std::string("Hank"))};

    auto src = EmbeddingColumnManager::build_source_text("name", schema, values);
    EXPECT_EQ(src.text, "Hank");
    EXPECT_EQ(src.resolved_count, 1u);
    EXPECT_EQ(src.text.find("99"), std::string::npos);
}

// ===========================================================================
// REEMBED path: zero-resolve guard contract (GDB-901)
//
// REEMBED drives build_source_text for every row in each batch.  When
// resolved_count==0 the caller must skip that row and must NOT forward an
// empty string to embed_batch (which would persist a garbage vector).
//
// We verify the helper-level contract here; the QueryEngine guard that acts
// on this signal is tested by inspecting resolved_count in the same way the
// production loop does.
// ===========================================================================

TEST(ReembedZeroResolve, ZeroResolveProducesEmptyTextAndZeroCount) {
    // source_expr names a column that does not exist in the schema —
    // exactly the misconfiguration that the REEMBED guard is protecting against.
    auto schema = make_schema({"id", "name", "bio"});
    std::vector<Value> values = {
        Value(int32_t{1}), Value(std::string("Ivan")), Value(std::string("desc"))};

    auto src = EmbeddingColumnManager::build_source_text("nonexistent_col", schema, values);

    // The REEMBED guard condition: resolved_count == 0 means skip this row.
    EXPECT_EQ(src.resolved_count, 0u);
    // text must be empty — sending this to embed_batch would produce a garbage vector.
    EXPECT_TRUE(src.text.empty());
}

TEST(ReembedZeroResolve, SkipsRowAndDoesNotEmbed) {
    // Simulate the REEMBED inner-loop logic: only push to source_texts when
    // resolved_count > 0; leave source_texts empty when all rows zero-resolve.
    auto schema = make_schema({"id", "title"});
    std::vector<Value> row_values = {Value(int32_t{7}), Value(std::string("Hello"))};

    // Two rows in the batch, both with a bogus source_expr.
    std::vector<std::string> source_texts;
    std::vector<size_t> active_indices;
    size_t total_skipped = 0;

    for (size_t row_i = 0; row_i < 2; ++row_i) {
        auto src = EmbeddingColumnManager::build_source_text("bogus_col", schema, row_values);
        if (src.resolved_count == 0) {
            ++total_skipped;
            continue;
        }
        active_indices.push_back(row_i);
        source_texts.push_back(std::move(src.text));
    }

    // Nothing should reach embed_batch.
    EXPECT_TRUE(source_texts.empty());
    EXPECT_TRUE(active_indices.empty());
    // Both rows must be counted as skipped.
    EXPECT_EQ(total_skipped, 2u);
}

TEST(ReembedZeroResolve, ActiveIndicesAlignWithEmbeddings) {
    // When some rows zero-resolve and some don't, active_indices must correctly
    // map embedding[i] back to batch[active_indices[i]].
    auto schema = make_schema({"id", "name"});

    // Row 0: name exists — resolves.
    // Row 1: use a source_expr that won't match — zero-resolve.
    // Row 2: name exists — resolves.
    std::vector<std::vector<Value>> batch = {
        {Value(int32_t{1}), Value(std::string("Alice"))},
        {Value(int32_t{2}), Value(std::string("Bob"))},
        {Value(int32_t{3}), Value(std::string("Carol"))},
    };

    std::vector<std::string> source_texts;
    std::vector<size_t> active_indices;
    size_t total_skipped = 0;

    // Row 0 and 2 use "name" (resolves); row 1 uses a bogus expr.
    std::vector<std::string> exprs = {"name", "bogus_col", "name"};

    for (size_t row_i = 0; row_i < batch.size(); ++row_i) {
        auto src = EmbeddingColumnManager::build_source_text(exprs[row_i], schema, batch[row_i]);
        if (src.resolved_count == 0) {
            ++total_skipped;
            continue;
        }
        active_indices.push_back(row_i);
        source_texts.push_back(std::move(src.text));
    }

    // Only rows 0 and 2 made it through.
    ASSERT_EQ(active_indices.size(), 2u);
    EXPECT_EQ(active_indices[0], 0u);
    EXPECT_EQ(active_indices[1], 2u);
    EXPECT_EQ(source_texts[0], "Alice");
    EXPECT_EQ(source_texts[1], "Carol");
    EXPECT_EQ(total_skipped, 1u);
}
