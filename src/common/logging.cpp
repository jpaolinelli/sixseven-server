#include "giodb/common/logging.h"

#include <spdlog/spdlog.h>

namespace giodb {

void init_logging(const std::string& level) {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%s:%#] %v");
    spdlog::set_level(spdlog::level::from_str(level));
}

}  // namespace giodb
