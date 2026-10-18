cmake_minimum_required(VERSION 3.20)
project(AdvancedGPUCulling LANGUAGES CXX)

get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()

include("${DY_ENGINE_SOURCE_DIR}/cmake/CompileShaders.cmake")
add_executable(AdvancedGPUCulling main.cpp)
set_target_properties(AdvancedGPUCulling PROPERTIES OUTPUT_NAME culling)
target_link_libraries(AdvancedGPUCulling PRIVATE dy_engine)
set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
dy_compile_shader(AdvancedGPUCulling cull comp
    GLSL "${shader_dir}/cull.glsl" HLSL "${shader_dir}/cull.hlsl"
    METAL "${shader_dir}/program.metal" METAL_ENTRY cullObjects)
dy_compile_shader(AdvancedGPUCulling vertex vert
    GLSL "${shader_dir}/vertex.glsl" HLSL "${shader_dir}/vertex.hlsl"
    METAL "${shader_dir}/program.metal" METAL_ENTRY vertexMain)
dy_compile_shader(AdvancedGPUCulling fragment frag
    GLSL "${shader_dir}/fragment.glsl" HLSL "${shader_dir}/fragment.hlsl"
    METAL "${shader_dir}/program.metal" METAL_ENTRY fragmentMain)
