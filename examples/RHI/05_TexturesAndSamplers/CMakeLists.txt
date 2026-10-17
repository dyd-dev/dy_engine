cmake_minimum_required(VERSION 3.20)
project(RhiTexturesAndSamplers LANGUAGES CXX)

get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()
if(COMMAND dy_example_support)
    dy_example_support()
endif()

add_executable(RhiTexturesAndSamplers main.cpp)
target_link_libraries(RhiTexturesAndSamplers PRIVATE dy_engine)

include("${DY_ENGINE_SOURCE_DIR}/cmake/CompileShaders.cmake")
set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
dy_compile_shader(RhiTexturesAndSamplers vertex vert
    GLSL "${shader_dir}/vertex.glsl"
    HLSL "${shader_dir}/vertex.hlsl"
    METAL "${shader_dir}/program.metal"
    METAL_ENTRY vertexMain)
dy_compile_shader(RhiTexturesAndSamplers fragment frag
    GLSL "${shader_dir}/fragment.glsl"
    HLSL "${shader_dir}/fragment.hlsl"
    METAL "${shader_dir}/program.metal"
    METAL_ENTRY fragmentMain)
