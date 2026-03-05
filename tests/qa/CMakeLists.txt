# SixSevenDB QA regression tests (test_qa_*.cpp)

file(GLOB QA_TEST_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_executable(sixseven_qa_tests ${QA_TEST_SOURCES})
target_include_directories(sixseven_qa_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../unit)
target_link_libraries(sixseven_qa_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        sixseven
)

include(GoogleTest)
gtest_discover_tests(sixseven_qa_tests PROPERTIES LABELS "qa")
