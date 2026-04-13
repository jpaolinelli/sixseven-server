# sixseven::index sources
add_library(sixseven_index STATIC
    btree_key.cpp
    btree_node.cpp
    btree_index.cpp
    btree_iterator.cpp
    btree_persistence.cpp
    hash_index.cpp
    hash_persistence.cpp
)
target_include_directories(sixseven_index PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(sixseven_index PUBLIC sixseven_common sixseven_storage)
