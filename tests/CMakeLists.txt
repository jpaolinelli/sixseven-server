# SixSevenDB test targets

find_package(GTest CONFIG REQUIRED)

add_subdirectory(unit)
add_subdirectory(qa)
add_subdirectory(integration)
add_subdirectory(benchmark)
add_subdirectory(e2e)
