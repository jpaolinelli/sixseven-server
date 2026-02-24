# giodb::txn sources

add_library(giodb_txn STATIC
    txn_manager.cpp
    mvcc.cpp
    vacuum.cpp
)

target_include_directories(giodb_txn PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(giodb_txn PUBLIC giodb_common giodb_storage)
