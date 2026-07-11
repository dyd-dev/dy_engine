cmake_minimum_required(VERSION 3.20)
project(dy_engine CXX)

# add module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

if(USE_METAL AND APPLE)
	enable_language(OBJCXX)
endif()

# main target
add_library(${PROJECT_NAME} STATIC
	"src/RHI/IDevice.cpp"
	"src/Graphics/Renderer.cpp"
	"src/Graphics/RenderPath.cpp"
	"src/Graphics/SkinningPass.cpp"
	"src/Graphics/GpuScene.cpp"
	"src/Graphics/Animation.cpp"
	"src/Graphics/Scene.cpp"
	"src/Graphics/ImageFile.cpp"
	"src/Graphics/Mesh.cpp"
	"src/Graphics/ModelLoader.cpp"
	"src/Graphics/ModelLoaderCommon.cpp"
	"src/Graphics/GltfModelLoader.cpp"
	"src/Graphics/UfbxModelLoader.cpp"
	"src/Graphics/ShadowMath.cpp"
	"src/Platform/Window.cpp"
)

# global options
include(Engine_Options)
target_link_libraries(${PROJECT_NAME} PUBLIC Engine_Options)

include(Dependencies)
include(BuildOptions)

include(CTest)
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
	add_subdirectory(tests)
elseif(BUILD_TESTING)
	message(STATUS "Local tests are not present; skipping test targets.")
endif()

# Only add the executable if this is the main project being built (not when included as a subdirectory in another project)
if(CMAKE_PROJECT_NAME  STREQUAL PROJECT_NAME)
	add_subdirectory(examples)
endif()

# ===== Configuration Messages =====
if(WIN32)
    message(STATUS "Windows 환경 감지됨!")
elseif(APPLE)
    message(STATUS "macOS 환경 감지됨!")
elseif(UNIX)
    message(STATUS "Linux 환경 감지됨!")
endif()
