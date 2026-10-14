cmake_minimum_required(VERSION 3.20)
project(AdvancedMeshShaderBasics LANGUAGES CXX)

get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()
include("${DY_ENGINE_SOURCE_DIR}/cmake/CompileShaders.cmake")

add_executable(AdvancedMeshShaderBasics main.cpp)
target_link_libraries(AdvancedMeshShaderBasics PRIVATE dy_engine)

dy_compile_shader(AdvancedMeshShaderBasics basic_ms mesh
    GLSL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/basic_ms.glsl"
    HLSL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/basic_ms.hlsl"
    METAL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/basic.metal" METAL_ENTRY meshMain)
dy_compile_shader(AdvancedMeshShaderBasics basic_ps frag
    GLSL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/basic_ps.glsl"
    HLSL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/basic_ps.hlsl"
    METAL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/basic.metal" METAL_ENTRY fragmentMain)
