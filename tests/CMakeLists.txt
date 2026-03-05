# SixSevenDB test targets

find_package(GTest CONFIG REQUIRED)

add_subdirectory(unit)
add_subdirectory(qa)
add_subdirectory(integration)
