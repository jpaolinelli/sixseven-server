#include "giodb/common/config.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

namespace giodb {

Config Config::load_defaults() {
    return Config{};
}

Result<Config> Config::load_from_file(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        return ok(load_defaults());
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to open config file: " + path);
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        return make_error(StatusCode::PARSE_ERROR,
                          std::string("malformed JSON in config file: ") + e.what());
    }

    Config config = load_defaults();

    if (j.contains("data_dir") && j["data_dir"].is_string()) {
        config.data_dir = j["data_dir"].get<std::string>();
    }
    if (j.contains("port") && j["port"].is_number_unsigned()) {
        config.port = j["port"].get<uint16_t>();
    }
    if (j.contains("log_level") && j["log_level"].is_string()) {
        config.log_level = j["log_level"].get<std::string>();
    }
    if (j.contains("buffer_pool_size_mb") && j["buffer_pool_size_mb"].is_number_unsigned()) {
        config.buffer_pool_size_mb = j["buffer_pool_size_mb"].get<size_t>();
    }
    if (j.contains("wal_segment_size_mb") && j["wal_segment_size_mb"].is_number_unsigned()) {
        config.wal_segment_size_mb = j["wal_segment_size_mb"].get<size_t>();
    }
    if (j.contains("max_connections") && j["max_connections"].is_number_unsigned()) {
        config.max_connections = j["max_connections"].get<size_t>();
    }

    return ok(std::move(config));
}

}  // namespace giodb
