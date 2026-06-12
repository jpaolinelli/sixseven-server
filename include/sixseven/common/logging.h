#pragma once

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace sixseven {

/// Initialize the logging system with the specified level.
/// Valid levels: "trace", "debug", "info", "warn", "error", "critical", "off"
void init_logging(const std::string& level = "info");

/// Initialize the logging system using a caller-supplied logger.
/// The logger is set as the spdlog default logger so all SIXSEVEN_LOG_* macros
/// route through it.  Intended for unit tests that need to capture output and
/// verify level-filtering behaviour — do not call from production code.
void init_logging(std::shared_ptr<spdlog::logger> logger);

} // namespace sixseven

// Project-wide logging macros with source location
#define SIXSEVEN_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define SIXSEVEN_LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define SIXSEVEN_LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define SIXSEVEN_LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define SIXSEVEN_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define SIXSEVEN_LOG_FATAL(...) SPDLOG_CRITICAL(__VA_ARGS__)
