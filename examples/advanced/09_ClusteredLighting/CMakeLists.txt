cmake_minimum_required(VERSION 3.20)
project(AdvancedClusteredLighting LANGUAGES CXX)

if(NOT TARGET dy_engine)
    find_path(DY_ENGINE_INCLUDE_DIR dyf/RHI.h REQUIRED)
    find_library(DY_ENGINE_LIBRARY NAMES dy_engine REQUIRED)
    add_library(dy_engine STATIC IMPORTED)
    set_target_properties(dy_engine PROPERTIES
        IMPORTED_LOCATION "${DY_ENGINE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DY_ENGINE_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${DY_ENGINE_LINK_LIBRARIES}")
endif()

add_executable(AdvancedClusteredLighting main.cpp)
target_compile_features(AdvancedClusteredLighting PRIVATE cxx_std_17)
target_link_libraries(AdvancedClusteredLighting PRIVATE dy_engine)
target_compile_definitions(AdvancedClusteredLighting PRIVATE "$<$<BOOL:${USE_METAL}>:DY_EXAMPLE_METAL>")

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/CompileShaders.cmake" OPTIONAL)
if(COMMAND dy_example_shader)
    dy_example_shader(AdvancedClusteredLighting vert Shaders/vertex.glsl Shaders/vertex.bin
        HLSL Shaders/vertex.hlsl METAL Shaders/vertex.metal METAL_ENTRY vertexMain)
    dy_example_shader(AdvancedClusteredLighting comp Shaders/cluster.glsl Shaders/cluster.bin
        HLSL Shaders/cluster.hlsl METAL Shaders/cluster.metal METAL_ENTRY computeMain)
    dy_example_shader(AdvancedClusteredLighting frag Shaders/lighting.glsl Shaders/lighting.bin
        HLSL Shaders/lighting.hlsl METAL Shaders/lighting.metal METAL_ENTRY fragmentMain)
endif()
