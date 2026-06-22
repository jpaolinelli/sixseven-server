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
/// backtrace() (to capture raw return addresses), and integer arithmetic on
/// stack-local fixed buffers. It does NOT call backtrace_symbols_fd() — on
/// macOS that function calls dladdr() under the dyld lock, which deadlocks if
/// any other thread is inside the dynamic linker when the signal fires. Raw
/// frame addresses and the image load address are printed instead; symbolize
/// them out-of-process with:
///
///     atos -o <binary> -l <load_addr> <addr1> <addr2> ...
///     addr2line -e <binary> -f -C <addr1> ...   # Linux
///
/// After printing the backtrace the handler calls _exit(128 + signo)
/// immediately. It does NOT restore SIG_DFL and re-raise the signal, so NO
/// core dump is produced. This is deliberate: on macOS, fatal signals routed
/// through Mach exception ports to ReportCrash can stall the process for
/// minutes; _exit bypasses that entirely. Diagnostic value comes from the
/// in-process backtrace already written to stderr, not from a core file.
/// (SA_RESETHAND is set in sa_flags but is never exercised because the
/// handler _exits before the signal can be re-delivered.)
///
/// Sets up an alternate signal stack on the calling thread (typically main)
/// via sigaltstack so that a stack-overflow crash can still run the handler.
/// Worker threads do not get an alternate stack — known limitation.
///
/// Idempotent: safe to call more than once.
void install_crash_handlers();

} // namespace sixseven
