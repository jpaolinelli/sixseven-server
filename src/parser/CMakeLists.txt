# sixseven::parser sources

add_library(sixseven_parser STATIC
    lexer.cpp
    parser.cpp
)

target_include_directories(sixseven_parser PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(sixseven_parser PUBLIC sixseven_common)
