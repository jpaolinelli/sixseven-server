# sixseven::catalog sources

add_library(sixseven_catalog STATIC
    catalog.cpp
)

target_include_directories(sixseven_catalog PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(sixseven_catalog PUBLIC sixseven_common)
