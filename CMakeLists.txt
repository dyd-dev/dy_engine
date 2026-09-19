cmake_minimum_required(VERSION 3.20)
project(dy_engine CXX)

# add module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(USE_METAL AND APPLE)
	enable_language(OBJCXX)
endif()

# main target
add_library(${PROJECT_NAME} STATIC
	"src/Core/ThreadPool.cpp"
	"src/RHI/IDevice.cpp"
	"src/RHI/ICommandList.cpp"
	"src/RHI/Resources.cpp"
	"src/dyf/Renderer.cpp"
	"src/dyf/Image.cpp"
	"src/dyf/Canvas.cpp"
	"src/dyf/Font.cpp"
	"src/dyf/MeshPrimitives.cpp"
	"src/dyf/Shaders.cpp"
	"src/RHI/RenderGraph.cpp"
	"src/dyf/CanvasRenderer.cpp"
    "src/dyf/PostProcess.cpp"
    "src/dyf/Lighting.cpp"
    "src/dyf/Profiler.cpp"
    "src/dyf/ProfilerSampler.cpp"
    "src/Platform/RenderDocCapture.cpp"
	"src/dyf/DrawData.cpp"
	"src/dyf/TextureCache.cpp"
	"src/dyf/Scene.cpp"
	"src/Platform/Window.cpp"
	"src/Platform/Input.cpp"
)

# global options
include(Engine_Options)
add_library(dy_engine_options ALIAS Engine_Options)
target_link_libraries(${PROJECT_NAME} PUBLIC Engine_Options)
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PUBLIC Threads::Threads)
target_include_directories(${PROJECT_NAME} BEFORE PUBLIC "${PROJECT_SOURCE_DIR}/src/Public")
target_include_directories(${PROJECT_NAME} PRIVATE "${PROJECT_SOURCE_DIR}/src")

option(DY_EXTEND_MODEL "Build the optional Model extension." OFF)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    include(CiChecks)
endif()
include(Dependencies)
include(BuildOptions)

# 기본 구현과 내장 셰이더는 같은 엔진 타깃에서 공개 RHI를 사용한다.
include(ShaderAssets)

if(DY_EXTEND_MODEL)
	add_subdirectory(src/Extend/Model)
endif()

# Only add the executable if this is the main project being built (not when included as a subdirectory in another project)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
	option(DY_BUILD_EXAMPLES "Build the self-contained dyf and RHI examples." OFF)
	if(DY_BUILD_EXAMPLES OR DY_CI)
		add_subdirectory(examples)
	endif()
    if(DY_CI)
        include(ExampleSupport)
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
if(WIN32)
    message(STATUS "Windows 환경 감지됨!")
elseif(APPLE)
    message(STATUS "macOS 환경 감지됨!")
elseif(UNIX)
    message(STATUS "Linux 환경 감지됨!")
endif()
