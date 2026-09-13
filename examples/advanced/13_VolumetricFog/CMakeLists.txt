cmake_minimum_required(VERSION 3.20)
project(AdvancedVolumetricFog LANGUAGES CXX)
get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()
include("${DY_ENGINE_SOURCE_DIR}/cmake/CompileShaders.cmake")
add_executable(AdvancedVolumetricFog main.cpp)
target_link_libraries(AdvancedVolumetricFog PRIVATE dy_engine)
set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
dy_compile_shader(AdvancedVolumetricFog vertex vert
    GLSL "${shader_dir}/vertex.glsl" HLSL "${shader_dir}/vertex.hlsl"
    METAL "${shader_dir}/vertex.metal" METAL_ENTRY vertexMain
    INCLUDE_DIRS "${shader_dir}")
dy_compile_shader(AdvancedVolumetricFog scene frag
    GLSL "${shader_dir}/scene.glsl" HLSL "${shader_dir}/scene.hlsl"
    METAL "${shader_dir}/scene.metal" METAL_ENTRY fragmentMain
    INCLUDE_DIRS "${shader_dir}")
dy_compile_shader(AdvancedVolumetricFog fog frag
    GLSL "${shader_dir}/fog.glsl" HLSL "${shader_dir}/fog.hlsl"
    METAL "${shader_dir}/fog.metal" METAL_ENTRY fragmentMain
    INCLUDE_DIRS "${shader_dir}")
