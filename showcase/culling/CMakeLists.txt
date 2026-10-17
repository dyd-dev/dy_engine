cmake_minimum_required(VERSION 3.20)
project(ShowcaseCulling LANGUAGES CXX)

get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    option(DY_EXTEND_ADVANCED_RENDERING "Build the optional AdvancedRendering extension." ON)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET dyf::AdvancedRendering)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        message(FATAL_ERROR "GPUCulling requires DY_EXTEND_ADVANCED_RENDERING=ON.")
    endif()
    message(STATUS "Skipping GPUCulling: enable DY_EXTEND_ADVANCED_RENDERING for the advanced raster passes.")
    return()
endif()

add_executable(ShowcaseCulling main.cpp)
set_target_properties(ShowcaseCulling PROPERTIES
    OUTPUT_NAME culling
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    FOLDER "Showcase")
target_link_libraries(ShowcaseCulling PRIVATE dy_engine dyf::AdvancedRendering)
add_custom_command(TARGET ShowcaseCulling POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${DY_ENGINE_SOURCE_DIR}/src/dyf/Assets/ltc/LICENSE"
        "$<TARGET_FILE_DIR:ShowcaseCulling>/AreaLightingLtc.LICENSE" VERBATIM)

