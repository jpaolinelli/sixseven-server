/// @file test_statement_deadline.cpp
/// Unit tests for the thread-local StatementDeadline / StatementDeadlineGuard
/// used to enforce the per-session statement_timeout (GDB-721).

#include "sixseven/common/statement_deadline.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace sixseven;

namespace {

class StatementDeadlineTest : public ::testing::Test {
protected:
    void SetUp() override { StatementDeadline::clear(); }
    void TearDown() override { StatementDeadline::clear(); }
};

TEST_F(StatementDeadlineTest, DisarmedByDefault) {
    EXPECT_FALSE(StatementDeadline::armed());
    EXPECT_FALSE(StatementDeadline::expired());
}

TEST_F(StatementDeadlineTest, ArmedFutureDeadlineNotExpired) {
    StatementDeadline::arm(StatementDeadline::Clock::now() + std::chrono::hours(1));
    EXPECT_TRUE(StatementDeadline::armed());
    EXPECT_FALSE(StatementDeadline::expired());
}

TEST_F(StatementDeadlineTest, ArmedPastDeadlineExpired) {
    StatementDeadline::arm(StatementDeadline::Clock::now() - std::chrono::milliseconds(1));
    EXPECT_TRUE(StatementDeadline::armed());
    EXPECT_TRUE(StatementDeadline::expired());
}

TEST_F(StatementDeadlineTest, ClearDisarms) {
    StatementDeadline::arm(StatementDeadline::Clock::now() - std::chrono::milliseconds(1));
    StatementDeadline::clear();
    EXPECT_FALSE(StatementDeadline::armed());
    EXPECT_FALSE(StatementDeadline::expired());
}

TEST_F(StatementDeadlineTest, GuardArmsAndClearsOnDestruction) {
    {
        StatementDeadlineGuard guard(60000);
        EXPECT_TRUE(StatementDeadline::armed());
        EXPECT_FALSE(StatementDeadline::expired());
    }
    EXPECT_FALSE(StatementDeadline::armed());
}

TEST_F(StatementDeadlineTest, GuardWithZeroTimeoutDoesNotArm) {
    StatementDeadlineGuard guard(0);
    EXPECT_FALSE(StatementDeadline::armed());
}

TEST_F(StatementDeadlineTest, GuardWithNegativeTimeoutDoesNotArm) {
    StatementDeadlineGuard guard(-5);
    EXPECT_FALSE(StatementDeadline::armed());
}

TEST_F(StatementDeadlineTest, GuardDeadlineExpiresAfterTimeout) {
    StatementDeadlineGuard guard(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(StatementDeadline::expired());
}

TEST_F(StatementDeadlineTest, DeadlineIsThreadLocal) {
    StatementDeadline::arm(StatementDeadline::Clock::now() - std::chrono::milliseconds(1));
    bool other_thread_armed = true;
    std::thread t([&] { other_thread_armed = StatementDeadline::armed(); });
    t.join();
    EXPECT_FALSE(other_thread_armed);
    EXPECT_TRUE(StatementDeadline::armed());
}

} // namespace
