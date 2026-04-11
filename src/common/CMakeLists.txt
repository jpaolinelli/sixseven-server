# sixseven::common sources

find_package(spdlog CONFIG REQUIRED)
find_package(tl-expected CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(sixseven_common STATIC
    logging.cpp
    config.cpp
    coercion.cpp
    crash_handler.cpp
    uuid.cpp
    secrets_manager.cpp
)

target_include_directories(sixseven_common PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(sixseven_common PUBLIC spdlog::spdlog tl::expected nlohmann_json::nlohmann_json OpenSSL::Crypto)
