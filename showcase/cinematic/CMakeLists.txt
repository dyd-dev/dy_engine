cmake_minimum_required(VERSION 3.20)
project(ShowcaseCinematic LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt")
if(NOT TARGET dy_engine)
    option(DY_EXTEND_MODEL "Build the optional Model extension." ON)
    option(DY_EXTEND_ADVANCED_RENDERING "Build the optional AdvancedRendering extension." ON)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET dyf::Model)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        message(FATAL_ERROR "CinematicShowcase requires DY_EXTEND_MODEL=ON.")
    endif()
    message(STATUS "Skipping CinematicShowcase: enable DY_EXTEND_MODEL to load environment assets.")
    return()
endif()

if(NOT TARGET dyf::AdvancedRendering)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        message(FATAL_ERROR "CinematicShowcase requires DY_EXTEND_ADVANCED_RENDERING=ON.")
    endif()
    message(STATUS "Skipping CinematicShowcase: enable DY_EXTEND_ADVANCED_RENDERING for the advanced raster passes.")
    return()
endif()

add_executable(ShowcaseCinematic main.cpp)
target_compile_features(ShowcaseCinematic PRIVATE cxx_std_20)
target_link_libraries(ShowcaseCinematic PRIVATE dyf::Model dyf::AdvancedRendering)
dyf_configure_showcase(ShowcaseCinematic showcase)

# The presentation opens all three assets in one process and keeps its window alive.
add_executable(ShowcasePresentation main.cpp)
target_compile_features(ShowcasePresentation PRIVATE cxx_std_20)
target_link_libraries(ShowcasePresentation PRIVATE dyf::Model dyf::AdvancedRendering)
target_compile_definitions(ShowcasePresentation PRIVATE DY_PRESENTATION_DEFAULT=1)
dyf_configure_showcase(ShowcasePresentation presentation)
set_property(TARGET ShowcaseCinematic APPEND PROPERTY DY_EXAMPLE_PRESETS
    "ShowcaseBistroExterior|exterior"
    "ShowcaseBistroNight|night"
    "ShowcaseBistroWine|interior"
    "ShowcaseEmerald|emerald"
    "ShowcaseKnight|exterior|--actor|knight|--comparison-mode|skinning")

if(WIN32)
    foreach(scene IN ITEMS exterior interior emerald)
        file(GENERATE OUTPUT "$<TARGET_FILE_DIR:ShowcaseCinematic>/Demo-${scene}.cmd"
            CONTENT "@echo off\n\"%~dp0$<TARGET_FILE_NAME:ShowcaseCinematic>\" ${scene} --presentation %*\nif errorlevel 1 pause\n"
            NEWLINE_STYLE CRLF)
    endforeach()
endif()
