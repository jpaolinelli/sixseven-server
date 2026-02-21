#include "giodb/common/config.h"
#include "giodb/common/logging.h"

int main(int argc, char* argv[]) {
    // Load config from file if provided, otherwise use defaults
    std::string config_path = (argc > 1) ? argv[1] : "giodb.json";
    auto config_result = giodb::Config::load_from_file(config_path);
    if (!config_result) {
        giodb::init_logging("error");
        GIODB_LOG_ERROR("Failed to load config: {}", config_result.error().message);
        return 1;
    }
    auto config = std::move(*config_result);

    giodb::init_logging(config.log_level);
    GIODB_LOG_INFO("GioDB Server v0.1.0 starting");
    GIODB_LOG_INFO("  data_dir: {}", config.data_dir);
    GIODB_LOG_INFO("  port: {}", config.port);
    GIODB_LOG_INFO("  buffer_pool: {} MB", config.buffer_pool_size_mb);
    GIODB_LOG_INFO("  max_connections: {}", config.max_connections);

    return 0;
}
