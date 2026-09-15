cmake_minimum_required(VERSION 3.20)
project(AdvancedClusteredLighting LANGUAGES CXX)
get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()
include("${DY_ENGINE_SOURCE_DIR}/cmake/CompileShaders.cmake")
add_executable(AdvancedClusteredLighting main.cpp)
target_link_libraries(AdvancedClusteredLighting PRIVATE dy_engine)
set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
dy_compile_shader(AdvancedClusteredLighting vertex vert
    GLSL "${shader_dir}/vertex.glsl" HLSL "${shader_dir}/vertex.hlsl"
    METAL "${shader_dir}/vertex.metal" METAL_ENTRY vertexMain
    INCLUDE_DIRS "${shader_dir}")
dy_compile_shader(AdvancedClusteredLighting cluster comp
    GLSL "${shader_dir}/cluster.glsl" HLSL "${shader_dir}/cluster.hlsl"
    METAL "${shader_dir}/cluster.metal" METAL_ENTRY computeMain
    INCLUDE_DIRS "${shader_dir}")
dy_compile_shader(AdvancedClusteredLighting lighting frag
    GLSL "${shader_dir}/lighting.glsl" HLSL "${shader_dir}/lighting.hlsl"
    METAL "${shader_dir}/lighting.metal" METAL_ENTRY fragmentMain
    INCLUDE_DIRS "${shader_dir}")
