cmake_minimum_required(VERSION 3.20)
project(AdvancedPBRLighting LANGUAGES CXX)
get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()
add_executable(AdvancedPBRLighting main.cpp)
target_link_libraries(AdvancedPBRLighting PRIVATE dy_engine)
set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
dy_compile_shader(AdvancedPBRLighting vertex vert
    GLSL "${shader_dir}/vertex.glsl" HLSL "${shader_dir}/vertex.hlsl"
    METAL "${shader_dir}/vertex.metal" METAL_ENTRY vertexMain
    INCLUDE_DIRS "${shader_dir}")
dy_compile_shader(AdvancedPBRLighting lighting frag
    GLSL "${shader_dir}/lighting.glsl" HLSL "${shader_dir}/lighting.hlsl"
    METAL "${shader_dir}/lighting.metal" METAL_ENTRY fragmentMain
    INCLUDE_DIRS "${shader_dir}")
