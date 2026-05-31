# ===== ===== GLFW ===== =====
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

if(UNIX AND NOT APPLE)
    set(DY_LINUX_WINDOW_SYSTEM "AUTO" CACHE STRING "Linux window system for GLFW: AUTO, X11, WAYLAND")
    set_property(CACHE DY_LINUX_WINDOW_SYSTEM PROPERTY STRINGS AUTO X11 WAYLAND)

    find_package(X11 QUIET)
    find_package(PkgConfig QUIET)

    set(DY_HAS_WAYLAND OFF)
    if(PkgConfig_FOUND)
        pkg_check_modules(WAYLAND_DEPS QUIET
            wayland-client
            wayland-cursor
            wayland-egl
            wayland-protocols
            xkbcommon
        )
        find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner)
        if(WAYLAND_DEPS_FOUND AND WAYLAND_SCANNER_EXECUTABLE)
            set(DY_HAS_WAYLAND ON)
        endif()
    endif()

    if(DY_LINUX_WINDOW_SYSTEM STREQUAL "X11")
        if(NOT X11_FOUND)
            message(FATAL_ERROR "DY_LINUX_WINDOW_SYSTEM=X11 was requested, but X11 development packages were not found.")
        endif()
        message(STATUS "GLFW: using X11")
        set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
        set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    elseif(DY_LINUX_WINDOW_SYSTEM STREQUAL "WAYLAND")
        if(NOT DY_HAS_WAYLAND)
            message(FATAL_ERROR "DY_LINUX_WINDOW_SYSTEM=WAYLAND was requested, but Wayland development packages were not found.")
        endif()
        message(STATUS "GLFW: using Wayland")
        set(GLFW_BUILD_X11 OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_WAYLAND ON CACHE BOOL "" FORCE)
    elseif(DY_LINUX_WINDOW_SYSTEM STREQUAL "AUTO")
        if(X11_FOUND)
            message(STATUS "GLFW: using X11")
            set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
            set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
        elseif(DY_HAS_WAYLAND)
            message(STATUS "GLFW: using Wayland")
            set(GLFW_BUILD_X11 OFF CACHE BOOL "" FORCE)
            set(GLFW_BUILD_WAYLAND ON CACHE BOOL "" FORCE)
        else()
            message(FATAL_ERROR "Neither X11 nor Wayland development packages were found for GLFW.")
        endif()
    else()
        message(FATAL_ERROR "DY_LINUX_WINDOW_SYSTEM must be AUTO, X11, or WAYLAND.")
    endif()
endif()

# ===== ===== Fetch ===== =====
message(STATUS "Download and Configure glfw...")

include(FetchContent)
FetchContent_Declare(
	glfw
	GIT_REPOSITORY "https://github.com/glfw/glfw.git"
	GIT_TAG "3.4"
)
FetchContent_MakeAvailable(glfw)

# stb_image 자동 다운로드 설정
FetchContent_Declare(
    stb
    GIT_REPOSITORY "https://github.com/nothings/stb.git"
    GIT_TAG "master"
)
FetchContent_MakeAvailable(stb)

target_link_libraries(${PROJECT_NAME} PRIVATE glfw)
target_compile_definitions(${PROJECT_NAME} PRIVATE GLFW_INCLUDE_NONE)

target_include_directories(${PROJECT_NAME} PUBLIC ${stb_SOURCE_DIR})

# fastgltf 자동 다운로드 설정
FetchContent_Declare(
    fastgltf
    GIT_REPOSITORY "https://github.com/spnda/fastgltf.git"
    GIT_TAG "v0.9.0" # 최신 버전
)
FetchContent_MakeAvailable(fastgltf)
target_link_libraries(${PROJECT_NAME} PUBLIC fastgltf::fastgltf)

# ufbx 자동 다운로드 설정
FetchContent_Declare(
    ufbx
    GIT_REPOSITORY "https://github.com/ufbx/ufbx.git"
    GIT_TAG "master"
)
FetchContent_MakeAvailable(ufbx)
target_include_directories(${PROJECT_NAME} PUBLIC ${ufbx_SOURCE_DIR})
set_source_files_properties("${ufbx_SOURCE_DIR}/ufbx.c" PROPERTIES LANGUAGE CXX)
target_sources(${PROJECT_NAME} PRIVATE "${ufbx_SOURCE_DIR}/ufbx.c")
