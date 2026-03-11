# sixseven::graph sources
add_library(sixseven_graph STATIC
    algorithm_registry.cpp
    connected_components.cpp
    pagerank.cpp
    edge_table.cpp
    graph_engine.cpp
)

target_include_directories(sixseven_graph PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(sixseven_graph PUBLIC sixseven_common sixseven_index sixseven_table sixseven_catalog)
