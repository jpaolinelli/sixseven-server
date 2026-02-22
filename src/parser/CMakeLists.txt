# giodb::parser sources

add_library(giodb_parser STATIC
    lexer.cpp
)

target_include_directories(giodb_parser PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(giodb_parser PUBLIC giodb_common)
