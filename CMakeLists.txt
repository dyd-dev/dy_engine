cmake_minimum_required(VERSION 3.20)
project(dy_engine CXX)

# Configuration and external dependencies.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(MsvcNinjaDependencies)
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

include(Engine_Options)
option(DY_ENABLE_SHADER_COMPILER "Embed the optional Vulkan GLSL compiler." OFF)
if(DY_ENABLE_SHADER_COMPILER)
    if(NOT USE_VULKAN)
        message(FATAL_ERROR "DY_ENABLE_SHADER_COMPILER currently requires the Vulkan backend.")
    endif()
    if(CMAKE_VERSION VERSION_LESS "3.27")
        message(FATAL_ERROR "DY_ENABLE_SHADER_COMPILER requires CMake 3.27 or newer for glslang.")
    endif()
endif()
if(DY_BUILD_CONFORMANCE_TESTS)
    enable_testing()
endif()
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
    src/RHI/ShaderCompiler.cpp
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

if(DY_ENABLE_SHADER_COMPILER)
    # Keep third-party options local and embed its objects in the engine archive.
    # Consumers need neither glslang archives/headers nor this CMake project.
    function(dy_build_shader_compiler)
        set(BUILD_SHARED_LIBS OFF)
        set(BUILD_EXTERNAL OFF)
        set(ENABLE_SPIRV ON)
        set(ENABLE_OPT OFF)
        set(ENABLE_HLSL OFF)
        set(ENABLE_GLSLANG_BINARIES OFF)
        set(ENABLE_SPVREMAPPER OFF)
        set(ENABLE_PCH OFF)
        set(GLSLANG_TESTS OFF)
        set(GLSLANG_ENABLE_INSTALL OFF)
        FetchContent_Declare(dy_glslang
            GIT_REPOSITORY "https://github.com/KhronosGroup/glslang.git"
            GIT_TAG "15.1.0"
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(dy_glslang)
        target_sources(dy_engine PRIVATE
            $<TARGET_OBJECTS:glslang>
            $<TARGET_OBJECTS:glslang-default-resource-limits>)
        target_include_directories(dy_engine SYSTEM PRIVATE "${dy_glslang_SOURCE_DIR}")
        target_compile_definitions(dy_engine PRIVATE DY_SHADER_COMPILER_ENABLED=1)
    endfunction()
    dy_build_shader_compiler()
endif()

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
    # Use the requested alignment for STL storage shared by engine consumers.
    target_compile_definitions(dy_engine_options INTERFACE _ENABLE_EXTENDED_ALIGNED_STORAGE)
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

# Examples and Showcase are added only for a top-level engine build.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    if(DY_BUILD_EXAMPLES)
        add_subdirectory(examples)
    endif()
    if(DY_BUILD_SHOWCASE)
        add_subdirectory(showcase)
    endif()
endif()

if(DY_BUILD_CONFORMANCE_TESTS)
    add_executable(CollectorStagingTests tests/CollectorStagingTests.cpp)
    target_link_libraries(CollectorStagingTests PRIVATE dy_engine_options Threads::Threads)
    add_test(NAME CollectorStaging COMMAND CollectorStagingTests)
    if(DY_ENABLE_LIVE_PROFILING)
        add_executable(LiveCaptureTests tests/LiveCaptureTests.cpp)
        target_link_libraries(LiveCaptureTests PRIVATE dy_engine dyf::ProfilerConnection)
        add_test(NAME LiveCapture COMMAND LiveCaptureTests)
    endif()
    foreach(test IN ITEMS RendererException RendererLifecycle RendererTextureRetry RendererLiveSettings
            RendererConfigBoundary FrameProfilerCalibration RhiAuditRecovery)
        add_executable(${test}Tests "tests/${test}Tests.cpp")
        if(USE_VULKAN OR USE_D3D12 OR USE_METAL)
            target_sources(${test}Tests PRIVATE src/Backends/Null/NullDevice.cpp)
        endif()
        target_include_directories(${test}Tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(${test}Tests PRIVATE dy_engine)
        if(NOT test STREQUAL "RhiAuditRecovery")
            add_test(NAME ${test} COMMAND ${test}Tests)
        endif()
    endforeach()
    foreach(case IN ITEMS backbuffer release graph inline alignment)
        add_test(NAME RhiAuditRecovery_${case} COMMAND RhiAuditRecoveryTests ${case})
    endforeach()
    foreach(test IN ITEMS RendererBatching SceneIncrementalUpload RenderMathRegression)
        add_executable(${test}Tests "tests/${test}Tests.cpp")
        if(NOT test STREQUAL "RenderMathRegression" AND (USE_VULKAN OR USE_D3D12 OR USE_METAL))
            target_sources(${test}Tests PRIVATE src/Backends/Null/NullDevice.cpp)
        endif()
        target_include_directories(${test}Tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(${test}Tests PRIVATE dy_engine)
        add_test(NAME ${test} COMMAND ${test}Tests)
    endforeach()
    foreach(case IN ITEMS reuse cull rotating invalid-rtt hdr edits inflight failure fallback bounds handles shadow bindless)
        add_test(NAME RendererBatching_${case} COMMAND RendererBatchingTests ${case})
    endforeach()
    foreach(case IN ITEMS same-value sparse submit-retry record-retry second-range-retry all-moving fragmented
            growth replacement aba two-renderers queued bindless lighting-retry draw-retry growth-retry
            bindless-retry scratch shadow-static light-order)
        add_test(NAME SceneIncrementalUpload_${case} COMMAND SceneIncrementalUploadTests ${case})
    endforeach()
    if(WIN32)
        add_executable(GlfwOwnershipTests tests/GlfwOwnershipTests.cpp)
        target_link_libraries(GlfwOwnershipTests PRIVATE dy_engine glfw user32)
        add_test(NAME GlfwOwnership COMMAND GlfwOwnershipTests)
        set_tests_properties(GlfwOwnership PROPERTIES RUN_SERIAL TRUE)
    endif()
    add_executable(ShadowAtlasCacheTests tests/ShadowAtlasCacheTests.cpp)
    if(USE_VULKAN OR USE_D3D12 OR USE_METAL)
        target_sources(ShadowAtlasCacheTests PRIVATE src/Backends/Null/NullDevice.cpp)
    endif()
    target_include_directories(ShadowAtlasCacheTests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(ShadowAtlasCacheTests PRIVATE dy_engine)
    add_test(NAME ShadowAtlasCache COMMAND ShadowAtlasCacheTests)
    add_executable(RhiCommandCloseTests tests/RhiCommandCloseTests.cpp)
    target_link_libraries(RhiCommandCloseTests PRIVATE dy_engine)
    add_test(NAME RhiCommandClose COMMAND RhiCommandCloseTests)
    add_executable(RhiStateTableTests tests/RhiStateTableTests.cpp)
    target_link_libraries(RhiStateTableTests PRIVATE dy_engine_options)
    add_test(NAME RhiStateTable COMMAND RhiStateTableTests)
    if(USE_VULKAN)
        add_executable(VulkanPresentWorkerTests tests/VulkanPresentWorkerTests.cpp)
        target_link_libraries(VulkanPresentWorkerTests PRIVATE dy_engine_options Threads::Threads)
        add_test(NAME VulkanPresentWorker COMMAND VulkanPresentWorkerTests)
        if(WIN32)
            find_package(Vulkan REQUIRED)
            target_link_libraries(VulkanPresentWorkerTests PRIVATE user32)
            add_executable(VulkanSwapchainPolicyTests tests/VulkanSwapchainPolicyTests.cpp
                src/Backends/Vulkan/VulkanSwapchain.cpp)
            target_include_directories(VulkanSwapchainPolicyTests PRIVATE "${PROJECT_SOURCE_DIR}/src" "${glfw_SOURCE_DIR}/include" ${Vulkan_INCLUDE_DIRS})
            target_compile_definitions(VulkanSwapchainPolicyTests PRIVATE GLFW_INCLUDE_NONE)
            target_link_libraries(VulkanSwapchainPolicyTests PRIVATE dy_engine_options user32)
            foreach(case IN ITEMS alpha transform)
                add_test(NAME VulkanSwapchainPolicy_${case} COMMAND VulkanSwapchainPolicyTests ${case})
            endforeach()
            add_executable(VulkanTextureViewTests tests/VulkanTextureViewTests.cpp)
            target_include_directories(VulkanTextureViewTests PRIVATE "${PROJECT_SOURCE_DIR}/src")
            target_link_libraries(VulkanTextureViewTests PRIVATE dy_engine Vulkan::Vulkan)
            add_test(NAME VulkanTextureView COMMAND VulkanTextureViewTests)
            add_executable(VulkanTimestampReuseTests tests/VulkanTimestampReuseTests.cpp)
            target_link_libraries(VulkanTimestampReuseTests PRIVATE dy_engine Vulkan::Vulkan)
            add_test(NAME VulkanTimestampReuse COMMAND VulkanTimestampReuseTests)
            set_tests_properties(VulkanTimestampReuse PROPERTIES RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
            add_executable(VulkanFrameReadinessTests tests/VulkanFrameReadinessTests.cpp)
            target_link_libraries(VulkanFrameReadinessTests PRIVATE dy_engine Vulkan::Vulkan user32)
            add_test(NAME VulkanFrameReadiness COMMAND VulkanFrameReadinessTests)
            set_tests_properties(VulkanFrameReadiness PROPERTIES RUN_SERIAL TRUE)
        endif()
        find_program(DY_STATE_TEST_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
        set(dy_state_shader "${CMAKE_CURRENT_BINARY_DIR}/rhi_state_validation.spv")
        add_custom_command(OUTPUT "${dy_state_shader}"
            COMMAND "${DY_STATE_TEST_GLSLC}" -fshader-stage=compute --target-env=vulkan1.2 -O
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/rhi_state_validation.comp" -o "${dy_state_shader}"
            DEPENDS tests/shaders/rhi_state_validation.comp VERBATIM)
        add_executable(RhiStateValidationTests tests/RhiStateValidationTests.cpp "${dy_state_shader}")
        target_link_libraries(RhiStateValidationTests PRIVATE dy_engine)
        target_compile_definitions(RhiStateValidationTests PRIVATE TEST_SHADER_PATH="${dy_state_shader}")
        add_test(NAME RhiStateValidation COMMAND RhiStateValidationTests)
        set_tests_properties(RhiStateValidation PROPERTIES RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
    endif()
endif()
