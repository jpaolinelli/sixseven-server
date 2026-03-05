# SixSevenDB source library and executables

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
add_library(sixseven INTERFACE)
target_link_libraries(sixseven INTERFACE sixseven_common sixseven_storage sixseven_table sixseven_index sixseven_catalog sixseven_graph sixseven_vector sixseven_parser sixseven_planner sixseven_executor sixseven_txn sixseven_server)

# Server executable
add_executable(sixseven-server main.cpp)
target_link_libraries(sixseven-server PRIVATE sixseven)

# CLI executable
add_executable(sixseven-cli cli/main.cpp)
target_link_libraries(sixseven-cli PRIVATE sixseven)
