#include "giodb/server/replication_slot.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

using namespace giodb;

namespace {

/// Temporary directory with automatic cleanup.
class TempDir {
public:
    TempDir() {
        path_ =
            std::filesystem::temp_directory_path() / ("slot_test_" + std::to_string(counter_++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    static inline std::atomic<int> counter_{0};
};

} // namespace

// -- Slot lifecycle -----------------------------------------------------------

TEST(ReplicationSlot, CreateSlot) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    auto result = mgr.create_slot("replica_1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->slot_name, "replica_1");
    EXPECT_EQ(result->slot_type, "physical");
    EXPECT_FALSE(result->active);
    EXPECT_EQ(result->restart_lsn, invalid_lsn);
    EXPECT_EQ(result->confirmed_flush_lsn, invalid_lsn);
    EXPECT_GT(result->created_at_us, 0u);
}

TEST(ReplicationSlot, CreateDuplicateSlot) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());

    auto result = mgr.create_slot("replica_1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::ALREADY_EXISTS);
}

TEST(ReplicationSlot, CreateEmptyNameRejected) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    auto result = mgr.create_slot("");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(ReplicationSlot, DropSlot) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    ASSERT_TRUE(mgr.drop_slot("replica_1").has_value());

    auto result = mgr.get_slot("replica_1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(ReplicationSlot, DropNonexistentSlot) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    auto result = mgr.drop_slot("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(ReplicationSlot, DropActiveSlotRejected) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    ASSERT_TRUE(mgr.activate_slot("replica_1", 100).has_value());

    auto result = mgr.drop_slot("replica_1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);

    // Deactivate, then drop should succeed.
    ASSERT_TRUE(mgr.deactivate_slot("replica_1").has_value());
    ASSERT_TRUE(mgr.drop_slot("replica_1").has_value());
}

TEST(ReplicationSlot, GetSlot) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());

    auto result = mgr.get_slot("replica_1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->slot_name, "replica_1");
}

TEST(ReplicationSlot, GetNonexistentSlot) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    auto result = mgr.get_slot("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(ReplicationSlot, ListSlots) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    EXPECT_TRUE(mgr.list_slots().empty());

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    ASSERT_TRUE(mgr.create_slot("replica_2").has_value());

    auto slots = mgr.list_slots();
    ASSERT_EQ(slots.size(), 2u);

    // Verify both slots are present (order not guaranteed).
    bool found_1 = false;
    bool found_2 = false;
    for (const auto& slot : slots) {
        if (slot.slot_name == "replica_1") {
            found_1 = true;
        }
        if (slot.slot_name == "replica_2") {
            found_2 = true;
        }
    }
    EXPECT_TRUE(found_1);
    EXPECT_TRUE(found_2);
}

// -- Activation / Deactivation ------------------------------------------------

TEST(ReplicationSlot, ActivateSlot) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    ASSERT_TRUE(mgr.activate_slot("replica_1", 100).has_value());

    auto slot = mgr.get_slot("replica_1");
    ASSERT_TRUE(slot.has_value());
    EXPECT_TRUE(slot->active);
    EXPECT_EQ(slot->restart_lsn, 100u);
}

TEST(ReplicationSlot, ActivateNonexistentSlot) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    auto result = mgr.activate_slot("nonexistent", 100);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(ReplicationSlot, DeactivateSlot) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    ASSERT_TRUE(mgr.activate_slot("replica_1", 100).has_value());
    ASSERT_TRUE(mgr.deactivate_slot("replica_1").has_value());

    auto slot = mgr.get_slot("replica_1");
    ASSERT_TRUE(slot.has_value());
    EXPECT_FALSE(slot->active);
    // restart_lsn should be retained after deactivation.
    EXPECT_EQ(slot->restart_lsn, 100u);
}

TEST(ReplicationSlot, DeactivateNonexistentSlot) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    auto result = mgr.deactivate_slot("nonexistent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(ReplicationSlot, ActivateWithInvalidLsn) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    ASSERT_TRUE(mgr.activate_slot("replica_1", invalid_lsn).has_value());

    auto slot = mgr.get_slot("replica_1");
    ASSERT_TRUE(slot.has_value());
    EXPECT_TRUE(slot->active);
    // restart_lsn should remain invalid since we passed invalid_lsn.
    EXPECT_EQ(slot->restart_lsn, invalid_lsn);
}

// -- Progress tracking --------------------------------------------------------

TEST(ReplicationSlot, UpdateConfirmedLsn) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    ASSERT_TRUE(mgr.activate_slot("replica_1", 50).has_value());

    mgr.update_confirmed_lsn("replica_1", 100);

    auto slot = mgr.get_slot("replica_1");
    ASSERT_TRUE(slot.has_value());
    EXPECT_EQ(slot->confirmed_flush_lsn, 100u);
    EXPECT_EQ(slot->restart_lsn, 100u);
}

TEST(ReplicationSlot, UpdateConfirmedLsnDoesNotRegress) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    mgr.update_confirmed_lsn("replica_1", 200);
    mgr.update_confirmed_lsn("replica_1", 100); // Lower — should be ignored.

    auto slot = mgr.get_slot("replica_1");
    ASSERT_TRUE(slot.has_value());
    EXPECT_EQ(slot->confirmed_flush_lsn, 200u);
    EXPECT_EQ(slot->restart_lsn, 200u);
}

TEST(ReplicationSlot, UpdateConfirmedLsnNonexistentSlot) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    // Should not crash.
    mgr.update_confirmed_lsn("nonexistent", 100);
}

// -- WAL retention ------------------------------------------------------------

TEST(ReplicationSlot, MinRestartLsnNoSlots) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    EXPECT_EQ(mgr.get_min_restart_lsn(), invalid_lsn);
}

TEST(ReplicationSlot, MinRestartLsnSingleSlot) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    mgr.update_confirmed_lsn("replica_1", 500);

    EXPECT_EQ(mgr.get_min_restart_lsn(), 500u);
}

TEST(ReplicationSlot, MinRestartLsnMultipleSlots) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    ASSERT_TRUE(mgr.create_slot("replica_2").has_value());
    ASSERT_TRUE(mgr.create_slot("replica_3").has_value());

    mgr.update_confirmed_lsn("replica_1", 300);
    mgr.update_confirmed_lsn("replica_2", 100);
    mgr.update_confirmed_lsn("replica_3", 500);

    EXPECT_EQ(mgr.get_min_restart_lsn(), 100u);
}

TEST(ReplicationSlot, MinRestartLsnIncludesInactiveSlots) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    ASSERT_TRUE(mgr.create_slot("active_replica").has_value());
    ASSERT_TRUE(mgr.create_slot("inactive_replica").has_value());

    ASSERT_TRUE(mgr.activate_slot("active_replica", 500).has_value());
    mgr.update_confirmed_lsn("active_replica", 500);

    mgr.update_confirmed_lsn("inactive_replica", 100);
    // inactive_replica has a lower restart_lsn — it should determine the minimum.

    EXPECT_EQ(mgr.get_min_restart_lsn(), 100u);
}

TEST(ReplicationSlot, MinRestartLsnWithUninitializedSlot) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    mgr.update_confirmed_lsn("replica_1", 500);

    ASSERT_TRUE(mgr.create_slot("replica_2").has_value());
    // replica_2 has restart_lsn = invalid_lsn (never connected).

    // Should return invalid_lsn since one slot needs all WAL.
    EXPECT_EQ(mgr.get_min_restart_lsn(), invalid_lsn);
}

TEST(ReplicationSlot, DropSlotReleasesRetention) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    ASSERT_TRUE(mgr.create_slot("slow_replica").has_value());
    ASSERT_TRUE(mgr.create_slot("fast_replica").has_value());

    mgr.update_confirmed_lsn("slow_replica", 100);
    mgr.update_confirmed_lsn("fast_replica", 500);

    EXPECT_EQ(mgr.get_min_restart_lsn(), 100u);

    // Drop the slow replica — retention should advance.
    ASSERT_TRUE(mgr.deactivate_slot("slow_replica").has_value());
    ASSERT_TRUE(mgr.drop_slot("slow_replica").has_value());

    EXPECT_EQ(mgr.get_min_restart_lsn(), 500u);
}

// -- Persistence --------------------------------------------------------------

TEST(ReplicationSlot, SaveAndLoad) {
    TempDir dir;

    // Create and persist slots.
    {
        ReplicationSlotManager mgr(dir.path());
        ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
        ASSERT_TRUE(mgr.create_slot("replica_2").has_value());

        ASSERT_TRUE(mgr.activate_slot("replica_1", 100).has_value());
        mgr.update_confirmed_lsn("replica_1", 200);
        mgr.update_confirmed_lsn("replica_2", 50);

        ASSERT_TRUE(mgr.save().has_value());
    }

    // Load into a new manager.
    {
        ReplicationSlotManager mgr(dir.path());
        ASSERT_TRUE(mgr.load().has_value());

        auto slots = mgr.list_slots();
        ASSERT_EQ(slots.size(), 2u);

        auto slot1 = mgr.get_slot("replica_1");
        ASSERT_TRUE(slot1.has_value());
        EXPECT_EQ(slot1->slot_name, "replica_1");
        EXPECT_FALSE(slot1->active); // All slots inactive after load.
        EXPECT_EQ(slot1->restart_lsn, 200u);
        EXPECT_EQ(slot1->confirmed_flush_lsn, 200u);

        auto slot2 = mgr.get_slot("replica_2");
        ASSERT_TRUE(slot2.has_value());
        EXPECT_EQ(slot2->slot_name, "replica_2");
        EXPECT_FALSE(slot2->active);
        EXPECT_EQ(slot2->restart_lsn, 50u);
        EXPECT_EQ(slot2->confirmed_flush_lsn, 50u);
    }
}

TEST(ReplicationSlot, LoadNoPersistenceFile) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    // Should succeed with no slots.
    ASSERT_TRUE(mgr.load().has_value());
    EXPECT_TRUE(mgr.list_slots().empty());
}

TEST(ReplicationSlot, PersistenceAfterCreate) {
    TempDir dir;

    // Create a slot (auto-persists).
    {
        ReplicationSlotManager mgr(dir.path());
        ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    }

    // Verify it was persisted.
    {
        ReplicationSlotManager mgr(dir.path());
        ASSERT_TRUE(mgr.load().has_value());
        ASSERT_EQ(mgr.list_slots().size(), 1u);
        EXPECT_EQ(mgr.list_slots()[0].slot_name, "replica_1");
    }
}

TEST(ReplicationSlot, PersistenceAfterDrop) {
    TempDir dir;

    // Create two slots.
    {
        ReplicationSlotManager mgr(dir.path());
        ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
        ASSERT_TRUE(mgr.create_slot("replica_2").has_value());
    }

    // Drop one.
    {
        ReplicationSlotManager mgr(dir.path());
        ASSERT_TRUE(mgr.load().has_value());
        ASSERT_TRUE(mgr.drop_slot("replica_1").has_value());
    }

    // Verify only one remains.
    {
        ReplicationSlotManager mgr(dir.path());
        ASSERT_TRUE(mgr.load().has_value());
        ASSERT_EQ(mgr.list_slots().size(), 1u);
        EXPECT_EQ(mgr.list_slots()[0].slot_name, "replica_2");
    }
}

// -- WAL accumulation warning -------------------------------------------------

TEST(ReplicationSlot, WalAccumulationWarning) {
    TempDir dir;
    // Set a low threshold so the warning triggers easily.
    ReplicationSlotManager mgr(dir.path(), 100);

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    mgr.update_confirmed_lsn("replica_1", 100);
    ASSERT_TRUE(mgr.deactivate_slot("replica_1").has_value());

    // Current LSN is 300, retained = 200 > 100 threshold.
    // This should log a warning (we can't assert on logs, but it should not crash).
    mgr.check_wal_accumulation(300);
}

TEST(ReplicationSlot, WalAccumulationNoWarningForActive) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path(), 100);

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    ASSERT_TRUE(mgr.activate_slot("replica_1", 100).has_value());
    mgr.update_confirmed_lsn("replica_1", 100);

    // Should not warn for active slots.
    mgr.check_wal_accumulation(300);
}

// -- Concurrent access --------------------------------------------------------

TEST(ReplicationSlot, ConcurrentCreateAndList) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    constexpr int num_threads = 8;
    constexpr int slots_per_thread = 10;

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&mgr, t] {
            for (int i = 0; i < slots_per_thread; ++i) {
                std::string name = "slot_" + std::to_string(t) + "_" + std::to_string(i);
                auto result = mgr.create_slot(name);
                EXPECT_TRUE(result.has_value());
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto slots = mgr.list_slots();
    EXPECT_EQ(slots.size(), static_cast<size_t>(num_threads * slots_per_thread));
}

TEST(ReplicationSlot, ConcurrentUpdateConfirmedLsn) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());
    ASSERT_TRUE(mgr.activate_slot("replica_1", 1).has_value());

    constexpr int num_threads = 4;
    constexpr int updates_per_thread = 100;

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&mgr, t] {
            for (int i = 0; i < updates_per_thread; ++i) {
                lsn_t lsn = static_cast<lsn_t>(t * updates_per_thread + i + 1);
                mgr.update_confirmed_lsn("replica_1", lsn);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto slot = mgr.get_slot("replica_1");
    ASSERT_TRUE(slot.has_value());
    // The final confirmed_flush_lsn should be the maximum across all updates.
    EXPECT_EQ(slot->confirmed_flush_lsn, static_cast<lsn_t>(num_threads * updates_per_thread));
}

TEST(ReplicationSlot, ConcurrentActivateDeactivate) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    ASSERT_TRUE(mgr.create_slot("replica_1").has_value());

    constexpr int iterations = 50;
    std::vector<std::thread> threads;

    threads.emplace_back([&mgr] {
        for (int i = 0; i < iterations; ++i) {
            (void)mgr.activate_slot("replica_1", static_cast<lsn_t>(i + 1));
        }
    });

    threads.emplace_back([&mgr] {
        for (int i = 0; i < iterations; ++i) {
            (void)mgr.deactivate_slot("replica_1");
        }
    });

    for (auto& t : threads) {
        t.join();
    }

    // Should not crash or deadlock; slot should be in a valid state.
    auto slot = mgr.get_slot("replica_1");
    ASSERT_TRUE(slot.has_value());
}

// -- Multiple slots WAL retention integration ---------------------------------

TEST(ReplicationSlot, MultipleSlotRetentionScenario) {
    TempDir dir;
    ReplicationSlotManager mgr(dir.path());

    // Create three slots simulating replicas at different progress levels.
    ASSERT_TRUE(mgr.create_slot("dc_east").has_value());
    ASSERT_TRUE(mgr.create_slot("dc_west").has_value());
    ASSERT_TRUE(mgr.create_slot("analytics").has_value());

    ASSERT_TRUE(mgr.activate_slot("dc_east", 1000).has_value());
    ASSERT_TRUE(mgr.activate_slot("dc_west", 800).has_value());
    ASSERT_TRUE(mgr.activate_slot("analytics", 200).has_value());

    mgr.update_confirmed_lsn("dc_east", 1000);
    mgr.update_confirmed_lsn("dc_west", 800);
    mgr.update_confirmed_lsn("analytics", 200);

    // The analytics replica is the slowest — it determines the retention point.
    EXPECT_EQ(mgr.get_min_restart_lsn(), 200u);

    // Analytics catches up.
    mgr.update_confirmed_lsn("analytics", 900);
    EXPECT_EQ(mgr.get_min_restart_lsn(), 800u); // dc_west is now the slowest.

    // dc_west catches up too.
    mgr.update_confirmed_lsn("dc_west", 1000);
    EXPECT_EQ(mgr.get_min_restart_lsn(), 900u); // analytics.

    // All catch up to 1000.
    mgr.update_confirmed_lsn("analytics", 1000);
    EXPECT_EQ(mgr.get_min_restart_lsn(), 1000u);
}
