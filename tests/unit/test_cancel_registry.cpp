#include "sixseven/server/cancel_registry.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>

namespace sixseven {
namespace {

// -- CancelRegistry tests -----------------------------------------------------

TEST(CancelRegistry, RegisterAndRequestCancel_CorrectSecret_SetsFlag) {
    CancelRegistry registry;
    registry.register_connection(42, 12345);

    auto flag = std::make_shared<std::atomic<bool>>(false);
    registry.set_cancel_flag(42, flag);

    registry.request_cancel(42, 12345);

    EXPECT_TRUE(flag->load());
}

TEST(CancelRegistry, RequestCancel_WrongSecret_DoesNotSetFlag) {
    CancelRegistry registry;
    registry.register_connection(42, 12345);

    auto flag = std::make_shared<std::atomic<bool>>(false);
    registry.set_cancel_flag(42, flag);

    registry.request_cancel(42, 99999); // Wrong secret.

    EXPECT_FALSE(flag->load());
}

TEST(CancelRegistry, RequestCancel_UnknownPid_NoOp) {
    CancelRegistry registry;
    // No connection registered -- should not crash.
    registry.request_cancel(999, 12345);
}

TEST(CancelRegistry, RequestCancel_NoStatementRunning_NoOp) {
    CancelRegistry registry;
    registry.register_connection(42, 12345);
    // No flag installed -- no statement running.
    registry.request_cancel(42, 12345); // Should not crash or hang.
}

TEST(CancelRegistry, UnregisterRemovesEntry) {
    CancelRegistry registry;
    registry.register_connection(42, 12345);
    registry.unregister_connection(42);

    auto flag = std::make_shared<std::atomic<bool>>(false);
    // set_cancel_flag after unregister is a no-op.
    registry.set_cancel_flag(42, flag);
    registry.request_cancel(42, 12345); // Should not set flag.
    EXPECT_FALSE(flag->load());
}

TEST(CancelRegistry, ClearCancelFlag_PreventsSubsequentCancel) {
    CancelRegistry registry;
    registry.register_connection(42, 12345);

    auto flag = std::make_shared<std::atomic<bool>>(false);
    registry.set_cancel_flag(42, flag);

    registry.clear_cancel_flag(42);

    registry.request_cancel(42, 12345); // Flag cleared -- should be no-op.
    EXPECT_FALSE(flag->load());
}

TEST(CancelRegistry, ConcurrentCancelFromAnotherThread) {
    CancelRegistry registry;
    registry.register_connection(7, 777);

    auto flag = std::make_shared<std::atomic<bool>>(false);
    registry.set_cancel_flag(7, flag);

    std::thread cancel_thread([&registry]() { registry.request_cancel(7, 777); });
    cancel_thread.join();

    EXPECT_TRUE(flag->load());
}

TEST(CancelRegistry, MultipleConnections_OnlyCancelsCorrectPid) {
    CancelRegistry registry;
    registry.register_connection(1, 100);
    registry.register_connection(2, 200);

    auto flag1 = std::make_shared<std::atomic<bool>>(false);
    auto flag2 = std::make_shared<std::atomic<bool>>(false);
    registry.set_cancel_flag(1, flag1);
    registry.set_cancel_flag(2, flag2);

    registry.request_cancel(1, 100); // Only pid=1 should be cancelled.

    EXPECT_TRUE(flag1->load());
    EXPECT_FALSE(flag2->load());
}

// -- StatementCancel / StatementCancelGuard tests (header-only) ---------------

} // namespace
} // namespace sixseven
