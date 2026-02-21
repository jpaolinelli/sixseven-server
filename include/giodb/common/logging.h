#pragma once

#include <spdlog/spdlog.h>

#include <string>

namespace giodb {

/// Initialize the logging system with the specified level.
/// Valid levels: "trace", "debug", "info", "warn", "error", "critical", "off"
void init_logging(const std::string& level = "info");

} // namespace giodb

// Project-wide logging macros with source location
#define GIODB_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define GIODB_LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define GIODB_LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define GIODB_LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define GIODB_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define GIODB_LOG_FATAL(...) SPDLOG_CRITICAL(__VA_ARGS__)
