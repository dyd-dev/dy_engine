cmake_minimum_required(VERSION 3.20)
project(RhiComputePipeline LANGUAGES CXX)

if(NOT TARGET dy_engine)
    find_path(DY_ENGINE_INCLUDE_DIR dyf/RHI.h REQUIRED)
    find_library(DY_ENGINE_LIBRARY NAMES dy_engine REQUIRED)
    add_library(dy_engine STATIC IMPORTED)
    set_target_properties(dy_engine PROPERTIES
        IMPORTED_LOCATION "${DY_ENGINE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DY_ENGINE_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${DY_ENGINE_LINK_LIBRARIES}")
endif()

add_executable(RhiComputePipeline main.cpp)
target_compile_features(RhiComputePipeline PRIVATE cxx_std_17)
target_link_libraries(RhiComputePipeline PRIVATE dy_engine)
target_compile_definitions(RhiComputePipeline PRIVATE "$<$<BOOL:${USE_METAL}>:DY_EXAMPLE_METAL>")
