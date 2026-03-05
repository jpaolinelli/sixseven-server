#include "sixseven/common/logging.h"

#include <gtest/gtest.h>

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

TEST(Logging, LevelFilteringWorks) {
    // Setting level to "error" should not crash when lower-level logs are emitted
    sixseven::init_logging("error");
    EXPECT_NO_THROW({
        SIXSEVEN_LOG_TRACE("should be filtered");
        SIXSEVEN_LOG_DEBUG("should be filtered");
        SIXSEVEN_LOG_INFO("should be filtered");
        SIXSEVEN_LOG_WARN("should be filtered");
        SIXSEVEN_LOG_ERROR("should appear");
    });
}
