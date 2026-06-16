#include "sixseven/catalog/catalog.h"
#include "sixseven/common/coercion.h"
#include "sixseven/common/types.h"
#include "sixseven/common/uuid.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "test_qa_helpers.h"

using namespace sixseven;

// =============================================================================
// parse_uuid adversarial tests (unit level)
// =============================================================================

// -- Boundary: strings near the valid length --

TEST(QA_GDB281_ParseUuid, ExactlyOneCharShort) {
    // 35 chars — missing the last hex digit.
    auto result = parse_uuid("d1458b55-f0bf-44d4-b191-e52f1ef1f60");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_ParseUuid, ExactlyOneCharLong) {
    // 37 chars — one extra hex digit.
    auto result = parse_uuid("d1458b55-f0bf-44d4-b191-e52f1ef1f60a1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_ParseUuid, SingleCharString) {
    auto result = parse_uuid("x");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_ParseUuid, VeryLongString) {
    // 1000 character string — well beyond valid UUID length.
    std::string long_str(1000, 'a');
    auto result = parse_uuid(long_str);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// -- Boundary: non-hex characters at every nibble position --

TEST(QA_GDB281_ParseUuid, NonHexAtFirstCharacter) {
    auto result = parse_uuid("g1458b55-f0bf-44d4-b191-e52f1ef1f60a");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_ParseUuid, NonHexAtSecondCharacter) {
    // 'z' at position 1 (low nibble of first byte).
    auto result = parse_uuid("dz458b55-f0bf-44d4-b191-e52f1ef1f60a");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_ParseUuid, NonHexAtLastCharacter) {
    // 'z' at position 35 (low nibble of last byte).
    auto result = parse_uuid("d1458b55-f0bf-44d4-b191-e52f1ef1f60z");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_ParseUuid, NonHexAtPositionJustBeforeHyphen) {
    // Position 7 (just before first hyphen at 8). Replace with 'x'.
    auto result = parse_uuid("d1458b5x-f0bf-44d4-b191-e52f1ef1f60a");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_ParseUuid, NonHexAtPositionJustAfterHyphen) {
    // Position 9 (just after first hyphen at 8). Replace with 'x'.
    auto result = parse_uuid("d1458b55-x0bf-44d4-b191-e52f1ef1f60a");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// -- Hyphen position edge cases --

TEST(QA_GDB281_ParseUuid, AllHyphens36Chars) {
    // 36 hyphens — right length but all hyphens.
    std::string all_hyphens(36, '-');
    auto result = parse_uuid(all_hyphens);
    // Hyphens at 8,13,18,23 are correct, but all other positions have '-'
    // which is not a valid hex character.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_ParseUuid, ExtraHyphensReplacingHex) {
    // Replace hex chars with hyphens at non-hyphen positions.
    auto result = parse_uuid("--------f0bf-44d4-b191-e52f1ef1f60a");
    // Position 8 is correctly a hyphen but positions 0-7 are all hyphens.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_ParseUuid, HyphensSwappedWithAdjacentChars) {
    // Hyphens shifted by one: positions 7, 12, 17, 22.
    auto result = parse_uuid("d1458b5-5f0bf-44d4-b191-e52f1ef1f60a");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_ParseUuid, NoHyphensCorrectLength) {
    // 32 hex chars without hyphens = 32 chars, not 36.
    auto result = parse_uuid("d1458b55f0bf44d4b191e52f1ef1f60a");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// -- Whitespace edge cases --

TEST(QA_GDB281_ParseUuid, LeadingWhitespace) {
    auto result = parse_uuid(" d1458b55-f0bf-44d4-b191-e52f1ef1f60a");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_ParseUuid, TrailingWhitespace) {
    auto result = parse_uuid("d1458b55-f0bf-44d4-b191-e52f1ef1f60a ");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_ParseUuid, SpacesInsteadOfHyphens) {
    auto result = parse_uuid("d1458b55 f0bf 44d4 b191 e52f1ef1f60a");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_ParseUuid, TabsInsteadOfHyphens) {
    auto result = parse_uuid("d1458b55\tf0bf\t44d4\tb191\te52f1ef1f60a");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// -- Special UUID values --

TEST(QA_GDB281_ParseUuid, NilUuid) {
    auto result = parse_uuid("00000000-0000-0000-0000-000000000000");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    Uuid expected = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(*result, expected);
}

TEST(QA_GDB281_ParseUuid, MaxUuid) {
    auto result = parse_uuid("ffffffff-ffff-ffff-ffff-ffffffffffff");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    Uuid expected = {0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff,
                     0xff};
    EXPECT_EQ(*result, expected);
}

TEST(QA_GDB281_ParseUuid, RoundTripByteValues) {
    // Parse two different UUIDs and ensure they produce different byte arrays.
    auto r1 = parse_uuid("00000000-0000-0000-0000-000000000001");
    auto r2 = parse_uuid("00000000-0000-0000-0000-000000000002");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_NE(*r1, *r2);
}

TEST(QA_GDB281_ParseUuid, KnownUuidByteMapping) {
    // Verify exact byte-by-byte mapping for a known UUID.
    // UUID: 01020304-0506-0708-090a-0b0c0d0e0f10
    auto result = parse_uuid("01020304-0506-0708-090a-0b0c0d0e0f10");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    Uuid expected = {0x01,
                     0x02,
                     0x03,
                     0x04,
                     0x05,
                     0x06,
                     0x07,
                     0x08,
                     0x09,
                     0x0a,
                     0x0b,
                     0x0c,
                     0x0d,
                     0x0e,
                     0x0f,
                     0x10};
    EXPECT_EQ(*result, expected);
}

// -- Braces / curly brace wrapper (common format, should be rejected) --

TEST(QA_GDB281_ParseUuid, BracedUuidFormat) {
    // Some systems use {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx} — 38 chars.
    auto result = parse_uuid("{d1458b55-f0bf-44d4-b191-e52f1ef1f60a}");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_ParseUuid, UrnPrefixedUuid) {
    // urn:uuid: prefix — should be rejected.
    auto result = parse_uuid("urn:uuid:d1458b55-f0bf-44d4-b191-e52f1ef1f60a");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// -- Null byte and control characters --

TEST(QA_GDB281_ParseUuid, NullByteInString) {
    // String with embedded null byte at position 4.
    std::string s = "d145" + std::string(1, '\0') + "b55-f0bf-44d4-b191-e52f1ef1f60a";
    auto result = parse_uuid(s);
    // Length won't be 36 (string constructor stops at null? no, std::string
    // handles embedded nulls), so either length or content will fail.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// =============================================================================
// can_coerce adversarial tests
// =============================================================================

TEST(QA_GDB281_Coerce, CanCoerceStringToUuid) {
    EXPECT_TRUE(can_coerce(TypeId::STRING, TypeId::UUID));
}

TEST(QA_GDB281_Coerce, CannotCoerceUuidToString) {
    // Reverse direction should NOT be allowed.
    EXPECT_FALSE(can_coerce(TypeId::UUID, TypeId::STRING));
}

TEST(QA_GDB281_Coerce, CannotCoerceIntToUuid) {
    EXPECT_FALSE(can_coerce(TypeId::INT32, TypeId::UUID));
    EXPECT_FALSE(can_coerce(TypeId::INT64, TypeId::UUID));
}

TEST(QA_GDB281_Coerce, CannotCoerceBoolToUuid) {
    EXPECT_FALSE(can_coerce(TypeId::BOOL, TypeId::UUID));
}

TEST(QA_GDB281_Coerce, CannotCoerceBlobToUuid) {
    EXPECT_FALSE(can_coerce(TypeId::BLOB, TypeId::UUID));
}

TEST(QA_GDB281_Coerce, CannotCoerceFloatToUuid) {
    EXPECT_FALSE(can_coerce(TypeId::FLOAT32, TypeId::UUID));
    EXPECT_FALSE(can_coerce(TypeId::FLOAT64, TypeId::UUID));
}

TEST(QA_GDB281_Coerce, CannotCoerceUuidToInt) {
    EXPECT_FALSE(can_coerce(TypeId::UUID, TypeId::INT32));
    EXPECT_FALSE(can_coerce(TypeId::UUID, TypeId::INT64));
}

TEST(QA_GDB281_Coerce, UuidIdentityCoercion) {
    EXPECT_TRUE(can_coerce(TypeId::UUID, TypeId::UUID));
}

// =============================================================================
// coerce() adversarial tests
// =============================================================================

TEST(QA_GDB281_Coerce, CoerceValidStringToUuid) {
    Value v(std::string{"550e8400-e29b-41d4-a716-446655440000"});
    auto result = coerce(v, TypeId::UUID);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->type_id(), TypeId::UUID);
    Uuid expected = {0x55,
                     0x0e,
                     0x84,
                     0x00,
                     0xe2,
                     0x9b,
                     0x41,
                     0xd4,
                     0xa7,
                     0x16,
                     0x44,
                     0x66,
                     0x55,
                     0x44,
                     0x00,
                     0x00};
    EXPECT_EQ(result->as_uuid(), expected);
}

TEST(QA_GDB281_Coerce, CoerceInvalidStringToUuidReturnsError) {
    Value v(std::string{"not-a-uuid"});
    auto result = coerce(v, TypeId::UUID);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_Coerce, CoerceEmptyStringToUuidReturnsError) {
    Value v(std::string{""});
    auto result = coerce(v, TypeId::UUID);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_Coerce, CoerceNullToUuidPreservesNull) {
    Value v;
    auto result = coerce(v, TypeId::UUID);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_null());
}

TEST(QA_GDB281_Coerce, CoerceUuidToUuidIdentity) {
    Uuid uuid = {0x01,
                 0x02,
                 0x03,
                 0x04,
                 0x05,
                 0x06,
                 0x07,
                 0x08,
                 0x09,
                 0x0a,
                 0x0b,
                 0x0c,
                 0x0d,
                 0x0e,
                 0x0f,
                 0x10};
    Value v(uuid);
    auto result = coerce(v, TypeId::UUID);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->as_uuid(), uuid);
}

TEST(QA_GDB281_Coerce, CoerceAlmostValidUuidStringReturnsError) {
    // Valid format but one character too many at the end.
    Value v(std::string{"d1458b55-f0bf-44d4-b191-e52f1ef1f60a0"});
    auto result = coerce(v, TypeId::UUID);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// =============================================================================
// fit_to_storage adversarial tests
// =============================================================================

TEST(QA_GDB281_FitToStorage, ValidStringToUuid) {
    Value v(std::string{"d1458b55-f0bf-44d4-b191-e52f1ef1f60a"});
    auto result = fit_to_storage(v, TypeId::UUID);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->type_id(), TypeId::UUID);
}

TEST(QA_GDB281_FitToStorage, InvalidStringToUuidReturnsError) {
    Value v(std::string{"garbage"});
    auto result = fit_to_storage(v, TypeId::UUID);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_FitToStorage, NullToUuidPreservesNull) {
    Value v;
    auto result = fit_to_storage(v, TypeId::UUID);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_null());
}

TEST(QA_GDB281_FitToStorage, IntToUuidReturnsError) {
    Value v(int32_t{42});
    auto result = fit_to_storage(v, TypeId::UUID);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(QA_GDB281_FitToStorage, UuidToUuidIdentity) {
    Uuid uuid = {0xaa,
                 0xbb,
                 0xcc,
                 0xdd,
                 0xee,
                 0xff,
                 0x00,
                 0x11,
                 0x22,
                 0x33,
                 0x44,
                 0x55,
                 0x66,
                 0x77,
                 0x88,
                 0x99};
    Value v(uuid);
    auto result = fit_to_storage(v, TypeId::UUID);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->as_uuid(), uuid);
}

// =============================================================================
// End-to-end SQL tests (QueryEngine)
// =============================================================================

class QA_GDB281_E2E : public ::testing::Test {
protected:
    void SetUp() override {
        bootstrap_qa_catalog(catalog_);

        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_qa_gdb281";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        if (!result.has_value()) {
            ADD_FAILURE() << "exec_ok failed for: " << sql
                          << "\n  error: " << result.error().message;
            return QueryResult{};
        }
        return std::move(*result);
    }

    void exec_error(const std::string& sql, StatusCode expected) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value()) << "expected error but got success for: " << sql;
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, expected)
                << "expected " << static_cast<int>(expected) << " but got "
                << static_cast<int>(result.error().code) << ": " << result.error().message;
        }
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// -- Acceptance Criterion: INSERT a UUID string literal into a UUID column --

TEST_F(QA_GDB281_E2E, InsertUuidStringLiteralIntoUuidColumn) {
    // The reproducer from the bug report.
    exec_ok("CREATE TABLE users_articles (user_id UUID, article_id INT)");
    auto ins = exec_ok("INSERT INTO users_articles (user_id, article_id) "
                       "VALUES ('d1458b55-f0bf-44d4-b191-e52f1ef1f60a', 1)");
    EXPECT_EQ(ins.affected_rows, 1);

    auto qr = exec_ok("SELECT user_id, article_id FROM users_articles");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].type_id(), TypeId::UUID);
    Uuid expected = {0xd1,
                     0x45,
                     0x8b,
                     0x55,
                     0xf0,
                     0xbf,
                     0x44,
                     0xd4,
                     0xb1,
                     0x91,
                     0xe5,
                     0x2f,
                     0x1e,
                     0xf1,
                     0xf6,
                     0x0a};
    EXPECT_EQ(qr.rows[0][0].as_uuid(), expected);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 1);
}

TEST_F(QA_GDB281_E2E, InsertMultipleUuidStringLiterals) {
    exec_ok("CREATE TABLE items (id UUID, name TEXT)");
    exec_ok("INSERT INTO items (id, name) "
            "VALUES ('00000000-0000-0000-0000-000000000001', 'first')");
    exec_ok("INSERT INTO items (id, name) "
            "VALUES ('00000000-0000-0000-0000-000000000002', 'second')");
    exec_ok("INSERT INTO items (id, name) "
            "VALUES ('ffffffff-ffff-ffff-ffff-ffffffffffff', 'max')");

    auto qr = exec_ok("SELECT id, name FROM items");
    ASSERT_EQ(qr.rows.size(), 3u);

    // Verify first row.
    Uuid id1 = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    EXPECT_EQ(qr.rows[0][0].as_uuid(), id1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "first");

    // Verify max UUID row.
    Uuid id_max = {0xff,
                   0xff,
                   0xff,
                   0xff,
                   0xff,
                   0xff,
                   0xff,
                   0xff,
                   0xff,
                   0xff,
                   0xff,
                   0xff,
                   0xff,
                   0xff,
                   0xff,
                   0xff};
    EXPECT_EQ(qr.rows[2][0].as_uuid(), id_max);
    EXPECT_EQ(qr.rows[2][1].as_string(), "max");
}

TEST_F(QA_GDB281_E2E, InsertNilUuidStringLiteral) {
    exec_ok("CREATE TABLE t (id UUID)");
    exec_ok("INSERT INTO t (id) VALUES ('00000000-0000-0000-0000-000000000000')");

    auto qr = exec_ok("SELECT id FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    Uuid nil = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(qr.rows[0][0].as_uuid(), nil);
}

// -- Error path: invalid UUID string should fail at execution --

TEST_F(QA_GDB281_E2E, InsertInvalidUuidStringFails) {
    exec_ok("CREATE TABLE t (id UUID)");
    exec_error("INSERT INTO t (id) VALUES ('not-a-valid-uuid')", StatusCode::TYPE_ERROR);
}

TEST_F(QA_GDB281_E2E, InsertEmptyStringForUuidFails) {
    exec_ok("CREATE TABLE t (id UUID)");
    exec_error("INSERT INTO t (id) VALUES ('')", StatusCode::TYPE_ERROR);
}

TEST_F(QA_GDB281_E2E, InsertTruncatedUuidStringFails) {
    exec_ok("CREATE TABLE t (id UUID)");
    exec_error("INSERT INTO t (id) VALUES ('d1458b55-f0bf-44d4-b191')", StatusCode::TYPE_ERROR);
}

// -- NULL handling with UUID columns --

TEST_F(QA_GDB281_E2E, InsertNullIntoNullableUuidColumn) {
    exec_ok("CREATE TABLE t (id UUID, name TEXT)");
    exec_ok("INSERT INTO t (id, name) VALUES (NULL, 'test')");

    auto qr = exec_ok("SELECT id, name FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_TRUE(qr.rows[0][0].is_null());
    EXPECT_EQ(qr.rows[0][1].as_string(), "test");
}

// -- Case insensitivity for UUID hex digits --

TEST_F(QA_GDB281_E2E, InsertUppercaseUuidString) {
    exec_ok("CREATE TABLE t (id UUID)");
    exec_ok("INSERT INTO t (id) VALUES ('D1458B55-F0BF-44D4-B191-E52F1EF1F60A')");

    auto qr = exec_ok("SELECT id FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].type_id(), TypeId::UUID);
    // Same byte values as lowercase.
    Uuid expected = {0xd1,
                     0x45,
                     0x8b,
                     0x55,
                     0xf0,
                     0xbf,
                     0x44,
                     0xd4,
                     0xb1,
                     0x91,
                     0xe5,
                     0x2f,
                     0x1e,
                     0xf1,
                     0xf6,
                     0x0a};
    EXPECT_EQ(qr.rows[0][0].as_uuid(), expected);
}

TEST_F(QA_GDB281_E2E, InsertMixedCaseUuidString) {
    exec_ok("CREATE TABLE t (id UUID)");
    exec_ok("INSERT INTO t (id) VALUES ('d1458B55-F0bf-44D4-b191-E52f1eF1f60A')");

    auto qr = exec_ok("SELECT id FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    Uuid expected = {0xd1,
                     0x45,
                     0x8b,
                     0x55,
                     0xf0,
                     0xbf,
                     0x44,
                     0xd4,
                     0xb1,
                     0x91,
                     0xe5,
                     0x2f,
                     0x1e,
                     0xf1,
                     0xf6,
                     0x0a};
    EXPECT_EQ(qr.rows[0][0].as_uuid(), expected);
}

// -- UUID with gen_uuid() still works alongside string literal inserts --

TEST_F(QA_GDB281_E2E, MixGenUuidAndStringLiteral) {
    exec_ok("CREATE TABLE t (id UUID DEFAULT gen_uuid(), name TEXT)");
    // Insert with gen_uuid() default.
    exec_ok("INSERT INTO t (name) VALUES ('auto')");
    // Insert with explicit UUID string literal.
    exec_ok("INSERT INTO t (id, name) "
            "VALUES ('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee', 'manual')");

    auto qr = exec_ok("SELECT id, name FROM t");
    ASSERT_EQ(qr.rows.size(), 2u);

    // Auto-generated UUID should be valid (not null).
    EXPECT_FALSE(qr.rows[0][0].is_null());
    EXPECT_EQ(qr.rows[0][0].type_id(), TypeId::UUID);

    // Manual UUID should match what was inserted.
    Uuid manual = {0xaa,
                   0xaa,
                   0xaa,
                   0xaa,
                   0xbb,
                   0xbb,
                   0xcc,
                   0xcc,
                   0xdd,
                   0xdd,
                   0xee,
                   0xee,
                   0xee,
                   0xee,
                   0xee,
                   0xee};
    EXPECT_EQ(qr.rows[1][0].as_uuid(), manual);
    EXPECT_EQ(qr.rows[1][1].as_string(), "manual");
}

// -- Stress: many UUID inserts --

TEST_F(QA_GDB281_E2E, StressInsertManyUuidStringLiterals) {
    exec_ok("CREATE TABLE t (id UUID, seq INT)");

    // Insert 20 rows with distinct UUID string literals.
    for (int i = 0; i < 20; ++i) {
        // Generate a UUID string with the counter in the last 2 hex digits.
        char buf[128];
        snprintf(buf,
                 sizeof(buf),
                 "INSERT INTO t (id, seq) VALUES "
                 "('00000000-0000-0000-0000-%012x', %d)",
                 i,
                 i);
        exec_ok(buf);
    }

    auto qr = exec_ok("SELECT id, seq FROM t");
    ASSERT_EQ(qr.rows.size(), 20u);

    // Verify first and last rows.
    EXPECT_EQ(qr.rows[0][1].as_int32(), 0);
    EXPECT_EQ(qr.rows[19][1].as_int32(), 19);

    // Verify all UUIDs are distinct.
    std::set<Uuid> seen;
    for (size_t i = 0; i < qr.rows.size(); ++i) {
        auto [it, inserted] = seen.insert(qr.rows[i][0].as_uuid());
        EXPECT_TRUE(inserted) << "duplicate UUID at row " << i;
    }
}

// -- WHERE clause with UUID string comparison --
//
// TAUTOLOGY GUARD: a regression that ignores the WHERE predicate (returns all
// rows) will fail the row-count == 1 assertion and/or the zero-row assertion.

TEST_F(QA_GDB281_E2E, SelectWhereUuidMatchesInserted) {
    exec_ok("CREATE TABLE t (id UUID, name TEXT)");
    // Insert three rows with distinct, hardcoded UUIDs so the expected values
    // in assertions below are fully deterministic.
    exec_ok("INSERT INTO t (id, name) VALUES "
            "('11111111-1111-1111-1111-111111111111', 'one')");
    exec_ok("INSERT INTO t (id, name) VALUES "
            "('22222222-2222-2222-2222-222222222222', 'two')");
    exec_ok("INSERT INTO t (id, name) VALUES "
            "('33333333-3333-3333-3333-333333333333', 'three')");

    // AC1: WHERE uuid = <one specific UUID> returns EXACTLY that one row.
    // Hardcoded expected: id = 22222222-..., name = 'two'.
    auto qr_match = exec_ok("SELECT id, name FROM t "
                            "WHERE id = '22222222-2222-2222-2222-222222222222'");
    ASSERT_EQ(qr_match.rows.size(), 1u)
        << "expected exactly 1 row; WHERE predicate may not be applied";
    EXPECT_EQ(qr_match.rows[0][1].as_string(), "two")
        << "returned row does not match the filtered UUID";

    // AC2: WHERE uuid = <valid-but-not-inserted UUID> returns ZERO rows.
    // 'ffffffff-...' was never inserted.
    auto qr_miss = exec_ok("SELECT id, name FROM t "
                           "WHERE id = 'ffffffff-ffff-ffff-ffff-ffffffffffff'");
    ASSERT_EQ(qr_miss.rows.size(), 0u)
        << "expected 0 rows for an uninserted UUID; WHERE predicate may not be "
           "applied";
}

// -- INSERT with wrong type for non-UUID columns still works --

TEST_F(QA_GDB281_E2E, UuidColumnDoesNotAffectOtherColumns) {
    exec_ok("CREATE TABLE t (id UUID, count INT, label TEXT)");
    exec_ok("INSERT INTO t (id, count, label) VALUES "
            "('abcdefab-cdef-abcd-efab-cdefabcdefab', 42, 'hello')");

    auto qr = exec_ok("SELECT id, count, label FROM t");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].type_id(), TypeId::UUID);
    EXPECT_EQ(qr.rows[0][1].as_int32(), 42);
    EXPECT_EQ(qr.rows[0][2].as_string(), "hello");
}
