#pragma once

#include <chrono>
#include <cstdint>

namespace sixseven {

/// Thread-local statement deadline used to enforce the per-session
/// `statement_timeout` variable (GDB-721).
///
/// The PostgreSQL protocol layer arms the deadline (via
/// StatementDeadlineGuard) just before invoking the query executor callback,
/// and the QueryEngine checks `expired()` between tuple pulls in its
/// root drain loop, aborting the statement with QUERY_CANCELED (SQLSTATE
/// 57014) when the deadline has passed.
///
/// Granularity: the check happens between `next()` calls at the root of the
/// iterator tree. A single blocking pipeline stage (e.g. a Sort gathering all
/// of its input inside one `next()` call) is only interrupted once it yields
/// control back to the drain loop.
///
/// Thread-local storage is used so the deadline follows the synchronous
/// execution path of one statement without changing the QueryExecutor
/// callback signature; concurrent sessions on other threads are unaffected.
class StatementDeadline {
public:
    using Clock = std::chrono::steady_clock;

    /// Arm the deadline for the current thread.
    static void arm(Clock::time_point deadline) {
        deadline_ = deadline;
        armed_ = true;
    }

    /// Disarm the deadline for the current thread.
    static void clear() { armed_ = false; }

    /// Return true if a deadline is armed on this thread.
    [[nodiscard]] static bool armed() { return armed_; }

    /// Return true if a deadline is armed and has passed.
    [[nodiscard]] static bool expired() { return armed_ && Clock::now() >= deadline_; }

private:
    static inline thread_local bool armed_ = false;
    static inline thread_local Clock::time_point deadline_{};
};

/// RAII guard that arms the thread-local statement deadline for a given
/// timeout in milliseconds. A timeout of 0 (the PostgreSQL default, meaning
/// "disabled") leaves the deadline disarmed.
class StatementDeadlineGuard {
public:
    explicit StatementDeadlineGuard(int64_t timeout_ms) {
        if (timeout_ms > 0) {
            armed_here_ = true;
            StatementDeadline::arm(StatementDeadline::Clock::now() +
                                   std::chrono::milliseconds(timeout_ms));
        }
    }

    ~StatementDeadlineGuard() {
        if (armed_here_) {
            StatementDeadline::clear();
        }
    }

    StatementDeadlineGuard(const StatementDeadlineGuard&) = delete;
    StatementDeadlineGuard& operator=(const StatementDeadlineGuard&) = delete;

private:
    bool armed_here_ = false;
};

} // namespace sixseven
