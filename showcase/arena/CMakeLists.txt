cmake_minimum_required(VERSION 3.20)
project(ShowcaseArena LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()

add_executable(ShowcaseArena main.cpp)
target_link_libraries(ShowcaseArena PRIVATE dy_engine)
dyf_configure_showcase(ShowcaseArena arena)
