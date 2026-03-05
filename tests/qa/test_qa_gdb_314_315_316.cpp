#include "sixseven/catalog/catalog.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace sixseven {
namespace {

// ============================================================================
// QA tests for GDB-314, GDB-315, GDB-316
//
// GDB-314: verify_pk_exists PK column search guard (pk_found flag)
// GDB-315: UNLINK PK existence validation (symmetric with LINK)
// GDB-316: clang-format violations in parser.cpp, enriched_traversal.cpp,
//          test_qa_gdb_257.cpp
// ============================================================================

class QA_GDB314_315_316 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb314_315_316";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());

        // Base schema: two tables with edges between them.
        exec_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR)");
        exec_ok("INSERT INTO users VALUES (1, 'Alice')");
        exec_ok("INSERT INTO users VALUES (2, 'Bob')");
        exec_ok("INSERT INTO users VALUES (3, 'Charlie')");
        exec_ok("CREATE EDGE TYPE follows FROM users TO users");
        exec_ok("LINK users(1) TO users(2) VIA follows");
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (!result.has_value()) {
            ADD_FAILURE() << sql << ": " << result.error().message;
            return {};
        }
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << sql << " should have failed";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected)
                << "Expected " << static_cast<int>(expected) << " but got "
                << static_cast<int>(result.error().code) << ": " << result.error().message;
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// GDB-314: verify_pk_exists now returns INTERNAL_ERROR when PK column missing
// ============================================================================

// The PK guard is not directly testable via SQL since the catalog always
// ensures PK column consistency. We verify the positive path works correctly
// to confirm the pk_found logic doesn't break normal PK lookups.

TEST_F(QA_GDB314_315_316, GDB314_LinkWithValidPKStillWorks) {
    // Verify the PK guard doesn't break the happy path.
    exec_ok("LINK users(2) TO users(3) VIA follows");
}

TEST_F(QA_GDB314_315_316, GDB314_LinkWithINT64PKTableWorks) {
    // BIGINT PK — confirm pk_found guard works for different PK types.
    exec_ok("CREATE TABLE events (id BIGINT PRIMARY KEY, label VARCHAR)");
    exec_ok("INSERT INTO events VALUES (100, 'launch')");
    exec_ok("INSERT INTO events VALUES (200, 'shutdown')");
    exec_ok("CREATE EDGE TYPE precedes FROM events TO events");
    exec_ok("LINK events(100) TO events(200) VIA precedes");
}

TEST_F(QA_GDB314_315_316, GDB314_LinkWithStringPKTableWorks) {
    // VARCHAR PK — confirm pk_found guard works for string PKs.
    exec_ok("CREATE TABLE codes (code VARCHAR PRIMARY KEY, label VARCHAR)");
    exec_ok("INSERT INTO codes VALUES ('A1', 'alpha')");
    exec_ok("INSERT INTO codes VALUES ('B2', 'bravo')");
    exec_ok("CREATE EDGE TYPE maps_to FROM codes TO codes");
    exec_ok("LINK codes('A1') TO codes('B2') VIA maps_to");
}

TEST_F(QA_GDB314_315_316, GDB314_LinkWithUuidPKTableWorks) {
    // UUID PK — confirm pk_found guard works with UUID columns.
    exec_ok("CREATE TABLE items (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO items VALUES "
            "('6f2fff6c-9762-4191-86e1-d34597e3c75a', 'item1')");
    exec_ok("INSERT INTO items VALUES "
            "('d1458b55-f0bf-44d4-b191-e52f1ef1f60a', 'item2')");
    exec_ok("CREATE EDGE TYPE relates FROM items TO items");
    exec_ok("LINK items('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
            "TO items('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA relates");
}

TEST_F(QA_GDB314_315_316, GDB314_UnlinkWithValidPKAfterGuardWorks) {
    // Both verify_pk_exists guard (GDB-314) and UNLINK validation (GDB-315)
    // cooperate: unlink a valid edge after the guard is in place.
    exec_ok("UNLINK users(1) FROM users(2) VIA follows");
}

// ============================================================================
// GDB-315: UNLINK PK existence validation
// ============================================================================

// -- Non-existent source/target --

TEST_F(QA_GDB314_315_316, GDB315_UnlinkNonExistentSourceFails) {
    exec_error("UNLINK users(999) FROM users(2) VIA follows", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB314_315_316, GDB315_UnlinkNonExistentTargetFails) {
    exec_error("UNLINK users(1) FROM users(999) VIA follows", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB314_315_316, GDB315_UnlinkBothNonExistentFails) {
    exec_error("UNLINK users(888) FROM users(999) VIA follows", StatusCode::NOT_FOUND);
}

// -- Boundary PK values --

TEST_F(QA_GDB314_315_316, GDB315_UnlinkNegativePKFails) {
    exec_error("UNLINK users(-1) FROM users(1) VIA follows", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB314_315_316, GDB315_UnlinkZeroPKFails) {
    // PK 0 doesn't exist in our test data.
    exec_error("UNLINK users(0) FROM users(1) VIA follows", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB314_315_316, GDB315_UnlinkLargePKFails) {
    exec_error("UNLINK users(2147483647) FROM users(1) VIA follows", StatusCode::NOT_FOUND);
}

// -- UNLINK valid edge that exists --

TEST_F(QA_GDB314_315_316, GDB315_UnlinkExistingEdgeSucceeds) {
    // Edge 1→2 was created in SetUp.
    exec_ok("UNLINK users(1) FROM users(2) VIA follows");
}

// -- UNLINK valid PKs but no edge between them --

TEST_F(QA_GDB314_315_316, GDB315_UnlinkNoEdgeBetweenValidPKs) {
    // Users 1 and 3 both exist but have no edge.
    // PKs exist, so validation passes. The graph engine handles
    // the "no edge" case (this should not crash or return NOT_FOUND
    // since the PKs are valid).
    auto result = engine_->execute("UNLINK users(1) FROM users(3) VIA follows");
    // Should either succeed (idempotent no-op) or fail gracefully.
    // The key assertion is: it must NOT crash or return INTERNAL_ERROR.
    if (!result.has_value()) {
        EXPECT_NE(result.error().code, StatusCode::INTERNAL_ERROR)
            << "UNLINK with valid PKs but no edge should not cause "
               "INTERNAL_ERROR: "
            << result.error().message;
    }
}

// -- UNLINK with UUID PKs --

TEST_F(QA_GDB314_315_316, GDB315_UnlinkUuidNonExistentSourceFails) {
    exec_ok("CREATE TABLE docs (id UUID PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO docs VALUES "
            "('6f2fff6c-9762-4191-86e1-d34597e3c75a', 'Doc1')");
    exec_ok("INSERT INTO docs VALUES "
            "('d1458b55-f0bf-44d4-b191-e52f1ef1f60a', 'Doc2')");
    exec_ok("CREATE EDGE TYPE refs FROM docs TO docs");
    exec_ok("LINK docs('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
            "TO docs('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA refs");

    // Non-existent UUID source.
    exec_error("UNLINK docs('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') "
               "FROM docs('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA refs",
               StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB314_315_316, GDB315_UnlinkUuidNonExistentTargetFails) {
    exec_ok("CREATE TABLE notes (id UUID PRIMARY KEY, body VARCHAR)");
    exec_ok("INSERT INTO notes VALUES "
            "('6f2fff6c-9762-4191-86e1-d34597e3c75a', 'Note1')");
    exec_ok("CREATE EDGE TYPE links FROM notes TO notes");

    exec_error("UNLINK notes('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
               "FROM notes('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') VIA links",
               StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB314_315_316, GDB315_UnlinkUuidValidEdgeSucceeds) {
    exec_ok("CREATE TABLE pages (id UUID PRIMARY KEY, text VARCHAR)");
    exec_ok("INSERT INTO pages VALUES "
            "('6f2fff6c-9762-4191-86e1-d34597e3c75a', 'Page1')");
    exec_ok("INSERT INTO pages VALUES "
            "('d1458b55-f0bf-44d4-b191-e52f1ef1f60a', 'Page2')");
    exec_ok("CREATE EDGE TYPE cites FROM pages TO pages");
    exec_ok("LINK pages('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
            "TO pages('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA cites");
    exec_ok("UNLINK pages('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
            "FROM pages('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA cites");
}

// -- UNLINK symmetry with LINK --

TEST_F(QA_GDB314_315_316, GDB315_LinkAndUnlinkSymmetricErrors) {
    // Verify LINK and UNLINK produce same error for the same bad PK.
    auto link_result = engine_->execute("LINK users(999) TO users(1) VIA follows");
    ASSERT_FALSE(link_result.has_value());

    auto unlink_result = engine_->execute("UNLINK users(999) FROM users(1) VIA follows");
    ASSERT_FALSE(unlink_result.has_value());

    // Both should return NOT_FOUND.
    EXPECT_EQ(link_result.error().code, StatusCode::NOT_FOUND);
    EXPECT_EQ(unlink_result.error().code, StatusCode::NOT_FOUND);
}

// -- Cross-table LINK and UNLINK --

TEST_F(QA_GDB314_315_316, GDB315_CrossTableUnlinkValidation) {
    exec_ok("CREATE TABLE products (pid INT PRIMARY KEY, pname VARCHAR)");
    exec_ok("INSERT INTO products VALUES (10, 'Widget')");
    exec_ok("CREATE EDGE TYPE purchased FROM users TO products");
    exec_ok("LINK users(1) TO products(10) VIA purchased");

    // Source exists, target doesn't.
    exec_error("UNLINK users(1) FROM products(999) VIA purchased", StatusCode::NOT_FOUND);

    // Source doesn't exist, target exists.
    exec_error("UNLINK users(999) FROM products(10) VIA purchased", StatusCode::NOT_FOUND);

    // Both exist — should succeed.
    exec_ok("UNLINK users(1) FROM products(10) VIA purchased");
}

// -- String PK UNLINK validation --

TEST_F(QA_GDB314_315_316, GDB315_StringPKUnlinkNonExistentFails) {
    exec_ok("CREATE TABLE tags (tag VARCHAR PRIMARY KEY, label VARCHAR)");
    exec_ok("INSERT INTO tags VALUES ('cpp', 'C++ language')");
    exec_ok("INSERT INTO tags VALUES ('rust', 'Rust language')");
    exec_ok("CREATE EDGE TYPE related FROM tags TO tags");
    exec_ok("LINK tags('cpp') TO tags('rust') VIA related");

    exec_error("UNLINK tags('python') FROM tags('rust') VIA related", StatusCode::NOT_FOUND);

    exec_error("UNLINK tags('cpp') FROM tags('java') VIA related", StatusCode::NOT_FOUND);
}

// -- UNLINK with invalid edge type --

TEST_F(QA_GDB314_315_316, GDB315_UnlinkInvalidEdgeType) {
    auto result = engine_->execute("UNLINK users(1) FROM users(2) VIA nonexistent");
    EXPECT_FALSE(result.has_value());
    // Should fail before PK validation even happens.
}

// -- UNLINK after DELETE of source row --

TEST_F(QA_GDB314_315_316, GDB315_UnlinkAfterSourceDeleted) {
    // Create edge, then delete the source row, then try to unlink.
    exec_ok("LINK users(1) TO users(3) VIA follows");
    exec_ok("DELETE FROM users WHERE id = 1");

    // Now source PK 1 doesn't exist — UNLINK should fail with NOT_FOUND.
    exec_error("UNLINK users(1) FROM users(3) VIA follows", StatusCode::NOT_FOUND);
}

TEST_F(QA_GDB314_315_316, GDB315_UnlinkAfterTargetDeleted) {
    exec_ok("LINK users(1) TO users(3) VIA follows");
    exec_ok("DELETE FROM users WHERE id = 3");

    // Target PK 3 doesn't exist — UNLINK should fail with NOT_FOUND.
    exec_error("UNLINK users(1) FROM users(3) VIA follows", StatusCode::NOT_FOUND);
}

// ============================================================================
// GDB-316: Formatting is verified by clang-format --dry-run -Werror
// in the build pipeline. These tests confirm the formatted files still
// compile and function correctly.
// ============================================================================

TEST_F(QA_GDB314_315_316, GDB316_ParserEmbeddingTypeStillParses) {
    // parser.cpp was reformatted — verify EMBEDDING type parsing works.
    auto result = engine_->execute("CREATE TABLE emb_test (id INT PRIMARY KEY, "
                                   "vec EMBEDDING(384, source='name', provider='builtin/384'))");
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST_F(QA_GDB314_315_316, GDB316_TraverseAfterFormatFix) {
    // enriched_traversal.cpp was reformatted — verify traversal still works.
    exec_ok("SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH");
}

} // namespace
} // namespace sixseven
