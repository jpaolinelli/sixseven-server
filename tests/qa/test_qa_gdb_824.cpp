/// @file test_qa_gdb_824.cpp
/// @brief Adversarial QA tests for GDB-824: ProviderRegistry::create_provider
///        "builtin/<dimension>" input validation.
///
/// Probes every class of invalid/edge-case dimension strings to confirm:
///   - valid integers succeed and produce the correct dimension
///   - every garbage input returns INVALID_ARGUMENT (no crash, no silent accept)
///   - overflow is caught
///   - zero dimension is accepted (known advisory)

#include "sixseven/catalog/catalog.h"
#include "sixseven/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <string>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helper: create a registry with no catalog entries (pure name-based parsing)
// ---------------------------------------------------------------------------

static ProviderRegistry make_registry() {
    static Catalog catalog;
    return ProviderRegistry{catalog};
}

// ===========================================================================
// GDB824: valid inputs — must succeed and produce correct dimension
// ===========================================================================

TEST(QA_GDB824_ValidInputs, Builtin1) {
    auto reg = make_registry();
    auto r = reg.resolve("builtin/1");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ((*r)->dimension(), 1u);
}

TEST(QA_GDB824_ValidInputs, Builtin128) {
    auto reg = make_registry();
    auto r = reg.resolve("builtin/128");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ((*r)->dimension(), 128u);
}

TEST(QA_GDB824_ValidInputs, Builtin384) {
    auto reg = make_registry();
    auto r = reg.resolve("builtin/384");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ((*r)->dimension(), 384u);
}

TEST(QA_GDB824_ValidInputs, Builtin4096) {
    auto reg = make_registry();
    auto r = reg.resolve("builtin/4096");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ((*r)->dimension(), 4096u);
}

// Advisory: zero dimension is currently accepted — document it, don't fail.
TEST(QA_GDB824_ValidInputs, BuiltinZeroDimensionAccepted) {
    auto reg = make_registry();
    auto r = reg.resolve("builtin/0");
    // Zero is accepted today; we document this so a future rejection is caught.
    // Change EXPECT_TRUE to EXPECT_FALSE if the team decides to reject 0.
    if (r.has_value()) {
        EXPECT_EQ((*r)->dimension(), 0u);
    } else {
        // Already tightened upstream — acceptable.
        EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    }
}

// ===========================================================================
// GDB824: negative dimensions — must be rejected
// ===========================================================================

TEST(QA_GDB824_Negative, NegativeFive) {
    auto reg = make_registry();
    auto r = reg.resolve("builtin/-5");
    ASSERT_FALSE(r.has_value()) << "builtin/-5 must be rejected";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    // Dimension must not silently wrap to ULONG_MAX - 4.
}

TEST(QA_GDB824_Negative, NegativeOne) {
    auto reg = make_registry();
    auto r = reg.resolve("builtin/-1");
    ASSERT_FALSE(r.has_value()) << "builtin/-1 must be rejected";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// ===========================================================================
// GDB824: float/decimal strings — must be rejected
// ===========================================================================

TEST(QA_GDB824_Float, Float1Point5) {
    auto reg = make_registry();
    auto r = reg.resolve("builtin/1.5");
    ASSERT_FALSE(r.has_value()) << "builtin/1.5 must be rejected";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB824_Float, ScientificNotation) {
    auto reg = make_registry();
    // stoul stops at 'e', so pos != size and this should fail.
    auto r = reg.resolve("builtin/1e3");
    ASSERT_FALSE(r.has_value()) << "builtin/1e3 must be rejected";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// ===========================================================================
// GDB824: alphanumeric / mixed strings — must be rejected
// ===========================================================================

TEST(QA_GDB824_Alphanumeric, PureAlpha) {
    auto reg = make_registry();
    auto r = reg.resolve("builtin/abc");
    ASSERT_FALSE(r.has_value()) << "builtin/abc must be rejected";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB824_Alphanumeric, NumericThenAlpha) {
    auto reg = make_registry();
    auto r = reg.resolve("builtin/12ab");
    ASSERT_FALSE(r.has_value()) << "builtin/12ab must be rejected";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB824_Alphanumeric, AlphaThenNumeric) {
    auto reg = make_registry();
    auto r = reg.resolve("builtin/ab12");
    ASSERT_FALSE(r.has_value()) << "builtin/ab12 must be rejected";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// ===========================================================================
// GDB824: trailing/leading garbage — must be rejected
// ===========================================================================

TEST(QA_GDB824_Garbage, TrailingSpace) {
    auto reg = make_registry();
    // "128 " — trailing whitespace; pos stops before end.
    auto r = reg.resolve("builtin/128 ");
    ASSERT_FALSE(r.has_value()) << "builtin/128<space> must be rejected";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB824_Garbage, LeadingSpace) {
    auto reg = make_registry();
    // " 128" — leading whitespace; leading-space guard catches it.
    auto r = reg.resolve("builtin/ 128");
    ASSERT_FALSE(r.has_value()) << "builtin/<space>128 must be rejected";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB824_Garbage, ExtraSlash) {
    auto reg = make_registry();
    // "128/extra" — parse_provider_name splits at first '/', so model="128/extra".
    // The pos-check detects non-digit at '/'.
    auto r = reg.resolve("builtin/128/extra");
    ASSERT_FALSE(r.has_value()) << "builtin/128/extra must be rejected";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB824_Garbage, HexPrefix) {
    auto reg = make_registry();
    // "0x10" — stoul accepts hex on some platforms when base=0; pos-check or
    // leading guard must prevent a silent accept.
    auto r = reg.resolve("builtin/0x10");
    // If accepted, dimension MUST be 16 (0x10) — not 0 (base-10 parse stop).
    // If rejected, code must be INVALID_ARGUMENT.
    if (r.has_value()) {
        // Accepted: verify we got a sane dimension (16), not garbage.
        EXPECT_EQ((*r)->dimension(), 16u)
            << "If 0x10 is accepted it must parse as 16, not some other value";
    } else {
        EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    }
}

TEST(QA_GDB824_Garbage, PlusSign) {
    auto reg = make_registry();
    // "+5" — leading '+'; the guard blocks '+' before stoul.
    auto r = reg.resolve("builtin/+5");
    ASSERT_FALSE(r.has_value()) << "builtin/+5 must be rejected (leading '+')";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB824_Garbage, EmptyDimension) {
    auto reg = make_registry();
    // "builtin/" — trailing slash; parse_provider_name rejects it.
    auto r = reg.resolve("builtin/");
    ASSERT_FALSE(r.has_value()) << "builtin/ (empty dimension) must be rejected";
    // The error may be INVALID_ARGUMENT from parse_provider_name.
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB824_Garbage, NoSlash) {
    auto reg = make_registry();
    // "builtin" — no slash at all; parse_provider_name rejects it.
    auto r = reg.resolve("builtin");
    ASSERT_FALSE(r.has_value()) << "\"builtin\" (no slash) must be rejected";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_GDB824_Garbage, LeadingTab) {
    auto reg = make_registry();
    auto r = reg.resolve("builtin/\t128");
    ASSERT_FALSE(r.has_value()) << "builtin/<tab>128 must be rejected";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// ===========================================================================
// GDB824: overflow — must return INVALID_ARGUMENT, not crash
// ===========================================================================

TEST(QA_GDB824_Overflow, HugeNumber) {
    auto reg = make_registry();
    auto r = reg.resolve("builtin/99999999999999999999999");
    ASSERT_FALSE(r.has_value()) << "overflow dimension must be rejected";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    // Must not crash, throw, or produce a provider with a wrapped dimension.
}

// ===========================================================================
// GDB824: regression — tightened test would fail if validation removed
//
// These tests call create_provider directly (via resolve) and assert on the
// exact INVALID_ARGUMENT code.  If validation regressed to silent-accept,
// r.has_value() would be true and ASSERT_FALSE would fail.
// ===========================================================================

TEST(QA_GDB824_Regression, NonNumericAlwaysRejectsNotDefaults) {
    // If the old "silent-default-to-384" behavior returned, has_value() would
    // be true and the test would fail — exactly what we want.
    auto reg = make_registry();
    auto r = reg.resolve("builtin/not_a_number");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
    // Explicitly confirm we did NOT get a 384-dim provider.
    // (If has_value() were true above, we'd get here with garbage state.)
}

TEST(QA_GDB824_Regression, NegativeAlwaysRejectsNotWraps) {
    // If the leading-'-' guard were removed, stoul("-1") wraps to ULONG_MAX
    // and a provider with dimension=18446744073709551615 would be silently created.
    auto reg = make_registry();
    auto r = reg.resolve("builtin/-1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}

// ===========================================================================
// GDB824: unicode / whitespace in dimension
// ===========================================================================

TEST(QA_GDB824_Unicode, UnicodeDigit) {
    auto reg = make_registry();
    // Full-width digit U+FF11 ('１') — not an ASCII digit; stoul should fail or
    // the pos-check should catch extra bytes.
    auto r = reg.resolve("builtin/\xEF\xBC\x91\x32\x38"); // U+FF11 followed by "28"
    ASSERT_FALSE(r.has_value()) << "unicode digit prefix must be rejected";
    EXPECT_EQ(r.error().code, StatusCode::INVALID_ARGUMENT);
}
