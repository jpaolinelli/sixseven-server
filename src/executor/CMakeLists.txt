# giodb::executor — Volcano iterator model query execution engine
add_library(giodb_executor STATIC
    expr_evaluator.cpp
    seq_scan.cpp
    index_scan.cpp
    hash_index_scan.cpp
    bitmap_scan.cpp
    filter.cpp
    project.cpp
    sort.cpp
    external_sort.cpp
    limit.cpp
    insert.cpp
    update.cpp
    delete.cpp
    nested_loop_join.cpp
    hash_join.cpp
    sort_merge_join.cpp
    hash_aggregate.cpp
    window_function.cpp
    subquery_source.cpp
    traversal.cpp
    enriched_traversal.cpp
    shortest_path.cpp
    pattern_match.cpp
    nearest_scan.cpp
    catalog_persistence.cpp
    storage_manager.cpp
    system_bootstrap.cpp
    settings_cache.cpp
    provider_cache.cpp
    explain.cpp
    planner.cpp
    query_engine.cpp
)

target_include_directories(giodb_executor PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(giodb_executor
    PUBLIC
        giodb_common
        giodb_table
        giodb_index
        giodb_parser
        giodb_planner
        giodb_graph
        giodb_vector
        giodb_server
)
