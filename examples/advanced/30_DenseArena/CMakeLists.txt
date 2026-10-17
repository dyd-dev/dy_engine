cmake_minimum_required(VERSION 3.20)
project(AdvancedDenseArena LANGUAGES CXX)

get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()

set(DY_DENSE_ARENA_ASSETS "/home/suhan/downloads/kenney_tiny-dungeon" CACHE PATH "Extracted Kenney Tiny Dungeon CC0 asset directory.")
file(TO_CMAKE_PATH "${DY_DENSE_ARENA_ASSETS}" dense_assets)
string(REPLACE "\"" "\\\"" dense_assets "${dense_assets}")
add_executable(AdvancedDenseArena main.cpp)
target_link_libraries(AdvancedDenseArena PRIVATE dy_engine)
add_custom_command(TARGET AdvancedDenseArena POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${DY_ENGINE_SOURCE_DIR}/src/dyf/Assets/ltc/LICENSE"
        "$<TARGET_FILE_DIR:AdvancedDenseArena>/AreaLightingLtc.LICENSE" VERBATIM)
target_compile_definitions(AdvancedDenseArena PRIVATE "DY_DENSE_ARENA_ASSETS=\"${dense_assets}\"")
set_property(TARGET AdvancedDenseArena PROPERTY FOLDER "Examples/advanced")
