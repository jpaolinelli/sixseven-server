# sixseven::table sources

add_library(sixseven_table STATIC
    tuple.cpp
    table_heap.cpp
    table_wal.cpp
)

target_include_directories(sixseven_table PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(sixseven_table PUBLIC sixseven_common sixseven_storage)
