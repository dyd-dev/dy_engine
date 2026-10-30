cmake_minimum_required(VERSION 3.20)
project(AdvancedBindlessMaterials LANGUAGES CXX)

if(NOT TARGET dy_engine)
    find_path(DY_ENGINE_INCLUDE_DIR dyf/RHI.h REQUIRED)
    find_library(DY_ENGINE_LIBRARY NAMES dy_engine REQUIRED)
    add_library(dy_engine STATIC IMPORTED)
    set_target_properties(dy_engine PROPERTIES
        IMPORTED_LOCATION "${DY_ENGINE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DY_ENGINE_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${DY_ENGINE_LINK_LIBRARIES}")
endif()

add_executable(AdvancedBindlessMaterials main.cpp)
target_compile_features(AdvancedBindlessMaterials PRIVATE cxx_std_17)
target_link_libraries(AdvancedBindlessMaterials PRIVATE dy_engine)
target_compile_definitions(AdvancedBindlessMaterials PRIVATE "$<$<BOOL:${USE_METAL}>:DY_EXAMPLE_METAL>")

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/CompileShaders.cmake" OPTIONAL)
if(COMMAND dy_example_shader)
    dy_example_shader(AdvancedBindlessMaterials vert Shaders/canvas_vs.glsl Shaders/vertex.bin
        HLSL Shaders/canvas_vs.hlsl METAL Shaders/canvas.metal METAL_ENTRY canvasVertex)
    dy_example_shader(AdvancedBindlessMaterials frag Shaders/array_ps.glsl Shaders/array_fragment.bin
        HLSL Shaders/array_ps.hlsl METAL Shaders/canvas.metal METAL_ENTRY arrayFragment)
endif()
