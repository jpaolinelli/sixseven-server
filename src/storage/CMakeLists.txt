# giodb::storage sources

add_library(giodb_storage STATIC
    serialization.cpp
    page.cpp
    overflow.cpp
    disk_manager.cpp
    buffer_pool.cpp
    wal_record.cpp
)

target_include_directories(giodb_storage PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(giodb_storage PUBLIC giodb_common)
