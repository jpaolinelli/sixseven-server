#include "sixseven/server/auth.h"

#include <gtest/gtest.h>

namespace sixseven {

TEST(AuthRandomBytes, ZeroLengthReturnsEmpty) {
    auto result = random_bytes(0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 0u);
}

TEST(AuthRandomBytes, Returns16Bytes) {
    auto result = random_bytes(16);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 16u);
}

TEST(AuthRandomBytes, Returns32Bytes) {
    auto result = random_bytes(32);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 32u);
}

TEST(AuthRandomBytes, TwoCallsDiffer) {
    auto r1 = random_bytes(32);
    auto r2 = random_bytes(32);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    // Two independent 32-byte draws from the CSPRNG must differ with
    // overwhelming probability (probability of collision is ~2^-256).
    EXPECT_NE(*r1, *r2);
}

} // namespace sixseven
