cmake_minimum_required(VERSION 3.20)
project(RhiRenderTargetsAndBarriers LANGUAGES CXX)

get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()
if(COMMAND dy_example_support)
    dy_example_support()
endif()

add_executable(RhiRenderTargetsAndBarriers main.cpp)
target_link_libraries(RhiRenderTargetsAndBarriers PRIVATE dy_engine)

include("${DY_ENGINE_SOURCE_DIR}/cmake/CompileShaders.cmake")
set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
dy_compile_shader(RhiRenderTargetsAndBarriers scene_vertex vert
    GLSL "${shader_dir}/scene_vertex.glsl"
    HLSL "${shader_dir}/scene_vertex.hlsl"
    METAL "${shader_dir}/program.metal"
    METAL_ENTRY vertexMain)
dy_compile_shader(RhiRenderTargetsAndBarriers scene_fragment frag
    GLSL "${shader_dir}/scene_fragment.glsl"
    HLSL "${shader_dir}/scene_fragment.hlsl"
    METAL "${shader_dir}/program.metal"
    METAL_ENTRY fragmentMain)
dy_compile_shader(RhiRenderTargetsAndBarriers display_vertex vert
    GLSL "${shader_dir}/display_vertex.glsl"
    HLSL "${shader_dir}/display_vertex.hlsl"
    METAL "${shader_dir}/program.metal"
    METAL_ENTRY displayVertex)
dy_compile_shader(RhiRenderTargetsAndBarriers display_fragment frag
    GLSL "${shader_dir}/display_fragment.glsl"
    HLSL "${shader_dir}/display_fragment.hlsl"
    METAL "${shader_dir}/program.metal"
    METAL_ENTRY displayFragment)
