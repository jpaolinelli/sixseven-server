/// QA adversarial tests for GDB-901: multi-column EMBEDDING source_expr support.
///
/// Attack surfaces:
///   1. MUTATION-GRADE: confirm old default-col-0 bug is dead on multi-col source_expr.
///   2. Concat semantics: order preservation, embedded commas/spaces, NULL positions,
///      all-NULL, non-STRING types interleaved, very long text, unicode, duplicate
///      column name, whitespace-padded names.
///   3. Zero-resolve / misconfig: BACKFILL skips, does NOT embed col 0.
///   4. Cross-path consistency: same source_expr -> same text on INSERT/BACKFILL proxy.
///   5. Regression: single-column path is byte-identical to pre-fix behaviour.

#include "sixseven/vector/embedding_column.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::vector<CatalogColumnDef> make_schema(const std::vector<std::string>& names,
                                                 const std::vector<TypeId>& types = {}) {
    std::vector<CatalogColumnDef> cols;
    for (size_t i = 0; i < names.size(); ++i) {
        CatalogColumnDef cd;
        cd.ordinal = static_cast<int32_t>(i);
        cd.name = names[i];
        cd.nullable = true;
        cd.type_id = (types.size() > i) ? types[i] : TypeId::STRING;
        cols.push_back(std::move(cd));
    }
    return cols;
}

// ---------------------------------------------------------------------------
// 1. MUTATION-GRADE: old default-col-0 bug is dead
// ---------------------------------------------------------------------------

// Confirm that for source_expr="name,active" the result is NOT column-0 text
// and IS the concatenation of name+active.
TEST(QA_GDB901_Mutation, MultiColExprNotCol0) {
    // Schema: col0=id (INT32, not STRING), col1=name, col2=active.
    auto schema =
        make_schema({"id", "name", "active"}, {TypeId::INT32, TypeId::STRING, TypeId::STRING});
    std::vector<Value> values = {
        Value(int32_t{1}), Value(std::string("Alice")), Value(std::string("yes"))};

    auto src = EmbeddingColumnManager::build_source_text("name,active", schema, values);

    // Old code: source_idx stays 0 -> embeds col0 text (INT32, would be empty).
    // New code: parsed concat -> "Alice yes".
    EXPECT_EQ(src.text, "Alice yes");
    EXPECT_EQ(src.resolved_count, 2u);
    // Explicitly verify col0 text is absent.
    EXPECT_EQ(src.text.find("1"), std::string::npos);
}

// Even when col0 IS a STRING, multi-col source_expr must not embed it when it
// is not named in source_expr.
TEST(QA_GDB901_Mutation, MultiColExprDoesNotLeakFirstCol) {
    auto schema = make_schema({"secret", "name", "active"});
    std::vector<Value> values = {Value(std::string("SHOULD_NOT_APPEAR")),
                                 Value(std::string("Bob")),
                                 Value(std::string("no"))};

    auto src = EmbeddingColumnManager::build_source_text("name,active", schema, values);

    EXPECT_EQ(src.text, "Bob no");
    EXPECT_EQ(src.text.find("SHOULD_NOT_APPEAR"), std::string::npos);
    EXPECT_EQ(src.resolved_count, 2u);
}

// Zero-resolve must never embed col 0 — resolved_count==0 and text empty.
TEST(QA_GDB901_Mutation, ZeroResolveNeverEmbedCol0) {
    auto schema = make_schema({"id", "name", "active"});
    std::vector<Value> values = {
        Value(std::string("col0-text")), Value(std::string("Bob")), Value(std::string("no"))};

    // source_expr names nothing that exists.
    auto src = EmbeddingColumnManager::build_source_text("nonexistent", schema, values);

    EXPECT_EQ(src.resolved_count, 0u);
    EXPECT_TRUE(src.text.empty());
    // No text from any column should leak.
    EXPECT_EQ(src.text.find("col0-text"), std::string::npos);
    EXPECT_EQ(src.text.find("Bob"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 2. Concat semantics — adversarial
// ---------------------------------------------------------------------------

// Order preservation: "active,name" must produce "yes Alice", not "Alice yes".
TEST(QA_GDB901_ConcatSemantics, OrderPreserved) {
    auto schema = make_schema({"id", "name", "active"});
    std::vector<Value> values = {
        Value(int32_t{1}), Value(std::string("Alice")), Value(std::string("yes"))};

    auto src = EmbeddingColumnManager::build_source_text("active,name", schema, values);
    EXPECT_EQ(src.text, "yes Alice");
    EXPECT_EQ(src.resolved_count, 2u);
}

// Values that themselves contain commas and spaces — they must pass through verbatim.
TEST(QA_GDB901_ConcatSemantics, EmbeddedCommasAndSpacesInValues) {
    auto schema = make_schema({"a", "b"});
    std::vector<Value> values = {Value(std::string("hello, world")), Value(std::string("foo bar"))};

    auto src = EmbeddingColumnManager::build_source_text("a,b", schema, values);
    EXPECT_EQ(src.text, "hello, world foo bar");
    EXPECT_EQ(src.resolved_count, 2u);
}

// NULL in first position: second part still emitted, no leading space.
TEST(QA_GDB901_ConcatSemantics, NullInFirstPosition) {
    auto schema = make_schema({"a", "b"});
    std::vector<Value> values = {Value(), Value(std::string("second"))};

    auto src = EmbeddingColumnManager::build_source_text("a,b", schema, values);
    EXPECT_EQ(src.text, "second");
    EXPECT_EQ(src.resolved_count, 2u);
}

// NULL in middle position.
TEST(QA_GDB901_ConcatSemantics, NullInMiddlePosition) {
    auto schema = make_schema({"a", "b", "c"});
    std::vector<Value> values = {Value(std::string("first")), Value(), Value(std::string("third"))};

    auto src = EmbeddingColumnManager::build_source_text("a,b,c", schema, values);
    EXPECT_EQ(src.text, "first third");
    EXPECT_EQ(src.resolved_count, 3u);
}

// NULL in last position: no trailing space.
TEST(QA_GDB901_ConcatSemantics, NullInLastPosition) {
    auto schema = make_schema({"a", "b"});
    std::vector<Value> values = {Value(std::string("first")), Value()};

    auto src = EmbeddingColumnManager::build_source_text("a,b", schema, values);
    EXPECT_EQ(src.text, "first");
    EXPECT_EQ(src.resolved_count, 2u);
}

// All NULL: text must be empty but resolved_count > 0 (distinguishable from misconfigured).
TEST(QA_GDB901_ConcatSemantics, AllNullResolvedCountPositive) {
    auto schema = make_schema({"a", "b"});
    std::vector<Value> values = {Value(), Value()};

    auto src = EmbeddingColumnManager::build_source_text("a,b", schema, values);
    EXPECT_TRUE(src.text.empty());
    EXPECT_EQ(src.resolved_count, 2u);
}

// Non-STRING types interleaved: INT32, BOOL, EMBEDDING all skipped; STRING contributes.
TEST(QA_GDB901_ConcatSemantics, NonStringTypesSkipped) {
    auto schema =
        make_schema({"num", "flag", "desc"}, {TypeId::INT32, TypeId::BOOL, TypeId::STRING});
    std::vector<Value> values = {
        Value(int32_t{42}), Value(true), Value(std::string("description"))};

    auto src = EmbeddingColumnManager::build_source_text("num,flag,desc", schema, values);
    EXPECT_EQ(src.text, "description");
    EXPECT_EQ(src.resolved_count, 3u); // all three resolved in schema
}

// Empty string value: skipped (not added to joined, no dangling space).
TEST(QA_GDB901_ConcatSemantics, EmptyStringValueSkipped) {
    auto schema = make_schema({"a", "b", "c"});
    std::vector<Value> values = {
        Value(std::string("first")), Value(std::string("")), Value(std::string("third"))};

    auto src = EmbeddingColumnManager::build_source_text("a,b,c", schema, values);
    EXPECT_EQ(src.text, "first third");
    EXPECT_EQ(src.resolved_count, 3u);
}

// Very long concatenated text (stress: 100 columns each with 100-char value).
TEST(QA_GDB901_ConcatSemantics, VeryLongConcatenation) {
    const size_t N = 100;
    std::vector<std::string> names;
    std::vector<TypeId> types;
    std::vector<Value> values;
    std::string source_expr;
    for (size_t i = 0; i < N; ++i) {
        names.push_back("col" + std::to_string(i));
        types.push_back(TypeId::STRING);
        values.push_back(Value(std::string(100, 'a' + static_cast<char>(i % 26))));
        if (i > 0)
            source_expr += ',';
        source_expr += "col" + std::to_string(i);
    }

    auto schema = make_schema(names, types);
    auto src = EmbeddingColumnManager::build_source_text(source_expr, schema, values);

    EXPECT_EQ(src.resolved_count, N);
    // Text = N parts of 100 chars joined by spaces = N*100 + (N-1) spaces.
    EXPECT_EQ(src.text.size(), N * 100 + (N - 1));
}

// Unicode / multibyte UTF-8 values pass through verbatim.
TEST(QA_GDB901_ConcatSemantics, UnicodeValues) {
    // Use raw UTF-8 byte sequences to avoid C++20 char8_t incompatibility.
    const std::string chinese = "\xe4\xb8\xad\xe6\x96\x87"; // "中文"
    const std::string french = "\xc3\xa9l\xc3\xa8ve";       // "élève"
    auto schema = make_schema({"a", "b"});
    std::vector<Value> values = {Value(chinese), Value(french)};

    auto src = EmbeddingColumnManager::build_source_text("a,b", schema, values);
    EXPECT_EQ(src.text, chinese + " " + french);
    EXPECT_EQ(src.resolved_count, 2u);
}

// Duplicate column name in source_expr: each occurrence resolves independently
// and the value is contributed twice.
TEST(QA_GDB901_ConcatSemantics, DuplicateColumnNameInSourceExpr) {
    auto schema = make_schema({"name"});
    std::vector<Value> values = {Value(std::string("Alice"))};

    auto src = EmbeddingColumnManager::build_source_text("name,name", schema, values);
    // Each "name" resolves -> resolved_count == 2, text = "Alice Alice".
    EXPECT_EQ(src.resolved_count, 2u);
    EXPECT_EQ(src.text, "Alice Alice");
}

// Column name with surrounding whitespace in source_expr is trimmed before matching.
TEST(QA_GDB901_ConcatSemantics, WhitespacePaddedColumnNamesInSourceExpr) {
    auto schema = make_schema({"name", "active"});
    std::vector<Value> values = {Value(std::string("Dave")), Value(std::string("yes"))};

    auto src = EmbeddingColumnManager::build_source_text(" name , active ", schema, values);
    EXPECT_EQ(src.text, "Dave yes");
    EXPECT_EQ(src.resolved_count, 2u);
}

// ---------------------------------------------------------------------------
// 3. Zero-resolve / misconfig guard
// ---------------------------------------------------------------------------

// Partial match (one valid + one invalid name) -> resolved_count > 0, partial text.
TEST(QA_GDB901_ZeroResolve, PartialMatchReturnsPartialText) {
    auto schema = make_schema({"name", "active"});
    std::vector<Value> values = {Value(std::string("Carol")), Value(std::string("yes"))};

    // "name" resolves, "missing" does not.
    auto src = EmbeddingColumnManager::build_source_text("name,missing", schema, values);
    EXPECT_EQ(src.text, "Carol");
    EXPECT_EQ(src.resolved_count, 1u); // only "name" resolved
}

// Completely bogus source_expr.
TEST(QA_GDB901_ZeroResolve, AllColumnsMissing) {
    auto schema = make_schema({"name", "active"});
    std::vector<Value> values = {Value(std::string("Carol")), Value(std::string("yes"))};

    auto src = EmbeddingColumnManager::build_source_text("bogus1,bogus2", schema, values);
    EXPECT_EQ(src.resolved_count, 0u);
    EXPECT_TRUE(src.text.empty());
}

// Empty source_expr -> zero resolve, empty text.
TEST(QA_GDB901_ZeroResolve, EmptySourceExpr) {
    auto schema = make_schema({"name"});
    std::vector<Value> values = {Value(std::string("Alice"))};

    auto src = EmbeddingColumnManager::build_source_text("", schema, values);
    EXPECT_EQ(src.resolved_count, 0u);
    EXPECT_TRUE(src.text.empty());
}

// Empty values vector does not crash.
TEST(QA_GDB901_ZeroResolve, EmptyValuesDoesNotCrash) {
    auto schema = make_schema({"name"});
    std::vector<Value> values;

    auto src = EmbeddingColumnManager::build_source_text("name", schema, values);
    // schema has "name" but values is empty -> i >= values.size() -> skipped.
    EXPECT_EQ(src.resolved_count, 1u); // column found in schema
    EXPECT_TRUE(src.text.empty());     // but value missing -> no text
}

// Empty schema does not crash.
TEST(QA_GDB901_ZeroResolve, EmptySchemaDoesNotCrash) {
    std::vector<CatalogColumnDef> schema;
    std::vector<Value> values = {Value(std::string("Alice"))};

    auto src = EmbeddingColumnManager::build_source_text("name", schema, values);
    EXPECT_EQ(src.resolved_count, 0u);
    EXPECT_TRUE(src.text.empty());
}

// ---------------------------------------------------------------------------
// 4. Cross-path consistency
// ---------------------------------------------------------------------------

// INSERT proxy: build insert_schema from column_names (like insert.cpp does),
// call build_source_text -> must match BACKFILL path (same helper, same schema).
TEST(QA_GDB901_CrossPath, InsertAndBackfillProxyAgreeSingleCol) {
    // Schema as seen by BACKFILL (full catalog schema).
    auto backfill_schema =
        make_schema({"id", "name", "active"}, {TypeId::INT32, TypeId::STRING, TypeId::STRING});
    std::vector<Value> values = {
        Value(int32_t{1}), Value(std::string("Alice")), Value(std::string("yes"))};

    // INSERT path builds its own schema slice from column_names_.
    // Columns: same as backfill schema here.
    std::vector<std::string> col_names = {"id", "name", "active"};
    std::vector<CatalogColumnDef> insert_schema;
    for (size_t i = 0; i < col_names.size(); ++i) {
        CatalogColumnDef cd;
        cd.name = col_names[i];
        cd.ordinal = static_cast<int32_t>(i);
        insert_schema.push_back(std::move(cd));
    }

    auto backfill_src = EmbeddingColumnManager::build_source_text("name", backfill_schema, values);
    auto insert_src = EmbeddingColumnManager::build_source_text("name", insert_schema, values);

    EXPECT_EQ(backfill_src.text, insert_src.text);
    EXPECT_EQ(backfill_src.resolved_count, insert_src.resolved_count);
}

TEST(QA_GDB901_CrossPath, InsertAndBackfillProxyAgreeMultiCol) {
    auto backfill_schema =
        make_schema({"id", "name", "active"}, {TypeId::INT32, TypeId::STRING, TypeId::STRING});
    std::vector<Value> values = {
        Value(int32_t{1}), Value(std::string("Alice")), Value(std::string("yes"))};

    std::vector<std::string> col_names = {"id", "name", "active"};
    std::vector<CatalogColumnDef> insert_schema;
    for (size_t i = 0; i < col_names.size(); ++i) {
        CatalogColumnDef cd;
        cd.name = col_names[i];
        cd.ordinal = static_cast<int32_t>(i);
        insert_schema.push_back(std::move(cd));
    }

    auto backfill_src =
        EmbeddingColumnManager::build_source_text("name,active", backfill_schema, values);
    auto insert_src =
        EmbeddingColumnManager::build_source_text("name,active", insert_schema, values);

    EXPECT_EQ(backfill_src.text, insert_src.text);
    EXPECT_EQ(backfill_src.resolved_count, insert_src.resolved_count);
}

// REEMBED uses table_schema->columns (same as BACKFILL). Verify text matches.
TEST(QA_GDB901_CrossPath, ReembedProxyMatchesBackfill) {
    // Full catalog schema — same as what REEMBED passes to build_source_text.
    auto full_schema =
        make_schema({"id", "description", "tags"}, {TypeId::INT32, TypeId::STRING, TypeId::STRING});
    std::vector<Value> values = {
        Value(int32_t{7}), Value(std::string("A widget")), Value(std::string("hardware,sale"))};

    auto reembed_src =
        EmbeddingColumnManager::build_source_text("description,tags", full_schema, values);
    auto backfill_src =
        EmbeddingColumnManager::build_source_text("description,tags", full_schema, values);

    // Both call the same helper with the same args so must be identical.
    EXPECT_EQ(reembed_src.text, backfill_src.text);
    EXPECT_EQ(reembed_src.resolved_count, backfill_src.resolved_count);
}

// ---------------------------------------------------------------------------
// 5. Regression: single-column byte-identical
// ---------------------------------------------------------------------------

TEST(QA_GDB901_Regression, SingleColByteIdenticalNoLeadingSpace) {
    auto schema =
        make_schema({"id", "name", "bio"}, {TypeId::INT32, TypeId::STRING, TypeId::STRING});
    std::vector<Value> values = {
        Value(int32_t{1}), Value(std::string("ExactText")), Value(std::string("unused"))};

    auto src = EmbeddingColumnManager::build_source_text("name", schema, values);
    EXPECT_EQ(src.text, "ExactText");
    EXPECT_EQ(src.resolved_count, 1u);
    // No leading or trailing spaces.
    EXPECT_EQ(src.text[0], 'E');
    EXPECT_EQ(src.text.back(), 't');
}

TEST(QA_GDB901_Regression, SingleColNullYieldsEmptyTextPositiveResolved) {
    auto schema = make_schema({"name"});
    std::vector<Value> values = {Value()}; // null

    auto src = EmbeddingColumnManager::build_source_text("name", schema, values);
    EXPECT_TRUE(src.text.empty());
    EXPECT_EQ(src.resolved_count, 1u); // column found -> resolved
}

// ---------------------------------------------------------------------------
// 6. parse_source_columns adversarial
// ---------------------------------------------------------------------------

TEST(QA_GDB901_ParseSourceColumns, ManyColumns) {
    std::string expr;
    const int N = 50;
    for (int i = 0; i < N; ++i) {
        if (i > 0)
            expr += ',';
        expr += "col" + std::to_string(i);
    }
    auto cols = EmbeddingColumnManager::parse_source_columns(expr);
    ASSERT_EQ(cols.size(), static_cast<size_t>(N));
    EXPECT_EQ(cols[0], "col0");
    EXPECT_EQ(cols[N - 1], "col" + std::to_string(N - 1));
}

TEST(QA_GDB901_ParseSourceColumns, TrailingCommaIgnored) {
    auto cols = EmbeddingColumnManager::parse_source_columns("a,b,");
    ASSERT_EQ(cols.size(), 2u);
    EXPECT_EQ(cols[0], "a");
    EXPECT_EQ(cols[1], "b");
}

TEST(QA_GDB901_ParseSourceColumns, LeadingCommaProducesEmptyFirst) {
    // ",b" -> first token is blank (skipped), second is "b".
    auto cols = EmbeddingColumnManager::parse_source_columns(",b");
    ASSERT_EQ(cols.size(), 1u);
    EXPECT_EQ(cols[0], "b");
}

TEST(QA_GDB901_ParseSourceColumns, OnlyCommas) {
    auto cols = EmbeddingColumnManager::parse_source_columns(",,,");
    EXPECT_TRUE(cols.empty());
}

TEST(QA_GDB901_ParseSourceColumns, OnlyWhitespace) {
    auto cols = EmbeddingColumnManager::parse_source_columns("   ");
    EXPECT_TRUE(cols.empty());
}

TEST(QA_GDB901_ParseSourceColumns, TabsNotTrimmed) {
    // Trim only trims ASCII space (0x20); tabs are not trimmed.
    // "\tname\t" -> token = "\tname\t" (tabs remain, start != npos).
    auto cols = EmbeddingColumnManager::parse_source_columns("\tname\t");
    ASSERT_EQ(cols.size(), 1u);
    // The token will have tabs still attached — this is the current spec
    // (only ASCII-space trimmed). Verify we at least got 1 token back.
    EXPECT_FALSE(cols[0].empty());
}
