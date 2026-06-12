# sixseven::txn sources

add_library(sixseven_txn STATIC
    txn_manager.cpp
    mvcc.cpp
    read_view.cpp
    vacuum.cpp
    lock_manager.cpp
    deadlock_detector.cpp
)

target_include_directories(sixseven_txn PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(sixseven_txn PUBLIC sixseven_common sixseven_storage sixseven_catalog)
