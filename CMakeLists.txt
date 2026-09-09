cmake_minimum_required(VERSION 3.20)
project(dy_engine CXX)

# add module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(USE_METAL AND APPLE)
	enable_language(OBJCXX)
endif()

# main target
add_library(${PROJECT_NAME} STATIC
	"src/RHI/IDevice.cpp"
	"src/Graphics/Renderer.cpp"
	"src/RHI/RenderGraph.cpp"
	"src/Graphics/Private/CanvasRenderer.cpp"
	"src/Graphics/Private/DrawData.cpp"
	"src/Graphics/Private/TextureCache.cpp"
	"src/Graphics/Animation.cpp"
	"src/Graphics/Scene.cpp"
	"src/Graphics/ImageFile.cpp"
	"src/Graphics/Model.cpp"
	"src/Graphics/ModelLoader.cpp"
	"src/Graphics/ModelLoaderCommon.cpp"
	"src/Graphics/GltfModelLoader.cpp"
	"src/Graphics/UfbxModelLoader.cpp"
	"src/Platform/Window.cpp"
)

# global options
include(Engine_Options)
add_library(dy_engine_options ALIAS Engine_Options)
target_link_libraries(${PROJECT_NAME} PUBLIC Engine_Options)
target_include_directories(${PROJECT_NAME} BEFORE PUBLIC "${PROJECT_SOURCE_DIR}/src/Public")
target_include_directories(${PROJECT_NAME} PRIVATE "${PROJECT_SOURCE_DIR}/src")

include(Dependencies)
include(BuildOptions)

# CPU font data and compiled shader assets are usable independently of Renderer.
add_library(dy_graphics_assets STATIC
	"src/Graphics/Canvas.cpp"
	"src/Graphics/Font.cpp"
	"src/Graphics/MeshPrimitives.cpp"
	"src/Graphics/Private/StockShaderAssets.cpp"
)
target_compile_features(dy_graphics_assets PUBLIC cxx_std_17)
target_link_libraries(dy_graphics_assets PUBLIC Engine_Options)
target_include_directories(dy_graphics_assets PUBLIC "${PROJECT_SOURCE_DIR}/src/Public")
target_include_directories(dy_graphics_assets PRIVATE "${PROJECT_SOURCE_DIR}/src" "${stb_SOURCE_DIR}")
include(ShaderAssets)
target_link_libraries(${PROJECT_NAME} PRIVATE dy_graphics_assets)

# Only add the executable if this is the main project being built (not when included as a subdirectory in another project)
if(CMAKE_PROJECT_NAME  STREQUAL PROJECT_NAME)
	option(DY_BUILD_EXAMPLES "Build the self-contained Graphics and RHI examples." OFF)
	if(DY_BUILD_EXAMPLES)
		add_subdirectory(examples)
	endif()
	include(CTest)
	if(BUILD_TESTING)
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
