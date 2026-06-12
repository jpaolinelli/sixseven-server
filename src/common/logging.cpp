#include "sixseven/common/logging.h"

#include <spdlog/spdlog.h>

namespace sixseven {

void init_logging(const std::string& level) {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%s:%#] %v");
    spdlog::set_level(spdlog::level::from_str(level));
}

void init_logging(std::shared_ptr<spdlog::logger> logger) {
    spdlog::set_default_logger(std::move(logger));
}

} // namespace sixseven
