cmake_minimum_required(VERSION 3.20)
project(AdvancedCinematicBaseline LANGUAGES CXX)

get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    option(DY_EXTEND_MODEL "Build the optional Model extension." ON)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET dyf::Model)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        message(FATAL_ERROR "CinematicBaseline requires DY_EXTEND_MODEL=ON.")
    endif()
    message(STATUS "Skipping CinematicBaseline: enable DY_EXTEND_MODEL.")
    return()
endif()

get_filename_component(asset_root_default "${DY_ENGINE_SOURCE_DIR}/../assets" ABSOLUTE)
set(DY_ASSET_ROOT "${asset_root_default}" CACHE PATH "Shared source asset directory.")
set(DY_BASELINE_SCENE "${DY_ASSET_ROOT}/main_sponza/NewSponza_Main_glTF_003.gltf" CACHE FILEPATH "Baseline source environment.")
file(TO_CMAKE_PATH "${DY_BASELINE_SCENE}" baseline_scene)
string(REPLACE "\"" "\\\"" baseline_scene "${baseline_scene}")

add_executable(AdvancedCinematicBaseline main.cpp)
target_link_libraries(AdvancedCinematicBaseline PRIVATE dyf::Model)
target_compile_definitions(AdvancedCinematicBaseline PRIVATE
    "DY_BASELINE_SCENE=\"${baseline_scene}\""
    "DY_BASELINE_CAMERA_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/SponzaComparison.camera.csv\"")
set_target_properties(AdvancedCinematicBaseline PROPERTIES
    OUTPUT_NAME baseline
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# 구형 내장 Canvas 셰이더를 표시 단계에서만 재사용하며 엔진 소스는 바꾸지 않는다.
get_target_property(engine_binary_dir dy_engine BINARY_DIR)
target_include_directories(AdvancedCinematicBaseline PRIVATE "${engine_binary_dir}/generated/stock_shaders")
if(TARGET dy_stock_shaders)
    add_dependencies(AdvancedCinematicBaseline dy_stock_shaders)
endif()
foreach(backend IN ITEMS D3D12 METAL VULKAN)
    if(USE_${backend})
        target_compile_definitions(AdvancedCinematicBaseline PRIVATE ENABLE_${backend})
    endif()
endforeach()
