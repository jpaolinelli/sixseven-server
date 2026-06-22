// QA adversarial tests for GDB-891:
// check_wal_accumulation void→std::vector<std::string> change.
//
// Focus areas:
//  1. MUTATION guards: comparisons and exemption logic
//  2. Multi-slot correctness (ordering independence)
//  3. Boundary values (retained == threshold vs threshold+1)
//  4. Prod caller still builds (ignoring return value compiles)

#include "sixseven/server/replication_slot.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

using namespace sixseven;

namespace {

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() / ("qa_gdb891_" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

// ---------------------------------------------------------------------------
// 1. MUTATION GUARDS
// ---------------------------------------------------------------------------

// If `retained_bytes > wal_retention_warning_bytes_` were flipped to `<`,
// WalAccumulationWarning would return empty. This test detects that mutation.
TEST(QA_GDB891_MutationGuards, OverThresholdReturnsSlotName) {
    TempDir dir;
    // threshold=100, retained=(300-100)=200 → strictly over threshold
    ReplicationSlotManager mgr(dir.path(), 100);

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    mgr.update_confirmed_lsn("replica_1", 100);
    ASSERT_TRUE(mgr.deactivate_slot("replica_1").has_value());

    auto warned = mgr.check_wal_accumulation(300);
    ASSERT_EQ(warned.size(), 1u) << "Expected exactly one warned slot";
    EXPECT_EQ(warned[0], "replica_1");
}

// If the active-slot exemption `if (slot.active || ...)` were inverted,
// active slots would appear in the returned list. This test detects that mutation.
TEST(QA_GDB891_MutationGuards, ActiveSlotNeverReturned) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path(), 100);

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    ASSERT_TRUE(mgr.activate_slot("replica_1", 100).has_value());
    mgr.update_confirmed_lsn("replica_1", 100);
    // slot is active; retained=(300-100)=200 > 100 threshold but must be exempt

    auto warned = mgr.check_wal_accumulation(300);
    EXPECT_TRUE(warned.empty()) << "Active slot must never appear in warned list";
}

// If the active check were removed entirely, active slots would be warned.
// Explicitly verify the active flag is what causes the exemption by toggling it.
TEST(QA_GDB891_MutationGuards, DeactivatingSlotMakesItWarnWorthy) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path(), 100);

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    ASSERT_TRUE(mgr.activate_slot("replica_1", 50).has_value());

    // While active: no warning
    auto warned_active = mgr.check_wal_accumulation(300);
    EXPECT_TRUE(warned_active.empty());

    // After deactivation: warning fires
    ASSERT_TRUE(mgr.deactivate_slot("replica_1").has_value());
    auto warned_inactive = mgr.check_wal_accumulation(300);
    ASSERT_EQ(warned_inactive.size(), 1u);
    EXPECT_EQ(warned_inactive[0], "replica_1");
}

// ---------------------------------------------------------------------------
// 2. MULTI-SLOT CORRECTNESS
// ---------------------------------------------------------------------------

// Two inactive over-threshold slots → both names returned (order may vary).
TEST(QA_GDB891_MultiSlot, TwoInactiveOverThresholdBothReturned) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path(), 100);

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    mgr.update_confirmed_lsn("replica_1", 50);
    ASSERT_TRUE(mgr.deactivate_slot("replica_1").has_value());

    ASSERT_TRUE(mgr.create_slot("replica_2").has_value());
    mgr.update_confirmed_lsn("replica_2", 80);
    ASSERT_TRUE(mgr.deactivate_slot("replica_2").has_value());

    // retained for replica_1 = 300-50 = 250 > 100
    // retained for replica_2 = 300-80 = 220 > 100
    auto warned = mgr.check_wal_accumulation(300);
    ASSERT_EQ(warned.size(), 2u) << "Both slots should be in the warned list";

    std::vector<std::string> sorted = warned;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(sorted[0], "replica_1");
    EXPECT_EQ(sorted[1], "replica_2");
}

// Mix: active (excluded), inactive-over-threshold (included),
//       inactive-under-threshold (excluded) → exactly one name returned.
TEST(QA_GDB891_MultiSlot, MixedSlotsReturnsOnlyOverThresholdInactive) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path(), 100);

    // active_slot: active → must be excluded regardless of WAL retained
    ASSERT_TRUE(mgr.create_slot("active_slot").has_value());
    ASSERT_TRUE(mgr.activate_slot("active_slot", 50).has_value());

    // over_slot: inactive, retained=250 > 100 → must be included
    ASSERT_TRUE(mgr.create_slot("over_slot").has_value());
    mgr.update_confirmed_lsn("over_slot", 50);
    ASSERT_TRUE(mgr.deactivate_slot("over_slot").has_value());

    // under_slot: inactive, retained=20 < 100 → must be excluded
    ASSERT_TRUE(mgr.create_slot("under_slot").has_value());
    mgr.update_confirmed_lsn("under_slot", 280);
    ASSERT_TRUE(mgr.deactivate_slot("under_slot").has_value());

    auto warned = mgr.check_wal_accumulation(300);
    ASSERT_EQ(warned.size(), 1u) << "Only the inactive over-threshold slot should be warned";
    EXPECT_EQ(warned[0], "over_slot");
}

// Empty slot map → empty result, no crash.
TEST(QA_GDB891_MultiSlot, NoSlotsReturnsEmpty) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path(), 100);

    auto warned = mgr.check_wal_accumulation(300);
    EXPECT_TRUE(warned.empty());
}

// ---------------------------------------------------------------------------
// 3. BOUNDARY VALUES
// ---------------------------------------------------------------------------

// retained == threshold → NOT warned (strict > required).
TEST(QA_GDB891_Boundary, RetainedExactlyEqualThresholdNotWarned) {
    TempDir dir;
    // threshold=200, retained=(300-100)=200 → exactly equal, must NOT warn
    ReplicationSlotManager mgr(dir.path(), 200);

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    mgr.update_confirmed_lsn("replica_1", 100);
    ASSERT_TRUE(mgr.deactivate_slot("replica_1").has_value());

    auto warned = mgr.check_wal_accumulation(300);
    EXPECT_TRUE(warned.empty())
        << "retained==threshold must not trigger warning (comparison is strict >)";
}

// retained == threshold + 1 → IS warned.
TEST(QA_GDB891_Boundary, RetainedOneOverThresholdIsWarned) {
    TempDir dir;
    // threshold=199, retained=(300-100)=200 → 200 > 199, must warn
    ReplicationSlotManager mgr(dir.path(), 199);

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    mgr.update_confirmed_lsn("replica_1", 100);
    ASSERT_TRUE(mgr.deactivate_slot("replica_1").has_value());

    auto warned = mgr.check_wal_accumulation(300);
    ASSERT_EQ(warned.size(), 1u);
    EXPECT_EQ(warned[0], "replica_1");
}

// current_lsn <= restart_lsn → retained bytes would be 0 or negative (unsigned
// wraps); the implementation only enters the warning path when current_lsn >
// restart_lsn, so this must never warn and must not crash.
TEST(QA_GDB891_Boundary, CurrentLsnBelowRestartLsnNoWarn) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path(), 100);

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    mgr.update_confirmed_lsn("replica_1", 500);
    ASSERT_TRUE(mgr.deactivate_slot("replica_1").has_value());

    // current_lsn (300) < restart_lsn (500) → no warning
    auto warned = mgr.check_wal_accumulation(300);
    EXPECT_TRUE(warned.empty());
}

// current_lsn == restart_lsn → retained == 0, no warning.
TEST(QA_GDB891_Boundary, CurrentLsnEqualsRestartLsnNoWarn) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path(), 0); // threshold=0 so any retained>0 would warn
    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    mgr.update_confirmed_lsn("replica_1", 300);
    ASSERT_TRUE(mgr.deactivate_slot("replica_1").has_value());

    auto warned = mgr.check_wal_accumulation(300);
    EXPECT_TRUE(warned.empty()) << "retained==0 must not warn even with threshold=0";
}

// ---------------------------------------------------------------------------
// 4. INVALID LSN EXEMPTION
// ---------------------------------------------------------------------------

// A slot with restart_lsn == invalid_lsn (never consumed any WAL) must also
// be exempted even though it's inactive.
TEST(QA_GDB891_InvalidLsn, NewlyCreatedSlotWithInvalidLsnNotWarned) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path(), 0); // threshold=0 → anything > 0 warns

    // create_slot leaves restart_lsn == invalid_lsn and active==false
    ASSERT_TRUE(mgr.create_slot("fresh_slot").has_value());
    // Never update_confirmed_lsn, so restart_lsn stays invalid_lsn

    auto warned = mgr.check_wal_accumulation(9999);
    EXPECT_TRUE(warned.empty()) << "Slot with invalid_lsn must be exempt from warnings";
}

// ---------------------------------------------------------------------------
// 5. PROD-CALLER SMOKE: ignoring the return value must compile (no [[nodiscard]])
// ---------------------------------------------------------------------------

// This test exercises the exact call pattern used by replication_health_monitor.cpp
// (discarding the return value). If the implementer added [[nodiscard]] this would
// generate a -Werror warning and fail to compile.
TEST(QA_GDB891_ProdCaller, DiscardReturnValueCompiles) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path(), 100);
    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    mgr.update_confirmed_lsn("replica_1", 100);
    ASSERT_TRUE(mgr.deactivate_slot("replica_1").has_value());

    // Intentional discard — mirrors how replication_health_monitor calls it.
    mgr.check_wal_accumulation(300); // return value discarded on purpose
    SUCCEED();                       // reaching here means the call compiled and ran without crash
}
