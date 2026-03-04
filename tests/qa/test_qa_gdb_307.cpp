#include "giodb/catalog/catalog.h"
#include "giodb/common/result.h"
#include "giodb/common/types.h"
#include "giodb/common/value.h"
#include "giodb/executor/query_engine.h"
#include "giodb/executor/storage_manager.h"
#include "giodb/graph/graph_engine.h"
#include "giodb/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace giodb {
namespace {

// ============================================================================
// QA Fixture for GDB-307: LINK/UNLINK UUID vertex references fail with
// "cannot compare INT64 with STRING"
//
// Tests verify that LINK and UNLINK correctly coerce key values to the PK type
// of the source and target tables. Covers UUID, STRING, INT, heterogeneous PK
// types, invalid inputs, edge cases, and stress scenarios.
// ============================================================================

class QA_GDB307_LinkKeyCoercion : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "giodb_qa_gdb307";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());
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
            EXPECT_EQ(result.error().code, expected);
        }
    }

    /// Extract an integer from a Value that may be INT32 or INT64.
    static int64_t val_to_int64(const Value& v) {
        if (v.type_id() == TypeId::INT32)
            return v.as_int32();
        return v.as_int64();
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
};

// ============================================================================
// Acceptance Criteria Verification
// ============================================================================

// AC1: LINK with UUID PK succeeds (STRING→UUID coercion).
TEST_F(QA_GDB307_LinkKeyCoercion, LinkWithUuidPkSucceeds) {
    exec_ok("CREATE TABLE people (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES "
            "('6f2fff6c-9762-4191-86e1-d34597e3c75a', 'Alice')");
    exec_ok("INSERT INTO people VALUES "
            "('d1458b55-f0bf-44d4-b191-e52f1ef1f60a', 'Bob')");
    exec_ok("CREATE EDGE TYPE friends FROM people TO people");

    auto link = exec_ok("LINK people('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
                        "TO people('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA friends");
    EXPECT_EQ(link.affected_rows, 1);
    EXPECT_EQ(link.message, "LINK");
}

// AC2: UNLINK with UUID PK succeeds.
TEST_F(QA_GDB307_LinkKeyCoercion, UnlinkWithUuidPkSucceeds) {
    exec_ok("CREATE TABLE people (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES "
            "('6f2fff6c-9762-4191-86e1-d34597e3c75a', 'Alice')");
    exec_ok("INSERT INTO people VALUES "
            "('d1458b55-f0bf-44d4-b191-e52f1ef1f60a', 'Bob')");
    exec_ok("CREATE EDGE TYPE friends FROM people TO people");
    exec_ok("LINK people('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
            "TO people('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA friends");

    auto unlink = exec_ok("UNLINK people('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
                          "FROM people('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA friends");
    EXPECT_EQ(unlink.affected_rows, 1);
    EXPECT_EQ(unlink.message, "UNLINK");
}

// AC3: Invalid UUID produces TYPE_ERROR.
TEST_F(QA_GDB307_LinkKeyCoercion, InvalidUuidProducesTypeError) {
    exec_ok("CREATE TABLE nodes (id UUID PRIMARY KEY, label VARCHAR)");
    exec_ok("CREATE EDGE TYPE connects FROM nodes TO nodes");

    // Completely invalid UUID string.
    exec_error("LINK nodes('not-a-uuid') TO nodes('also-bad') VIA connects",
               StatusCode::TYPE_ERROR);
}

// AC4: STRING PK works (non-UUID non-integer PKs).
TEST_F(QA_GDB307_LinkKeyCoercion, LinkWithStringPkSucceeds) {
    exec_ok("CREATE TABLE tags (slug VARCHAR PRIMARY KEY, label VARCHAR)");
    exec_ok("INSERT INTO tags VALUES ('cpp', 'C++')");
    exec_ok("INSERT INTO tags VALUES ('db', 'Database')");
    exec_ok("CREATE EDGE TYPE related FROM tags TO tags");

    auto link = exec_ok("LINK tags('cpp') TO tags('db') VIA related");
    EXPECT_EQ(link.affected_rows, 1);
}

// AC5: Heterogeneous source/target PK types.
TEST_F(QA_GDB307_LinkKeyCoercion, HeterogeneousPkTypesUuidAndInt) {
    exec_ok("CREATE TABLE authors (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE TABLE articles (id INT PRIMARY KEY, title VARCHAR)");
    exec_ok("INSERT INTO authors VALUES "
            "('aabbccdd-1122-3344-5566-778899aabbcc', 'Eve')");
    exec_ok("INSERT INTO articles VALUES (42, 'GioDB Internals')");
    exec_ok("CREATE EDGE TYPE wrote FROM authors TO articles");

    auto link = exec_ok("LINK authors('aabbccdd-1122-3344-5566-778899aabbcc') "
                        "TO articles(42) VIA wrote");
    EXPECT_EQ(link.affected_rows, 1);

    auto unlink = exec_ok("UNLINK authors('aabbccdd-1122-3344-5566-778899aabbcc') "
                          "FROM articles(42) VIA wrote");
    EXPECT_EQ(unlink.affected_rows, 1);
}

// ============================================================================
// Adversarial: Boundary Values and Edge Cases
// ============================================================================

// UUID boundary: all-zeros UUID.
TEST_F(QA_GDB307_LinkKeyCoercion, LinkWithAllZerosUuid) {
    exec_ok("CREATE TABLE items (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO items VALUES ('00000000-0000-0000-0000-000000000000', 'Zero')");
    exec_ok("INSERT INTO items VALUES ('ffffffff-ffff-ffff-ffff-ffffffffffff', 'Max')");
    exec_ok("CREATE EDGE TYPE refs FROM items TO items");

    auto link = exec_ok("LINK items('00000000-0000-0000-0000-000000000000') "
                        "TO items('ffffffff-ffff-ffff-ffff-ffffffffffff') VIA refs");
    EXPECT_EQ(link.affected_rows, 1);
}

// UUID boundary: all-fs UUID (max UUID value).
TEST_F(QA_GDB307_LinkKeyCoercion, LinkWithAllFsUuid) {
    exec_ok("CREATE TABLE items (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO items VALUES ('ffffffff-ffff-ffff-ffff-ffffffffffff', 'Max')");
    exec_ok("INSERT INTO items VALUES ('00000000-0000-0000-0000-000000000001', 'One')");
    exec_ok("CREATE EDGE TYPE refs FROM items TO items");

    auto link = exec_ok("LINK items('ffffffff-ffff-ffff-ffff-ffffffffffff') "
                        "TO items('00000000-0000-0000-0000-000000000001') VIA refs");
    EXPECT_EQ(link.affected_rows, 1);
}

// Self-referencing LINK (source == target UUID).
TEST_F(QA_GDB307_LinkKeyCoercion, SelfReferentialLinkUuid) {
    exec_ok("CREATE TABLE nodes (id UUID PRIMARY KEY, label VARCHAR)");
    exec_ok("INSERT INTO nodes VALUES ('aabbccdd-1122-3344-5566-778899aabbcc', 'Self')");
    exec_ok("CREATE EDGE TYPE loops FROM nodes TO nodes");

    auto link = exec_ok("LINK nodes('aabbccdd-1122-3344-5566-778899aabbcc') "
                        "TO nodes('aabbccdd-1122-3344-5566-778899aabbcc') VIA loops");
    EXPECT_EQ(link.affected_rows, 1);
}

// ============================================================================
// Adversarial: Invalid UUID Formats
// ============================================================================

// UUID with wrong number of characters.
TEST_F(QA_GDB307_LinkKeyCoercion, InvalidUuidTooShort) {
    exec_ok("CREATE TABLE nodes (id UUID PRIMARY KEY, label VARCHAR)");
    exec_ok("CREATE EDGE TYPE connects FROM nodes TO nodes");

    exec_error("LINK nodes('aabbccdd-1122-3344-5566') "
               "TO nodes('aabbccdd-1122-3344-5566-778899aabbcc') VIA connects",
               StatusCode::TYPE_ERROR);
}

// UUID with invalid characters.
TEST_F(QA_GDB307_LinkKeyCoercion, InvalidUuidBadChars) {
    exec_ok("CREATE TABLE nodes (id UUID PRIMARY KEY, label VARCHAR)");
    exec_ok("CREATE EDGE TYPE connects FROM nodes TO nodes");

    exec_error("LINK nodes('gggggggg-hhhh-iiii-jjjj-kkkkkkkkkkkk') "
               "TO nodes('aabbccdd-1122-3344-5566-778899aabbcc') VIA connects",
               StatusCode::TYPE_ERROR);
}

// Empty string as UUID.
TEST_F(QA_GDB307_LinkKeyCoercion, EmptyStringAsUuid) {
    exec_ok("CREATE TABLE nodes (id UUID PRIMARY KEY, label VARCHAR)");
    exec_ok("CREATE EDGE TYPE connects FROM nodes TO nodes");

    exec_error("LINK nodes('') TO nodes('') VIA connects", StatusCode::TYPE_ERROR);
}

// Only source key is invalid UUID, target is valid.
TEST_F(QA_GDB307_LinkKeyCoercion, InvalidSourceKeyOnlyUuid) {
    exec_ok("CREATE TABLE nodes (id UUID PRIMARY KEY, label VARCHAR)");
    exec_ok("INSERT INTO nodes VALUES ('aabbccdd-1122-3344-5566-778899aabbcc', 'Valid')");
    exec_ok("CREATE EDGE TYPE connects FROM nodes TO nodes");

    exec_error("LINK nodes('bad-uuid') "
               "TO nodes('aabbccdd-1122-3344-5566-778899aabbcc') VIA connects",
               StatusCode::TYPE_ERROR);
}

// Only target key is invalid UUID, source is valid.
TEST_F(QA_GDB307_LinkKeyCoercion, InvalidTargetKeyOnlyUuid) {
    exec_ok("CREATE TABLE nodes (id UUID PRIMARY KEY, label VARCHAR)");
    exec_ok("INSERT INTO nodes VALUES ('aabbccdd-1122-3344-5566-778899aabbcc', 'Valid')");
    exec_ok("CREATE EDGE TYPE connects FROM nodes TO nodes");

    exec_error("LINK nodes('aabbccdd-1122-3344-5566-778899aabbcc') "
               "TO nodes('bad-uuid') VIA connects",
               StatusCode::TYPE_ERROR);
}

// ============================================================================
// Adversarial: Type Mismatch — Non-coercible types to UUID
// ============================================================================

// INT literal to UUID PK should fail — cannot coerce INT to UUID.
TEST_F(QA_GDB307_LinkKeyCoercion, IntLiteralToUuidPkFails) {
    exec_ok("CREATE TABLE nodes (id UUID PRIMARY KEY, label VARCHAR)");
    exec_ok("CREATE EDGE TYPE connects FROM nodes TO nodes");

    exec_error("LINK nodes(42) TO nodes(99) VIA connects", StatusCode::TYPE_ERROR);
}

// FLOAT literal to UUID PK should fail.
TEST_F(QA_GDB307_LinkKeyCoercion, FloatLiteralToUuidPkFails) {
    exec_ok("CREATE TABLE nodes (id UUID PRIMARY KEY, label VARCHAR)");
    exec_ok("CREATE EDGE TYPE connects FROM nodes TO nodes");

    exec_error("LINK nodes(3.14) TO nodes(2.71) VIA connects", StatusCode::TYPE_ERROR);
}

// BOOL literal to UUID PK should fail.
TEST_F(QA_GDB307_LinkKeyCoercion, BoolLiteralToUuidPkFails) {
    exec_ok("CREATE TABLE nodes (id UUID PRIMARY KEY, label VARCHAR)");
    exec_ok("CREATE EDGE TYPE connects FROM nodes TO nodes");

    exec_error("LINK nodes(TRUE) TO nodes(FALSE) VIA connects", StatusCode::TYPE_ERROR);
}

// ============================================================================
// Adversarial: PK Declaration Styles
// ============================================================================

// Inline PK (column-level) — the specific path fixed by GDB-307.
TEST_F(QA_GDB307_LinkKeyCoercion, InlinePkDeclarationCoerces) {
    exec_ok("CREATE TABLE a (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE TABLE b (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO a VALUES ('11111111-1111-1111-1111-111111111111', 'A')");
    exec_ok("INSERT INTO b VALUES ('22222222-2222-2222-2222-222222222222', 'B')");
    exec_ok("CREATE EDGE TYPE links FROM a TO b");

    auto link = exec_ok("LINK a('11111111-1111-1111-1111-111111111111') "
                        "TO b('22222222-2222-2222-2222-222222222222') VIA links");
    EXPECT_EQ(link.affected_rows, 1);
}

// Table-level PK constraint — must also coerce.
TEST_F(QA_GDB307_LinkKeyCoercion, TableLevelPkConstraintCoerces) {
    exec_ok("CREATE TABLE a (id UUID, name VARCHAR, PRIMARY KEY (id))");
    exec_ok("CREATE TABLE b (id UUID, name VARCHAR, PRIMARY KEY (id))");
    exec_ok("INSERT INTO a VALUES ('11111111-1111-1111-1111-111111111111', 'A')");
    exec_ok("INSERT INTO b VALUES ('22222222-2222-2222-2222-222222222222', 'B')");
    exec_ok("CREATE EDGE TYPE links FROM a TO b");

    auto link = exec_ok("LINK a('11111111-1111-1111-1111-111111111111') "
                        "TO b('22222222-2222-2222-2222-222222222222') VIA links");
    EXPECT_EQ(link.affected_rows, 1);
}

// ============================================================================
// Adversarial: INT PK coercion from string literal
// ============================================================================

// String literal '42' to INT PK — fit_to_storage should narrow.
TEST_F(QA_GDB307_LinkKeyCoercion, StringLiteralToIntPkFails) {
    // String to INT is not a valid coercion — cannot coerce STRING to INT.
    exec_ok("CREATE TABLE items (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE EDGE TYPE refs FROM items TO items");

    // fit_to_storage cannot coerce STRING → INT, so this should fail.
    exec_error("LINK items('42') TO items('99') VIA refs", StatusCode::TYPE_ERROR);
}

// ============================================================================
// Adversarial: Non-existent Edge Type
// ============================================================================

TEST_F(QA_GDB307_LinkKeyCoercion, LinkNonExistentEdgeType) {
    exec_ok("CREATE TABLE nodes (id UUID PRIMARY KEY, label VARCHAR)");
    exec_ok("INSERT INTO nodes VALUES "
            "('aabbccdd-1122-3344-5566-778899aabbcc', 'N1')");

    exec_error("LINK nodes('aabbccdd-1122-3344-5566-778899aabbcc') "
               "TO nodes('aabbccdd-1122-3344-5566-778899aabbcc') VIA ghost",
               StatusCode::NOT_FOUND);
}

// ============================================================================
// Adversarial: LINK-UNLINK-LINK round-trip with UUID
// ============================================================================

TEST_F(QA_GDB307_LinkKeyCoercion, LinkUnlinkRelinkUuid) {
    exec_ok("CREATE TABLE people (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES "
            "('6f2fff6c-9762-4191-86e1-d34597e3c75a', 'Alice')");
    exec_ok("INSERT INTO people VALUES "
            "('d1458b55-f0bf-44d4-b191-e52f1ef1f60a', 'Bob')");
    exec_ok("CREATE EDGE TYPE follows FROM people TO people");

    // LINK
    auto r1 = exec_ok("LINK people('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
                      "TO people('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA follows");
    EXPECT_EQ(r1.affected_rows, 1);

    // UNLINK
    auto r2 = exec_ok("UNLINK people('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
                      "FROM people('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA follows");
    EXPECT_EQ(r2.affected_rows, 1);

    // Re-LINK
    auto r3 = exec_ok("LINK people('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
                      "TO people('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA follows");
    EXPECT_EQ(r3.affected_rows, 1);
}

// ============================================================================
// Adversarial: Stress — Many LINK/UNLINK with UUID PKs
// ============================================================================

TEST_F(QA_GDB307_LinkKeyCoercion, StressManyUuidLinks) {
    exec_ok("CREATE TABLE people (id UUID PRIMARY KEY, name VARCHAR)");

    // Insert 20 nodes with deterministic UUIDs.
    std::vector<std::string> uuids;
    for (int i = 0; i < 20; ++i) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%08x-0000-0000-0000-%012x", i, i);
        uuids.emplace_back(buf);
        exec_ok("INSERT INTO people VALUES ('" + uuids.back() + "', 'Node" + std::to_string(i) +
                "')");
    }

    exec_ok("CREATE EDGE TYPE knows FROM people TO people");

    // Create a chain of edges: 0→1, 1→2, ..., 18→19.
    for (int i = 0; i < 19; ++i) {
        auto r =
            exec_ok("LINK people('" + uuids[i] + "') TO people('" + uuids[i + 1] + "') VIA knows");
        EXPECT_EQ(r.affected_rows, 1) << "LINK failed at i=" << i;
    }

    // Unlink every other edge: 0→1, 2→3, 4→5, ...
    for (int i = 0; i < 19; i += 2) {
        auto r = exec_ok("UNLINK people('" + uuids[i] + "') FROM people('" + uuids[i + 1] +
                         "') VIA knows");
        EXPECT_EQ(r.affected_rows, 1) << "UNLINK failed at i=" << i;
    }
}

// ============================================================================
// Adversarial: TRAVERSE after UUID LINK — full round-trip verification
// ============================================================================

TEST_F(QA_GDB307_LinkKeyCoercion, TraverseAfterUuidLink) {
    exec_ok("CREATE TABLE people (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES "
            "('6f2fff6c-9762-4191-86e1-d34597e3c75a', 'Alice')");
    exec_ok("INSERT INTO people VALUES "
            "('d1458b55-f0bf-44d4-b191-e52f1ef1f60a', 'Bob')");
    exec_ok("INSERT INTO people VALUES "
            "('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee', 'Charlie')");
    exec_ok("CREATE EDGE TYPE follows FROM people TO people");

    exec_ok("LINK people('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
            "TO people('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') VIA follows");
    exec_ok("LINK people('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
            "TO people('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee') VIA follows");

    // TRAVERSE OUT from Alice should find Bob and Charlie.
    auto qr = exec_ok("SELECT name, __depth "
                      "FROM TRAVERSE follows "
                      "FROM people('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
                      "DIRECTION OUT");
    ASSERT_EQ(qr.rows.size(), 2u);

    std::vector<std::string> names;
    for (const auto& row : qr.rows) {
        names.push_back(row[0].as_string());
        EXPECT_EQ(val_to_int64(row[1]), 1) << "depth should be 1 for direct neighbors";
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "Bob");
    EXPECT_EQ(names[1], "Charlie");
}

// ============================================================================
// Adversarial: Uppercase UUID strings
// ============================================================================

TEST_F(QA_GDB307_LinkKeyCoercion, UppercaseUuidStringCoercion) {
    exec_ok("CREATE TABLE items (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO items VALUES "
            "('AABBCCDD-1122-3344-5566-778899AABBCC', 'Upper')");
    exec_ok("INSERT INTO items VALUES "
            "('11223344-5566-7788-99aa-bbccddeeff00', 'Lower')");
    exec_ok("CREATE EDGE TYPE refs FROM items TO items");

    auto link = exec_ok("LINK items('AABBCCDD-1122-3344-5566-778899AABBCC') "
                        "TO items('11223344-5566-7788-99aa-bbccddeeff00') VIA refs");
    EXPECT_EQ(link.affected_rows, 1);
}

// ============================================================================
// Adversarial: LINK with edge properties and UUID keys
// ============================================================================

TEST_F(QA_GDB307_LinkKeyCoercion, LinkWithPropertiesAndUuidKeys) {
    exec_ok("CREATE TABLE people (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO people VALUES "
            "('6f2fff6c-9762-4191-86e1-d34597e3c75a', 'Alice')");
    exec_ok("INSERT INTO people VALUES "
            "('d1458b55-f0bf-44d4-b191-e52f1ef1f60a', 'Bob')");
    exec_ok("CREATE EDGE TYPE follows FROM people TO people PROPERTIES (weight FLOAT, since INT)");

    auto link = exec_ok("LINK people('6f2fff6c-9762-4191-86e1-d34597e3c75a') "
                        "TO people('d1458b55-f0bf-44d4-b191-e52f1ef1f60a') "
                        "VIA follows SET weight = 0.95, since = 2024");
    EXPECT_EQ(link.affected_rows, 1);
}

// ============================================================================
// Adversarial: Reversed heterogeneous — INT source, UUID target
// ============================================================================

TEST_F(QA_GDB307_LinkKeyCoercion, IntSourceUuidTarget) {
    exec_ok("CREATE TABLE departments (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("CREATE TABLE employees (id UUID PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO departments VALUES (1, 'Engineering')");
    exec_ok("INSERT INTO employees VALUES "
            "('aabbccdd-1122-3344-5566-778899aabbcc', 'Eve')");
    exec_ok("CREATE EDGE TYPE has_member FROM departments TO employees");

    auto link = exec_ok("LINK departments(1) "
                        "TO employees('aabbccdd-1122-3344-5566-778899aabbcc') VIA has_member");
    EXPECT_EQ(link.affected_rows, 1);

    auto unlink = exec_ok("UNLINK departments(1) "
                          "FROM employees('aabbccdd-1122-3344-5566-778899aabbcc') VIA has_member");
    EXPECT_EQ(unlink.affected_rows, 1);
}

// ============================================================================
// Adversarial: UUID with mixed case in LINK and UNLINK
// ============================================================================

TEST_F(QA_GDB307_LinkKeyCoercion, MixedCaseUuidLinkAndUnlink) {
    exec_ok("CREATE TABLE items (id UUID PRIMARY KEY, name VARCHAR)");
    // Insert with lowercase.
    exec_ok("INSERT INTO items VALUES "
            "('aabbccdd-1122-3344-5566-778899aabbcc', 'A')");
    exec_ok("INSERT INTO items VALUES "
            "('11223344-5566-7788-99aa-bbccddeeff00', 'B')");
    exec_ok("CREATE EDGE TYPE refs FROM items TO items");

    // LINK with uppercase UUID — should coerce and match.
    auto link = exec_ok("LINK items('AABBCCDD-1122-3344-5566-778899AABBCC') "
                        "TO items('11223344-5566-7788-99AA-BBCCDDEEFF00') VIA refs");
    EXPECT_EQ(link.affected_rows, 1);

    // UNLINK with lowercase UUID — should find and remove the edge.
    auto unlink = exec_ok("UNLINK items('aabbccdd-1122-3344-5566-778899aabbcc') "
                          "FROM items('11223344-5566-7788-99aa-bbccddeeff00') VIA refs");
    EXPECT_EQ(unlink.affected_rows, 1);
}

// ============================================================================
// Adversarial: LINK with INT64 narrowing — INT64 literal to INT32 PK
// ============================================================================

TEST_F(QA_GDB307_LinkKeyCoercion, Int64LiteralToInt32PkCoerces) {
    exec_ok("CREATE TABLE items (id INT PRIMARY KEY, name VARCHAR)");
    exec_ok("INSERT INTO items VALUES (1, 'One')");
    exec_ok("INSERT INTO items VALUES (2, 'Two')");
    exec_ok("CREATE EDGE TYPE refs FROM items TO items");

    // 1 and 2 parse as INT64 literals; coercion should narrow to INT32.
    auto link = exec_ok("LINK items(1) TO items(2) VIA refs");
    EXPECT_EQ(link.affected_rows, 1);
}

} // namespace
} // namespace giodb
