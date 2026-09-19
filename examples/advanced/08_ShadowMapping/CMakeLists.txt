cmake_minimum_required(VERSION 3.20)
project(AdvancedShadowMapping LANGUAGES CXX)
get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()
include("${DY_ENGINE_SOURCE_DIR}/cmake/CompileShaders.cmake")
add_executable(AdvancedShadowMapping main.cpp)
target_link_libraries(AdvancedShadowMapping PRIVATE dy_engine)
set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
dy_compile_shader(AdvancedShadowMapping vertex vert
    GLSL "${shader_dir}/vertex.glsl" HLSL "${shader_dir}/vertex.hlsl"
    METAL "${shader_dir}/vertex.metal" METAL_ENTRY vertexMain
    INCLUDE_DIRS "${shader_dir}")
dy_compile_shader(AdvancedShadowMapping shadowDepth frag
    GLSL "${shader_dir}/shadowDepth.glsl" HLSL "${shader_dir}/shadowDepth.hlsl"
    METAL "${shader_dir}/shadowDepth.metal" METAL_ENTRY fragmentMain
    INCLUDE_DIRS "${shader_dir}")
dy_compile_shader(AdvancedShadowMapping lighting frag
    GLSL "${shader_dir}/lighting.glsl" HLSL "${shader_dir}/lighting.hlsl"
    METAL "${shader_dir}/lighting.metal" METAL_ENTRY fragmentMain
    INCLUDE_DIRS "${shader_dir}")
