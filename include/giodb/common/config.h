#pragma once

#include "giodb/common/result.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace giodb {

struct Config {
    std::string data_dir = "./data";
    uint16_t port = 5432;
    std::string log_level = "info";
    size_t buffer_pool_size_mb = 256;
    size_t wal_segment_size_mb = 16;
    size_t max_connections = 100;

    /// Create a Config with all default values.
    static Config load_defaults();

    /// Load config from a JSON file. Missing fields use defaults.
    /// Returns defaults if the file does not exist.
    /// Returns an error if the file exists but contains malformed JSON.
    static Result<Config> load_from_file(const std::string& path);
};

} // namespace giodb
