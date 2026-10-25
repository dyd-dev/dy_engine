cmake_minimum_required(VERSION 3.20)
project(RhiRenderGraph LANGUAGES CXX)

if(NOT TARGET dy_engine)
    find_path(DY_ENGINE_INCLUDE_DIR dyf/RHI.h REQUIRED)
    find_library(DY_ENGINE_LIBRARY NAMES dy_engine REQUIRED)
    add_library(dy_engine STATIC IMPORTED)
    set_target_properties(dy_engine PROPERTIES
        IMPORTED_LOCATION "${DY_ENGINE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DY_ENGINE_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${DY_ENGINE_LINK_LIBRARIES}")
endif()

add_executable(RhiRenderGraph main.cpp)
target_compile_features(RhiRenderGraph PRIVATE cxx_std_17)
target_link_libraries(RhiRenderGraph PRIVATE dy_engine)
target_compile_definitions(RhiRenderGraph PRIVATE "$<$<BOOL:${USE_METAL}>:DY_EXAMPLE_METAL>")

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/CompileShaders.cmake" OPTIONAL)
if(COMMAND dy_example_shader)
    dy_example_shader(RhiRenderGraph comp Shaders/compute.glsl Shaders/compute.bin
        HLSL Shaders/compute.hlsl METAL Shaders/program.metal METAL_ENTRY computeMain)
    dy_example_shader(RhiRenderGraph vert Shaders/vertex.glsl Shaders/vertex.bin
        HLSL Shaders/vertex.hlsl METAL Shaders/program.metal METAL_ENTRY vertexMain)
    dy_example_shader(RhiRenderGraph frag Shaders/fragment.glsl Shaders/fragment.bin
        HLSL Shaders/fragment.hlsl METAL Shaders/program.metal METAL_ENTRY fragmentMain)
    dy_example_shader(RhiRenderGraph vert Shaders/display_vertex.glsl Shaders/display_vertex.bin
        HLSL Shaders/display_vertex.hlsl METAL Shaders/program.metal METAL_ENTRY displayVertex)
    dy_example_shader(RhiRenderGraph frag Shaders/display_fragment.glsl Shaders/display_fragment.bin
        HLSL Shaders/display_fragment.hlsl METAL Shaders/program.metal METAL_ENTRY displayFragment)
endif()
