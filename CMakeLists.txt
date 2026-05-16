cmake_minimum_required(VERSION 3.20)
project(dy_engine CXX)

# add module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# main target
add_library(${PROJECT_NAME} STATIC
	"src/RHI/IDevice.cpp"
	"src/Graphics/Renderer.cpp"
	"src/Graphics/ObjLoader.cpp"
	"src/Platform/Window.cpp"
)

# global options
include(Engine_Options)
target_link_libraries(${PROJECT_NAME} PUBLIC Engine_Options)

include(Dependencies)
include(BuildOptions)

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
