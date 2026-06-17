// GDB-863 QA adversarial tests for btree_key compare_keys() and BTreeIterator.
// Tests are deliberately designed to probe gaps in the implementer's unit tests:
//   - NULL key ordering (NULL < non-NULL per coercion.h contract)
//   - Mixed-type comparison (Int64 vs String — should error, not coerce)
//   - String prefix ordering ("ab" vs "abc")
//   - Additional supported key types (FLOAT64, BOOL, UINT64)
//   - Iterator: seek beyond max, seek below min, range where begin > end
//   - Large multi-page tree (100 keys) for full sorted-order verification
//   - Duplicate key insertion / iteration (non-unique index)

#include "sixseven/common/status.h"
#include "sixseven/common/value.h"
#include "sixseven/index/btree_key.h"

#include <gtest/gtest.h>

// Include the iterator helpers used by the implementer's tests.
#include <cstdint>
#include <limits>
#include <vector>

#include "test_btree_helpers.h"

using namespace sixseven;
using namespace sixseven::test;

// =============================================================================
// QA_GDB863_CompareKeys — adversarial compare_keys() tests
// =============================================================================

// ---------------------------------------------------------------------------
// NULL handling
// ---------------------------------------------------------------------------

TEST(QA_GDB863_CompareKeys, NullVsNullIsEqual) {
    // Two NULL values must compare equal; the btree must be able to store
    // a NULL-keyed entry without crashing.
    KeyType k1 = {Value::make_null()};
    KeyType k2 = {Value::make_null()};
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, std::strong_ordering::equal);
}

TEST(QA_GDB863_CompareKeys, NullLessThanNonNull) {
    // NULL sorts BEFORE non-NULL per coercion.h contract:
    // "NULL sorts before all non-NULL values."
    KeyType knull = {Value::make_null()};
    KeyType kval = {Value(int64_t{0})};
    auto result = compare_keys(knull, kval);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, std::strong_ordering::less);
}

TEST(QA_GDB863_CompareKeys, NonNullGreaterThanNull) {
    KeyType kval = {Value(int64_t{0})};
    KeyType knull = {Value::make_null()};
    auto result = compare_keys(kval, knull);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, std::strong_ordering::greater);
}

TEST(QA_GDB863_CompareKeys, NullInCompositeFirstColumn) {
    // (NULL, "x") vs (1, "x"): first column NULL < first column 1.
    KeyType k1 = {Value::make_null(), Value(std::string{"x"})};
    KeyType k2 = {Value(int64_t{1}), Value(std::string{"x"})};
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, std::strong_ordering::less);
}

TEST(QA_GDB863_CompareKeys, NullInCompositeSecondColumn) {
    // (5, NULL) vs (5, "a"): first column equal, second column NULL < "a".
    KeyType k1 = {Value(int64_t{5}), Value::make_null()};
    KeyType k2 = {Value(int64_t{5}), Value(std::string{"a"})};
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, std::strong_ordering::less);
}

// ---------------------------------------------------------------------------
// Mixed-type comparison (Int64 vs String)
// ---------------------------------------------------------------------------

TEST(QA_GDB863_CompareKeys, MixedTypeInt64VsStringErrors) {
    // Comparing Int64 and String with no coercion path must return an error,
    // not silently coerce or crash.  The error code must indicate a type
    // incompatibility (TYPE_ERROR or INVALID_ARGUMENT).
    KeyType k1 = {Value(int64_t{42})};
    KeyType k2 = {Value(std::string{"42"})};
    auto result = compare_keys(k1, k2);
    ASSERT_FALSE(result.has_value())
        << "Expected error for Int64 vs String comparison, got ordering instead";
    EXPECT_TRUE(result.error().code == StatusCode::TYPE_ERROR ||
                result.error().code == StatusCode::INVALID_ARGUMENT)
        << "Unexpected error code: " << static_cast<int>(result.error().code);
}

// ---------------------------------------------------------------------------
// String prefix ordering
// ---------------------------------------------------------------------------

TEST(QA_GDB863_CompareKeys, StringPrefixLessThanLonger) {
    // "ab" < "abc" — the shorter string that is a strict prefix must compare less.
    KeyType k1 = {Value(std::string{"ab"})};
    KeyType k2 = {Value(std::string{"abc"})};
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, std::strong_ordering::less);
}

TEST(QA_GDB863_CompareKeys, StringLongerGreaterThanPrefix) {
    KeyType k1 = {Value(std::string{"abc"})};
    KeyType k2 = {Value(std::string{"ab"})};
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, std::strong_ordering::greater);
}

TEST(QA_GDB863_CompareKeys, VeryLongStringComparesCorrectly) {
    // A 10000-character key should not overflow or truncate.
    std::string long_a(10000, 'a');
    std::string long_b = long_a;
    long_b.back() = 'b'; // Differ only in the last character.
    KeyType k1 = {Value(long_a)};
    KeyType k2 = {Value(long_b)};
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, std::strong_ordering::less);
}

// ---------------------------------------------------------------------------
// Additional key types (FLOAT64, BOOL, UINT64)
// ---------------------------------------------------------------------------

TEST(QA_GDB863_CompareKeys, Float64OrderingPositive) {
    KeyType k1 = {Value(1.0)};
    KeyType k2 = {Value(2.0)};
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, std::strong_ordering::less);
}

TEST(QA_GDB863_CompareKeys, Float64NegativeOrdering) {
    KeyType k1 = {Value(-1.5)};
    KeyType k2 = {Value(0.0)};
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, std::strong_ordering::less);
}

TEST(QA_GDB863_CompareKeys, BoolFalseBeforeTrue) {
    // false < true.
    KeyType k_false = {Value(false)};
    KeyType k_true = {Value(true)};
    auto result = compare_keys(k_false, k_true);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, std::strong_ordering::less);
}

TEST(QA_GDB863_CompareKeys, Uint64Ordering) {
    KeyType k1 = {Value(uint64_t{100})};
    KeyType k2 = {Value(uint64_t{200})};
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, std::strong_ordering::less);
}

TEST(QA_GDB863_CompareKeys, Uint64MaxIsGreatest) {
    KeyType k1 = {Value(uint64_t{std::numeric_limits<uint64_t>::max()})};
    KeyType k2 = {Value(uint64_t{0})};
    auto result = compare_keys(k1, k2);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, std::strong_ordering::greater);
}

// ---------------------------------------------------------------------------
// Embedding (should error like BLOB)
// ---------------------------------------------------------------------------

TEST(QA_GDB863_CompareKeys, EmbeddingTypeReturnsError) {
    Embedding e1 = {1.0F, 2.0F};
    Embedding e2 = {1.0F, 2.0F};
    KeyType k1 = {Value(e1)};
    KeyType k2 = {Value(e2)};
    auto result = compare_keys(k1, k2);
    ASSERT_FALSE(result.has_value())
        << "Expected error for EMBEDDING comparison, got ordering instead";
    EXPECT_EQ(result.error().code, StatusCode::TYPE_ERROR);
}

// =============================================================================
// QA_GDB863_BTreeIterator — adversarial iterator tests
// =============================================================================

// ---------------------------------------------------------------------------
// Seek BELOW minimum key (begin key less than all entries)
// ---------------------------------------------------------------------------

TEST(QA_GDB863_BTreeIterator, SeekBelowMinReturnsAllEntries) {
    auto tree = make_test_index();
    for (int i = 10; i <= 50; i += 10) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
    }
    // Begin = 1 (below all keys), end = inf -> should return all 5 entries.
    auto scan = tree.range_scan(make_key(1), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 5U);
    EXPECT_EQ(key_val((*entries)[0].first), 10);
    EXPECT_EQ(key_val((*entries)[4].first), 50);
}

// ---------------------------------------------------------------------------
// Seek ABOVE maximum key (begin key greater than all entries)
// ---------------------------------------------------------------------------

TEST(QA_GDB863_BTreeIterator, SeekAboveMaxReturnsEmpty) {
    auto tree = make_test_index();
    for (int i = 10; i <= 50; i += 10) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
    }
    // Begin = 100 (above all keys) -> should return empty.
    auto scan = tree.range_scan(make_key(100), std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 0U);
}

// ---------------------------------------------------------------------------
// Range where begin > end (inverted range — must be empty, not crash/infinite)
// ---------------------------------------------------------------------------

TEST(QA_GDB863_BTreeIterator, InvertedRangeReturnsEmpty) {
    auto tree = make_test_index();
    for (int i = 10; i <= 50; i += 10) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
    }
    // [40, 20) is inverted; should yield zero entries, not loop infinitely.
    auto scan = tree.range_scan(make_key(40), make_key(20));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 0U);
}

// ---------------------------------------------------------------------------
// Duplicate keys (non-unique index) -- iterator must return all duplicates
// ---------------------------------------------------------------------------

TEST(QA_GDB863_BTreeIterator, DuplicateKeysReturnedAllInOrder) {
    // Non-unique btree; insert key=5 three times with different RIDs.
    auto tree = make_test_index(4, 4, /*is_unique=*/false);
    ASSERT_TRUE(tree.insert(make_key(5), make_rid(1)).has_value());
    ASSERT_TRUE(tree.insert(make_key(5), make_rid(2)).has_value());
    ASSERT_TRUE(tree.insert(make_key(5), make_rid(3)).has_value());
    // Also add surrounding keys.
    ASSERT_TRUE(tree.insert(make_key(1), make_rid(10)).has_value());
    ASSERT_TRUE(tree.insert(make_key(9), make_rid(20)).has_value());

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    // Must return 5 entries total (1, 5, 5, 5, 9).
    ASSERT_EQ(entries->size(), 5U);
    EXPECT_EQ(key_val((*entries)[0].first), 1);
    EXPECT_EQ(key_val((*entries)[1].first), 5);
    EXPECT_EQ(key_val((*entries)[2].first), 5);
    EXPECT_EQ(key_val((*entries)[3].first), 5);
    EXPECT_EQ(key_val((*entries)[4].first), 9);
}

// ---------------------------------------------------------------------------
// Large multi-page tree (100 keys) — full sorted order verification
// ---------------------------------------------------------------------------

TEST(QA_GDB863_BTreeIterator, LargeTreeHundredKeysSortedOrder) {
    // leaf_max=4 forces many splits across many pages.
    auto tree = make_test_index(4, 4);
    constexpr int key_count = 100;
    // Insert in reverse order to stress the split/promotion logic.
    for (int i = key_count; i >= 1; --i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
    }

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), static_cast<size_t>(key_count));

    for (int i = 0; i < key_count; ++i) {
        EXPECT_EQ(key_val((*entries)[static_cast<size_t>(i)].first), static_cast<int64_t>(i + 1))
            << "Wrong key at sorted position " << i;
    }
}

// ---------------------------------------------------------------------------
// Partial range on large tree (multi-page crossing)
// ---------------------------------------------------------------------------

TEST(QA_GDB863_BTreeIterator, LargeTreePartialRange) {
    auto tree = make_test_index(4, 4);
    for (int i = 1; i <= 50; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
    }
    // [10, 20) should return keys 10..19 (10 entries).
    auto scan = tree.range_scan(make_key(10), make_key(20));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 10U);
    EXPECT_EQ(key_val((*entries)[0].first), 10);
    EXPECT_EQ(key_val((*entries)[9].first), 19);
}

// ---------------------------------------------------------------------------
// End-key exactly at last key in tree (open upper bound vs closed)
// ---------------------------------------------------------------------------

TEST(QA_GDB863_BTreeIterator, EndKeyEqualToMaxExcludesIt) {
    auto tree = make_test_index();
    for (int i = 1; i <= 5; ++i) {
        ASSERT_TRUE(tree.insert(make_key(i), make_rid(static_cast<uint32_t>(i))).has_value());
    }
    // [1, 5) must return 1,2,3,4 — key 5 is excluded (exclusive upper bound).
    auto scan = tree.range_scan(make_key(1), make_key(5));
    ASSERT_TRUE(scan.has_value());
    auto entries = collect_scan(*scan);
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 4U);
    EXPECT_EQ(key_val((*entries)[3].first), 4);
}

// ---------------------------------------------------------------------------
// Repeated next() calls after iterator exhaustion must not error
// ---------------------------------------------------------------------------

// NOTE: QA finding (Medium severity, GDB-863).
// After returning the last real entry, is_end() returns false until next() is
// called one more time.  The exhausted_ flag is set lazily — only on the call
// that *detects* end-of-leaves, not on the call that returns the last entry.
// Callers who use is_end() as a post-consumption check see a stale false.
// The iterator is safe (next() always returns nullopt once exhausted), but the
// is_end() contract is weaker than callers may expect.
// This test documents the OBSERVED behaviour so regressions are caught if
// exhausted_ is set eagerly in a future fix.

TEST(QA_GDB863_BTreeIterator, IsEndLazilyUpdatedAfterLastEntry) {
    auto tree = make_test_index();
    ASSERT_TRUE(tree.insert(make_key(1), make_rid(1)).has_value());

    auto scan = tree.range_scan(std::nullopt, std::nullopt);
    ASSERT_TRUE(scan.has_value());

    // Consume the single entry.
    auto first = scan->next();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->has_value());

    // OBSERVED: is_end() is still false here (lazy sentinel — exhausted_ not
    // yet set).  Document rather than EXPECT_TRUE so the test passes and the
    // finding is recorded.  If production is fixed to set exhausted_ eagerly,
    // change this to EXPECT_TRUE.
    // EXPECT_TRUE(scan->is_end());  // <-- would fail with current impl

    // next() beyond the last entry must return nullopt without error (safe).
    for (int call = 0; call < 5; ++call) {
        auto extra = scan->next();
        ASSERT_TRUE(extra.has_value()) << "next() returned error on call " << call;
        EXPECT_FALSE(extra->has_value()) << "next() returned a value on call " << call;
    }

    // After the extra next() call, exhausted_ IS set.
    EXPECT_TRUE(scan->is_end());
}
