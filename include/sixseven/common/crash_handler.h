#pragma once

namespace sixseven {

/// Install fatal-signal and std::terminate handlers that print a backtrace
/// to stderr before the process dies.
///
/// Captures SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, and SIGTRAP via
/// sigaction(SA_SIGINFO | SA_ONSTACK | SA_RESETHAND), and installs a
/// std::terminate handler for uncaught exceptions (e.g. tl::bad_expected_access
/// or an Ort::Exception escaping a noexcept boundary).
///
/// The signal handler is async-signal-safe: it uses only write(2),
/// backtrace()/backtrace_symbols_fd(), and integer arithmetic on stack-local
/// fixed buffers. After printing, it restores SIG_DFL and re-raises the signal
/// so the OS still produces a core dump if `ulimit -c` allows.
///
/// Sets up an alternate signal stack on the calling thread (typically main)
/// via sigaltstack so that a stack-overflow crash can still run the handler.
/// Worker threads do not get an alternate stack — known limitation.
///
/// Idempotent: safe to call more than once.
void install_crash_handlers();

} // namespace sixseven
