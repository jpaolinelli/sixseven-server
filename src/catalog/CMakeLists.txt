# giodb::catalog sources

add_library(giodb_catalog STATIC
    catalog.cpp
)

target_include_directories(giodb_catalog PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(giodb_catalog PUBLIC giodb_common)
