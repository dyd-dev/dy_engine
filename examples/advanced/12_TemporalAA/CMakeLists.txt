cmake_minimum_required(VERSION 3.20)
project(AdvancedTemporalAA LANGUAGES CXX)
get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()
include("${DY_ENGINE_SOURCE_DIR}/cmake/CompileShaders.cmake")
add_executable(AdvancedTemporalAA main.cpp)
target_link_libraries(AdvancedTemporalAA PRIVATE dy_engine)
set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
dy_compile_shader(AdvancedTemporalAA vertex vert
    GLSL "${shader_dir}/vertex.glsl" HLSL "${shader_dir}/vertex.hlsl"
    METAL "${shader_dir}/vertex.metal" METAL_ENTRY vertexMain
    INCLUDE_DIRS "${shader_dir}")
dy_compile_shader(AdvancedTemporalAA scene frag
    GLSL "${shader_dir}/scene.glsl" HLSL "${shader_dir}/scene.hlsl"
    METAL "${shader_dir}/scene.metal" METAL_ENTRY fragmentMain
    INCLUDE_DIRS "${shader_dir}")
dy_compile_shader(AdvancedTemporalAA resolve frag
    GLSL "${shader_dir}/resolve.glsl" HLSL "${shader_dir}/resolve.hlsl"
    METAL "${shader_dir}/resolve.metal" METAL_ENTRY fragmentMain
    INCLUDE_DIRS "${shader_dir}")
dy_compile_shader(AdvancedTemporalAA present frag
    GLSL "${shader_dir}/present.glsl" HLSL "${shader_dir}/present.hlsl"
    METAL "${shader_dir}/present.metal" METAL_ENTRY fragmentMain
    INCLUDE_DIRS "${shader_dir}")
