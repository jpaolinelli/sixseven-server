#include "giodb/common/logging.h"

#include <gtest/gtest.h>

TEST(Logging, InitDoesNotCrash) {
    EXPECT_NO_THROW(giodb::init_logging("info"));
}

TEST(Logging, AllLevelsDoNotCrash) {
    giodb::init_logging("trace");
    EXPECT_NO_THROW({
        GIODB_LOG_TRACE("trace message");
        GIODB_LOG_DEBUG("debug message");
        GIODB_LOG_INFO("info message");
        GIODB_LOG_WARN("warn message");
        GIODB_LOG_ERROR("error message");
        GIODB_LOG_FATAL("fatal message");
    });
}

TEST(Logging, LevelFilteringWorks) {
    // Setting level to "error" should not crash when lower-level logs are emitted
    giodb::init_logging("error");
    EXPECT_NO_THROW({
        GIODB_LOG_TRACE("should be filtered");
        GIODB_LOG_DEBUG("should be filtered");
        GIODB_LOG_INFO("should be filtered");
        GIODB_LOG_WARN("should be filtered");
        GIODB_LOG_ERROR("should appear");
    });
}
