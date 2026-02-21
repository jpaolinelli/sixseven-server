# giodb::common sources

find_package(spdlog CONFIG REQUIRED)
find_package(tl-expected CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(giodb_common STATIC
    logging.cpp
    config.cpp
)

target_include_directories(giodb_common PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(giodb_common PUBLIC spdlog::spdlog tl::expected nlohmann_json::nlohmann_json)
