# giodb::table sources

add_library(giodb_table STATIC
    tuple.cpp
    table_heap.cpp
)

target_include_directories(giodb_table PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(giodb_table PUBLIC giodb_common giodb_storage)
