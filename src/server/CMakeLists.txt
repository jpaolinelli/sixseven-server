# sixseven::server sources

add_library(sixseven_server STATIC
    auth.cpp
    connection.cpp
    event_loop.cpp
    pg_protocol.cpp
    server.cpp
    session.cpp
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

target_include_directories(sixseven_server PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(sixseven_server PUBLIC sixseven_storage sixseven_common)
