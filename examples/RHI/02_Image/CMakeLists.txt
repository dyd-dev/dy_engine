cmake_minimum_required(VERSION 3.20)
project(RhiImage LANGUAGES CXX)

get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()
include("${DY_ENGINE_SOURCE_DIR}/cmake/CompileShaders.cmake")

add_executable(RhiImage main.cpp)
target_link_libraries(RhiImage PRIVATE dy_engine)
set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
dy_compile_shader(RhiImage vertex vert
    GLSL "${shader_dir}/canvas_vs.glsl"
    HLSL "${shader_dir}/canvas_vs.hlsl"
    METAL "${shader_dir}/canvas.metal"
    METAL_ENTRY canvasVertex
    INCLUDE_DIRS "${shader_dir}")
dy_compile_shader(RhiImage fragment frag
    GLSL "${shader_dir}/canvas_ps.glsl"
    HLSL "${shader_dir}/canvas_ps.hlsl"
    METAL "${shader_dir}/canvas.metal"
    METAL_ENTRY canvasFragment
    INCLUDE_DIRS "${shader_dir}")
target_compile_definitions(RhiImage PRIVATE DY_EXAMPLE_IMAGE="${CMAKE_CURRENT_SOURCE_DIR}/Assets/Default_albedo.jpg")
