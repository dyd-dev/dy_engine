cmake_minimum_required(VERSION 3.20)
project(dy_engine CXX)

# global options
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
# ===== ===== Language (legacy) ===== =====
# set(CMAKE_CXX_STANDARD 20)
# set(CMAKE_CXX_STANDARD_REQUIRED ON)

# abstract target for engine options
add_library(Engine_Options INTERFACE)

# ===== ===== Language ===== =====
target_compile_features(Engine_Options INTERFACE cxx_std_17)

# ===== ===== Include ===== =====
target_include_directories(Engine_Options INTERFACE
	${PROJECT_SOURCE_DIR}/src/Public
)

# ===== ===== options ===== =====
option(DY_ENABLE_SIMD "Enable dy::Math SIMD code paths when supported by the target CPU." ON)
option(DY_EXTEND_MODEL "Build the optional Model extension." ON)
option(DY_BUILD_EXAMPLES "Build the self-contained examples." ON)
option(DY_ENABLE_TRACY "Embed Tracy profiling instrumentation in engine applications." ON)
option(DY_TRACY_RAW_PLOTS "Publish per-frame Tracy plots in addition to 5 Hz averages." OFF)
option(DY_ENABLE_RENDERDOC "Enable capture controls for an already injected RenderDoc API." OFF)
option(DY_ENABLE_PIX "Enable D3D12 PIX events using WinPixEventRuntime (MSVC)." ${MSVC})

include(CheckSpec)

# main target
add_library(${PROJECT_NAME} STATIC
	"src/RHI/IDevice.cpp"
	"src/RHI/ICommandList.cpp"
	"src/RHI/Resources.cpp"
    "src/RHI/RenderGraph.cpp"

	"src/dyf/Renderer.cpp"
	"src/dyf/Image.cpp"
	"src/dyf/Canvas.cpp"
	"src/dyf/Font.cpp"
	"src/dyf/MeshPrimitives.cpp"
	"src/dyf/Shaders.cpp"
	"src/dyf/CanvasRenderer.cpp"
    "src/dyf/PostProcess.cpp"
    "src/dyf/Lighting.cpp"
	"src/dyf/DrawData.cpp"
	"src/dyf/TextureCache.cpp"
	"src/dyf/Scene.cpp"

	"src/Platform/Window.cpp"
	"src/Platform/ThreadPool.cpp"
	"src/Platform/RenderDocCapture.cpp"

    "src/dyf/Profiler.cpp"
    "src/dyf/ProfilerSampler.cpp"
)
target_link_libraries(${PROJECT_NAME} PUBLIC Engine_Options)
target_include_directories(${PROJECT_NAME} PRIVATE "${PROJECT_SOURCE_DIR}/src")

# ===== ===== Compiler options ===== =====
if(MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE /W4 /utf-8 /wd4201)
else()
	target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()

# ===== ===== Dependencies and backend ===== =====
include(Dependencies)
if(WIN32)
	if(USE_METAL)
		message(FATAL_ERROR "Windows supports D3D12 or Vulkan, not Metal.")
	endif()
	if(USE_D3D12 AND USE_VULKAN)
		message(FATAL_ERROR "Select only one Windows backend: USE_D3D12 or USE_VULKAN.")
	endif()
endif()

if(USE_D3D12)
	set(DY_BACKEND D3D12)
elseif(USE_METAL)
	set(DY_BACKEND Metal)
elseif(USE_VULKAN)
	set(DY_BACKEND Vulkan)
else()
	set(DY_BACKEND Null)
endif()

if(NOT DY_BACKEND STREQUAL "Null")
	string(TOUPPER "${DY_BACKEND}" backend_define)
	target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_${backend_define})
endif()
set_property(TARGET ${PROJECT_NAME} PROPERTY DY_BACKEND "${DY_BACKEND}")
add_subdirectory(src/Backends/${DY_BACKEND})

include(Shader)
dy_add_engine_shaders(dy_engine)

if(DY_EXTEND_MODEL)
	add_subdirectory(src/Extend/Model)
endif()

if(DY_BUILD_EXAMPLES AND CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
	add_subdirectory(examples)
endif()

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR AND (DY_BUILD_PROFILING_TESTS OR DY_BUILD_TESTS))
    enable_testing()
    add_subdirectory(tests)
endif()
