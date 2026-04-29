# sixseven::executor — Volcano iterator model query execution engine
add_library(sixseven_executor STATIC
    algorithm_scan.cpp
    expr_evaluator.cpp
    count_scan.cpp
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
    edge_traversal.cpp
    shortest_path.cpp
    pattern_match.cpp
    variable_length_match.cpp
    match_shortest_path.cpp
    nearest_scan.cpp
    virtual_catalog_scan.cpp
    catalog_persistence.cpp
    index_manager.cpp
    storage_manager.cpp
    system_bootstrap.cpp
    settings_cache.cpp
    provider_cache.cpp
    explain.cpp
    planner.cpp
    pk_value_string.cpp
    query_engine.cpp
)

target_include_directories(sixseven_executor PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(sixseven_executor
    PUBLIC
        sixseven_common
        sixseven_table
        sixseven_index
        sixseven_parser
        sixseven_planner
        sixseven_graph
        sixseven_vector
        sixseven_server
)
