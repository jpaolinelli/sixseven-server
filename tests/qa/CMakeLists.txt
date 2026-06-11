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
)

include(GoogleTest)
gtest_discover_tests(sixseven_qa_tests PROPERTIES LABELS "qa")

# GDB-716: CI completion check — verifies the entire QA binary runs to
# completion (exit 0). A crash or abort inside any test will fail this check
# even if gtest_discover_tests does not catch it.
add_test(
    NAME QA_BinaryRunsToCompletion
    COMMAND sixseven_qa_tests
)
set_tests_properties(QA_BinaryRunsToCompletion PROPERTIES LABELS "qa")
