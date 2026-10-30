cmake_minimum_required(VERSION 3.20)
project(RhiGraphicsPipeline LANGUAGES CXX)

if(NOT TARGET dy_engine)
    find_path(DY_ENGINE_INCLUDE_DIR dyf/RHI.h REQUIRED)
    find_library(DY_ENGINE_LIBRARY NAMES dy_engine REQUIRED)
    add_library(dy_engine STATIC IMPORTED)
    set_target_properties(dy_engine PROPERTIES
        IMPORTED_LOCATION "${DY_ENGINE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DY_ENGINE_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${DY_ENGINE_LINK_LIBRARIES}")
endif()

add_executable(RhiGraphicsPipeline main.cpp)
target_compile_features(RhiGraphicsPipeline PRIVATE cxx_std_17)
target_link_libraries(RhiGraphicsPipeline PRIVATE dy_engine)
target_compile_definitions(RhiGraphicsPipeline PRIVATE "$<$<BOOL:${USE_METAL}>:DY_EXAMPLE_METAL>")

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/CompileShaders.cmake" OPTIONAL)
if(COMMAND dy_example_shader)
    dy_example_shader(RhiGraphicsPipeline vert Shaders/vertex.glsl Shaders/vertex.bin
        HLSL Shaders/vertex.hlsl METAL Shaders/program.metal METAL_ENTRY vertexMain)
    dy_example_shader(RhiGraphicsPipeline frag Shaders/fragment.glsl Shaders/fragment.bin
        HLSL Shaders/fragment.hlsl METAL Shaders/program.metal METAL_ENTRY fragmentMain)
endif()
