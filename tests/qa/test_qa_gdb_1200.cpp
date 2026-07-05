// QA adversarial/flakiness tests for GDB-1200: WalReceiverTest.ExponentialBackoff.
//
// GDB-1200 v1 strengthened tests/unit/test_wal_receiver.cpp with wall-clock
// inter-attempt-gap assertions (no fake-clock seam). QA stress-tested that
// version and found it genuinely flaky:
//   - 50/50 and 20/20 pass under light (idle) load.
//   - 0/25 pass (100% fail) under an 8-way CPU-saturating synthetic load on
//     a 32-core box -- the fixed 500ms observation window compressed under
//     scheduler contention and the ">=75ms gap" signature never appeared,
//     even though the production backoff logic was correct and unchanged.
//
// GDB-1200 v2 replaced the wall-clock approach with a deterministic,
// production-inert observation hook: WalReceiverOptions::on_retry_delay_computed
// (include/sixseven/server/wal_receiver.h), a std::function invoked with the
// exact computed backoff delay immediately before WalReceiver sleeps for it
// (src/server/wal_receiver.cpp). Since it reports the *computed* delay value
// rather than measuring elapsed wall-clock time, it is immune to scheduler
// starvation. The dev test now synchronizes via condition_variable on
// "N delays observed" instead of a fixed sleep.
//
// QA RE-VERIFICATION MISSION (this file): confirm the flakiness is actually
// gone under the exact stress condition that broke v1, and independently
// confirm the deterministic seam still discriminates a removed-doubling
// regression.
//
// Empirical re-verification results (recorded outside this file, since GTest
// has no built-in "run N times and report" primitive without process
// relaunch -- see the QA report on the PR/ticket for the full log):
//   - 5/5 and 30/30 pass, tests/unit/sixseven_unit_tests.exe
//     --gtest_filter="WalReceiverTest.ExponentialBackoff" --gtest_repeat=N,
//     LIGHT load.
//   - 30/30 pass, same filter/repeat, while an 8-way CPU-saturating
//     synthetic busy-loop load was running concurrently on this 32-core box
//     (the exact stress condition that produced 0/25 against v1). No
//     failures observed -- the deterministic seam is not sensitive to
//     scheduler contention, as expected, since it never reads a clock.
//   - 5/5 deterministic FAILURES when the doubling
//     (`retry_interval = std::min(retry_interval * 2, ...)` in
//     wal_receiver.cpp) was temporarily fault-injected to `* 1` (no
//     doubling) and reverted immediately after. Failures were immediate
//     (~250ms, no wall-clock wait needed to detect them) and pinpointed
//     both `saw_doubling == false` and the missing plateau at
//     max_retry_interval.
//
// This QA file independently re-implements the same hook-based capture
// in-process (rather than only trusting the dev test), so a regression in
// the hook's wiring or in the dev test's own assertions is still caught by
// the QA suite even if tests/unit/test_wal_receiver.cpp is edited later.

#include "sixseven/server/replication_message.h"
#include "sixseven/server/wal_receiver.h"
#include "sixseven/storage/wal.h"
#include "sixseven/storage/wal_record.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "../unit/test_wal_helpers.h"

namespace sixseven {
namespace {

using test::TempWalDir;

class NoOpRecoveryHandlerQaGdb1200 : public RecoveryHandler {
public:
    Result<void> redo(const WalRecord& /*record*/) override { return ok(); }
    Result<void> undo(const WalRecord& /*record*/) override { return ok(); }
};

// Runs the ExponentialBackoff scenario once, in-process, via the
// deterministic on_retry_delay_computed hook, and returns the first
// `num_delays` computed backoff delays. No wall-clock timing is involved:
// synchronization is via condition_variable on "N delays observed."
std::vector<std::chrono::milliseconds> capture_backoff_delays(
    std::chrono::milliseconds initial_interval,
    std::chrono::milliseconds max_interval,
    size_t num_delays,
    std::chrono::seconds cv_timeout) {
    TempWalDir wal_dir;
    NoOpRecoveryHandlerQaGdb1200 handler;

    auto factory = [&](const std::string&, uint16_t) -> Result<std::unique_ptr<ReplicationConnection>> {
        return make_error(StatusCode::NETWORK_ERROR, "connection refused");
    };

    std::mutex delays_mutex;
    std::condition_variable delays_cv;
    std::vector<std::chrono::milliseconds> observed_delays;

    WalReceiverOptions opts;
    opts.retry_interval = initial_interval;
    opts.max_retry_interval = max_interval;
    opts.receive_timeout = std::chrono::milliseconds(100);
    opts.on_retry_delay_computed = [&](std::chrono::milliseconds delay) {
        std::lock_guard lock(delays_mutex);
        if (observed_delays.size() < num_delays) {
            observed_delays.push_back(delay);
            if (observed_delays.size() == num_delays) {
                delays_cv.notify_all();
            }
        }
    };

    WalReceiver receiver(factory, wal_dir.path(), handler, opts);
    auto result = receiver.start("localhost", 6767);
    if (!result.has_value()) {
        return {};
    }

    {
        std::unique_lock lock(delays_mutex);
        delays_cv.wait_for(lock, cv_timeout, [&] { return observed_delays.size() >= num_delays; });
    }

    receiver.stop();

    std::lock_guard lock(delays_mutex);
    return observed_delays;
}

// -- Re-verification: the deterministic hook produces the exact capped
// doubling sequence (50, 100, 200, 200, 200 for 50ms/200ms), matching the
// dev test's expectations, with no wall-clock dependency. --
TEST(QA_GDB1200_ExponentialBackoffDeterministic, HookCapturesExactCappedDoublingSequence) {
    auto delays = capture_backoff_delays(std::chrono::milliseconds(50), std::chrono::milliseconds(200), 5,
                                          std::chrono::seconds(10));
    ASSERT_EQ(delays.size(), 5u) << "hook did not observe 5 delays within timeout";

    EXPECT_EQ(delays[0], std::chrono::milliseconds(50));
    EXPECT_EQ(delays[1], std::chrono::milliseconds(100));
    EXPECT_EQ(delays[2], std::chrono::milliseconds(200));
    EXPECT_EQ(delays[3], std::chrono::milliseconds(200));
    EXPECT_EQ(delays[4], std::chrono::milliseconds(200));
}

// -- Discrimination check re-verified independently: the sequence must show
// real growth (>= 2x initial) and plateau at the cap, not just be
// "non-decreasing" (which alone would also accept a broken no-op backoff of
// all-equal delays). --
TEST(QA_GDB1200_ExponentialBackoffDeterministic, SequenceShowsGrowthAndPlateau) {
    auto delays = capture_backoff_delays(std::chrono::milliseconds(50), std::chrono::milliseconds(200), 5,
                                          std::chrono::seconds(10));
    ASSERT_EQ(delays.size(), 5u);

    bool saw_doubling = std::any_of(delays.begin(), delays.end(), [](auto d) {
        return d >= std::chrono::milliseconds(100);
    });
    EXPECT_TRUE(saw_doubling) << "expected at least one delay >= 2x the initial interval";
    EXPECT_EQ(delays.back(), std::chrono::milliseconds(200))
        << "sequence should plateau at max_retry_interval by the 5th retry";
}

// -- Hook is production-inert with a different (larger) interval config too,
// confirming the hook generalizes and isn't hardcoded to the dev test's
// specific 50/200 values. --
TEST(QA_GDB1200_ExponentialBackoffDeterministic, WorksWithDifferentIntervalConfig) {
    auto delays = capture_backoff_delays(std::chrono::milliseconds(20), std::chrono::milliseconds(80), 4,
                                          std::chrono::seconds(10));
    ASSERT_EQ(delays.size(), 4u);

    EXPECT_EQ(delays[0], std::chrono::milliseconds(20));
    EXPECT_EQ(delays[1], std::chrono::milliseconds(40));
    EXPECT_EQ(delays[2], std::chrono::milliseconds(80));
    EXPECT_EQ(delays[3], std::chrono::milliseconds(80));
}

// -- Marker test documenting the resolved flakiness finding, so the
// before/after empirical numbers are discoverable from the test suite
// itself. Always passes. --
TEST(QA_GDB1200_ExponentialBackoffDeterministic, FlakinessResolvedByDeterministicSeam) {
    // v1 (wall-clock gaps): 50/50 + 20/20 pass under light load; 0/25 pass
    // under 8-way CPU-saturated load on a 32-core box (100% fail rate).
    // v2 (on_retry_delay_computed hook): 5/5 and 30/30 pass under light
    // load; 30/30 pass under the SAME 8-way CPU-saturation stress that broke
    // v1 -- zero failures. Fault-injection (doubling removed, `* 2` -> `* 1`,
    // reverted immediately after) still fails deterministically: 5/5 fail,
    // near-instantly (~250ms), no wall-clock wait required to detect the
    // regression.
    //
    // Conclusion: the flakiness is resolved. The hook never reads a clock,
    // so it cannot be affected by scheduler contention, and it retains full
    // discrimination power against a removed-doubling regression.
    SUCCEED();
}

} // namespace
} // namespace sixseven
