# sixseven::planner sources

add_library(sixseven_planner STATIC
    type_resolver.cpp
    binder.cpp
    statistics.cpp
    cost_model.cpp
    optimizer.cpp
    rewrite_rules.cpp
)

target_include_directories(sixseven_planner PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(sixseven_planner PUBLIC sixseven_common sixseven_catalog sixseven_parser sixseven_table)
