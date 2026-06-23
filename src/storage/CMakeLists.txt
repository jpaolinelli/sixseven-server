# sixseven::storage sources

add_library(sixseven_storage STATIC
    serialization.cpp
    page.cpp
    overflow.cpp
    disk_manager.cpp
    buffer_pool.cpp
    wal_record.cpp
    wal.cpp
    wal_recovery.cpp
    wal_archive.cpp
    clean_shutdown_marker.cpp
)

target_include_directories(sixseven_storage PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(sixseven_storage PUBLIC sixseven_common)
