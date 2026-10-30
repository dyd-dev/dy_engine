cmake_minimum_required(VERSION 3.20)
project(RhiDeviceAndPresent LANGUAGES CXX)

get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()
if(COMMAND dy_example_support)
    dy_example_support()
endif()

add_executable(RhiDeviceAndPresent main.cpp)
target_link_libraries(RhiDeviceAndPresent PRIVATE dy_engine)
