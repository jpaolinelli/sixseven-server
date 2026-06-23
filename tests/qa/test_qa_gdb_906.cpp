#include "sixseven/common/config.h"
#include "sixseven/common/status.h"
#include "sixseven/server/promotion_manager.h"
#include "sixseven/server/wal_receiver.h"
#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

using namespace sixseven;

// Helper: build a Config with max_lag_bytes set
static Config make_cfg(int64_t max_lag) {
    Config cfg = Config::load_defaults();
    cfg.replication_promote_max_lag_bytes = max_lag;
    return cfg;
}

// Helper: build a ReplicationState with explicit lsns
static ReplicationState make_state(lsn_t received, lsn_t applied) {
    ReplicationState s;
    s.received_lsn = received;
    s.applied_lsn  = applied;
    return s;
}

// =============================================================================
// GDB906 - Lag Guard adversarial tests for check_lag_guard()
// =============================================================================

// --- AC: limit disabled (0) is always ok regardless of lag magnitude ---

TEST(QA_GDB906_LagGuard, LimitDisabledZeroLag) {
    auto cfg   = make_cfg(0);
    auto state = make_state(500, 500);
    EXPECT_TRUE(PromotionManager::check_lag_guard(cfg, state).has_value());
}

TEST(QA_GDB906_LagGuard, LimitDisabledMassiveLag) {
    auto cfg   = make_cfg(0);
    auto state = make_state(1000000, 0);
    EXPECT_TRUE(PromotionManager::check_lag_guard(cfg, state).has_value());
}

// --- AC: lag strictly over limit returns REPLICATION_ERROR ---

TEST(QA_GDB906_LagGuard, LagOverLimitReturnsReplicationError) {
    auto cfg   = make_cfg(100);
    auto state = make_state(200, 99); // lag = 101 > 100
    auto r     = PromotionManager::check_lag_guard(cfg, state);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::REPLICATION_ERROR);
    EXPECT_NE(r.error().message.find("replay lag too high"), std::string::npos);
}

TEST(QA_GDB906_LagGuard, LagOneBytePastLimitReturnsError) {
    auto cfg   = make_cfg(100);
    auto state = make_state(201, 100); // lag = 101
    auto r     = PromotionManager::check_lag_guard(cfg, state);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::REPLICATION_ERROR);
}

// --- AC: lag exactly at limit is ok (strict >) ---

TEST(QA_GDB906_LagGuard, LagExactlyAtLimitIsOk) {
    auto cfg   = make_cfg(100);
    auto state = make_state(200, 100); // lag = 100, not > 100
    EXPECT_TRUE(PromotionManager::check_lag_guard(cfg, state).has_value());
}

// --- AC: zero lag is always ok when limit active ---

TEST(QA_GDB906_LagGuard, ZeroLagWithLimitActiveIsOk) {
    auto cfg   = make_cfg(1);
    auto state = make_state(500, 500); // lag = 0
    EXPECT_TRUE(PromotionManager::check_lag_guard(cfg, state).has_value());
}

// --- AC: invalid_lsn sentinels skip the guard entirely ---

TEST(QA_GDB906_LagGuard, InvalidLsnsSkipGuardBothInvalid) {
    auto cfg = make_cfg(1); // limit active
    ReplicationState state;  // both default to invalid_lsn (0)
    EXPECT_TRUE(PromotionManager::check_lag_guard(cfg, state).has_value());
}

TEST(QA_GDB906_LagGuard, InvalidLsnsSkipGuardReceivedInvalid) {
    auto cfg   = make_cfg(1);
    auto state = make_state(invalid_lsn, 500);
    EXPECT_TRUE(PromotionManager::check_lag_guard(cfg, state).has_value());
}

TEST(QA_GDB906_LagGuard, InvalidLsnsSkipGuardAppliedInvalid) {
    auto cfg   = make_cfg(1);
    auto state = make_state(500, invalid_lsn);
    // received_lsn(500) != invalid_lsn(0) but applied_lsn == invalid_lsn:
    // guard condition short-circuits -> ok()
    EXPECT_TRUE(PromotionManager::check_lag_guard(cfg, state).has_value());
}

// --- Adversarial: applied_lsn > received_lsn (backwards / impossible in normal
//     operation but possible via unsigned arithmetic). The guard uses
//     received_lsn > applied_lsn as a gating condition, so backwards LSNs
//     never enter the lag arithmetic and must return ok(). ---

TEST(QA_GDB906_LagGuard, BackwardsLsnsReturnOk) {
    auto cfg   = make_cfg(1); // tiny limit active
    // applied ahead of received - should not trip guard
    auto state = make_state(100, 200);
    EXPECT_TRUE(PromotionManager::check_lag_guard(cfg, state).has_value())
        << "backwards LSNs (applied > received) must not trip the lag guard";
}

// --- Adversarial: limit of 1 (minimum non-zero threshold) ---

TEST(QA_GDB906_LagGuard, LimitOneExactLagOfOneIsOk) {
    auto cfg   = make_cfg(1);
    auto state = make_state(1001, 1000); // lag = 1, not > 1
    EXPECT_TRUE(PromotionManager::check_lag_guard(cfg, state).has_value());
}

TEST(QA_GDB906_LagGuard, LimitOneExactLagOfTwoIsError) {
    auto cfg   = make_cfg(1);
    auto state = make_state(1002, 1000); // lag = 2 > 1
    auto r     = PromotionManager::check_lag_guard(cfg, state);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, StatusCode::REPLICATION_ERROR);
}

// --- Adversarial: INT64_MAX as limit (practical upper bound) ---

TEST(QA_GDB906_LagGuard, LimitInt64MaxAllowsLargeLag) {
    auto cfg   = make_cfg(std::numeric_limits<int64_t>::max());
    // lag = INT64_MAX - 1, still <= INT64_MAX
    lsn_t applied   = 1;
    lsn_t received  = static_cast<lsn_t>(std::numeric_limits<int64_t>::max());
    auto state = make_state(received, applied);
    EXPECT_TRUE(PromotionManager::check_lag_guard(cfg, state).has_value());
}

// --- Adversarial: very large unsigned lag that wraps int64_t cast.
//     If received_lsn - applied_lsn > INT64_MAX the cast to int64_t
//     produces a negative number, which is NOT > any positive max_lag_bytes,
//     so the guard is silently bypassed (false negative / pre-existing behavior).
//     This test documents the actual behavior; if the behavior changes the test
//     will catch the regression. ---

TEST(QA_GDB906_LagGuard, UltraLargeLagCastBehaviorDocumented) {
    // Construct a lag of UINT64_MAX (received=UINT64_MAX-1, applied=invalid_lsn
    // is 0, so we use applied=0 which equals invalid_lsn — that would make the
    // guard skip). Use applied=1 so both LSNs are valid and the arithmetic fires.
    lsn_t received = std::numeric_limits<uint64_t>::max(); // UINT64_MAX
    lsn_t applied  = 1;                                    // lag = UINT64_MAX-1
    auto cfg       = make_cfg(100);
    auto state     = make_state(received, applied);
    // received_lsn > applied_lsn is true; lag cast = (int64_t)(UINT64_MAX-1) = -2
    // -2 > 100 is false -> guard passes (false negative, pre-existing behavior)
    // We assert the actual result so any change is caught:
    auto r = PromotionManager::check_lag_guard(cfg, state);
    // Document pre-existing false-negative: guard returns ok() for massive lag
    EXPECT_TRUE(r.has_value())
        << "Pre-existing: int64 cast wraps on huge unsigned lag; guard is bypassed. "
           "See GDB-906 QA report for filing details.";
}

// --- Mutation-grade: deleting the guard or inverting the comparison must fail ---

TEST(QA_GDB906_LagGuard, MutationKill_GuardDeletionWouldPass) {
    // If check_lag_guard were deleted (always returns ok()), this test would
    // fail because we ASSERT an error below.
    auto cfg   = make_cfg(50);
    auto state = make_state(200, 100); // lag = 100 > 50
    auto r     = PromotionManager::check_lag_guard(cfg, state);
    ASSERT_FALSE(r.has_value())
        << "Must return error when lag > limit; deleting the guard would break this";
    EXPECT_EQ(r.error().code, StatusCode::REPLICATION_ERROR);
    EXPECT_NE(r.error().message.find("replay lag too high"), std::string::npos);
}

TEST(QA_GDB906_LagGuard, MutationKill_ComparisonInversionWouldFail) {
    // If the comparison were inverted to >=, LagExactlyAtLimitIsOk would still
    // pass, but this exact-boundary test (lag == limit) must return ok().
    auto cfg   = make_cfg(100);
    auto state = make_state(200, 100); // lag exactly 100
    auto r     = PromotionManager::check_lag_guard(cfg, state);
    ASSERT_TRUE(r.has_value())
        << "Exact boundary (lag == limit) must succeed; inverted >= would break this";
}

// --- Mutation-grade: error message content ---

TEST(QA_GDB906_LagGuard, ErrorMessageContainsLagTooHigh) {
    auto cfg   = make_cfg(10);
    auto state = make_state(100, 50); // lag = 50 > 10
    auto r     = PromotionManager::check_lag_guard(cfg, state);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("replay lag too high"), std::string::npos)
        << "Error message must contain 'replay lag too high'; got: " << r.error().message;
}
