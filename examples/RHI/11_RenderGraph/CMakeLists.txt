cmake_minimum_required(VERSION 3.20)
project(RhiRenderGraph LANGUAGES CXX)

get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()
if(COMMAND dy_example_support)
    dy_example_support()
endif()

add_executable(RhiRenderGraph main.cpp)
target_link_libraries(RhiRenderGraph PRIVATE dy_engine)

include("${DY_ENGINE_SOURCE_DIR}/cmake/CompileShaders.cmake")
set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
dy_compile_shader(RhiRenderGraph compute comp
    GLSL "${shader_dir}/compute.glsl"
    HLSL "${shader_dir}/compute.hlsl"
    METAL "${shader_dir}/program.metal"
    METAL_ENTRY computeMain)
dy_compile_shader(RhiRenderGraph vertex vert
    GLSL "${shader_dir}/vertex.glsl"
    HLSL "${shader_dir}/vertex.hlsl"
    METAL "${shader_dir}/program.metal"
    METAL_ENTRY vertexMain)
dy_compile_shader(RhiRenderGraph fragment frag
    GLSL "${shader_dir}/fragment.glsl"
    HLSL "${shader_dir}/fragment.hlsl"
    METAL "${shader_dir}/program.metal"
    METAL_ENTRY fragmentMain)
dy_compile_shader(RhiRenderGraph display_vertex vert
    GLSL "${shader_dir}/display_vertex.glsl"
    HLSL "${shader_dir}/display_vertex.hlsl"
    METAL "${shader_dir}/program.metal"
    METAL_ENTRY displayVertex)
dy_compile_shader(RhiRenderGraph display_fragment frag
    GLSL "${shader_dir}/display_fragment.glsl"
    HLSL "${shader_dir}/display_fragment.hlsl"
    METAL "${shader_dir}/program.metal"
    METAL_ENTRY displayFragment)
