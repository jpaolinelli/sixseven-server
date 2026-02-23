# giodb::vector sources
add_library(giodb_vector STATIC
    hnsw_page.cpp
    hnsw_index.cpp
)
target_include_directories(giodb_vector PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(giodb_vector PUBLIC giodb_common giodb_storage)
