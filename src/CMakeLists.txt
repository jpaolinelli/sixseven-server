# GioDB source library and executables

# Core library (INTERFACE for now, will become STATIC once sources are added)
add_library(giodb INTERFACE)
target_include_directories(giodb INTERFACE ${PROJECT_SOURCE_DIR}/include)

# Server executable
add_executable(giodb-server main.cpp)
target_link_libraries(giodb-server PRIVATE giodb)

# CLI executable
add_executable(giodb-cli cli/main.cpp)
target_link_libraries(giodb-cli PRIVATE giodb)

# Module subdirectories
add_subdirectory(common)
add_subdirectory(storage)
add_subdirectory(catalog)
add_subdirectory(index)
add_subdirectory(table)
add_subdirectory(graph)
add_subdirectory(vector)
add_subdirectory(parser)
add_subdirectory(planner)
add_subdirectory(executor)
add_subdirectory(txn)
add_subdirectory(server)
