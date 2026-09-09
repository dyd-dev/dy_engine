cmake_minimum_required(VERSION 3.20)
project(RhiGeometryCamera LANGUAGES CXX)

get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()
include("${DY_ENGINE_SOURCE_DIR}/cmake/CompileShaders.cmake")

add_executable(RhiGeometryCamera main.cpp)
target_link_libraries(RhiGeometryCamera PRIVATE dy_engine)
set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
dy_compile_shader(RhiGeometryCamera vertex vert
    GLSL "${shader_dir}/mesh_vs.glsl"
    HLSL "${shader_dir}/mesh_vs.hlsl"
    METAL "${shader_dir}/mesh_vs.metal"
    METAL_ENTRY vertexMain
    DEFINES RENDERER_ENABLE_SHADOWS=0 RENDERER_VERTEX_ENTRY=vertexMain
    INCLUDE_DIRS "${shader_dir}")
dy_compile_shader(RhiGeometryCamera fragment frag
    GLSL "${shader_dir}/mesh_ps.glsl"
    HLSL "${shader_dir}/mesh_ps.hlsl"
    METAL "${shader_dir}/mesh_ps.metal"
    METAL_ENTRY fragmentMain
    DEFINES RENDERER_ENABLE_SHADOWS=0 RENDERER_FRAGMENT_ENTRY=fragmentMain
    INCLUDE_DIRS "${shader_dir}")
