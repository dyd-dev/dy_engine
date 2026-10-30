cmake_minimum_required(VERSION 3.20)
project(DyfModelAnim LANGUAGES CXX)

if(NOT TARGET dy_engine)
    find_path(DY_ENGINE_INCLUDE_DIR dyf/RHI.h REQUIRED)
    find_library(DY_ENGINE_LIBRARY NAMES dy_engine REQUIRED)
    add_library(dy_engine STATIC IMPORTED)
    set_target_properties(dy_engine PROPERTIES
        IMPORTED_LOCATION "${DY_ENGINE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DY_ENGINE_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${DY_ENGINE_LINK_LIBRARIES}")
endif()

if(NOT TARGET dyf::Model)
    find_library(DY_MODEL_LIBRARY NAMES dy_extend_model REQUIRED)
    add_library(dyf::Model STATIC IMPORTED)
    set_target_properties(dyf::Model PROPERTIES
        IMPORTED_LOCATION "${DY_MODEL_LIBRARY}"
        INTERFACE_LINK_LIBRARIES "dy_engine;${DY_MODEL_LINK_LIBRARIES}")
endif()

add_executable(DyfModelAnim main.cpp)
target_compile_features(DyfModelAnim PRIVATE cxx_std_17)
target_link_libraries(DyfModelAnim PRIVATE dyf::Model)
