# GioDB QA regression tests (test_qa_*.cpp)

file(GLOB QA_TEST_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_executable(giodb_qa_tests ${QA_TEST_SOURCES})
target_include_directories(giodb_qa_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../unit)
target_link_libraries(giodb_qa_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        giodb
)

include(GoogleTest)
gtest_discover_tests(giodb_qa_tests PROPERTIES LABELS "qa")
