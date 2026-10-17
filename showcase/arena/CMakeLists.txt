cmake_minimum_required(VERSION 3.20)
project(ShowcaseArena LANGUAGES CXX)

get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
get_filename_component(asset_default "${DY_ENGINE_SOURCE_DIR}/../assets" ABSOLUTE)
set(DY_ASSET_ROOT "${asset_default}" CACHE PATH "Showcase asset directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()

set(DY_DENSE_ARENA_ASSETS "${DY_ASSET_ROOT}/kenney_tiny-dungeon" CACHE PATH "Extracted Kenney Tiny Dungeon CC0 asset directory.")
file(TO_CMAKE_PATH "${DY_DENSE_ARENA_ASSETS}" dense_assets)
string(REPLACE "\"" "\\\"" dense_assets "${dense_assets}")
add_executable(ShowcaseArena main.cpp)
set_target_properties(ShowcaseArena PROPERTIES
    OUTPUT_NAME arena
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    FOLDER "Showcase")
target_link_libraries(ShowcaseArena PRIVATE dy_engine)
add_custom_command(TARGET ShowcaseArena POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${DY_ENGINE_SOURCE_DIR}/src/dyf/Assets/ltc/LICENSE"
        "$<TARGET_FILE_DIR:ShowcaseArena>/AreaLightingLtc.LICENSE" VERBATIM)
target_compile_definitions(ShowcaseArena PRIVATE "DY_DENSE_ARENA_ASSETS=\"${dense_assets}\"")
