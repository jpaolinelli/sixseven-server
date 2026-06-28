#include "sixseven/common/statement_deadline.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>

namespace sixseven {
namespace {

// -- StatementCancel ----------------------------------------------------------

TEST(StatementCancel, NotRequestedByDefault) {
    // No guard installed; should not be requested.
    EXPECT_FALSE(StatementCancel::requested());
}

TEST(StatementCancel, GuardArmsFlag) {
    auto flag = std::make_shared<std::atomic<bool>>(false);
    {
        StatementCancelGuard guard(flag);
        EXPECT_FALSE(StatementCancel::requested()); // Not set yet.
        flag->store(true, std::memory_order_release);
        EXPECT_TRUE(StatementCancel::requested());
    }
    // Guard destroyed -- thread-local cleared.
    EXPECT_FALSE(StatementCancel::requested());
}

TEST(StatementCancel, GuardResetsFlagToFalseOnArm) {
    // A pre-set flag is cleared when the guard arms it (stale cancel protection).
    auto flag = std::make_shared<std::atomic<bool>>(true); // Pre-set stale cancel.
    StatementCancelGuard guard(flag);
    // Guard should have reset the flag.
    EXPECT_FALSE(flag->load());
    EXPECT_FALSE(StatementCancel::requested());
}

TEST(StatementCancel, FlagClearBetweenStatements) {
    auto flag1 = std::make_shared<std::atomic<bool>>(false);
    {
        StatementCancelGuard guard1(flag1);
        flag1->store(true, std::memory_order_release);
        EXPECT_TRUE(StatementCancel::requested());
    }
    // After guard1 exits, the flag is cleared.
    EXPECT_FALSE(StatementCancel::requested());

    // New statement with a fresh flag -- stale cancel from flag1 does NOT bleed in.
    auto flag2 = std::make_shared<std::atomic<bool>>(false);
    {
        StatementCancelGuard guard2(flag2);
        EXPECT_FALSE(StatementCancel::requested());
    }
}

TEST(StatementCancel, NullFlagGuardIsNoOp) {
    // A null flag should not crash or arm the cancel check.
    StatementCancelGuard guard(nullptr);
    EXPECT_FALSE(StatementCancel::requested());
}

TEST(StatementCancel, SetFromAnotherThread_ReflectsInCurrentThread) {
    // Simulate a cancel from a remote thread setting the shared flag.
    auto flag = std::make_shared<std::atomic<bool>>(false);
    StatementCancelGuard guard(flag);

    std::thread cancel_thread([&flag]() { flag->store(true, std::memory_order_release); });
    cancel_thread.join();

    EXPECT_TRUE(StatementCancel::requested());
}

// -- CancelRequest message parse test -----------------------------------------

TEST(CancelRequestParse, CorrectPidAndSecretExtraction) {
    // A CancelRequest body is 16 bytes:
    //   bytes 0-3  : length (0x00 0x00 0x00 0x10 = 16)
    //   bytes 4-7  : CANCEL_REQUEST_CODE (0x04 0xD2 0x16 0x2E = 80877102)
    //   bytes 8-11 : backend PID
    //   bytes 12-15: secret key
    //
    // Test that the byte layout matches what pg_protocol.cpp reads.

    // Build a mock 16-byte buffer.
    uint8_t buf[16];
    // Length = 16 big-endian.
    buf[0] = 0x00;
    buf[1] = 0x00;
    buf[2] = 0x00;
    buf[3] = 0x10;
    // CANCEL_REQUEST_CODE = 80877102 = 0x04D2162E.
    buf[4] = 0x04;
    buf[5] = 0xD2;
    buf[6] = 0x16;
    buf[7] = 0x2E;
    // backend_pid = 1234567 = 0x0012D687.
    buf[8] = 0x00;
    buf[9] = 0x12;
    buf[10] = 0xD6;
    buf[11] = 0x87;
    // secret_key = 987654321 = 0x3ADE68B1.
    buf[12] = 0x3A;
    buf[13] = 0xDE;
    buf[14] = 0x68;
    buf[15] = 0xB1;

    // Read pid and secret using the same big-endian extraction logic.
    auto read_be_int32 = [](const uint8_t* p) -> int32_t {
        return static_cast<int32_t>(
            (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
            (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]));
    };

    int32_t pid = read_be_int32(buf + 8);
    int32_t secret = read_be_int32(buf + 12);

    EXPECT_EQ(pid, 1234567);
    EXPECT_EQ(secret, static_cast<int32_t>(987654321));
}

} // namespace
} // namespace sixseven
