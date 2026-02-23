# giodb::graph sources
add_library(giodb_graph STATIC
    edge_table.cpp
    graph_engine.cpp
)

target_include_directories(giodb_graph PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(giodb_graph PUBLIC giodb_common giodb_index giodb_table giodb_catalog)
