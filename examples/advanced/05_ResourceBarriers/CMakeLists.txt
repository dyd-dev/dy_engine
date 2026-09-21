cmake_minimum_required(VERSION 3.20)
project(AdvancedResourceBarriers LANGUAGES CXX)
get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()
add_executable(AdvancedResourceBarriers main.cpp)
target_link_libraries(AdvancedResourceBarriers PRIVATE dy_engine)
dy_compile_shader(AdvancedResourceBarriers vertex vert
    GLSL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/vertex.glsl"
    HLSL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/vertex.hlsl"
    METAL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/program.metal" METAL_ENTRY vertexMain)
dy_compile_shader(AdvancedResourceBarriers fragment frag
    GLSL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/fragment.glsl"
    HLSL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/fragment.hlsl"
    METAL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/program.metal" METAL_ENTRY fragmentMain)
dy_compile_shader(AdvancedResourceBarriers producer frag
    GLSL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/producer.glsl"
    HLSL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/producer.hlsl"
    METAL "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/program.metal" METAL_ENTRY producerMain)
