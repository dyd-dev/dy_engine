cmake_minimum_required(VERSION 3.20)
project(AdvancedVolumetricFog LANGUAGES CXX)

if(NOT TARGET dy_engine)
    find_path(DY_ENGINE_INCLUDE_DIR dyf/RHI.h REQUIRED)
    find_library(DY_ENGINE_LIBRARY NAMES dy_engine REQUIRED)
    add_library(dy_engine STATIC IMPORTED)
    set_target_properties(dy_engine PROPERTIES
        IMPORTED_LOCATION "${DY_ENGINE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DY_ENGINE_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${DY_ENGINE_LINK_LIBRARIES}")
endif()

add_executable(AdvancedVolumetricFog main.cpp)
target_compile_features(AdvancedVolumetricFog PRIVATE cxx_std_17)
target_link_libraries(AdvancedVolumetricFog PRIVATE dy_engine)
target_compile_definitions(AdvancedVolumetricFog PRIVATE "$<$<BOOL:${USE_METAL}>:DY_EXAMPLE_METAL>")

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/CompileShaders.cmake" OPTIONAL)
if(COMMAND dy_example_shader)
    dy_example_shader(AdvancedVolumetricFog vert Shaders/vertex.glsl Shaders/vertex.bin
        HLSL Shaders/vertex.hlsl METAL Shaders/vertex.metal METAL_ENTRY vertexMain)
    dy_example_shader(AdvancedVolumetricFog frag Shaders/scene.glsl Shaders/scene.bin
        HLSL Shaders/scene.hlsl METAL Shaders/scene.metal METAL_ENTRY fragmentMain)
    dy_example_shader(AdvancedVolumetricFog frag Shaders/fog.glsl Shaders/fog.bin
        HLSL Shaders/fog.hlsl METAL Shaders/fog.metal METAL_ENTRY fragmentMain)
endif()
