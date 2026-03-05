#pragma once

#include <spdlog/spdlog.h>

#include <string>

namespace sixseven {

/// Initialize the logging system with the specified level.
/// Valid levels: "trace", "debug", "info", "warn", "error", "critical", "off"
void init_logging(const std::string& level = "info");

} // namespace sixseven

// Project-wide logging macros with source location
#define SIXSEVEN_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define SIXSEVEN_LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define SIXSEVEN_LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define SIXSEVEN_LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define SIXSEVEN_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define SIXSEVEN_LOG_FATAL(...) SPDLOG_CRITICAL(__VA_ARGS__)
