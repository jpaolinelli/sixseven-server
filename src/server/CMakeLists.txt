# giodb::server sources

add_library(giodb_server STATIC
    auth.cpp
    connection.cpp
    event_loop.cpp
    pg_protocol.cpp
    server.cpp
    thread_pool.cpp
    promotion_manager.cpp
    replication_health_monitor.cpp
    replication_message.cpp
    replication_slot.cpp
    sync_replication.cpp
    wal_receiver.cpp
    wal_sender.cpp
    wal_sender_manager.cpp
)

target_include_directories(giodb_server PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(giodb_server PUBLIC giodb_storage giodb_common)
