# GioDB source library and executables

# Module subdirectories (must come before aggregate library)
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

# Aggregate library — links all module libraries
add_library(giodb INTERFACE)
target_link_libraries(giodb INTERFACE giodb_common giodb_storage giodb_table giodb_index giodb_catalog giodb_parser giodb_planner)

# Server executable
add_executable(giodb-server main.cpp)
target_link_libraries(giodb-server PRIVATE giodb)

# CLI executable
add_executable(giodb-cli cli/main.cpp)
target_link_libraries(giodb-cli PRIVATE giodb)
