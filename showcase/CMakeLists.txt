cmake_minimum_required(VERSION 3.20)
project(DyfShowcase LANGUAGES CXX)

get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    option(DY_EXTEND_MODEL "Build the optional Model extension." ON)
    option(DY_EXTEND_ADVANCED_RENDERING "Build the optional AdvancedRendering extension." ON)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET dyf::Model OR NOT TARGET dyf::AdvancedRendering)
    message(FATAL_ERROR "Showcase requires DY_EXTEND_MODEL=ON and DY_EXTEND_ADVANCED_RENDERING=ON.")
endif()

add_subdirectory(cinematic targets/cinematic)
add_subdirectory(arena targets/arena)
add_subdirectory(culling targets/culling)
