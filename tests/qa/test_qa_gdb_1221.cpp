/// @file test_qa_gdb_1221.cpp
/// QA adversarial tests for GDB-1221: behavior-preserving dedup of inline
/// to_lower/tolower copies onto the shared sixseven::to_lower helper.
///
/// Sites deduped (per implementation handoff):
///   1. src/server/auth.cpp:parse_auth_method — security-relevant auth
///      method dispatch (trust/md5/scram-sha-256).
///   2. src/parser/parser.cpp:parse_type_spec — EMBEDDING named-parameter
///      matching (SOURCE=/PROVIDER=).
///   3. src/executor/query_engine.cpp:execute_create_index — CREATE INDEX
///      USING <method> normalization.
///
/// Correctly left inline (verify NOT broken by the dedup):
///   4. src/executor/query_engine.cpp:parse_isolation_level — trims +
///      whitespace-collapses in the same loop; NOT a pure case-fold, so it
///      must remain intact and unaffected by this ticket.
///
/// Adversarial focus:
///   - Case variants (upper/lower/mixed) resolve identically.
///   - Empty string still errors.
///   - A high-bit byte (0xFF) doesn't crash the auth path and is rejected.
///   - Whitespace-collapse behavior for SET TRANSACTION ISOLATION LEVEL is
///     unaffected (doubled internal spaces still resolve).
///   - No regression in error paths (unknown auth method / unknown index
///     method / duplicate embedding params).

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/server/auth.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// 1. Auth method parsing (security-relevant dispatch)
// =============================================================================

TEST(QA_GDB1221_Auth, AllCaseVariantsOfTrustResolveIdentically) {
    for (const std::string& variant : {"trust", "TRUST", "Trust", "TrUsT"}) {
        auto method = parse_auth_method(variant);
        ASSERT_TRUE(method.has_value()) << "variant: " << variant;
        EXPECT_EQ(*method, AuthMethod::TRUST) << "variant: " << variant;
    }
}

TEST(QA_GDB1221_Auth, AllCaseVariantsOfMd5ResolveIdentically) {
    for (const std::string& variant : {"md5", "MD5", "Md5", "mD5"}) {
        auto method = parse_auth_method(variant);
        ASSERT_TRUE(method.has_value()) << "variant: " << variant;
        EXPECT_EQ(*method, AuthMethod::MD5) << "variant: " << variant;
    }
}

TEST(QA_GDB1221_Auth, AllCaseVariantsOfScramResolveIdentically) {
    for (const std::string& variant :
         {"scram-sha-256", "SCRAM-SHA-256", "Scram-Sha-256", "sCrAm-ShA-256"}) {
        auto method = parse_auth_method(variant);
        ASSERT_TRUE(method.has_value()) << "variant: " << variant;
        EXPECT_EQ(*method, AuthMethod::SCRAM_SHA_256) << "variant: " << variant;
    }
}

TEST(QA_GDB1221_Auth, EmptyStringStillErrors) {
    auto method = parse_auth_method("");
    ASSERT_FALSE(method.has_value());
    EXPECT_EQ(method.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_FALSE(method.error().message.empty());
}

TEST(QA_GDB1221_Auth, HighBitByteDoesNotCrashAndIsRejected) {
    // 0xFF is not a valid ASCII char; std::tolower on it must not be UB
    // (to_lower casts to unsigned char before calling std::tolower).
    std::string malicious;
    malicious.push_back(static_cast<char>(0xFF));
    malicious += "trust";

    auto method = parse_auth_method(malicious);
    ASSERT_FALSE(method.has_value());
    EXPECT_EQ(method.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB1221_Auth, AllHighBitBytesDoNotCrash) {
    // Sweep every byte value 0x80-0xFF through parse_auth_method to catch
    // any UB introduced by the shared to_lower helper.
    for (int b = 0x80; b <= 0xFF; ++b) {
        std::string s;
        s.push_back(static_cast<char>(b));
        auto method = parse_auth_method(s);
        EXPECT_FALSE(method.has_value()) << "byte: " << b;
    }
}

TEST(QA_GDB1221_Auth, UnknownMethodStillErrorsWithMessage) {
    auto method = parse_auth_method("kerberos");
    ASSERT_FALSE(method.has_value());
    EXPECT_EQ(method.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(method.error().message.find("kerberos"), std::string::npos);
}

TEST(QA_GDB1221_Auth, WhitespaceIsNotTrimmedByToLowerAloneStillErrors) {
    // to_lower is a pure case-fold; leading/trailing whitespace must NOT be
    // silently accepted (that would be a behavior change vs. main, which
    // also does not trim).
    auto method = parse_auth_method(" trust");
    ASSERT_FALSE(method.has_value());
    EXPECT_EQ(method.error().code, StatusCode::INVALID_ARGUMENT);

    auto method2 = parse_auth_method("trust ");
    ASSERT_FALSE(method2.has_value());
    EXPECT_EQ(method2.error().code, StatusCode::INVALID_ARGUMENT);
}

// =============================================================================
// 2. EMBEDDING named-parameter parsing (parser.cpp)
// =============================================================================

namespace {

StmtPtr parse_one_stmt(std::string_view sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    EXPECT_TRUE(tokens.has_value()) << (tokens ? "" : tokens.error().message);
    if (!tokens)
        return nullptr;
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_TRUE(stmts.has_value()) << (stmts ? "" : stmts.error().message);
    if (!stmts || stmts->size() != 1)
        return nullptr;
    return std::move((*stmts)[0]);
}

} // namespace

TEST(QA_GDB1221_Parser, EmbeddingNamedParamsAllCaseVariants) {
    for (const std::string& source_key : {"source", "SOURCE", "Source", "SoUrCe"}) {
        for (const std::string& provider_key : {"provider", "PROVIDER", "Provider"}) {
            std::string sql = "CREATE TABLE t (id INT, vec EMBEDDING(384, " + source_key +
                              "='title', " + provider_key + "='openai'))";
            auto stmt = parse_one_stmt(sql);
            ASSERT_NE(stmt, nullptr) << "sql: " << sql;
            auto* ct = dynamic_cast<CreateTableStmt*>(stmt.get());
            ASSERT_NE(ct, nullptr);
            ASSERT_EQ(ct->columns.size(), 2u);
            EXPECT_EQ(ct->columns[1].type.source, "title") << "sql: " << sql;
            EXPECT_EQ(ct->columns[1].type.provider, "openai") << "sql: " << sql;
        }
    }
}

TEST(QA_GDB1221_Parser, EmbeddingUnknownNamedParamStillErrors) {
    Lexer lexer("CREATE TABLE t (id INT, vec EMBEDDING(384, BOGUS='x', PROVIDER='openai'))");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_FALSE(stmts.has_value());
}

TEST(QA_GDB1221_Parser, EmbeddingDuplicateSourceParamStillErrors) {
    Lexer lexer(
        "CREATE TABLE t (id INT, vec EMBEDDING(384, SOURCE='a', Source='b'))");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens.has_value());
    Parser parser(std::move(*tokens));
    auto stmts = parser.parse_all();
    EXPECT_FALSE(stmts.has_value());
}

// =============================================================================
// 3. CREATE INDEX USING <method> normalization (query_engine.cpp)
// =============================================================================

class QA_GDB1221_CreateIndex : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_1221_index";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
        auto r = engine_->execute("CREATE TABLE users (id INT, email VARCHAR, age INT)");
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

TEST_F(QA_GDB1221_CreateIndex, UsingMethodAllCaseVariantsAccepted) {
    int i = 0;
    for (const std::string& method : {"hash", "HASH", "Hash", "hAsH"}) {
        std::string idx_name = "idx_case_" + std::to_string(i++);
        std::string sql = "CREATE INDEX " + idx_name + " ON users (email) USING " + method;
        auto r = engine_->execute(sql);
        ASSERT_TRUE(r.has_value()) << "sql: " << sql << " err: "
                                    << (r ? "" : r.error().message);
    }
}

// NOTE: execute_create_index does not validate stmt.method against an
// allowlist on main either -- any method name is accepted and stored
// verbatim (lowercased) as index_type. This is pre-existing, unrelated to
// GDB-1221's dedup. The QA-relevant claim here is narrower: unknown methods
// are normalized to lowercase identically regardless of case, matching main.
TEST_F(QA_GDB1221_CreateIndex, UnknownMethodNormalizedToLowercaseRegardlessOfCase) {
    auto r1 = engine_->execute("CREATE INDEX idx_bogus_1 ON users (email) USING NotARealMethod");
    ASSERT_TRUE(r1.has_value()) << (r1 ? "" : r1.error().message);

    auto r2 = engine_->execute("CREATE INDEX idx_bogus_2 ON users (email) USING notarealmethod");
    ASSERT_TRUE(r2.has_value()) << (r2 ? "" : r2.error().message);

    auto idx1 = catalog_.get_index(default_database_id, "idx_bogus_1");
    auto idx2 = catalog_.get_index(default_database_id, "idx_bogus_2");
    ASSERT_TRUE(idx1.has_value());
    ASSERT_TRUE(idx2.has_value());
    EXPECT_EQ(idx1->index_type, "notarealmethod");
    EXPECT_EQ(idx2->index_type, "notarealmethod");
}

// =============================================================================
// 4. parse_isolation_level — correctly left inline; must remain intact
// =============================================================================

TEST(QA_GDB1221_IsolationLevel, DoubledInternalWhitespaceStillNormalizes) {
    // "Read  Committed" (two spaces) exercises the whitespace-collapse loop
    // that lives alongside the case-fold in parse_isolation_level. This proves
    // the site correctly left inline (not migrated to to_lower) still works.
    auto level = QueryEngine::parse_isolation_level("Read  Committed");
    ASSERT_TRUE(level.has_value()) << level.error().message;
    EXPECT_EQ(*level, IsolationLevel::READ_COMMITTED);
}

TEST(QA_GDB1221_IsolationLevel, LeadingTrailingWhitespaceTrimmed) {
    auto level = QueryEngine::parse_isolation_level("   serializable   ");
    ASSERT_TRUE(level.has_value()) << level.error().message;
    EXPECT_EQ(*level, IsolationLevel::SERIALIZABLE);
}

TEST(QA_GDB1221_IsolationLevel, MixedCaseWithTabsAndSpacesNormalizes) {
    auto level = QueryEngine::parse_isolation_level("\tSnapshot   \tIsolation\t");
    ASSERT_TRUE(level.has_value()) << level.error().message;
    EXPECT_EQ(*level, IsolationLevel::SNAPSHOT_ISOLATION);
}

TEST(QA_GDB1221_IsolationLevel, RepeatableReadAliasStillWorks) {
    auto level = QueryEngine::parse_isolation_level("REPEATABLE   READ");
    ASSERT_TRUE(level.has_value()) << level.error().message;
    EXPECT_EQ(*level, IsolationLevel::SNAPSHOT_ISOLATION);
}

TEST(QA_GDB1221_IsolationLevel, UnknownLevelStillErrors) {
    auto level = QueryEngine::parse_isolation_level("bogus level");
    ASSERT_FALSE(level.has_value());
}

TEST(QA_GDB1221_IsolationLevel, EmptyStringStillErrors) {
    auto level = QueryEngine::parse_isolation_level("");
    ASSERT_FALSE(level.has_value());
}
