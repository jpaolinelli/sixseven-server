# giodb::server sources

add_library(giodb_server STATIC
    promotion_manager.cpp
    replication_message.cpp
    replication_slot.cpp
    sync_replication.cpp
    wal_receiver.cpp
    wal_sender.cpp
    wal_sender_manager.cpp
)

target_include_directories(giodb_server PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(giodb_server PUBLIC giodb_storage giodb_common)
