#include "sixseven/common/status.h"
#include "sixseven/common/value.h"
#include "sixseven/index/btree_key.h"

#include <gtest/gtest.h>

using namespace sixseven;

// =============================================================================
// GDB-863: BTreeKey -- compare_keys unit tests
// =============================================================================

// ---------------------------------------------------------------------------
// Arity mismatch
// ---------------------------------------------------------------------------

TEST(BTreeKeyTest, ArityMismatchReturnsError) {
    KeyType k1 = {Value(int64_t{1})};
    KeyType k2 = {Value(int64_t{1}), Value(int64_t{2})};

    auto result = compare_keys(k1, k2);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(BTreeKeyTest, ArityMismatchBothEmpty) {
    // Two empty keys have arity 0 == 0; loop never runs, result is equal.
    KeyType k1;
    KeyType k2;
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::equal);
}

// ---------------------------------------------------------------------------
// INT64 single-column ordering
// ---------------------------------------------------------------------------

TEST(BTreeKeyTest, Int64LessOrdering) {
    KeyType k1 = {Value(int64_t{10})};
    KeyType k2 = {Value(int64_t{20})};
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::less);
}

TEST(BTreeKeyTest, Int64GreaterOrdering) {
    KeyType k1 = {Value(int64_t{20})};
    KeyType k2 = {Value(int64_t{10})};
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::greater);
}

TEST(BTreeKeyTest, Int64EqualOrdering) {
    KeyType k1 = {Value(int64_t{42})};
    KeyType k2 = {Value(int64_t{42})};
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::equal);
}

// ---------------------------------------------------------------------------
// INT64 boundary values
// ---------------------------------------------------------------------------

TEST(BTreeKeyTest, Int64MinLessThanMax) {
    KeyType kmin = {Value(std::numeric_limits<int64_t>::min())};
    KeyType kmax = {Value(std::numeric_limits<int64_t>::max())};
    auto result = compare_keys(kmin, kmax);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::less);
}

TEST(BTreeKeyTest, Int64MaxGreaterThanMin) {
    KeyType kmax = {Value(std::numeric_limits<int64_t>::max())};
    KeyType kmin = {Value(std::numeric_limits<int64_t>::min())};
    auto result = compare_keys(kmax, kmin);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::greater);
}

TEST(BTreeKeyTest, Int64ZeroVsPositive) {
    KeyType kzero = {Value(int64_t{0})};
    KeyType kpos = {Value(int64_t{1})};
    auto result = compare_keys(kzero, kpos);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::less);
}

TEST(BTreeKeyTest, Int64ZeroVsNegative) {
    KeyType kzero = {Value(int64_t{0})};
    KeyType kneg = {Value(int64_t{-1})};
    auto result = compare_keys(kzero, kneg);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::greater);
}

// ---------------------------------------------------------------------------
// STRING single-column ordering
// ---------------------------------------------------------------------------

TEST(BTreeKeyTest, StringLexicographicOrdering) {
    KeyType ka = {Value(std::string{"apple"})};
    KeyType kb = {Value(std::string{"banana"})};
    auto result = compare_keys(ka, kb);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::less);
}

TEST(BTreeKeyTest, StringEqualOrdering) {
    KeyType k1 = {Value(std::string{"hello"})};
    KeyType k2 = {Value(std::string{"hello"})};
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::equal);
}

TEST(BTreeKeyTest, EmptyStringLessThanNonEmpty) {
    KeyType kempty = {Value(std::string{""})};
    KeyType knonempty = {Value(std::string{"a"})};
    auto result = compare_keys(kempty, knonempty);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::less);
}

// ---------------------------------------------------------------------------
// Composite (INT64, STRING) key -- lexicographic tie-breaking
// ---------------------------------------------------------------------------

TEST(BTreeKeyTest, CompositeKeyFirstColumnDecides) {
    // (1, "z") < (2, "a")  -- first column determines order.
    KeyType k1 = {Value(int64_t{1}), Value(std::string{"z"})};
    KeyType k2 = {Value(int64_t{2}), Value(std::string{"a"})};
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::less);
}

TEST(BTreeKeyTest, CompositeKeySecondColumnDecides) {
    // (5, "apple") < (5, "banana") -- first equal, second decides.
    KeyType k1 = {Value(int64_t{5}), Value(std::string{"apple"})};
    KeyType k2 = {Value(int64_t{5}), Value(std::string{"banana"})};
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::less);
}

TEST(BTreeKeyTest, CompositeKeyFullyEqual) {
    KeyType k1 = {Value(int64_t{7}), Value(std::string{"same"})};
    KeyType k2 = {Value(int64_t{7}), Value(std::string{"same"})};
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::strong_ordering::equal);
}

// ---------------------------------------------------------------------------
// Non-comparable type error propagation (BLOB)
// ---------------------------------------------------------------------------

TEST(BTreeKeyTest, BlobTypeReturnsError) {
    // BLOB is not comparable; compare_keys must propagate the error.
    std::vector<uint8_t> blob_data = {0x01, 0x02, 0x03};
    KeyType k1 = {Value(blob_data)};
    KeyType k2 = {Value(blob_data)};
    auto result = compare_keys(k1, k2);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

TEST(BTreeKeyTest, BlobInCompositeKeyReturnsError) {
    // Even when the first column is equal, hitting a BLOB in column 2 must error.
    std::vector<uint8_t> blob_data = {0xAB};
    KeyType k1 = {Value(int64_t{1}), Value(blob_data)};
    KeyType k2 = {Value(int64_t{1}), Value(blob_data)};
    auto result = compare_keys(k1, k2);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// ---------------------------------------------------------------------------
// Transitivity / ordering consistency
// ---------------------------------------------------------------------------

TEST(BTreeKeyTest, Int64Transitivity) {
    KeyType ka = {Value(int64_t{1})};
    KeyType kb = {Value(int64_t{2})};
    KeyType kc = {Value(int64_t{3})};

    auto ab = compare_keys(ka, kb);
    auto bc = compare_keys(kb, kc);
    auto ac = compare_keys(ka, kc);

    ASSERT_TRUE(ab.has_value());
    ASSERT_TRUE(bc.has_value());
    ASSERT_TRUE(ac.has_value());

    EXPECT_EQ(*ab, std::strong_ordering::less);
    EXPECT_EQ(*bc, std::strong_ordering::less);
    EXPECT_EQ(*ac, std::strong_ordering::less);
}
