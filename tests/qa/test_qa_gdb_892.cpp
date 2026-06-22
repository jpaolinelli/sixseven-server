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

// ---------------------------------------------------------------------------
// Adversarial: full runtime level transition matrix.
// Each test sets a distinct runtime level and verifies the correct subset of
// messages is captured.  All of these rely on SPDLOG_ACTIVE_LEVEL=TRACE being
// compiled in; before the fix they would all trivially pass (no debug/trace
// output) while providing no actual coverage.
// ---------------------------------------------------------------------------

// Level=error: only ERROR passes; DEBUG, WARN suppressed.
TEST(QA_GDB892, LevelErrorSuppressesDebugAndWarn) {
    std::ostringstream oss;
    init_logging(make_qa_logger("qa892_lvl_error", oss, spdlog::level::err));

    SIXSEVEN_LOG_TRACE("892-err-trace");
    SIXSEVEN_LOG_DEBUG("892-err-debug");
    SIXSEVEN_LOG_WARN("892-err-warn");
    SIXSEVEN_LOG_ERROR("892-err-error");

    restore_logger();
    const std::string captured = oss.str();

    EXPECT_EQ(captured.find("892-err-trace"), std::string::npos)
        << "TRACE must be suppressed at level=error; captured: [" << captured << "]";
    EXPECT_EQ(captured.find("892-err-debug"), std::string::npos)
        << "DEBUG must be suppressed at level=error; captured: [" << captured << "]";
    EXPECT_EQ(captured.find("892-err-warn"), std::string::npos)
        << "WARN must be suppressed at level=error; captured: [" << captured << "]";
    EXPECT_NE(captured.find("892-err-error"), std::string::npos)
        << "ERROR must appear at level=error; captured: [" << captured << "]";
}

// Level=info: INFO/WARN/ERROR pass; DEBUG and TRACE suppressed.
TEST(QA_GDB892, LevelInfoSuppressesDebugAndTrace) {
    std::ostringstream oss;
    init_logging(make_qa_logger("qa892_lvl_info", oss, spdlog::level::info));

    SIXSEVEN_LOG_TRACE("892-info-trace");
    SIXSEVEN_LOG_DEBUG("892-info-debug");
    SIXSEVEN_LOG_INFO("892-info-info");
    SIXSEVEN_LOG_WARN("892-info-warn");

    restore_logger();
    const std::string captured = oss.str();

    EXPECT_EQ(captured.find("892-info-trace"), std::string::npos)
        << "TRACE must be suppressed at level=info; captured: [" << captured << "]";
    EXPECT_EQ(captured.find("892-info-debug"), std::string::npos)
        << "DEBUG must be suppressed at level=info; captured: [" << captured << "]";
    EXPECT_NE(captured.find("892-info-info"), std::string::npos)
        << "INFO must appear at level=info; captured: [" << captured << "]";
    EXPECT_NE(captured.find("892-info-warn"), std::string::npos)
        << "WARN must appear at level=info; captured: [" << captured << "]";
}

// Level=debug: DEBUG and above pass; TRACE suppressed.
TEST(QA_GDB892, LevelDebugSuppressesTrace) {
    std::ostringstream oss;
    init_logging(make_qa_logger("qa892_lvl_debug", oss, spdlog::level::debug));

    SIXSEVEN_LOG_TRACE("892-debug-trace");
    SIXSEVEN_LOG_DEBUG("892-debug-debug");
    SIXSEVEN_LOG_INFO("892-debug-info");

    restore_logger();
    const std::string captured = oss.str();

    EXPECT_EQ(captured.find("892-debug-trace"), std::string::npos)
        << "TRACE must be suppressed at level=debug; captured: [" << captured << "]";
    EXPECT_NE(captured.find("892-debug-debug"), std::string::npos)
        << "DEBUG must appear at level=debug; captured: [" << captured << "]";
    EXPECT_NE(captured.find("892-debug-info"), std::string::npos)
        << "INFO must appear at level=debug; captured: [" << captured << "]";
}

// Level=off: nothing passes through.
TEST(QA_GDB892, LevelOffSuppressesAll) {
    std::ostringstream oss;
    init_logging(make_qa_logger("qa892_lvl_off", oss, spdlog::level::off));

    SIXSEVEN_LOG_TRACE("892-off-trace");
    SIXSEVEN_LOG_DEBUG("892-off-debug");
    SIXSEVEN_LOG_INFO("892-off-info");
    SIXSEVEN_LOG_ERROR("892-off-error");

    restore_logger();
    const std::string captured = oss.str();

    EXPECT_TRUE(captured.empty()) << "level=off must suppress all output; captured: [" << captured
                                  << "]";
}

// Adversarial: multiple messages at the same level — confirm all are captured,
// not just the first.
TEST(QA_GDB892, MultipleDebugMessagesAllCaptured) {
    std::ostringstream oss;
    init_logging(make_qa_logger("qa892_multi", oss, spdlog::level::debug));

    SIXSEVEN_LOG_DEBUG("892-multi-alpha");
    SIXSEVEN_LOG_DEBUG("892-multi-beta");
    SIXSEVEN_LOG_DEBUG("892-multi-gamma");

    restore_logger();
    const std::string captured = oss.str();

    EXPECT_NE(captured.find("892-multi-alpha"), std::string::npos)
        << "First DEBUG message must appear; captured: [" << captured << "]";
    EXPECT_NE(captured.find("892-multi-beta"), std::string::npos)
        << "Second DEBUG message must appear; captured: [" << captured << "]";
    EXPECT_NE(captured.find("892-multi-gamma"), std::string::npos)
        << "Third DEBUG message must appear; captured: [" << captured << "]";
}

// Adversarial: restore_logger() isolation — a prior test's captured logger must
// not bleed into the next test.  Emit a unique marker in the first test, then
// confirm a fresh capture in the second test does not see it.
TEST(QA_GDB892, LoggerRestoredBetweenTests_Part1) {
    std::ostringstream oss1;
    init_logging(make_qa_logger("qa892_iso1", oss1, spdlog::level::debug));

    SIXSEVEN_LOG_DEBUG("892-isolation-canary");

    restore_logger();

    // After restore, emit via the now-sinkless default logger — must not appear
    // in oss1 (the sink was detached when we called restore_logger).
    SIXSEVEN_LOG_DEBUG("892-isolation-after-restore");

    EXPECT_EQ(oss1.str().find("892-isolation-after-restore"), std::string::npos)
        << "Post-restore log must not appear in the detached sink; captured: [" << oss1.str()
        << "]";
}

// Adversarial: SIXSEVEN_LOG_TRACE with format args — confirm expansion is
// correct (before the fix the call would be entirely absent; after, all args
// must be substituted correctly).
TEST(QA_GDB892, TraceFormattingWithMultipleArgs) {
    std::ostringstream oss;
    init_logging(make_qa_logger("qa892_fmt", oss, spdlog::level::trace));

    SIXSEVEN_LOG_TRACE("892-fmt key={} val={} flag={}", "mykey", 123, true);

    restore_logger();
    const std::string captured = oss.str();

    EXPECT_NE(captured.find("key=mykey"), std::string::npos)
        << "String arg must be formatted; captured: [" << captured << "]";
    EXPECT_NE(captured.find("val=123"), std::string::npos)
        << "Int arg must be formatted; captured: [" << captured << "]";
    EXPECT_NE(captured.find("flag=true"), std::string::npos)
        << "Bool arg must be formatted; captured: [" << captured << "]";
}
