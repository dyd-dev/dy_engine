cmake_minimum_required(VERSION 3.20)
project(dy_engine_app LANGUAGES CXX C)

get_filename_component(DY_ENGINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
option(DY_EXTEND_MODEL "Build the optional Model extension." ON)
option(DY_EXTEND_ADVANCED_RENDERING "Build the optional AdvancedRendering extension." ON)
option(DY_ENABLE_TRACY "Embed Tracy profiling instrumentation in engine applications." OFF)
set(DY_EXTEND_IMGUI ON CACHE BOOL "Build the optional Dear ImGui extension." FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory("${DY_ENGINE_ROOT}" dy_engine_core EXCLUDE_FROM_ALL)

get_filename_component(DY_EXAMPLES_ASSET_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../../../build/generated/examples-assets" ABSOLUTE)
add_subdirectory("${DY_ENGINE_ROOT}/examples" examples)

set(native_app_glue "${ANDROID_NDK}/sources/android/native_app_glue")
add_library(dy_engine_app SHARED
    AndroidMain.cpp
    "${native_app_glue}/android_native_app_glue.c")
target_include_directories(dy_engine_app PRIVATE "${native_app_glue}" "${DY_ENGINE_ROOT}/src")
target_link_libraries(dy_engine_app PRIVATE dyf_examples android log)
if(TARGET dyf::Tracy)
    target_link_libraries(dy_engine_app PRIVATE dyf::Tracy)
endif()
if(TARGET dyf::RenderDoc)
    target_link_libraries(dy_engine_app PRIVATE dyf::RenderDoc)
endif()
