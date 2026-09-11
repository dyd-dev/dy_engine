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

if(DY_ENABLE_RENDERDOC)
	if(APPLE)
		message(FATAL_ERROR "DY_ENABLE_RENDERDOC is unavailable for Metal/macOS; use Xcode GPU Capture instead.")
	endif()

	set(DY_RENDERDOC_ROOT "" CACHE PATH "RenderDoc installation or source root containing renderdoc_app.h")
	find_path(DY_RENDERDOC_INCLUDE_DIR
		NAMES renderdoc_app.h
		HINTS
			"${DY_RENDERDOC_ROOT}"
			"$ENV{RENDERDOC_ROOT}"
			"$ENV{ProgramFiles}/RenderDoc"
		PATH_SUFFIXES "" include include/renderdoc renderdoc/api/app
	)
	if(NOT DY_RENDERDOC_INCLUDE_DIR)
		message(FATAL_ERROR
			"DY_ENABLE_RENDERDOC=ON requires renderdoc_app.h. Set DY_RENDERDOC_ROOT to the RenderDoc install or source root.")
	endif()

	target_include_directories(${PROJECT_NAME} PRIVATE "${DY_RENDERDOC_INCLUDE_DIR}")
	target_compile_definitions(${PROJECT_NAME} PRIVATE DY_RENDERDOC_ENABLED=1)
	if(UNIX AND NOT APPLE)
		target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS})
	endif()
endif()

if(USE_D3D12 AND WIN32)
	set(DY_WINPIX_VERSION "1.0.240308001")
	FetchContent_Declare(
		winpixeventruntime
		URL "https://www.nuget.org/api/v2/package/WinPixEventRuntime/${DY_WINPIX_VERSION}"
		URL_HASH "SHA256=726acc93d6968e2146261a1e415521747d50ad69894c2b42b5d0d4c29fd66ec4"
	)
	FetchContent_MakeAvailable(winpixeventruntime)

	if(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
		set(DY_WINPIX_ARCH "ARM64")
	else()
		set(DY_WINPIX_ARCH "x64")
	endif()

	add_library(WinPixEventRuntime SHARED IMPORTED GLOBAL)
	set_target_properties(WinPixEventRuntime PROPERTIES
		IMPORTED_LOCATION "${winpixeventruntime_SOURCE_DIR}/bin/${DY_WINPIX_ARCH}/WinPixEventRuntime.dll"
		IMPORTED_IMPLIB "${winpixeventruntime_SOURCE_DIR}/bin/${DY_WINPIX_ARCH}/WinPixEventRuntime.lib"
		INTERFACE_INCLUDE_DIRECTORIES "${winpixeventruntime_SOURCE_DIR}/Include"
	)
	add_library(WinPixEventRuntime::WinPixEventRuntime ALIAS WinPixEventRuntime)
	target_link_libraries(${PROJECT_NAME} PRIVATE WinPixEventRuntime::WinPixEventRuntime)
endif()

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
    GIT_TAG "31c1ad37456438565541f4919958214b6e762fb4"
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
    GIT_TAG "fcc5d6ba444cfd3eb80677dba5e37e493941abe5"
)
FetchContent_MakeAvailable(ufbx)
target_include_directories(${PROJECT_NAME} PUBLIC ${ufbx_SOURCE_DIR})
set_source_files_properties("${ufbx_SOURCE_DIR}/ufbx.c" PROPERTIES LANGUAGE CXX)
target_sources(${PROJECT_NAME} PRIVATE "${ufbx_SOURCE_DIR}/ufbx.c")
