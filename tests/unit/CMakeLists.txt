# GioDB unit tests

add_executable(giodb_unit_tests
    test_sanity.cpp
    test_logging.cpp
    test_result.cpp
    test_config.cpp
    test_types.cpp
    test_value.cpp
    test_coercion.cpp
    test_serialization.cpp
    test_page.cpp
    test_tuple.cpp
    test_overflow.cpp
    test_disk_manager.cpp
    test_buffer_pool.cpp
    test_wal_record.cpp
    test_wal.cpp
    test_wal_recovery.cpp
    test_btree_node.cpp
    test_btree.cpp
    test_btree_concurrency.cpp
    test_table_heap.cpp
    test_catalog.cpp
    test_lexer.cpp
    test_ast.cpp
)

target_link_libraries(giodb_unit_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        giodb
)

include(GoogleTest)
gtest_discover_tests(giodb_unit_tests)
