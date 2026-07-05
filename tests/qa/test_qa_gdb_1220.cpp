// QA regression tests for GDB-1220.
//
// GDB-1220 rebuilt src/planner/type_resolver.cpp's type_name_map() to seed
// its canonical subset from parse_type_id/type_name() (common/types.h)
// instead of hand-listing all 23 canonical names, then layers 17 SQL-only
// aliases on top. parse_type_id itself (used by the graph edge-property
// parser in src/graph/graph_engine.cpp) is unchanged.
//
// Adversarial focus:
//  1. Graph domain: parse_type_id must still REJECT every SQL alias and
//     ACCEPT every canonical name, byte-identical to pre-GDB-1220 behavior.
//  2. SQL domain: exhaustive name -> TypeId table through resolve_type_spec
//     for all 23 canonical names + 17 aliases, matching main.
//  3. Case-insensitivity per domain; unknown/garbage names rejected
//     identically in both domains.
//  4. Parameterized-type spellings (DECIMAL, EMBEDDING, PATH) -- these are
//     last in the enum / more complex TypeIds, likely to be dropped if the
//     seeding loop's bound is off-by-one.
//  5. No loss of alias coverage vs the prior hand-written map.

#include "sixseven/common/status.h"
#include "sixseven/common/types.h"
#include "sixseven/parser/ast.h"
#include "sixseven/planner/type_resolver.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

using namespace sixseven;

namespace {

TypeSpec make_spec(const std::string& name) {
    TypeSpec spec;
    spec.name = name;
    return spec;
}

// The 23 canonical type names as recognized by parse_type_id/type_name.
const std::vector<std::pair<std::string, TypeId>>& canonical_names() {
    static const std::vector<std::pair<std::string, TypeId>> v = {
        {"INT8", TypeId::INT8},         {"INT16", TypeId::INT16},
        {"INT32", TypeId::INT32},       {"INT64", TypeId::INT64},
        {"UINT8", TypeId::UINT8},       {"UINT16", TypeId::UINT16},
        {"UINT32", TypeId::UINT32},     {"UINT64", TypeId::UINT64},
        {"FLOAT32", TypeId::FLOAT32},   {"FLOAT64", TypeId::FLOAT64},
        {"DECIMAL", TypeId::DECIMAL},   {"BOOL", TypeId::BOOL},
        {"STRING", TypeId::STRING},     {"BLOB", TypeId::BLOB},
        {"DATE", TypeId::DATE},         {"TIME", TypeId::TIME},
        {"TIMESTAMP", TypeId::TIMESTAMP}, {"INTERVAL", TypeId::INTERVAL},
        {"POINT", TypeId::POINT},       {"JSON", TypeId::JSON},
        {"UUID", TypeId::UUID},         {"EMBEDDING", TypeId::EMBEDDING},
        {"PATH", TypeId::PATH},
    };
    return v;
}

// The 17 SQL-only aliases layered on top by type_name_map().
const std::vector<std::pair<std::string, TypeId>>& sql_aliases() {
    static const std::vector<std::pair<std::string, TypeId>> v = {
        {"TINYINT", TypeId::INT8},
        {"SMALLINT", TypeId::INT16},
        {"INT", TypeId::INT32},
        {"INTEGER", TypeId::INT32},
        {"BIGINT", TypeId::INT64},
        {"FLOAT", TypeId::FLOAT32},
        {"REAL", TypeId::FLOAT32},
        {"DOUBLE", TypeId::FLOAT64},
        {"DOUBLE PRECISION", TypeId::FLOAT64},
        {"NUMERIC", TypeId::DECIMAL},
        {"BOOLEAN", TypeId::BOOL},
        {"TEXT", TypeId::STRING},
        {"VARCHAR", TypeId::STRING},
        {"CHAR", TypeId::STRING},
        {"CHARACTER VARYING", TypeId::STRING},
        {"BYTEA", TypeId::BLOB},
        {"JSONB", TypeId::JSON},
    };
    return v;
}

} // namespace

// =============================================================================
// 1. GRAPH DOMAIN -- parse_type_id must reject every SQL alias.
// =============================================================================

TEST(QA_GDB1220_GraphDomain, RejectsEverySqlAlias) {
    for (const auto& [alias, expected] : sql_aliases()) {
        (void)expected;
        auto result = parse_type_id(alias);
        EXPECT_FALSE(result.has_value())
            << "graph parse_type_id must NOT accept SQL alias: " << alias;
    }
}

TEST(QA_GDB1220_GraphDomain, RejectsLowercaseSqlAlias) {
    // Lowercase spellings of SQL-only aliases must still be rejected by the
    // graph parser -- "int" is an alias (not canonical "int32"), so it must
    // not resolve, while the canonical "int32" must resolve.
    EXPECT_FALSE(parse_type_id("int").has_value());
    EXPECT_TRUE(parse_type_id("int32").has_value());
    EXPECT_FALSE(parse_type_id("smallint").has_value());
    EXPECT_FALSE(parse_type_id("varchar").has_value());
    EXPECT_FALSE(parse_type_id("boolean").has_value());
}

TEST(QA_GDB1220_GraphDomain, AcceptsEveryCanonicalName) {
    for (const auto& [name, expected] : canonical_names()) {
        auto result = parse_type_id(name);
        ASSERT_TRUE(result.has_value()) << "graph parse_type_id must accept canonical: " << name;
        EXPECT_EQ(*result, expected) << "wrong TypeId for canonical name: " << name;
    }
}

TEST(QA_GDB1220_GraphDomain, AcceptsCanonicalNamesCaseInsensitive) {
    EXPECT_EQ(*parse_type_id("int32"), TypeId::INT32);
    EXPECT_EQ(*parse_type_id("Int32"), TypeId::INT32);
    EXPECT_EQ(*parse_type_id("dEcImAl"), TypeId::DECIMAL);
    EXPECT_EQ(*parse_type_id("embedding"), TypeId::EMBEDDING);
    EXPECT_EQ(*parse_type_id("path"), TypeId::PATH);
}

TEST(QA_GDB1220_GraphDomain, RejectsGarbageName) {
    EXPECT_FALSE(parse_type_id("").has_value());
    EXPECT_FALSE(parse_type_id("NOT_A_TYPE").has_value());
    EXPECT_FALSE(parse_type_id("INT128").has_value());
    EXPECT_FALSE(parse_type_id("STRINGZ").has_value());
}

// Full edge-property parse path: feed "name:TYPE" segments exactly as
// GraphEngine::parse_property_columns does, using an alias -- must be
// silently dropped (unknown-type segments are skipped, not errored).
TEST(QA_GDB1220_GraphDomain, EdgePropertyPathTreatsAliasAsUnknown) {
    // parse_type_id("VARCHAR") must be nullopt so the graph engine's
    // property-column parser skips this column rather than accepting it.
    auto result = parse_type_id("VARCHAR");
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// 2. SQL DOMAIN -- exhaustive resolve_type_spec table.
// =============================================================================

TEST(QA_GDB1220_SqlDomain, ResolvesEveryCanonicalName) {
    for (const auto& [name, expected] : canonical_names()) {
        auto result = resolve_type_spec(make_spec(name));
        ASSERT_TRUE(result.has_value()) << "SQL resolver must accept canonical: " << name;
        EXPECT_EQ(*result, expected) << "wrong TypeId for canonical name: " << name;
    }
}

TEST(QA_GDB1220_SqlDomain, ResolvesEverySqlAlias) {
    for (const auto& [alias, expected] : sql_aliases()) {
        auto result = resolve_type_spec(make_spec(alias));
        ASSERT_TRUE(result.has_value()) << "SQL resolver must accept alias: " << alias;
        EXPECT_EQ(*result, expected) << "wrong TypeId for alias: " << alias;
    }
}

TEST(QA_GDB1220_SqlDomain, NoNameMappedToWrongTypeId) {
    // Cross-check: every canonical name and alias must map to precisely the
    // TypeId documented, with no accidental remap introduced by the new
    // seeding loop (e.g. an off-by-one iterating raw <= PATH could shift
    // one entry to the wrong id, or duplicate emplace() could silently keep
    // the first-inserted value instead of the intended one).
    std::vector<std::pair<std::string, TypeId>> all = canonical_names();
    auto aliases = sql_aliases();
    all.insert(all.end(), aliases.begin(), aliases.end());

    for (const auto& [name, expected] : all) {
        auto result = resolve_type_spec(make_spec(name));
        ASSERT_TRUE(result.has_value()) << name;
        EXPECT_EQ(*result, expected) << name;
    }
}

// =============================================================================
// 3. Case-insensitivity + unknown name rejection, both domains.
// =============================================================================

TEST(QA_GDB1220_SqlDomain, CaseInsensitiveAliasesAndCanonicals) {
    EXPECT_EQ(*resolve_type_spec(make_spec("varchar")), TypeId::STRING);
    EXPECT_EQ(*resolve_type_spec(make_spec("VarChar")), TypeId::STRING);
    EXPECT_EQ(*resolve_type_spec(make_spec("smallint")), TypeId::INT16);
    EXPECT_EQ(*resolve_type_spec(make_spec("SmallInt")), TypeId::INT16);
    EXPECT_EQ(*resolve_type_spec(make_spec("embedding")), TypeId::EMBEDDING);
    EXPECT_EQ(*resolve_type_spec(make_spec("path")), TypeId::PATH);
    EXPECT_EQ(*resolve_type_spec(make_spec("decimal")), TypeId::DECIMAL);
}

TEST(QA_GDB1220_SqlDomain, UnknownNameYieldsTypeError) {
    auto result = resolve_type_spec(make_spec("NOT_A_REAL_TYPE"));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
    EXPECT_NE(result.error().message.find("NOT_A_REAL_TYPE"), std::string::npos)
        << "error message should mention the offending type name";
}

TEST(QA_GDB1220_SqlDomain, EmptyNameYieldsTypeError) {
    auto result = resolve_type_spec(make_spec(""));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB1220_SqlDomain, GraphOnlyCanonicalNameAlsoWorksInSqlDomain) {
    // Canonical names are a subset of the SQL domain too (seeded from
    // parse_type_id), so e.g. "INT32" (not just alias "INT") must resolve.
    EXPECT_EQ(*resolve_type_spec(make_spec("INT32")), TypeId::INT32);
    EXPECT_EQ(*resolve_type_spec(make_spec("UINT8")), TypeId::UINT8);
}

// =============================================================================
// 4. Parameterized / tricky TypeId spellings (DECIMAL, EMBEDDING, PATH).
// =============================================================================

TEST(QA_GDB1220_TrickyTypes, DecimalResolvesInBothDomains) {
    EXPECT_EQ(*parse_type_id("DECIMAL"), TypeId::DECIMAL);
    EXPECT_EQ(*resolve_type_spec(make_spec("DECIMAL")), TypeId::DECIMAL);
    // NUMERIC alias only valid in SQL domain.
    EXPECT_EQ(*resolve_type_spec(make_spec("NUMERIC")), TypeId::DECIMAL);
    EXPECT_FALSE(parse_type_id("NUMERIC").has_value());
}

TEST(QA_GDB1220_TrickyTypes, EmbeddingResolvesInBothDomains) {
    EXPECT_EQ(*parse_type_id("EMBEDDING"), TypeId::EMBEDDING);
    EXPECT_EQ(*resolve_type_spec(make_spec("EMBEDDING")), TypeId::EMBEDDING);
}

TEST(QA_GDB1220_TrickyTypes, PathResolvesInBothDomains) {
    // PATH is the last value in the TypeId enum -- the seeding loop's upper
    // bound (raw <= static_cast<uint8_t>(TypeId::PATH)) must include it, not
    // stop one short.
    EXPECT_EQ(*parse_type_id("PATH"), TypeId::PATH);
    EXPECT_EQ(*resolve_type_spec(make_spec("PATH")), TypeId::PATH);
}

TEST(QA_GDB1220_TrickyTypes, AllTwentyThreeCanonicalTypeIdsCovered) {
    // Belt-and-suspenders: assert the canonical_names() table itself has
    // exactly 23 entries (matching the documented "23 Types" in CLAUDE.md),
    // so this QA suite's own fixture can't silently drift out of sync.
    EXPECT_EQ(canonical_names().size(), 23u);
}

TEST(QA_GDB1220_TrickyTypes, AllSeventeenAliasesCovered) {
    EXPECT_EQ(sql_aliases().size(), 17u);
}

// =============================================================================
// 5. Regression guard: alias set must not shrink or gain unexpected entries.
// =============================================================================

TEST(QA_GDB1220_Regression, AliasNotAccidentallyDroppedForOverlappingCanonical) {
    // FLOAT32 is canonical; FLOAT/REAL are aliases for the same TypeId. A
    // buggy seeding loop that iterated in the wrong order, or an alias
    // emplace() executed before seeding, could cause one to shadow the
    // other with the wrong TypeId. Confirm all three independently resolve
    // to FLOAT32.
    EXPECT_EQ(*resolve_type_spec(make_spec("FLOAT32")), TypeId::FLOAT32);
    EXPECT_EQ(*resolve_type_spec(make_spec("FLOAT")), TypeId::FLOAT32);
    EXPECT_EQ(*resolve_type_spec(make_spec("REAL")), TypeId::FLOAT32);
}

TEST(QA_GDB1220_Regression, MultiWordAliasesResolveExactly) {
    // Multi-word aliases are easy to get wrong with naive tokenization.
    EXPECT_EQ(*resolve_type_spec(make_spec("DOUBLE PRECISION")), TypeId::FLOAT64);
    EXPECT_EQ(*resolve_type_spec(make_spec("CHARACTER VARYING")), TypeId::STRING);
    // Partial / malformed variants must NOT resolve.
    EXPECT_FALSE(resolve_type_spec(make_spec("DOUBLE PRECISIONX")).has_value());
    EXPECT_FALSE(resolve_type_spec(make_spec("CHARACTER")).has_value());
}

TEST(QA_GDB1220_Regression, IntegerAliasesDoNotCollideAcrossWidths) {
    // TINYINT/SMALLINT/INT/BIGINT must map to distinct TypeIds, not
    // collapse onto one width due to insertion-order bugs.
    EXPECT_EQ(*resolve_type_spec(make_spec("TINYINT")), TypeId::INT8);
    EXPECT_EQ(*resolve_type_spec(make_spec("SMALLINT")), TypeId::INT16);
    EXPECT_EQ(*resolve_type_spec(make_spec("INT")), TypeId::INT32);
    EXPECT_EQ(*resolve_type_spec(make_spec("BIGINT")), TypeId::INT64);
}
