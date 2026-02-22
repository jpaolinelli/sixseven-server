# giodb::index sources
add_library(giodb_index STATIC
    btree_key.cpp
    btree_node.cpp
    btree_index.cpp
    btree_iterator.cpp
)
target_include_directories(giodb_index PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(giodb_index PUBLIC giodb_common giodb_storage)
