#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

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
            // Clamp so now + timeout cannot overflow the clock's nanosecond
            // counter (GDB-1238): an overflowed deadline lands in the past and
            // cancels every query instantly.
            const auto now = StatementDeadline::Clock::now();
            const auto max_delta = StatementDeadline::Clock::time_point::max() - now;
            const auto requested = std::chrono::milliseconds(timeout_ms);
            if (requested >= std::chrono::duration_cast<std::chrono::milliseconds>(max_delta)) {
                StatementDeadline::arm(StatementDeadline::Clock::time_point::max());
            } else {
                StatementDeadline::arm(now + requested);
            }
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

/// Thread-local statement cancel flag used to interrupt a running query from
/// another thread (GDB-956).
///
/// The protocol layer creates a fresh std::atomic<bool> flag per statement and
/// installs it via StatementCancelGuard just before invoking the query executor.
/// The flag is shared with the CancelRegistry so a CancelRequest arriving on a
/// different OS thread can set it atomically.  The QueryEngine checks
/// StatementCancel::requested() at the same points where it already checks
/// StatementDeadline::expired(), returning SQLSTATE 57014 "canceling statement
/// due to user request" when the flag is set.
///
/// The flag is RESET at the start of each new guard so a stale cancel cannot
/// kill a subsequent statement.
class StatementCancel {
public:
    /// Arm the cancel check for the current thread by installing a shared flag.
    /// Resets the flag to false (stale cancel protection).
    static void arm(std::shared_ptr<std::atomic<bool>> flag) {
        flag->store(false, std::memory_order_relaxed);
        flag_ = std::move(flag);
    }

    /// Disarm: drop the shared_ptr so the flag is no longer checked.
    static void clear() { flag_.reset(); }

    /// Return true if a cancel has been requested for the current statement.
    [[nodiscard]] static bool requested() {
        return flag_ && flag_->load(std::memory_order_acquire);
    }

    /// Return the current flag (may be null).
    static std::shared_ptr<std::atomic<bool>> current_flag() { return flag_; }

private:
    static inline thread_local std::shared_ptr<std::atomic<bool>> flag_;
};

/// RAII guard that arms the thread-local statement cancel flag.
/// Clears the flag in its destructor so a stale cancel cannot bleed into the
/// next statement.
class StatementCancelGuard {
public:
    explicit StatementCancelGuard(std::shared_ptr<std::atomic<bool>> flag) {
        if (flag) {
            StatementCancel::arm(std::move(flag));
            armed_here_ = true;
        }
    }

    ~StatementCancelGuard() {
        if (armed_here_) {
            StatementCancel::clear();
        }
    }

    StatementCancelGuard(const StatementCancelGuard&) = delete;
    StatementCancelGuard& operator=(const StatementCancelGuard&) = delete;

private:
    bool armed_here_ = false;
};

} // namespace sixseven
