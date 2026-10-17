cmake_minimum_required(VERSION 3.20)
project(dy_engine CXX)

# add module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(APPLE)
	enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

# main target
add_library(${PROJECT_NAME} STATIC
	"src/RHI/ThreadPool.cpp"
	"src/RHI/IDevice.cpp"
	"src/RHI/ICommandList.cpp"
	"src/RHI/Resources.cpp"
    "src/RHI/LiveRhi.cpp"
	"src/dyf/RendererLive.cpp"
	"src/dyf/Renderer.cpp"
    "src/dyf/RendererFacade.cpp"
    "src/dyf/SceneRendering.cpp"
    "src/dyf/RendererPipelines.cpp"
    "src/dyf/LightingData.cpp"
    "src/dyf/RenderResources.cpp"
    "src/dyf/OutputPresentation.cpp"
	"src/dyf/Image.cpp"
	"src/dyf/Canvas.cpp"
	"src/dyf/Font.cpp"
	"src/dyf/MeshPrimitives.cpp"
    "src/dyf/Lod.cpp"
    "src/dyf/GpuVisibility.cpp"
    "src/dyf/GpuScene.cpp"
    "src/dyf/BasicLightingResources.cpp"
    "src/dyf/BasicEnvironmentResources.cpp"
    "src/dyf/LocalLightData.cpp"
    "src/dyf/AreaLightingLtc.cpp"
	"src/dyf/Shaders.cpp"
	"src/RHI/RenderGraph.cpp"
	"src/dyf/CanvasRenderer.cpp"
    "src/dyf/PostProcess.cpp"
    "src/dyf/OutputColor.cpp"
    "src/dyf/EnvironmentCapture.cpp"
    "src/dyf/EnvironmentDepthCapture.cpp"
    "src/dyf/FrameResources.cpp"
    "src/dyf/Lighting.cpp"
    "src/dyf/Profiler.cpp"
    "src/dyf/ProfilerSampler.cpp"
    "src/Diagnostics/ProfilerControls.cpp"
    "src/Platform/Log.cpp"
    "src/Platform/LogSession.cpp"
	"src/dyf/DrawData.cpp"
    "src/dyf/GeometryUpload.cpp"
    "src/dyf/MaterialBindings.cpp"
    "src/dyf/ShadowPass.cpp"
    "src/dyf/ForwardPass.cpp"
	"src/dyf/TextureCache.cpp"
	"src/dyf/Scene.cpp"
	"src/Platform/Input.cpp"
	"src/Platform/ActionMap.cpp"
    "src/Platform/Window.cpp"
)

# 공통 Window는 선택된 플랫폼 팩터리만 사용한다.
if(ANDROID)
    set(DY_WINDOW_BACKEND_SOURCES src/Platform/Android/AndroidWindow.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(DY_WINDOW_BACKEND_SOURCES src/Platform/iOS/IOSWindow.mm)
    target_link_libraries(${PROJECT_NAME} PRIVATE "-framework UIKit")
elseif(WIN32)
    set(DY_WINDOW_BACKEND_SOURCES src/Platform/Glfw/GlfwWindow.cpp src/Platform/Windows/WindowsWindow.cpp)
elseif(APPLE)
    set(DY_WINDOW_BACKEND_SOURCES src/Platform/Glfw/GlfwWindow.cpp src/Platform/macOS/MacOSWindow.mm)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(DY_WINDOW_BACKEND_SOURCES src/Platform/Glfw/GlfwWindow.cpp src/Platform/Linux/LinuxWindow.cpp)
else()
    message(FATAL_ERROR "No Window backend for ${CMAKE_SYSTEM_NAME}.")
endif()
target_sources(${PROJECT_NAME} PRIVATE ${DY_WINDOW_BACKEND_SOURCES})

# global options
include(Engine_Options)
add_library(dy_engine_options ALIAS Engine_Options)
target_link_libraries(${PROJECT_NAME} PUBLIC Engine_Options)
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PUBLIC Threads::Threads)
target_include_directories(${PROJECT_NAME} BEFORE PUBLIC "${PROJECT_SOURCE_DIR}/src/Public")
target_include_directories(${PROJECT_NAME} PRIVATE "${PROJECT_SOURCE_DIR}/src")

add_library(dy_profiler_external STATIC
    src/Diagnostics/External/Profiler.cpp
    src/Diagnostics/External/RenderDocCapture.cpp)
add_library(dyf::ExternalProfilers ALIAS dy_profiler_external)
target_link_libraries(dy_profiler_external PUBLIC Engine_Options PRIVATE Threads::Threads)
target_link_libraries(${PROJECT_NAME} PRIVATE dy_profiler_external)

add_library(dy_live_profiling STATIC src/Diagnostics/LiveProfiling.cpp)
add_library(dyf::LiveProfiling ALIAS dy_live_profiling)
target_link_libraries(dy_live_profiling PUBLIC Engine_Options Threads::Threads)
target_include_directories(dy_live_profiling PRIVATE "${PROJECT_SOURCE_DIR}/src")

add_library(dy_live_connection STATIC EXCLUDE_FROM_ALL
    src/Diagnostics/LiveProtocol.cpp src/Diagnostics/LiveTransport.cpp)
add_library(dyf::LiveClient ALIAS dy_live_connection)
target_link_libraries(dy_live_connection PUBLIC dyf::LiveProfiling)
if(WIN32)
    target_link_libraries(dy_live_connection PRIVATE advapi32)
endif()
target_link_libraries(${PROJECT_NAME} PUBLIC dy_live_profiling)
if(DY_ENABLE_LIVE_CONNECTION)
    if(NOT DY_ENABLE_LIVE_PROFILING)
        message(FATAL_ERROR "DY_ENABLE_LIVE_CONNECTION requires DY_ENABLE_LIVE_PROFILING=ON.")
    endif()
    target_link_libraries(${PROJECT_NAME} PUBLIC dyf::LiveClient)
endif()

option(DY_EXTEND_MODEL "Build the optional Model extension." OFF)
option(DY_EXTEND_IMGUI "Build the optional Dear ImGui extension." OFF)
option(DY_EXTEND_ADVANCED_RENDERING "Build the optional AdvancedRendering extension." OFF)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    include(.github/ci/CiChecks.cmake)
endif()
include(Dependencies)
include(BuildOptions)
include(LogCapture)

# 기본 구현과 내장 셰이더는 같은 엔진 타깃에서 공개 RHI를 사용한다.
include(ShaderAssets)

if(DY_EXTEND_ADVANCED_RENDERING)
    add_subdirectory(src/Extend/AdvancedRendering)
endif()

if(DY_EXTEND_MODEL)
	add_subdirectory(src/Extend/Model)
endif()
if(DY_EXTEND_IMGUI OR DY_BUILD_EXECUTION_VISUALIZER)
	add_subdirectory(src/Extend/ImGui)
endif()
if(DY_BUILD_EXECUTION_VISUALIZER)
    add_subdirectory(src/Extend/ExecutionVisualizer)
endif()

# Only add the executable if this is the main project being built (not when included as a subdirectory in another project)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
	option(DY_BUILD_EXAMPLES "Build the self-contained dyf and RHI examples." OFF)
	if(DY_BUILD_EXAMPLES OR DY_CI)
		add_subdirectory(examples)
	endif()
    if(DY_CI)
        enable_testing()
        add_subdirectory(.github/ci)
    endif()
    option(DY_BUILD_PROFILING_TESTS "Build profiling contract tests." ${DY_CI})
    option(DY_BUILD_TESTS "Build RHI contract tests (Null backend requires no GPU)." ${DY_CI})
    if(DY_BUILD_PROFILING_TESTS OR DY_BUILD_TESTS)
        enable_testing()
        add_subdirectory(tests)
    endif()
endif()

# ===== Configuration Messages =====
if(ANDROID)
    message(STATUS "Android 환경 감지됨! (API ${ANDROID_PLATFORM_LEVEL}, ABI ${ANDROID_ABI})")
elseif(WIN32)
    message(STATUS "Windows 환경 감지됨!")
elseif(APPLE)
    message(STATUS "macOS 환경 감지됨!")
elseif(UNIX)
    message(STATUS "Linux 환경 감지됨!")
endif()
