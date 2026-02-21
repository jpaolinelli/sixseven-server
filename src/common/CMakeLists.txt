# giodb::common sources

find_package(spdlog CONFIG REQUIRED)

add_library(giodb_common STATIC
    logging.cpp
)

target_include_directories(giodb_common PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(giodb_common PUBLIC spdlog::spdlog)
