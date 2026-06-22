// QA regression test for GDB-892: SIXSEVEN_LOG_TRACE/DEBUG compile to no-ops.
//
// Acceptance criteria under test:
//   AC1: A regression test reproduces the original wrong behaviour and now
//        passes after the fix.
//   AC2: Fix implemented — SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE in Debug
//        builds so SIXSEVEN_LOG_DEBUG/TRACE call sites exist in the binary.
//   AC3: ctest -L unit -L qa green; -Werror build clean.
//
// Mutation grade:
//   Before the fix, SPDLOG_ACTIVE_LEVEL defaulted to SPDLOG_LEVEL_INFO and
//   SPDLOG_DEBUG/SPDLOG_TRACE were preprocessed away.  The SIXSEVEN_LOG_DEBUG
//   call below would not emit anything regardless of the runtime level, so
//   captured.find("debug-marker") would return npos and the test would FAIL.
//   After the fix the call site exists in the binary and the runtime level
//   controls whether it passes through — making the test PASS.
//
// Pattern: uses the init_logging(shared_ptr<spdlog::logger>) overload
// (logging.h:19) to install a capturing ostream_sink, exactly as used in
// tests/unit/test_logging.cpp, so the capture idiom is well-established.

#include "sixseven/common/logging.h"

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <sstream>
#include <string>

using namespace sixseven;

namespace {

// Build a capturing logger at the given runtime level.
std::shared_ptr<spdlog::logger>
make_qa_logger(const std::string& name, std::ostringstream& oss, spdlog::level::level_enum level) {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    sink->set_pattern("%v");
    auto logger = std::make_shared<spdlog::logger>(name, std::move(sink));
    logger->set_level(level);
    logger->flush_on(spdlog::level::trace);
    return logger;
}

// Restore a clean default logger so subsequent tests are not affected.
void restore_logger() {
    init_logging(std::make_shared<spdlog::logger>("qa_892_restored"));
    init_logging("info");
}

} // namespace

// ---------------------------------------------------------------------------
// AC1/AC2: SIXSEVEN_LOG_DEBUG and SIXSEVEN_LOG_TRACE produce output when the
// runtime level allows it.  This is the primary mutation-grade regression test:
// if SPDLOG_ACTIVE_LEVEL were still INFO (the broken default) the call sites
// would not exist in the binary and the assertions below would fail.
// ---------------------------------------------------------------------------

TEST(QA_GDB892, DebugMessageCapturedWhenRuntimeLevelIsDebug) {
    std::ostringstream oss;
    init_logging(make_qa_logger("qa892_debug", oss, spdlog::level::debug));

    SIXSEVEN_LOG_DEBUG("gdb892-debug-marker {}", 42);

    restore_logger();

    const std::string captured = oss.str();
    EXPECT_NE(captured.find("gdb892-debug-marker 42"), std::string::npos)
        << "SIXSEVEN_LOG_DEBUG must produce output when runtime level=debug. "
           "If this fails, SPDLOG_ACTIVE_LEVEL is still gated at INFO (GDB-892 regression). "
           "Captured: ["
        << captured << "]";
}

TEST(QA_GDB892, TraceMessageCapturedWhenRuntimeLevelIsTrace) {
    std::ostringstream oss;
    init_logging(make_qa_logger("qa892_trace", oss, spdlog::level::trace));

    SIXSEVEN_LOG_TRACE("gdb892-trace-marker {}", 99);

    restore_logger();

    const std::string captured = oss.str();
    EXPECT_NE(captured.find("gdb892-trace-marker 99"), std::string::npos)
        << "SIXSEVEN_LOG_TRACE must produce output when runtime level=trace. "
           "If this fails, SPDLOG_ACTIVE_LEVEL is still gated at INFO (GDB-892 regression). "
           "Captured: ["
        << captured << "]";
}

// Sanity: INFO is always compiled in and must be captured at level=info.
TEST(QA_GDB892, InfoMessageAlwaysCaptured) {
    std::ostringstream oss;
    init_logging(make_qa_logger("qa892_info", oss, spdlog::level::info));

    SIXSEVEN_LOG_INFO("gdb892-info-sanity");

    restore_logger();

    const std::string captured = oss.str();
    EXPECT_NE(captured.find("gdb892-info-sanity"), std::string::npos)
        << "SIXSEVEN_LOG_INFO must always be captured; captured: [" << captured << "]";
}

// Runtime filtering still works: debug is compiled in but filtered at
// runtime level=warn.  Confirms compile-time and runtime gates are independent.
TEST(QA_GDB892, DebugSuppressedAtRuntimeWarnLevel) {
    std::ostringstream oss;
    init_logging(make_qa_logger("qa892_suppress", oss, spdlog::level::warn));

    SIXSEVEN_LOG_DEBUG("gdb892-should-not-appear");
    SIXSEVEN_LOG_WARN("gdb892-should-appear-warn");

    restore_logger();

    const std::string captured = oss.str();
    EXPECT_EQ(captured.find("gdb892-should-not-appear"), std::string::npos)
        << "DEBUG must be suppressed at runtime level=warn; captured: [" << captured << "]";
    EXPECT_NE(captured.find("gdb892-should-appear-warn"), std::string::npos)
        << "WARN must appear at runtime level=warn; captured: [" << captured << "]";
}
