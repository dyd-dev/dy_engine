cmake_minimum_required(VERSION 3.20)
project(dy_engine_app LANGUAGES CXX C)

set(DY_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../..")

# Vulkan 백엔드 활성화
set(USE_VULKAN ON CACHE BOOL "Enable Vulkan Backend" FORCE)
set(DY_BUILD_EXAMPLES OFF CACHE BOOL "Disable desktop examples" FORCE)
set(DY_BUILD_TESTS OFF CACHE BOOL "Disable tests" FORCE)

# dy_engine 엔진 코어 추가
add_subdirectory("${DY_ENGINE_ROOT}" dy_engine_core EXCLUDE_FROM_ALL)

# NDK native_app_glue
set(NATIVE_APP_GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")

# 셰이더 컴파일러 스크립트
include("${DY_ENGINE_ROOT}/cmake/CompileShaders.cmake")

# 기본 샘플 설정 (07_PBRLighting 또는 08_ShadowMapping)
if(NOT DY_ACTIVE_SAMPLE)
    set(DY_ACTIVE_SAMPLE "07_PBRLighting")
endif()

message(STATUS "Selected Android Sample: ${DY_ACTIVE_SAMPLE}")

if(DY_ACTIVE_SAMPLE STREQUAL "08_ShadowMapping")
    set(SAMPLE_SOURCE "${DY_ENGINE_ROOT}/examples/advanced/08_ShadowMapping/main.cpp")
    set(SHADER_DIR "${DY_ENGINE_ROOT}/examples/advanced/08_ShadowMapping/Shaders")

    add_library(dy_engine_app SHARED
        AndroidMain.cpp
        "${SAMPLE_SOURCE}"
        "${NATIVE_APP_GLUE_DIR}/android_native_app_glue.c"
    )

    dy_compile_shader(dy_engine_app vertex vert
        GLSL "${SHADER_DIR}/vertex.glsl" HLSL "${SHADER_DIR}/vertex.hlsl"
        METAL "${SHADER_DIR}/vertex.metal" METAL_ENTRY vertexMain
        INCLUDE_DIRS "${SHADER_DIR}")
    dy_compile_shader(dy_engine_app shadowDepth frag
        GLSL "${SHADER_DIR}/shadowDepth.glsl" HLSL "${SHADER_DIR}/shadowDepth.hlsl"
        METAL "${SHADER_DIR}/shadowDepth.metal" METAL_ENTRY fragmentMain
        INCLUDE_DIRS "${SHADER_DIR}")
    dy_compile_shader(dy_engine_app lighting frag
        GLSL "${SHADER_DIR}/lighting.glsl" HLSL "${SHADER_DIR}/lighting.hlsl"
        METAL "${SHADER_DIR}/lighting.metal" METAL_ENTRY fragmentMain
        INCLUDE_DIRS "${SHADER_DIR}")

    target_compile_definitions(dy_engine_app PRIVATE DY_ACTIVE_SAMPLE_SHADOW=1)
else()
    set(SAMPLE_SOURCE "${DY_ENGINE_ROOT}/examples/advanced/07_PBRLighting/main.cpp")
    set(SHADER_DIR "${DY_ENGINE_ROOT}/examples/advanced/07_PBRLighting/Shaders")

    add_library(dy_engine_app SHARED
        AndroidMain.cpp
        "${SAMPLE_SOURCE}"
        "${NATIVE_APP_GLUE_DIR}/android_native_app_glue.c"
    )

    dy_compile_shader(dy_engine_app vertex vert
        GLSL "${SHADER_DIR}/vertex.glsl" HLSL "${SHADER_DIR}/vertex.hlsl"
        METAL "${SHADER_DIR}/vertex.metal" METAL_ENTRY vertexMain
        INCLUDE_DIRS "${SHADER_DIR}")
    dy_compile_shader(dy_engine_app lighting frag
        GLSL "${SHADER_DIR}/lighting.glsl" HLSL "${SHADER_DIR}/lighting.hlsl"
        METAL "${SHADER_DIR}/lighting.metal" METAL_ENTRY fragmentMain
        INCLUDE_DIRS "${SHADER_DIR}")

    target_compile_definitions(dy_engine_app PRIVATE DY_ACTIVE_SAMPLE_PBR=1)
endif()

target_include_directories(dy_engine_app PRIVATE
    "${NATIVE_APP_GLUE_DIR}"
    "${DY_ENGINE_ROOT}/src/Public"
    "${DY_ENGINE_ROOT}/examples/advanced/common"
)

if(TARGET Vulkan::Vulkan)
    set(DY_VULKAN_TARGET Vulkan::Vulkan)
else()
    find_library(VULKAN_LIB vulkan)
    set(DY_VULKAN_TARGET ${VULKAN_LIB})
endif()

target_link_libraries(dy_engine_app PRIVATE
    dy_engine
    android
    log
    ${DY_VULKAN_TARGET}
)
