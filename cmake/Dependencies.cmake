# ===== ===== Fetch ===== =====
message(STATUS "Include FetchContent...")

include(FetchContent)

# ===== ===== GLFW ===== =====
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

message(STATUS "Download and Configure glfw...")
FetchContent_Declare(
	glfw
	GIT_REPOSITORY "https://github.com/glfw/glfw.git"
	GIT_TAG "3.4"
)
FetchContent_MakeAvailable(glfw)

target_link_libraries(${PROJECT_NAME} PRIVATE glfw)
target_compile_definitions(${PROJECT_NAME} PRIVATE GLFW_INCLUDE_NONE)

# ===== ===== stb_image ===== =====
message(STATUS "Download and Configure stb_image...")
FetchContent_Declare(
    stb
    GIT_REPOSITORY "https://github.com/nothings/stb.git"
    GIT_TAG "31c1ad37456438565541f4919958214b6e762fb4"
)
FetchContent_MakeAvailable(stb)

target_include_directories(${PROJECT_NAME} PRIVATE ${stb_SOURCE_DIR})

# ===== ===== Tracy ===== =====
if(DY_ENABLE_TRACY)
    message(STATUS "Download and Configure Tracy...")
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    set(TRACY_NO_FRAME_IMAGE ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        tracy
        GIT_REPOSITORY "https://github.com/wolfpld/tracy.git"
        GIT_TAG "v0.13.1"
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(tracy)
    target_link_libraries(Engine_Options INTERFACE Tracy::TracyClient)
    target_compile_definitions(Engine_Options INTERFACE DY_TRACY_ENABLED=1)
    if(DY_TRACY_RAW_PLOTS)
        target_compile_definitions(Engine_Options INTERFACE DY_TRACY_RAW_PLOTS_ENABLED=1)
    endif()
endif()

include(Profiling)