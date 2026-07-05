# SixSevenDB QA regression tests (test_qa_*.cpp)

file(GLOB QA_TEST_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_executable(sixseven_qa_tests ${QA_TEST_SOURCES})
target_include_directories(sixseven_qa_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../unit
)
target_link_libraries(sixseven_qa_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        sixseven
        sixseven_cli_lib
)

include(GoogleTest)
gtest_discover_tests(sixseven_qa_tests PROPERTIES LABELS "qa")

# GDB-710/GDB-1224: CI completion check — verifies the QA suites run to
# completion without SIGABRT (exit 0). A crash or unchecked Result dereference
# inside any in-scope test will fail this gate even if gtest_discover_tests
# does not catch it.
#
# GDB-1224 migrated the remaining QA suites to bootstrap_qa_catalog() so the
# full binary can run unfiltered.
add_test(
    NAME QA_BinaryRunsToCompletion
    COMMAND sixseven_qa_tests
)
set_tests_properties(QA_BinaryRunsToCompletion PROPERTIES LABELS "qa")
