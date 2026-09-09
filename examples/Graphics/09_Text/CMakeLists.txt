cmake_minimum_required(VERSION 3.20)
project(GraphicsText LANGUAGES CXX)

get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()
include("${DY_ENGINE_SOURCE_DIR}/cmake/CompileShaders.cmake")

add_executable(GraphicsText main.cpp)
target_link_libraries(GraphicsText PRIVATE dy_engine)
set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
dy_compile_shader(GraphicsText vertex vert
    GLSL "${shader_dir}/canvas_vs.glsl"
    HLSL "${shader_dir}/canvas_vs.hlsl"
    METAL "${shader_dir}/canvas.metal"
    METAL_ENTRY canvasVertex
    INCLUDE_DIRS "${shader_dir}")
dy_compile_shader(GraphicsText fragment frag
    GLSL "${shader_dir}/canvas_ps.glsl"
    HLSL "${shader_dir}/canvas_ps.hlsl"
    METAL "${shader_dir}/canvas.metal"
    METAL_ENTRY canvasFragment
    INCLUDE_DIRS "${shader_dir}")
set(DY_EXAMPLE_FONT "" CACHE FILEPATH "TrueType font for the text example.")
target_compile_definitions(GraphicsText PRIVATE DY_EXAMPLE_FONT="${DY_EXAMPLE_FONT}")
