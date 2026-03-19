# ===== ===== GLFW ===== =====
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

# ===== ===== Fetch ===== =====
message(STATUS "Download and Configure glfw...")

include(FetchContent)
FetchContent_Declare(
	glfw
	GIT_REPOSITORY "https://github.com/glfw/glfw.git"
	GIT_TAG "3.4"
)
FetchContent_MakeAvailable(glfw)

target_link_libraries(${PROJECT_NAME} PRIVATE glfw)
target_compile_definitions(${PROJECT_NAME} PRIVATE GLFW_INCLUDE_NONE)