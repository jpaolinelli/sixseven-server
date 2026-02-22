# giodb::planner sources

add_library(giodb_planner STATIC
    type_resolver.cpp
    binder.cpp
)

target_include_directories(giodb_planner PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(giodb_planner PUBLIC giodb_common giodb_catalog giodb_parser)
