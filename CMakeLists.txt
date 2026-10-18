cmake_minimum_required(VERSION 3.20)
project(dy_engine CXX)

# Configuration and external dependencies.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()
include(Engine_Options)
find_package(Threads REQUIRED)
include(Dependencies)

# Targets.
add_library(dy_engine_options INTERFACE)
add_library(dy_engine STATIC)

# Sources.
target_sources(dy_engine PRIVATE
    src/RHI/ICommandList.cpp
    src/RHI/IDevice.cpp
    src/RHI/RenderGraph.cpp
    src/RHI/Resources.cpp
    src/RHI/ThreadPool.cpp

    src/dyf/BasicEnvironmentResources.cpp
    src/dyf/BasicLightingResources.cpp
    src/dyf/Canvas.cpp
    src/dyf/CanvasRenderer.cpp
    src/dyf/DrawData.cpp
    src/dyf/EnvironmentCapture.cpp
    src/dyf/EnvironmentDepthCapture.cpp
    src/dyf/Font.cpp
    src/dyf/ForwardPass.cpp
    src/dyf/FrameResources.cpp
    src/dyf/GeometryUpload.cpp
    src/dyf/GpuScene.cpp
    src/dyf/GpuVisibility.cpp
    src/dyf/Image.cpp
    src/dyf/Lighting.cpp
    src/dyf/LightingData.cpp
    src/dyf/LocalLightData.cpp
    src/dyf/Lod.cpp
    src/dyf/MaterialBindings.cpp
    src/dyf/MeshPrimitives.cpp
    src/dyf/OutputColor.cpp
    src/dyf/OutputPresentation.cpp
    src/dyf/PostProcess.cpp
    src/dyf/ProfilerOverlay.cpp
    src/dyf/RenderResources.cpp
    src/dyf/Renderer.cpp
    src/dyf/RendererFacade.cpp
    src/dyf/RendererLive.cpp
    src/dyf/RendererPipelines.cpp
    src/dyf/Scene.cpp
    src/dyf/SceneRendering.cpp
    src/dyf/Shaders.cpp
    src/dyf/ShadowPass.cpp
    src/dyf/TextureCache.cpp

    src/Platform/ActionMap.cpp
    src/Platform/Input.cpp
    src/Platform/Windows/Window.cpp

    src/Platform/Profiler/FrameProfiler.cpp
    src/Platform/Profiler/RhiProfiler.cpp
)

set(dy_window_libraries)
set(dy_window_include_directories)
if(ANDROID)
    set(DY_WINDOW_BACKEND_SOURCES src/Platform/Windows/AndroidWindow.cpp)
    set(dy_window_libraries android log)
    set(dy_window_include_directories "${ANDROID_NDK}/sources/android/native_app_glue")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(DY_WINDOW_BACKEND_SOURCES src/Platform/Windows/IOSWindow.mm)
    set(dy_window_libraries "-framework UIKit")
elseif(WIN32)
    set(DY_WINDOW_BACKEND_SOURCES
        src/Platform/Windows/GlfwWindow.cpp src/Platform/Windows/WindowsWindow.cpp)
    set(dy_window_libraries glfw)
elseif(APPLE)
    set(DY_WINDOW_BACKEND_SOURCES
        src/Platform/Windows/GlfwWindow.cpp src/Platform/Windows/MacOSWindow.mm)
    set(dy_window_libraries glfw "-framework AppKit")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(DY_WINDOW_BACKEND_SOURCES
        src/Platform/Windows/GlfwWindow.cpp src/Platform/Windows/LinuxWindow.cpp)
    set(dy_window_libraries glfw)
else()
    message(FATAL_ERROR "No Window backend for ${CMAKE_SYSTEM_NAME}.")
endif()
target_sources(dy_engine PRIVATE ${DY_WINDOW_BACKEND_SOURCES})

if(USE_D3D12)
    set(dy_backend_name D3D12)
elseif(USE_METAL)
    set(dy_backend_name Metal)
elseif(USE_VULKAN)
    set(dy_backend_name Vulkan)
else()
    set(dy_backend_name Null)
endif()
string(TOUPPER "${dy_backend_name}" DY_BACKEND_NORMALIZED)
add_subdirectory("src/Backends/${dy_backend_name}")
add_subdirectory(src/Platform/Profiler)

# Library dependencies.
target_link_libraries(dy_engine
    PUBLIC dy_engine_options Threads::Threads dyf::Profiler
    PRIVATE meshoptimizer ${dy_window_libraries})

# Include paths.
target_include_directories(dy_engine_options INTERFACE "${PROJECT_SOURCE_DIR}/src/Public")
target_include_directories(dy_engine PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${stb_SOURCE_DIR}"
    "${bcdec_SOURCE_DIR}"
    ${dy_window_include_directories})

# Compile features and definitions.
target_compile_features(dy_engine_options INTERFACE cxx_std_17)
if(DY_ENABLE_LIVE_PROFILING)
    target_compile_definitions(dy_engine_options INTERFACE DY_LIVE_PROFILING_ENABLED=1)
endif()
if(DY_ENABLE_TRACY)
    target_compile_definitions(dy_engine_options INTERFACE DY_PROFILER_TRACING_ENABLED=1)
endif()
if(DY_ENABLE_RENDERDOC)
    target_compile_definitions(dy_engine_options INTERFACE DY_RENDERDOC_ENABLED=1)
endif()
if(DY_ENABLE_SIMD)
    target_compile_definitions(dy_engine_options INTERFACE DY_SIMD_ENABLED=1)
endif()
if(NOT DY_BACKEND_NORMALIZED STREQUAL "NULL")
    target_compile_definitions(dy_engine PRIVATE "ENABLE_${DY_BACKEND_NORMALIZED}")
endif()
if("glfw" IN_LIST dy_window_libraries)
    target_compile_definitions(dy_engine PRIVATE GLFW_INCLUDE_NONE)
endif()

# Compiler options.
if(MSVC)
    target_compile_options(dy_engine_options INTERFACE /W4 /utf-8 /wd4201)
else()
    target_compile_options(dy_engine_options INTERFACE -Wall -Wextra)
endif()
if(DY_ENABLE_SIMD AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86|i[3-6]86)$")
    if(MSVC)
        target_compile_options(dy_engine_options INTERFACE /arch:SSE2)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(dy_engine_options INTERFACE -msse2)
    endif()
endif()

# Generated shaders.
include(CompileShaders)
dy_build_stock_shaders()

# External tool adapters keep their SDK settings on their own targets.
if(DY_ENABLE_TRACY)
    add_subdirectory(src/Extends/Tracy)
endif()
if(DY_ENABLE_RENDERDOC)
    add_subdirectory(src/Extends/RenderDoc)
endif()

# Extensions and applications.
if(DY_EXTEND_ADVANCED_RENDERING)
    add_subdirectory(src/Extends/AdvancedRendering)
endif()

if(DY_EXTEND_MODEL)
    add_subdirectory(src/Extends/Model)
endif()
if(DY_EXTEND_IMGUI)
    add_subdirectory(src/Extends/ImGui)
endif()

# Examples are added only for a top-level engine build.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    if(DY_BUILD_EXAMPLES)
        add_subdirectory(examples)
    endif()
endif()
