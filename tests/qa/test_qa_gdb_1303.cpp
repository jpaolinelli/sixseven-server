// Adversarial QA tests for GDB-1303:
// Serializer for TypeId::PATH was dropping Path::total_weight -- fixed by
// writing/reading an 8-byte LE double before the step count/steps in
// serialize()/deserialize()/payload_size() (src/storage/serialization.cpp),
// mirroring the GDB-799 fix already applied to tuple.cpp/external_sort.cpp.
//
// Attack surface probed here:
//   - Boundary total_weight values: 0.0, negative, -0.0, NaN, +-Infinity,
//     DBL_MAX, DBL_MIN (denormal-adjacent), very small/large magnitudes.
//   - Multi-step paths with varying total_weight, and total_weight combined
//     with STRING/UUID PK tag-byte encoding from GDB-1292 (offset math must
//     not be disturbed by the new 8-byte prefix).
//   - Truncated / malformed buffers: cut off mid total_weight field, cut off
//     exactly at the total_weight/step-count boundary, cut off mid-step-data
//     after a valid total_weight+count header. Must fail clean (Result
//     error), never read out of bounds -- validated under ASan.
//   - serialized_size()/payload_size() agreement with actual serialize()
//     output size for PATH values (buffer-overrun guard for callers that
//     pre-size buffers).

#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/storage/serialization.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>

namespace sixseven {
namespace {

Path make_path(double total_weight) {
    Path p;
    p.total_weight = total_weight;
    p.steps.emplace_back(Value(int64_t{1}), int64_t{10});
    return p;
}

// ---------------------------------------------------------------------------
// Boundary total_weight values
// ---------------------------------------------------------------------------

TEST(QA_GDB1303_TotalWeightBoundaries, ZeroRoundTrips) {
    Value v(make_path(0.0));
    auto bytes = serialize(v);
    auto restored = deserialize(bytes, TypeId::PATH);
    ASSERT_TRUE(restored.has_value()) << restored.error().message;
    EXPECT_DOUBLE_EQ(restored->as_path().total_weight, 0.0);
}

TEST(QA_GDB1303_TotalWeightBoundaries, NegativeZeroRoundTrips) {
    Value v(make_path(-0.0));
    auto bytes = serialize(v);
    auto restored = deserialize(bytes, TypeId::PATH);
    ASSERT_TRUE(restored.has_value()) << restored.error().message;
    // -0.0 == 0.0 numerically; verify sign bit is preserved bit-for-bit too.
    EXPECT_DOUBLE_EQ(restored->as_path().total_weight, -0.0);
    EXPECT_TRUE(std::signbit(restored->as_path().total_weight));
}

TEST(QA_GDB1303_TotalWeightBoundaries, NegativeWeightRoundTrips) {
    Value v(make_path(-42.5));
    auto bytes = serialize(v);
    auto restored = deserialize(bytes, TypeId::PATH);
    ASSERT_TRUE(restored.has_value()) << restored.error().message;
    EXPECT_DOUBLE_EQ(restored->as_path().total_weight, -42.5);
}

TEST(QA_GDB1303_TotalWeightBoundaries, NaNRoundTrips) {
    Value v(make_path(std::numeric_limits<double>::quiet_NaN()));
    auto bytes = serialize(v);
    auto restored = deserialize(bytes, TypeId::PATH);
    ASSERT_TRUE(restored.has_value()) << restored.error().message;
    EXPECT_TRUE(std::isnan(restored->as_path().total_weight));
}

TEST(QA_GDB1303_TotalWeightBoundaries, PositiveInfinityRoundTrips) {
    Value v(make_path(std::numeric_limits<double>::infinity()));
    auto bytes = serialize(v);
    auto restored = deserialize(bytes, TypeId::PATH);
    ASSERT_TRUE(restored.has_value()) << restored.error().message;
    EXPECT_TRUE(std::isinf(restored->as_path().total_weight));
    EXPECT_GT(restored->as_path().total_weight, 0.0);
}

TEST(QA_GDB1303_TotalWeightBoundaries, NegativeInfinityRoundTrips) {
    Value v(make_path(-std::numeric_limits<double>::infinity()));
    auto bytes = serialize(v);
    auto restored = deserialize(bytes, TypeId::PATH);
    ASSERT_TRUE(restored.has_value()) << restored.error().message;
    EXPECT_TRUE(std::isinf(restored->as_path().total_weight));
    EXPECT_LT(restored->as_path().total_weight, 0.0);
}

TEST(QA_GDB1303_TotalWeightBoundaries, MaxDoubleRoundTrips) {
    Value v(make_path(std::numeric_limits<double>::max()));
    auto bytes = serialize(v);
    auto restored = deserialize(bytes, TypeId::PATH);
    ASSERT_TRUE(restored.has_value()) << restored.error().message;
    EXPECT_DOUBLE_EQ(restored->as_path().total_weight, std::numeric_limits<double>::max());
}

TEST(QA_GDB1303_TotalWeightBoundaries, LowestDoubleRoundTrips) {
    Value v(make_path(std::numeric_limits<double>::lowest()));
    auto bytes = serialize(v);
    auto restored = deserialize(bytes, TypeId::PATH);
    ASSERT_TRUE(restored.has_value()) << restored.error().message;
    EXPECT_DOUBLE_EQ(restored->as_path().total_weight, std::numeric_limits<double>::lowest());
}

TEST(QA_GDB1303_TotalWeightBoundaries, DenormalMinRoundTrips) {
    Value v(make_path(std::numeric_limits<double>::denorm_min()));
    auto bytes = serialize(v);
    auto restored = deserialize(bytes, TypeId::PATH);
    ASSERT_TRUE(restored.has_value()) << restored.error().message;
    EXPECT_DOUBLE_EQ(restored->as_path().total_weight, std::numeric_limits<double>::denorm_min());
}

// ---------------------------------------------------------------------------
// Multi-step paths with varying total_weight + PK type interaction
// ---------------------------------------------------------------------------

TEST(QA_GDB1303_MultiStep, ManyStepsWithFractionalWeight) {
    Path p;
    p.total_weight = 123.456;
    for (int64_t i = 0; i < 25; ++i) {
        p.steps.emplace_back(Value(int64_t{i}), i == 24 ? int64_t{-1} : i);
    }
    Value v(std::move(p));
    auto bytes = serialize(v);
    auto restored = deserialize(bytes, TypeId::PATH);
    ASSERT_TRUE(restored.has_value()) << restored.error().message;
    const auto& rp = restored->as_path();
    ASSERT_EQ(rp.steps.size(), 25u);
    EXPECT_DOUBLE_EQ(rp.total_weight, 123.456);
    for (int64_t i = 0; i < 25; ++i) {
        EXPECT_EQ(rp.steps[static_cast<size_t>(i)].node_pk_as_int64(), i);
    }
}

TEST(QA_GDB1303_MultiStep, StringAndUuidPksWithWeightPreserveOffsets) {
    // Combines GDB-1292's tag-byte STRING/UUID PK encoding with GDB-1303's
    // 8-byte total_weight prefix -- verifies the prefix does not shift the
    // per-step tag-byte offset math.
    Path p;
    p.total_weight = 9.75;
    p.steps.emplace_back(Value(std::string("first-node")), int64_t{1});
    Uuid u{};
    for (size_t i = 0; i < u.size(); ++i)
        u[i] = static_cast<uint8_t>(i * 3);
    p.steps.emplace_back(Value(u), int64_t{2});
    p.steps.emplace_back(Value(std::string("last-node,with[chars]")), int64_t{-1});

    Value v(std::move(p));
    auto bytes = serialize(v);
    auto restored = deserialize(bytes, TypeId::PATH);
    ASSERT_TRUE(restored.has_value()) << restored.error().message;
    const auto& rp = restored->as_path();
    ASSERT_EQ(rp.steps.size(), 3u);
    EXPECT_DOUBLE_EQ(rp.total_weight, 9.75);
    EXPECT_EQ(rp.steps[0].node_pk().as_string(), "first-node");
    EXPECT_EQ(rp.steps[1].node_pk().as_uuid(), u);
    EXPECT_EQ(rp.steps[2].node_pk().as_string(), "last-node,with[chars]");
}

// ---------------------------------------------------------------------------
// Truncated / malformed buffer handling -- must fail clean, never read OOB.
// ---------------------------------------------------------------------------

TEST(QA_GDB1303_TruncatedBuffers, TruncatedMidTotalWeightField) {
    Path p;
    p.total_weight = 3.14;
    p.steps.emplace_back(Value(int64_t{1}), int64_t{1});
    Value v(std::move(p));
    auto bytes = serialize(v);
    ASSERT_GT(bytes.size(), 5u);
    // Cut off after 4 bytes -- inside the 8-byte total_weight field, before
    // even the step count is reachable.
    std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + 4);
    auto restored = deserialize(truncated, TypeId::PATH);
    EXPECT_FALSE(restored.has_value());
}

TEST(QA_GDB1303_TruncatedBuffers, TruncatedExactlyAtWeightCountBoundary) {
    Path p;
    p.total_weight = 3.14;
    p.steps.emplace_back(Value(int64_t{1}), int64_t{1});
    Value v(std::move(p));
    auto bytes = serialize(v);
    ASSERT_GT(bytes.size(), 12u);
    // Cut off exactly after total_weight (8 bytes) + partial count (2 of 4
    // bytes) -- boundary condition for the check_size(sizeof(double)+4).
    std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + 10);
    auto restored = deserialize(truncated, TypeId::PATH);
    EXPECT_FALSE(restored.has_value());
}

TEST(QA_GDB1303_TruncatedBuffers, TruncatedMidStepData) {
    Path p;
    p.total_weight = 2.5;
    p.steps.emplace_back(Value(std::string("truncate-me-please")), int64_t{7});
    Value v(std::move(p));
    auto bytes = serialize(v);
    ASSERT_GT(bytes.size(), 20u);
    // total_weight (8) + count (4) + tag byte (1) is intact, but the string
    // payload itself is cut short.
    std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + 16);
    auto restored = deserialize(truncated, TypeId::PATH);
    EXPECT_FALSE(restored.has_value());
}

TEST(QA_GDB1303_TruncatedBuffers, EmptyBufferFailsClean) {
    std::vector<uint8_t> empty;
    auto restored = deserialize(empty, TypeId::PATH);
    EXPECT_FALSE(restored.has_value());
}

TEST(QA_GDB1303_TruncatedBuffers, SingleByteBufferFailsClean) {
    std::vector<uint8_t> one_byte{0x42};
    auto restored = deserialize(one_byte, TypeId::PATH);
    EXPECT_FALSE(restored.has_value());
}

TEST(QA_GDB1303_TruncatedBuffers, ClaimedStepCountExceedsBufferFailsClean) {
    // Valid total_weight, but a step count that claims far more steps than
    // the remaining buffer could possibly hold.
    //
    // Layout produced by the top-level serialize(): [1-byte null flag]
    // [8-byte total_weight][4-byte step count]{steps...}. The null flag
    // (buf.push_back(0x01) for non-null values, see serialize()'s entry
    // block) is a top-level convention applied to every TypeId, not part of
    // PATH's own payload_size() accounting, so the step-count field sits at
    // byte offset 9, not 8.
    Path p;
    p.total_weight = 1.0;
    Value v(std::move(p)); // empty steps -- header only
    auto bytes = serialize(v);
    ASSERT_EQ(bytes.size(), 1 + sizeof(double) + 4u);
    // Overwrite the step-count field (bytes [9..13)) with a huge bogus value.
    bytes[9] = 0xFF;
    bytes[10] = 0xFF;
    bytes[11] = 0xFF;
    bytes[12] = 0x7F;
    auto restored = deserialize(bytes, TypeId::PATH);
    EXPECT_FALSE(restored.has_value());
}

// ---------------------------------------------------------------------------
// serialized_size()/payload_size() agreement for PATH with total_weight.
// ---------------------------------------------------------------------------

TEST(QA_GDB1303_SizeAgreement, SerializedSizeMatchesActualOutputWithWeight) {
    Path p;
    p.total_weight = 55.5;
    p.steps.emplace_back(Value(std::string("size-check")), int64_t{3});
    p.steps.emplace_back(Value(int64_t{7}), int64_t{-1});
    Value v(std::move(p));

    size_t predicted = serialized_size(v);
    auto bytes = serialize(v);
    EXPECT_EQ(predicted, bytes.size());
}

TEST(QA_GDB1303_SizeAgreement, EmptyPathSizeIncludesWeightPrefix) {
    Path p;
    p.total_weight = 1.0;
    Value v(std::move(p));
    auto bytes = serialize(v);
    // 1-byte top-level null flag (see serialize()'s buf.push_back(0x01) for
    // non-null values) + 8 bytes total_weight + 4 bytes step count, no steps.
    EXPECT_EQ(bytes.size(), 1 + sizeof(double) + 4u);
    EXPECT_EQ(serialized_size(v), bytes.size());
}

}  // namespace
}  // namespace sixseven
