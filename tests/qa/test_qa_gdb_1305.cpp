// QA regression tests for GDB-1305: unbounded hash-index bucket-split
// caused by weak low-bit hash mixing in HashIndex::hash_key().
//
// The implementer's fix (fmix64 avalanche finalizer) was verified only
// against small-integer keys. These tests adversarially confirm the fix
// holds for the OTHER Value types that feed directory_index()'s low-bit
// extraction: strings (including pathological shared-prefix strings),
// UUIDs, DATE/TIMESTAMP, and composite multi-column keys -- plus a
// re-derivation of the original integer repro.

#include "sixseven/index/btree_key.h"
#include "sixseven/index/hash_index.h"
#include "sixseven/index/rid.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sixseven::test {

namespace {

RID mk_rid(uint32_t page_id, uint16_t slot_id = 0) {
    return RID{page_id, slot_id};
}

// Bound used throughout: a healthy extendible hash over a few hundred
// well-distributed keys with a small bucket capacity needs single-digit to
// low-double-digit bits. The original bug grew global_depth_ by one on
// every post-overflow insert, so it would blow past this bound immediately.
constexpr uint32_t kBoundedDepth = 20;

} // namespace

// ---------------------------------------------------------------------------
// 1. Re-derivation of the original integer repro (regression guard).
// ---------------------------------------------------------------------------

TEST(QA_GDB1305, IntegerKeysManySplitsStayBounded) {
    HashIndexConfig config;
    config.key_types = {TypeId::INT64};
    config.bucket_capacity = 4;
    HashIndex idx(std::move(config));

    constexpr int N = 500;
    for (int i = 1; i <= N; ++i) {
        auto r = idx.insert({Value(static_cast<int64_t>(i))}, mk_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value()) << "insert failed at i=" << i;
    }

    EXPECT_EQ(idx.size(), static_cast<uint64_t>(N));
    EXPECT_LT(idx.global_depth(), kBoundedDepth);
    EXPECT_GT(idx.bucket_count(), 1u);

    for (int i = 1; i <= N; ++i) {
        auto r = idx.search({Value(static_cast<int64_t>(i))});
        ASSERT_TRUE(r.has_value());
        ASSERT_TRUE(r->has_value()) << "missing key=" << i;
        EXPECT_EQ(r->value().page_id, static_cast<uint32_t>(i));
    }
}

// ---------------------------------------------------------------------------
// 2. STRING keys, including a pathological shared-prefix set that could
//    collide differently than integers if the finalizer only mixed
//    high bits or relied on std::hash<std::string> alone.
// ---------------------------------------------------------------------------

TEST(QA_GDB1305, StringKeysSplitWithBoundedDepth) {
    HashIndexConfig config;
    config.key_types = {TypeId::STRING};
    config.bucket_capacity = 4;
    HashIndex idx(std::move(config));

    constexpr int N = 400;
    std::vector<std::string> keys;
    keys.reserve(N);
    for (int i = 0; i < N; ++i) {
        keys.push_back("k" + std::to_string(i));
    }
    for (int i = 0; i < N; ++i) {
        auto r = idx.insert({Value(keys[static_cast<size_t>(i)])},
                             mk_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value()) << "insert failed at i=" << i;
    }

    EXPECT_LT(idx.global_depth(), kBoundedDepth)
        << "string keys triggered unbounded directory growth";
    EXPECT_GT(idx.bucket_count(), 1u);

    for (int i = 0; i < N; ++i) {
        auto r = idx.search({Value(keys[static_cast<size_t>(i)])});
        ASSERT_TRUE(r.has_value());
        ASSERT_TRUE(r->has_value()) << "missing key=" << keys[static_cast<size_t>(i)];
        EXPECT_EQ(r->value().page_id, static_cast<uint32_t>(i));
    }
}

TEST(QA_GDB1305, SharedPrefixStringKeysSplitWithBoundedDepth) {
    // All keys share a long common prefix and differ only in the last
    // couple of characters -- a classic pathological case for hash
    // functions/mixers that under-weight the tail of the input.
    HashIndexConfig config;
    config.key_types = {TypeId::STRING};
    config.bucket_capacity = 4;
    HashIndex idx(std::move(config));

    constexpr int N = 300;
    std::vector<std::string> keys;
    keys.reserve(N);
    const std::string prefix(64, 'a');
    for (int i = 0; i < N; ++i) {
        keys.push_back(prefix + std::to_string(i));
    }
    for (int i = 0; i < N; ++i) {
        auto r = idx.insert({Value(keys[static_cast<size_t>(i)])},
                             mk_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value()) << "insert failed at i=" << i;
    }

    EXPECT_LT(idx.global_depth(), kBoundedDepth)
        << "shared-prefix string keys triggered unbounded directory growth";
    EXPECT_GT(idx.bucket_count(), 1u);

    for (int i = 0; i < N; ++i) {
        auto r = idx.search({Value(keys[static_cast<size_t>(i)])});
        ASSERT_TRUE(r.has_value());
        ASSERT_TRUE(r->has_value()) << "missing shared-prefix key index=" << i;
        EXPECT_EQ(r->value().page_id, static_cast<uint32_t>(i));
    }
}

// ---------------------------------------------------------------------------
// 3. UUID keys, sequential in their low bytes (mirrors how sequential
//    UUIDv7/ULID-style primary keys behave in practice).
// ---------------------------------------------------------------------------

TEST(QA_GDB1305, SequentialUuidKeysSplitWithBoundedDepth) {
    HashIndexConfig config;
    config.key_types = {TypeId::UUID};
    config.bucket_capacity = 4;
    HashIndex idx(std::move(config));

    constexpr int N = 300;
    std::vector<Uuid> keys;
    keys.reserve(N);
    for (int i = 0; i < N; ++i) {
        Uuid u{};
        u.fill(0);
        // Vary only the low-order bytes sequentially, leaving all
        // high-order bytes identical -- the UUID analogue of the
        // small-integer repro's all-zero mantissa bits.
        u[15] = static_cast<uint8_t>(i & 0xFF);
        u[14] = static_cast<uint8_t>((i >> 8) & 0xFF);
        keys.push_back(u);
    }
    for (int i = 0; i < N; ++i) {
        auto r =
            idx.insert({Value(keys[static_cast<size_t>(i)])}, mk_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value()) << "insert failed at i=" << i;
    }

    EXPECT_LT(idx.global_depth(), kBoundedDepth)
        << "sequential UUID keys triggered unbounded directory growth";
    EXPECT_GT(idx.bucket_count(), 1u);

    for (int i = 0; i < N; ++i) {
        auto r = idx.search({Value(keys[static_cast<size_t>(i)])});
        ASSERT_TRUE(r.has_value());
        ASSERT_TRUE(r->has_value()) << "missing uuid key index=" << i;
        EXPECT_EQ(r->value().page_id, static_cast<uint32_t>(i));
    }
}

// ---------------------------------------------------------------------------
// 4. DATE and TIMESTAMP keys, sequential (the exact shape of a
//    date-partitioned or time-series primary key).
// ---------------------------------------------------------------------------

TEST(QA_GDB1305, SequentialDateKeysSplitWithBoundedDepth) {
    HashIndexConfig config;
    config.key_types = {TypeId::DATE};
    config.bucket_capacity = 4;
    HashIndex idx(std::move(config));

    constexpr int N = 300;
    for (int i = 0; i < N; ++i) {
        Date d{};
        d.days_since_epoch = 19000 + i; // sequential calendar days
        auto r = idx.insert({Value(d)}, mk_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value()) << "insert failed at i=" << i;
    }

    EXPECT_LT(idx.global_depth(), kBoundedDepth)
        << "sequential DATE keys triggered unbounded directory growth";
    EXPECT_GT(idx.bucket_count(), 1u);

    for (int i = 0; i < N; ++i) {
        Date d{};
        d.days_since_epoch = 19000 + i;
        auto r = idx.search({Value(d)});
        ASSERT_TRUE(r.has_value());
        ASSERT_TRUE(r->has_value()) << "missing date key index=" << i;
        EXPECT_EQ(r->value().page_id, static_cast<uint32_t>(i));
    }
}

TEST(QA_GDB1305, SequentialTimestampKeysSplitWithBoundedDepth) {
    HashIndexConfig config;
    config.key_types = {TypeId::TIMESTAMP};
    config.bucket_capacity = 4;
    HashIndex idx(std::move(config));

    constexpr int N = 300;
    constexpr int64_t base_us = 1'700'000'000LL * 1'000'000LL;
    for (int i = 0; i < N; ++i) {
        Timestamp ts{};
        ts.microseconds = base_us + i; // sequential, 1us apart
        auto r = idx.insert({Value(ts)}, mk_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value()) << "insert failed at i=" << i;
    }

    EXPECT_LT(idx.global_depth(), kBoundedDepth)
        << "sequential TIMESTAMP keys triggered unbounded directory growth";
    EXPECT_GT(idx.bucket_count(), 1u);

    for (int i = 0; i < N; ++i) {
        Timestamp ts{};
        ts.microseconds = base_us + i;
        auto r = idx.search({Value(ts)});
        ASSERT_TRUE(r.has_value());
        ASSERT_TRUE(r->has_value()) << "missing timestamp key index=" << i;
        EXPECT_EQ(r->value().page_id, static_cast<uint32_t>(i));
    }
}

// ---------------------------------------------------------------------------
// 5. Composite (multi-column) keys -- exercises the boost::hash_combine
//    loop across iterations before the single finalizer step runs.
// ---------------------------------------------------------------------------

TEST(QA_GDB1305, CompositeIntStringKeysSplitWithBoundedDepth) {
    HashIndexConfig config;
    config.key_types = {TypeId::INT64, TypeId::STRING};
    config.bucket_capacity = 4;
    HashIndex idx(std::move(config));

    constexpr int N = 300;
    for (int i = 0; i < N; ++i) {
        KeyType key = {Value(static_cast<int64_t>(i)), Value(std::string("row") + std::to_string(i))};
        auto r = idx.insert(key, mk_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value()) << "insert failed at i=" << i;
    }

    EXPECT_LT(idx.global_depth(), kBoundedDepth)
        << "composite keys triggered unbounded directory growth";
    EXPECT_GT(idx.bucket_count(), 1u);

    for (int i = 0; i < N; ++i) {
        KeyType key = {Value(static_cast<int64_t>(i)), Value(std::string("row") + std::to_string(i))};
        auto r = idx.search(key);
        ASSERT_TRUE(r.has_value());
        ASSERT_TRUE(r->has_value()) << "missing composite key index=" << i;
        EXPECT_EQ(r->value().page_id, static_cast<uint32_t>(i));
    }
}

// Composite key where the FIRST column is constant across all rows and only
// the second column varies -- this stresses the hash_combine loop's
// dependency on the running `combined` seed rather than on a fresh h each
// time, which is a distinct failure mode from a single-column key.
TEST(QA_GDB1305, CompositeConstantFirstColumnKeysSplitWithBoundedDepth) {
    HashIndexConfig config;
    config.key_types = {TypeId::INT64, TypeId::INT64};
    config.bucket_capacity = 4;
    HashIndex idx(std::move(config));

    constexpr int N = 300;
    for (int i = 0; i < N; ++i) {
        KeyType key = {Value(static_cast<int64_t>(0)), Value(static_cast<int64_t>(i))};
        auto r = idx.insert(key, mk_rid(static_cast<uint32_t>(i)));
        ASSERT_TRUE(r.has_value()) << "insert failed at i=" << i;
    }

    EXPECT_LT(idx.global_depth(), kBoundedDepth)
        << "constant-first-column composite keys triggered unbounded directory growth";
    EXPECT_GT(idx.bucket_count(), 1u);

    for (int i = 0; i < N; ++i) {
        KeyType key = {Value(static_cast<int64_t>(0)), Value(static_cast<int64_t>(i))};
        auto r = idx.search(key);
        ASSERT_TRUE(r.has_value());
        ASSERT_TRUE(r->has_value()) << "missing composite (const,varies) key index=" << i;
        EXPECT_EQ(r->value().page_id, static_cast<uint32_t>(i));
    }
}

} // namespace sixseven::test
