cmake_minimum_required(VERSION 3.20)
project(AdvancedDepthSampling LANGUAGES CXX)
get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()
include("${DY_ENGINE_SOURCE_DIR}/cmake/CompileShaders.cmake")
add_executable(AdvancedDepthSampling main.cpp)
target_link_libraries(AdvancedDepthSampling PRIVATE dy_engine)
dy_compile_shader(AdvancedDepthSampling vertex vert
    GLSL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/vertex.glsl"
    HLSL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/vertex.hlsl"
    METAL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/program.metal" METAL_ENTRY vertexMain)
dy_compile_shader(AdvancedDepthSampling fragment frag
    GLSL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/fragment.glsl"
    HLSL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/fragment.hlsl"
    METAL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/program.metal" METAL_ENTRY fragmentMain)
dy_compile_shader(AdvancedDepthSampling producer frag
    GLSL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/producer.glsl"
    HLSL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/producer.hlsl"
    METAL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/program.metal" METAL_ENTRY producerMain)
