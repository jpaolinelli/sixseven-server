#include "sixseven/common/logging.h"

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <sstream>
#include <string>

// NOTE: SPDLOG_ACTIVE_LEVEL defaults to SPDLOG_LEVEL_INFO in this build, so
// SIXSEVEN_LOG_TRACE and SIXSEVEN_LOG_DEBUG compile to no-ops.  The tests
// below only exercise levels that are compiled in (INFO and above).

TEST(Logging, InitDoesNotCrash) {
    EXPECT_NO_THROW(sixseven::init_logging("info"));
}

TEST(Logging, AllLevelsDoNotCrash) {
    sixseven::init_logging("trace");
    EXPECT_NO_THROW({
        SIXSEVEN_LOG_TRACE("trace message");
        SIXSEVEN_LOG_DEBUG("debug message");
        SIXSEVEN_LOG_INFO("info message");
        SIXSEVEN_LOG_WARN("warn message");
        SIXSEVEN_LOG_ERROR("error message");
        SIXSEVEN_LOG_FATAL("fatal message");
    });
}

// Helper: detach the capturing logger so the spdlog default no longer
// references a test-local ostringstream after the test returns. A sinkless
// logger makes subsequent SIXSEVEN_LOG_* calls harmless no-ops.
static void restore_default_logger() {
    sixseven::init_logging(std::make_shared<spdlog::logger>("test_restored_default"));
    sixseven::init_logging("info");
}

// Helper: build a logger that writes to a string stream at the given level.
static std::shared_ptr<spdlog::logger> make_capturing_logger(const std::string& name,
                                                             std::ostringstream& oss,
                                                             spdlog::level::level_enum level) {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    sink->set_pattern("%v");
    auto logger = std::make_shared<spdlog::logger>(name, std::move(sink));
    logger->set_level(level);
    logger->flush_on(spdlog::level::trace);
    return logger;
}

// Verify that setting level=error suppresses INFO and WARN but passes ERROR.
TEST(Logging, LevelFilteringWorks) {
    std::ostringstream oss;
    sixseven::init_logging(make_capturing_logger("test_error", oss, spdlog::level::err));

    SIXSEVEN_LOG_INFO("info-msg");
    SIXSEVEN_LOG_WARN("warn-msg");
    SIXSEVEN_LOG_ERROR("error-msg");

    const std::string captured = oss.str();

    EXPECT_NE(captured.find("error-msg"), std::string::npos)
        << "ERROR message must appear when level=error; captured: " << captured;
    EXPECT_EQ(captured.find("info-msg"), std::string::npos)
        << "INFO message must be suppressed when level=error; captured: " << captured;
    EXPECT_EQ(captured.find("warn-msg"), std::string::npos)
        << "WARN message must be suppressed when level=error; captured: " << captured;

    restore_default_logger();
}

// Verify that setting level=warn allows WARN and ERROR but suppresses INFO.
TEST(Logging, WarnLevelSuppressesInfo) {
    std::ostringstream oss;
    sixseven::init_logging(make_capturing_logger("test_warn", oss, spdlog::level::warn));

    SIXSEVEN_LOG_INFO("info-only");
    SIXSEVEN_LOG_WARN("warn-only");
    SIXSEVEN_LOG_ERROR("error-only");

    const std::string captured = oss.str();

    EXPECT_EQ(captured.find("info-only"), std::string::npos)
        << "INFO must be suppressed at level=warn; captured: " << captured;
    EXPECT_NE(captured.find("warn-only"), std::string::npos)
        << "WARN must appear at level=warn; captured: " << captured;
    EXPECT_NE(captured.find("error-only"), std::string::npos)
        << "ERROR must appear at level=warn; captured: " << captured;

    restore_default_logger();
}

// Verify that level=info captures INFO, WARN, and ERROR.
TEST(Logging, InfoLevelCapturesInfoAndAbove) {
    std::ostringstream oss;
    sixseven::init_logging(make_capturing_logger("test_info", oss, spdlog::level::info));

    SIXSEVEN_LOG_INFO("info-check");
    SIXSEVEN_LOG_WARN("warn-check");
    SIXSEVEN_LOG_ERROR("error-check");

    const std::string captured = oss.str();

    EXPECT_NE(captured.find("info-check"), std::string::npos)
        << "INFO must appear at level=info; captured: " << captured;
    EXPECT_NE(captured.find("warn-check"), std::string::npos)
        << "WARN must appear at level=info; captured: " << captured;
    EXPECT_NE(captured.find("error-check"), std::string::npos)
        << "ERROR must appear at level=info; captured: " << captured;

    restore_default_logger();
}
